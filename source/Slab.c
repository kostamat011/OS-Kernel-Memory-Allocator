#define _CRT_SECURE_NO_WARNINGS
#include "slab.h"
#include <string.h>
#include "Utility.h"
#include "BuddyAllocator.h"



void init_cache(kmem_cache_t* new_cache, const char* name, size_t size, void(*ctor)(void*), void(*dtor)(void*)) {

	// init name of the cache and all empty slab lists
	strcpy(new_cache->name, name);
	new_cache->slabs_empty = NULL;
	new_cache->slabs_partial = NULL;
	new_cache->slabs_full = NULL;

	// init slab count and object count and used % to 0
	new_cache->slab_count = 0;
	new_cache->object_count = 0;

	// shrink protect
	new_cache->recently_added = 1;

	// set object size
	new_cache->obj_size = size;

	//calculate size for free map zone and unused space and num of objects per slab

	calculate_slab_areas(size,
		&new_cache->free_map_size,
		&new_cache->objects_per_slab,
		&new_cache->unused_space);

	// init L1 offset to 0
	new_cache->next_L1_offset = 0;

	// init ctor and dtor to null 
	new_cache->ctor = ctor;
	new_cache->dtor = dtor;

	// init error code to 0
	new_cache->error_code = 0;

	// insert into list
	new_cache->next = kmem_header->cache_head;
	kmem_header->cache_head = new_cache;

	new_cache->cache_mutex = CreateMutex(NULL, FALSE, NULL);
	if (!new_cache->cache_mutex) {
		printf("Error creating mutex for cache: %s\n", new_cache->name);
	}
}

void move_partial_full(kmem_cache_t* cache)
{
	// take first slab from partial list and move it to full list
	kmem_slab_t* slab = cache->slabs_partial;
	cache->slabs_partial = slab->next;
	slab->next = cache->slabs_full;
	cache->slabs_full = slab;
}

void move_empty_partial(kmem_cache_t* cache)
{
	// take first slab from empty list and move it to partial list
	kmem_slab_t* slab = cache->slabs_empty;
	cache->slabs_empty = slab->next;
	slab->next = cache->slabs_partial;
	cache->slabs_partial = slab;
}

void move_full_partial(kmem_cache_t* cache, kmem_slab_t* slab)
{
	// remove slab from full list and insert it to partial list
	kmem_slab_t* curr = cache->slabs_full, *prev = NULL;
	while (curr && curr != slab) {
		prev = curr;
		curr = curr->next;
	}
	// not found in full slabs, stop
	if (!curr) return;

	// found, remove it from full list
	if (!prev) {
		cache->slabs_full = curr->next;
	}
	else {
		prev->next = curr->next;
	}

	// add to partial list
	slab->next = cache->slabs_partial;
	cache->slabs_partial = slab;

}

void move_partial_empty(kmem_cache_t* cache, kmem_slab_t* slab)
{
	// remove slab from full list and insert it to partial list
	kmem_slab_t* curr = cache->slabs_partial, * prev = NULL;
	while (curr && curr != slab) {
		prev = curr;
		curr = curr->next;
	}
	// not found in partial slabs, stop
	if (!curr) return;

	// found, remove it from partial list
	if (!prev) {
		cache->slabs_partial = curr->next;
	}
	else {
		prev->next = curr->next;
	}

	// add to empty list
	slab->next = cache->slabs_empty;
	cache->slabs_empty = slab;
}

int slab_empty(kmem_cache_t* cache,kmem_slab_t* slab)
{
	octet* free = slab->free_slots_map;
	for (int i = 0; i < cache->free_map_size; ++i) {
		
		// if any bit is 1 slab is not empty
		if (*free != 0)
			return 0;

		++free;
	}
}

int partial_slab_full(kmem_cache_t* cache)
{
	// return 1 if partial slab became full else return 0

	octet* free_map = cache->slabs_partial->free_slots_map;

	// iterate trough all slots
	for (int i = 0; i < cache->objects_per_slab; ++i)
	{
		// after every 8 slots move to next octet in free map
		if ((i % BITS_PER_BYTE == 0) && (i != 0))
			++free_map;

		// if slot is empty slab is not full, stop
		unsigned shift = (BITS_PER_BYTE - (i % BITS_PER_BYTE) - 1);
		octet mask = 0b1 << shift;
		if ((*free_map & mask)==0) {
			return 0;
		}

	}

	return 1; 
}

