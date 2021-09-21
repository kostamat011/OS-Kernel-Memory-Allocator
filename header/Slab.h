#pragma once
#include <stdlib.h>
#include <Windows.h>
#include <math.h>

#define BLOCK_SIZE (4096)
#define CACHE_L1_LINE_SIZE (64)
#define CACHE_NAME_SIZE (64)
#define SMALL_BUFFER_NUM (18)            // size of small buffers array
#define	SMALL_BUFFER_LOWER_LIMIT (5)     // min size of small buffer is 2^5
#define	SMALL_BUFFER_UPPER_LIMIT (17)    // max size of small buffer is 2^17
#define BITS_PER_BYTE (8)

#define OBJ_NOT_FOUND      (96543)
#define OBJ_FOUND_FULL     (96542)
#define OBJ_FOUND_PARTIAL  (96541)

#define SLOT_FOUND_PARTIAL (87654)
#define SLOT_FOUND_EMPTY   (87653)
#define SLOT_NOT_FOUND     (87652)

#define ALLOCATION_ERROR (1)
#define DEALLOCATION_ERROR (2)
#define INCONSISTENT_SLAB_ERROR (3)

#ifndef POINTER_TYPES_DEFINITIONS_
#define POINTER_TYPES_DEFINITIONS_
// 1 byte wide pointer for free moving trough memory
typedef char* ptr_t;

// 4096 bytes wide pointer for moving trough blocks
typedef struct {
	int a[1024];
}*block_ptr_t;

#endif

typedef char octet;

typedef struct kmem_slab {

	unsigned L1_offset;              // L1 offset for current slab
	void* obj_start_addr;            // starting address of first slot 
	octet* free_slots_map;			 // pointer to bit map of free slots

	struct kmem_slab* next;          // pointer to next slab inside of cache
	
}kmem_slab_t;

typedef struct kmem_cache_s {

	char name[CACHE_NAME_SIZE];

	kmem_slab_t* slabs_empty;        // list of empty slabs
	kmem_slab_t* slabs_partial;      // list of partially full slabs
	kmem_slab_t* slabs_full;         // list of full slabs

	unsigned object_count;           // total number of objects in all slabs
	unsigned slab_count;             // number of slabs
	unsigned objects_per_slab;       // number of slots in each slab
	size_t free_map_size;			 // size of free slot bit map in each slab
	size_t unused_space;             // remainder from last object to end of slab
	size_t obj_size;                 // size of contained objects in bytes
	int recently_added;				 // 1 if added after last shrink attempt

	int next_L1_offset;				 // offset for next slab that will be added		

	void (*ctor)(void*);             // constructor called for contained objects
	void (*dtor)(void*);             // destructor called for contained objects

	struct kmem_cache_s* next;       // pointer to next cache in list of caches

	HANDLE cache_mutex;              // handle to mutex for synchronization on this cache

	int error_code;

}kmem_cache_t;


typedef struct kmem_header {

	kmem_cache_t cache_of_caches;   // cache for all other caches

	kmem_cache_t small_buffer_caches[SMALL_BUFFER_NUM]; // array of small buffer caches (2^5 - 2^17 size)

	ptr_t header_end; // used to keep track of next free address inside 1st block

	kmem_cache_t* cache_head; // head of list of all caches

	HANDLE cache_list_mutex; // mutex for synchronization on list of all caches

}kmem_header_t;

static kmem_header_t* kmem_header;   //global kmem_header

static unsigned calculate_slab_blocks(size_t obj_size);
static void calculate_slab_areas(size_t obj_size, size_t* map_size_p, unsigned* num_of_obj_p, size_t* unused_space_p);
static unsigned total_cache_blocks(kmem_cache_t* cachep);
static void init_cache(kmem_cache_t* new_cache, const char* name, size_t size, void(*ctor)(void*), void(*dtor)(void*));
static void move_partial_full(kmem_cache_t* cache);
static void move_empty_partial(kmem_cache_t* cache);
static void move_full_partial(kmem_cache_t* cache, kmem_slab_t* slab);
static void move_partial_empty(kmem_cache_t* cache, kmem_slab_t* slab);
static int slab_empty(kmem_cache_t* cache, kmem_slab_t* slab);
static int partial_slab_full(kmem_cache_t* cache);
static int get_free_slot(kmem_cache_t* parent_cache, void** address);
static int find_containing_slab(kmem_cache_t* cachep, void* objp, kmem_slab_t** res);
static int extend_cache(kmem_cache_t* cache);
 kmem_cache_t* find_cache(const char* name);
 void print_list_of_caches();


void kmem_init(void* space, int block_num); //Initialization
kmem_cache_t* kmem_cache_create(const char* name, size_t size, void (*ctor)(void*), void (*dtor)(void*)); // Allocate cache
int kmem_cache_shrink(kmem_cache_t* cachep); // Shrink cache
void* kmem_cache_alloc(kmem_cache_t* cachep); // Allocate one object from cache
void kmem_cache_free(kmem_cache_t* cachep, void* objp); // Deallocate one object from cache
void* kmalloc(size_t size); // Alloacate one small memory buffer
void kfree(const void* objp); // Deallocate one small memory buffer
void kmem_cache_destroy(kmem_cache_t* cachep); // Deallocate cache
void kmem_cache_info(kmem_cache_t* cachep); // Print cache info
int kmem_cache_error(kmem_cache_t* cachep); // Print error message