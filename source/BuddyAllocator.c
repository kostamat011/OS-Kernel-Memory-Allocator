#include"BuddyAllocator.h"
#include"Utility.h"


buddy_header_t* b_header = NULL;

void b_init(void* memstart, int blocknum) {

	// put buddy header in first recieved block 
	// currently header takes 140B
	b_header = (buddy_header_t*)memstart;

	// create mutex for buddy allocator
	b_header->buddy_mutex = CreateMutex(NULL, FALSE, NULL);
	if (!b_header->buddy_mutex) {
		printf("Error creating mutex for buddy allocator");
	}

	// first block is reserved for buddy header
	b_header->header_start = (ptr_t)memstart;

	// record end of buddy header to know where free space continues inside 1st block
	b_header->header_end = b_header->header_start + sizeof(buddy_header_t);

	// avaliable memory starts from block 2 
	b_header->mem_start = (block_ptr_t)memstart + 1;
	blocknum--;

	// set the number of avaliable blocks (including 1 block for header)
	b_header->block_num = blocknum;

	// initialization of buddy lists
	for (int i = 0; i < BUDDY_SIZE; ++i){
		b_header->buddies[i] = NULL;
	}
	
	// initialization of free space
	block_ptr_t current_mem = b_header->mem_start;
	while (blocknum != 0) {

		// closest lower logarithm of 2 = index of list to add free space
		int i = closest_lower_log2(blocknum);
		
		// allocate one node in i-th list
		b_header->buddies[i] = (mem_node_t*) (current_mem);
		b_header->buddies[i]->next = NULL;

		int shift = pow(2, i);
		// next free portion begins after 2^i blocks
		current_mem += shift; 

		// continue for the remaining blocks
		blocknum -= pow(2, i);
	}
	
}

void * b_alloc(int block_num) {

	//*****************************mutex wait************************************
	DWORD wait_result = WaitForSingleObject(b_header->buddy_mutex, INFINITE);
	// could not get mutex
	if (wait_result != WAIT_OBJECT_0) {
		return NULL;
	}
	//***************************************************************************


	// if it asks for more memory than total amount of memory stop now
	if (block_num > b_header->block_num) {
		printf("NOT ENOUGH MEMORY. ALLOCATION FAILED\n");
		return NULL;
	}

	void* ret_addr = NULL;

	// block_number is rounded to nearest higher power of 2
	// index in buddies is nearest higher log of 2 
	int buddy_index = closest_higher_log2(block_num);

	// if there is a free portion in requested size take it
	if (b_header->buddies[buddy_index] != NULL) {
		ret_addr = b_header->buddies[buddy_index];
	}

	// else find first larger free portion and do splitting until desired size
	else {

		// remember where to stop splitting
		int saved_index = buddy_index;

		// iterate trough array of lists to find first next larger portion
		for (; buddy_index < BUDDY_SIZE; ++buddy_index) {
			if (b_header->buddies[buddy_index] != NULL) break;
		}

		// next larger is found
		if (buddy_index != BUDDY_SIZE) {

			// splitting will be done untill current list becomes the one from which we need to take a block
			while (buddy_index > saved_index)
			{
				// take first node from current list
				mem_node_t* temp = b_header->buddies[buddy_index];

				// remove it from current list
				b_header->buddies[buddy_index] = b_header->buddies[buddy_index]->next;

				// split it to left and right buddy
				// left buddy begins on same addres as the whole block 
				mem_node_t* left = temp;

				// start of the right buddy is shifted from start of left buddy by n/2 blocks
				// where n = ( 2 ^ buddy_index )
				mem_node_t* right = (mem_node_t*) ((block_ptr_t)left + (int)pow(2, (buddy_index - 1)));

				// add both buddies to lower list (buddies[index - 1])
				//printf("izvrseno je cepanje. u listu buddies[%d] se dodaju: %d i %d\n", buddy_index - 1, right, left);
				right->next = b_header->buddies[buddy_index - 1];
				left->next = right;
				b_header->buddies[buddy_index - 1] = left;

				// move to lower list and continue 
				buddy_index--;
			}

			// now there is a block to take in requested size list
			ret_addr = b_header->buddies[buddy_index];
		}

	}

	// free address is found
	if (ret_addr != NULL)
	{
		// remove it from its list
		b_header->buddies[buddy_index] = b_header->buddies[buddy_index]->next;
	}

	// free address is not found
	else {
		printf("NOT ENOUGH MEMORY. ALLOCATION FAILED\n");
	}

	//*****************************mutex signal************************************
	if (!ReleaseMutex(b_header->buddy_mutex)) {
		printf("Error in releasing buddy mutex\n");
	}
	//*****************************************************************************

	return ret_addr;
}