int get_free_slot(kmem_cache_t* parent_cache, void** address) {

	// if partial slot exist take from it, else take from empty
	kmem_slab_t* curr_slab = parent_cache->slabs_partial;
	if (!curr_slab) {
		curr_slab = parent_cache->slabs_empty;
	}

	// if there is no empty or partial stop
	if (!curr_slab) {
		*address = 0;
		return SLOT_NOT_FOUND;
	}

	octet* free_map = curr_slab->free_slots_map;
	ptr_t current_addr = (ptr_t)curr_slab->obj_start_addr;

	// iterate trough all slots to find free slot
	for (int i = 0; i < parent_cache->objects_per_slab; ++i)
	{
		// after every 8 slots move to next octet in free map
		if ((i % BITS_PER_BYTE == 0) && (i != 0))
			++free_map;

		// prepare mask for checking 
		unsigned shift = (BITS_PER_BYTE - (i % BITS_PER_BYTE) - 1);
		octet mask = 0b1 << shift;

		// if slot is empty mark it as full and return it's address
		if ( (*free_map & mask) == 0) {

			*free_map |= mask;

			*address = current_addr;

			return(curr_slab == parent_cache->slabs_partial) ? SLOT_FOUND_PARTIAL : SLOT_FOUND_EMPTY;
		}

		// if slot is full move to next slot
		else {
			current_addr += parent_cache->obj_size;
		}

	}

	// this was partial or empty slab but free slot not found
	// this means there is an error in the cache
	parent_cache->error_code = INCONSISTENT_SLAB_ERROR;
	*address = 0;
	return SLOT_NOT_FOUND;
}

int find_containing_slab(kmem_cache_t* cachep, void* objp, kmem_slab_t** res)
{
	kmem_slab_t* current_slab = cachep->slabs_full;

	// first check full slabs
	while (current_slab) {

		// check if addr is > than first obj addr and < than last
		ptr_t first = (ptr_t)current_slab->obj_start_addr;
		ptr_t last = first + cachep->objects_per_slab * cachep->obj_size;
		if (objp >= first && objp <= last) {

			*res = current_slab;
			return OBJ_FOUND_FULL;
		}

		current_slab = current_slab->next;
	}


	// if not found in full check partial slabs
	if (!current_slab) {

		current_slab = cachep->slabs_partial;
		while (current_slab) {

			// check if addr is > than first obj addr and < than last
			ptr_t first = (ptr_t)current_slab->obj_start_addr;
			ptr_t last = first + cachep->objects_per_slab * cachep->obj_size;
			if (objp >= first && objp <= last) {

				*res = current_slab;
				return OBJ_FOUND_PARTIAL;
			}

			current_slab = current_slab->next;
		}
	}

	// not found
	return OBJ_NOT_FOUND;
}


unsigned calculate_slab_blocks(size_t obj_size)
{
	// minimal cache contains header, 1 octet for map and 1 object
	size_t min_size = sizeof(kmem_slab_t) + sizeof(octet) + obj_size;

	// minimal number of blocks
	int block_num = ceil((double)min_size / BLOCK_SIZE);

	// it is rounded to first higher pow of 2 
	return closest_higher_pow2(block_num);
}




void calculate_slab_areas(size_t obj_size, size_t *map_size_p, unsigned *num_of_obj_p, size_t *unused_space_p){
	
	// slab space = header + free map + objects(slots)
	// header is always fixed size
	// this function has to find sizes of free map zone and objects zone
	// function also returns size of unused space in the slab

	size_t slab_size = calculate_slab_blocks(obj_size)*BLOCK_SIZE;

	unsigned num_of_obj = 0;
	size_t map_size = 1;

	// free space for map and object is total size - header size
	size_t space = slab_size - sizeof(kmem_slab_t);

	// increment both values in a loop while there is free space
	while(1){

		// map size should increase by 1 byte for every 8 slots
		size_t next_map_size = ((num_of_obj + 1) % BITS_PER_BYTE == 0) ? map_size + 1 : map_size;

		// check if adding one more slot will cause overflow
		if ((next_map_size + (num_of_obj + 1) * obj_size) <= space) {

			// no overflow, add one more slot
			++num_of_obj;
			map_size = next_map_size;
		}

		else {
			// overflow, stop
			break;
		}
	} 

	*map_size_p = map_size;
	*num_of_obj_p = num_of_obj;

	// space that is left after objects until the end of slab is unused
	*unused_space_p = space - (map_size + num_of_obj * obj_size);

}

unsigned total_cache_blocks(kmem_cache_t* cachep)
{
	// total size = header + num of slabs * size of 1 slab
	unsigned total_size = sizeof(kmem_cache_t) + cachep->slab_count * calculate_slab_blocks(cachep->obj_size) * BLOCK_SIZE;
	unsigned total_blocks = ceil((double)(total_size) / BLOCK_SIZE);
	return total_blocks;
}



kmem_cache_t* find_cache(const char* name){

	//*****************************mutex wait************************************
	DWORD wait_result = WaitForSingleObject(kmem_header->cache_list_mutex, INFINITE);
	// could not get mutex
	if (wait_result != WAIT_OBJECT_0) {
		return NULL;
	}
	//***************************************************************************

	// iterate trough all caches in list to find requested name
	kmem_cache_t* curr = kmem_header->cache_head;
	while (curr) {
		if (strcmp(curr->name, name) == 0) {

			//*****************************mutex signal************************************
			if (!ReleaseMutex(kmem_header->cache_list_mutex)) {
				printf("Error in releasing mutex for list of all caches");
			}
			//*****************************************************************************
			return curr;
		}
		curr = curr->next;
	}

	//*****************************mutex signal************************************
	if (!ReleaseMutex(kmem_header->cache_list_mutex)) {
		printf("Error in releasing mutex for list of all caches");
	}
	//*****************************************************************************

	return NULL; // not found
}

void print_list_of_caches(){
	kmem_cache_t* curr = kmem_header->cache_head;
	while (curr) {
		kmem_cache_info(curr);
		printf("\n");
		curr = curr->next;
	}
}

void kmem_init(void* space, int block_num){

	// initialize buddy allocator
	b_init(space, block_num);

	// place kmem header after buddy header inside 1st block
	kmem_header = (kmem_header_t*)b_header->header_end;

	// init list of all caches to NULL
	kmem_header->cache_head = NULL;


	// create mutex for list of all caches
	kmem_header->cache_list_mutex = CreateMutex(NULL, FALSE, NULL);
	if (!kmem_header->cache_list_mutex) {
		printf("Error creating mutex for list of all caches");
	}
	
	// initialize cache of caches
	init_cache(&kmem_header->cache_of_caches, "cachecache", sizeof(kmem_cache_t), NULL, NULL);

	// initialize small mem buffers
	char name_buffer[CACHE_NAME_SIZE];
	for (int i = SMALL_BUFFER_LOWER_LIMIT; i <= SMALL_BUFFER_UPPER_LIMIT; ++i) {

		// name of small buffer
		sprintf(name_buffer, "size-%d", i);
		init_cache(&kmem_header->small_buffer_caches[i], name_buffer, pow(2, i), NULL, NULL);
	}

	// set head of caches list to cache of caches
	kmem_header->cache_head = &kmem_header->cache_of_caches;

	// set ending address 
	kmem_header->header_end = (ptr_t)kmem_header + sizeof(kmem_header_t);
}

kmem_cache_t* kmem_cache_create(const char* name, size_t size, void(*ctor)(void*), void(*dtor)(void*)){

	// if cache already exists return it
	kmem_cache_t* found = find_cache(name);
	if (found) return found;

	// allocate one cache from cache of caches
	ptr_t free_addr = (ptr_t)kmem_cache_alloc(&kmem_header->cache_of_caches);

	if (!free_addr) {
		kmem_header->cache_of_caches.error_code = ALLOCATION_ERROR;
		printf("ERROR in kmem_cache_create: allocation failed\nerror code: %d\n",kmem_header->cache_of_caches.error_code);
		return NULL;
	}

	// initialize new cache
	kmem_cache_t* new_cache = (kmem_cache_t*)free_addr;
	init_cache(new_cache, name, size, ctor, dtor);

	return new_cache;
}