void b_free(void* addr, int block_num)
{
	// if attempted to free NULL pointer stop
	if (!addr) {
		return;
	}

	//*****************************mutex wait************************************
	DWORD wait_result = WaitForSingleObject(b_header->buddy_mutex, INFINITE);
	// could not get mutex
	if (wait_result != WAIT_OBJECT_0) {
		return NULL;
	}
	//***************************************************************************

	// round to nearest higher pow 2
	block_num = closest_higher_pow2(block_num);

	// freeing of memmory is done in chunks with power of 2 sizes
	// merging of nodes may occur on every level except the last (largest)
	block_ptr_t current_mem = (block_ptr_t) addr;
	while (block_num != 0) {

		// closest lower logarithm of 2 - index of list to add the node
		int i = closest_lower_log2(block_num);

		// add new free node to the list
		mem_node_t* current_node = (mem_node_t*)current_mem;
		current_node->next = b_header->buddies[i];
		b_header->buddies[i] = current_node;

		// try to do merging 
		b_merge(i);

		// next free portion begins after 2^i blocks
		current_mem += (int)pow(2, i);

		// continue for remainder of freeing blocks
		block_num -= pow(2, i);
	}

	//*****************************mutex signal************************************
	if (!ReleaseMutex(b_header->buddy_mutex)) {
		printf("Error in releasing buddy mutex\n");
	}
	//*****************************************************************************
}

void b_merge(int buddy_index)
{
	// merging can be propagated until last level or until no buddies are found for merging
	while (buddy_index<BUDDY_SIZE-1)
	{
		// when merging begins the new added node will always be the head of the list
		mem_node_t* current_node = b_header->buddies[buddy_index];

		mem_node_t* buddy = NULL;

		// first check if current node is left or right buddy in it's pair
		// every right buddy has 1 and every left buddy has 0 at specific position in binary notation
		// that position is (log2 of the size) + (BLOCK_BIT_NUM) from the left
		// that rule is only applied when addresses start from 0
		// memory start addr must be substracted from node addr to simulate addresses starting from 0

		// prepare mask for checking 
		int check = 0b1 << (buddy_index+BLOCK_BIT_NUM);

		// current is the right buddy
		if ( ((uintptr_t)current_node - (uintptr_t)b_header->mem_start) & check) {

			// his left buddy is ( 2 ^ buddy_index ) blocks behind 
			buddy = (mem_node_t*)((block_ptr_t)current_node - (int)pow(2, buddy_index));
		}

		// current is the left buddy
		else {

			// his right buddy is ( 2 ^ buddy_index ) blocks forward
			buddy = (mem_node_t*)((block_ptr_t)current_node + (int)pow(2, buddy_index));

		}

		// find the missing buddy in the list
		mem_node_t* prev_node = NULL;
		while (current_node && (current_node != buddy)) {
			prev_node = current_node;
			current_node = current_node->next;
		}

		// buddy is not found, stop merging
		if (!current_node) return;

		// buddy is found, do merging
		else {

			mem_node_t* first_node = b_header->buddies[buddy_index];

			// remove the buddy from the list
			prev_node->next = current_node->next;

			// remove new node from the list
			b_header->buddies[buddy_index] = b_header->buddies[buddy_index]->next;

			// merge them in one node and insert it to higher list (buddies[index+1])
			// merged node always begins where left buddy begins

			// first node is left node, add him to higher list
			if (first_node < current_node) {
				first_node->next = b_header->buddies[buddy_index + 1];
				b_header->buddies[buddy_index + 1] = first_node;
			}

			// buddy is left node, add him to higher list
			else {
				current_node->next = b_header->buddies[buddy_index + 1];
				b_header->buddies[buddy_index + 1] = current_node;
			}

			//printf("spojeni su cvorovi sa pocecima %d i %d i spojeni cvor je ubacen u buddies[%d]\n", first_node, current_node, buddy_index + 1);

			buddy_index++;
		}
	}
}

void b_print_state() {
	for (int i = 0; i < BUDDY_SIZE; ++i) {
		printf("Lista buddies[%d]: ", i);
		mem_node_t* current = b_header->buddies[i];
		while (current != NULL) {
			printf("-> %d ", current);
			current = current->next;
		}
		printf("\n");
	}
}
//
//int main() {
//	b_init((int*)(malloc(31*4096)), 25);
//	b_alloc(8183);
//}
//