int extend_cache(kmem_cache_t* cache) {

	// return is error code

	// calculate num of blocks needed for 1 slab and allocate it
	unsigned block_num = calculate_slab_blocks(cache->obj_size);
	kmem_slab_t* new_slab = (kmem_slab_t*)b_alloc(block_num);

	if (!new_slab) {
		//buddy allocation failed, error code 1
		return 1;
	}

	// free map starts after header
	new_slab->free_slots_map = (octet*)new_slab + sizeof(kmem_slab_t);

	// initialize free map to al 0 (all free slots)
	octet* free_map = new_slab->free_slots_map;
	for (int i = 0; i < cache->free_map_size; ++i) {
		*(free_map) = 0;
		++free_map;
	}

	// assign L1 offset from cache
	new_slab->L1_offset = cache->next_L1_offset;

	// objects start after header + free map size + L1 offset
	new_slab->obj_start_addr = (ptr_t)new_slab + sizeof(kmem_slab_t) + cache->free_map_size + cache->next_L1_offset;

	// update next L1 offset for cache
	cache->next_L1_offset = (cache->next_L1_offset + 64 > cache->unused_space)
		? 0
		: cache->next_L1_offset + 64;


	// initialize all objects by calling constructors
	ptr_t current_addr = (ptr_t)new_slab->obj_start_addr;
	for (int i = 0; i < cache->objects_per_slab; ++i) {

		if (cache->ctor != NULL) {
			cache->ctor(current_addr);
		}

		current_addr += cache->obj_size;
	}

	// add new slab to empty slabs list 
	new_slab->next = cache->slabs_empty;
	cache->slabs_empty = new_slab;

	// incr cache slab count and update used_pct
	cache->slab_count++;
	cache->recently_added = 1;

	return 0;
}


int kmem_cache_shrink(kmem_cache_t* cachep){

	//*****************************mutex wait************************************
	DWORD wait_result = WaitForSingleObject(cachep->cache_mutex, INFINITE);
	// could not get mutex
	if (wait_result == WAIT_ABANDONED) {
		return NULL;
	}
	//***************************************************************************

	if (cachep->recently_added==1) {
		cachep->recently_added = 0;
		//*****************************mutex signal************************************
		if (!ReleaseMutex(cachep->cache_mutex)) {
			printf("Error in releasing mutex for cache: %s\n", cachep->name);
		}
		//*****************************************************************************
		return 0;
	}

	// deallocate all empty slabs 
	// update slab count of cachep
	// returns count of deallocated slabs

	kmem_slab_t* curr_slab = cachep->slabs_empty;

	int cnt = 0;

	while (curr_slab) {
		kmem_slab_t* tmp = curr_slab;
		curr_slab = curr_slab->next;
		b_free(tmp, calculate_slab_blocks(cachep->obj_size));
		++cnt;
		cachep->slab_count--;
	}

	cachep->slabs_empty = NULL;

	//*****************************mutex signal************************************
	if (!ReleaseMutex(cachep->cache_mutex)) {
		printf("Error in releasing mutex for cache: %s\n", cachep->name);
	}
	//*****************************************************************************

	return cnt;
}

void* kmem_cache_alloc(kmem_cache_t* cachep)
{
	//*****************************mutex wait************************************
	DWORD wait_result = WaitForSingleObject(cachep->cache_mutex, INFINITE);
	// could not get mutex
	if (wait_result != WAIT_OBJECT_0) {
		return NULL;
	}
	//***************************************************************************

	void* free_addr = NULL;

	// try to find free slot in partial or empty slab
	// if free slot is found address of slot is returned through free_addr argument
	// if free slot is found this function will adjust its bit to not free (1)
	int result_code = get_free_slot(cachep, &free_addr);
	
	// no empty or partial slab is found, must extend the cache
	if (result_code==SLOT_NOT_FOUND) {

		int ecd = extend_cache(cachep);

		if (ecd != 0) {

			// error - cache extension failed
			printf("ERROR: cache extension failed. error code %d\n", ecd);
			cachep->error_code = ecd;

			//*****************************mutex signal************************************
			if (!ReleaseMutex(cachep->cache_mutex)) {
				printf("Error in releasing mutex for cache: %s\n", cachep->name);
			}
			//*****************************************************************************

			return NULL;
		}

		// call get_free_slot again, now 1 emtpy slab must exist
		result_code = get_free_slot(cachep, &free_addr);
	}

	// incr object count and update used_pct
	cachep->object_count++;

	// found free slot in partial slab
	if (result_code == SLOT_FOUND_PARTIAL) {

		// if partial slab is now full move it to full list
		if (partial_slab_full(cachep)) {
			move_partial_full(cachep);
		}

	}

	// found free slot in empty slab
	else if (result_code == SLOT_FOUND_EMPTY) {

		// empty slab is now partial move it to partial list
		move_empty_partial(cachep);
		// if partial slab is now full move it to full list
		if (partial_slab_full(cachep)) {
			move_partial_full(cachep);
		}

	}

	// error - free slot not found even when new empty slab was created
	else if (result_code == SLOT_NOT_FOUND) {

		cachep->error_code = INCONSISTENT_SLAB_ERROR;
		printf("ERROR: kmalloc failed. no free slot found in empty slab. cache %s error code %d\n",cachep->name, cachep->error_code);
	}
	
	//*****************************mutex signal************************************
	if (!ReleaseMutex(cachep->cache_mutex)) {
		printf("Error in releasing mutex for cache: %s\n", cachep->name);
	}
	//*****************************************************************************

	// return address of the object
	return free_addr;
}

void kmem_cache_free(kmem_cache_t* cachep, void* objp)
{
	//*****************************mutex wait************************************
	DWORD wait_result = WaitForSingleObject(cachep->cache_mutex, INFINITE);
	// could not get mutex
	if (wait_result != WAIT_OBJECT_0) {
		return;
	}
	//***************************************************************************

	kmem_slab_t* current_slab = NULL;

	// check in what slab objects address fall into
	// address of slab is returned trough current_slab argument

	int result_code = find_containing_slab(cachep, objp, &current_slab);

	
	// found , adjust free bit in container slab
	if (result_code!=OBJ_NOT_FOUND) {

		octet* free_map = current_slab->free_slots_map;

		for (int i = 0; i < cachep->objects_per_slab; ++i)
		{
			// after every 8 slots move to next octet in free map
			if ((i % BITS_PER_BYTE == 0) && (i != 0))
				++free_map;

			// calculate address of next slot
			ptr_t addr = (ptr_t)current_slab->obj_start_addr + i * cachep->obj_size;

			// prepare mask for checking if object is free
			unsigned shift = (BITS_PER_BYTE - (i % BITS_PER_BYTE) - 1);
			octet mask = 1 << shift;

			// found the object
			if (addr == objp) {

				// if object is already free print message and exit
				if (!(*free_map & mask)) {

					cachep->error_code = DEALLOCATION_ERROR;
					printf("ERROR: kmem_cache_free: slot is already free.\nerror code: %d\n", cachep->error_code);

					//*****************************mutex signal************************************
					if (!ReleaseMutex(cachep->cache_mutex)) {
						printf("Error in releasing mutex for cache: %s\n", cachep->name);
					}
					//*****************************************************************************

					return;
				}

				// call destructor if defines
				if (cachep->ctor != NULL)
					cachep->ctor(addr);

				/*// call constructor if defined
				if (cachep->ctor != NULL)
					cachep->ctor(addr);*/

				// switch free bit to 0
				// to kill bit 2 mask is ~(1<<(8-2-1)) = 11011111
				*free_map &= (~mask);

				// decr object count 
				cachep->object_count--;

				// if object was in full slab that slab is now partial
				if (result_code == OBJ_FOUND_FULL) {
					move_full_partial(cachep, current_slab);

					// if that slab became empty move it to empty list
					if (slab_empty(cachep, current_slab))
					{
						move_partial_empty(cachep, current_slab);
					}
				}

				// if object was in partial slab than check if slab became empty
				else if (result_code == OBJ_FOUND_PARTIAL) {

					// if that slab became empty move it to empty list
					if (slab_empty(cachep, current_slab))
					{
						move_partial_empty(cachep, current_slab);
					}
				}

				//*****************************mutex signal************************************
				if (!ReleaseMutex(cachep->cache_mutex)) {
					printf("Error in releasing mutex for cache: %s\n", cachep->name);
				}
				//*****************************************************************************

				return;
			}

			
		}
	}

	/*else {
		// objects address was not found in the slab 
		cachep->error_code = 3;
		//printf("ERROR in kmem_cache_free: object's address not found in the slab\nerror code: %d\n", cachep->error_code);
	}*/
}

void* kmalloc(size_t size)
{
	// size must be rounded to closest higher power of 2 
	size = closest_higher_pow2(size);

	// small_buffers[i] is size 2 ^ i
	int small_buff_index = log2(size);

	// small buffer size must be 2^5 - 2^17
	if (small_buff_index < 5 || small_buff_index > 17) {
		printf("ERROR in kmalloc: small buffer in that size does not exist.\n");
		return NULL;
	}

	//*****************************mutex wait****************************************
	DWORD wait_result = WaitForSingleObject(kmem_header->small_buffer_caches[small_buff_index].cache_mutex, INFINITE);
	// could not get mutex
	if (wait_result != WAIT_OBJECT_0) {
		return;
	}
	//*******************************************************************************

	void* addr = kmem_cache_alloc(&(kmem_header->small_buffer_caches[small_buff_index]));
	if (!addr) {
		printf("ERROR in kmalloc: allocation failed\nerror code: %d\n", kmem_header->small_buffer_caches[small_buff_index].error_code);
	}
	//*****************************mutex signal************************************
	if (!ReleaseMutex(kmem_header->small_buffer_caches[small_buff_index].cache_mutex)) {
		printf("Error in releasing mutex for cache: %s\n", kmem_header->small_buffer_caches[small_buff_index].name);
	}
	//*****************************************************************************
	return addr;
}

void kfree(const void* objp)
{

	// iterate trough all small buffer caches and call kmem_cache_free

	for (int i = SMALL_BUFFER_LOWER_LIMIT; i <= SMALL_BUFFER_UPPER_LIMIT; ++i) {
		kmem_cache_t* current_cache = &(kmem_header->small_buffer_caches[i]);
		int num_of_objs = kmem_header->small_buffer_caches[i].object_count;
		kmem_cache_free(&(kmem_header->small_buffer_caches[i]), (void*)objp);

		// if free succeded then leave loop
		if (num_of_objs == kmem_header->small_buffer_caches[i].object_count + 1)
			break;
	}

}

void kmem_cache_destroy(kmem_cache_t* cachep)
{
	//*****************************mutex wait****************************************
	// this wait is on mutex for list of caches
	// wait on caches mutex will be done in kmem_cache_free call
	DWORD wait_result = WaitForSingleObject(kmem_header->cache_list_mutex, INFINITE);
	// could not get mutex
	if (wait_result != WAIT_OBJECT_0) {
		return;
	}
	//*******************************************************************************

	// remove cache from list of caches
	kmem_cache_t* curr = kmem_header->cache_head, * prev = NULL;
	while (curr && curr != cachep) {
		prev = curr;
		curr = curr->next;
	}
	if (!curr)
		return;
	if (curr == kmem_header->cache_head) {
		kmem_header->cache_head = curr->next;
	}
	else {
		prev->next = curr->next;
	}

	//*****************************mutex signal************************************
	if (!ReleaseMutex(kmem_header->cache_list_mutex)) {
		printf("Error in releasing mutex for cache: %s\n", cachep->name);
	}
	//*****************************************************************************


	// deallocate it from cache of caches
	kmem_cache_free(&(kmem_header->cache_of_caches), cachep);
}

void kmem_cache_info(kmem_cache_t* cachep)
{	
	WaitForSingleObject(kmem_header->cache_of_caches.cache_mutex, INFINITE);
	printf("\n");
	printf("Cache name: %s\n", cachep->name);
	printf("Object size: %dB\n", cachep->obj_size);
	printf("Cache size: %d blocks\n", total_cache_blocks(cachep));
	printf("Number of slabs: %d\n", cachep->slab_count);
	printf("Number of objects per slab: %d\n", cachep->objects_per_slab);
	int used_pct = (cachep->slab_count==0) ? 0: 100 * (double)(cachep->object_count) / (double)(cachep->slab_count * cachep->objects_per_slab);
	printf("Fullnes %: %d%%\n", used_pct );
	ReleaseMutex(kmem_header->cache_of_caches.cache_mutex);
	printf("\n");
}

int kmem_cache_error(kmem_cache_t* cachep)
{
	printf("ERROR CODE: %d\n", cachep->error_code);
	return cachep->error_code;
}
