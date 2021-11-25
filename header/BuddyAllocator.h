#pragma once
#include<stdio.h>
#include<stdlib.h>
#include<Windows.h>

#define BLOCK_SIZE (4096)           // fixed size of allocation block
#define BLOCK_BIT_NUM (12)          // 2 ^ 12 = BLOCK_SIZE 
#define STEP_SIZE (1024)            // (BLOCK_SIZE/4) used for pointer arithmetic with 4B pointers 
#define STEP_BIT_NUM (10)           // 2 ^ 10 = STEP_SIZE
#define BUDDY_SIZE (32)             // size of buddies[] array. this allows 2^32 * 4096B = ~17TB

#ifndef POINTER_TYPES_DEFINITIONS_
#define POINTER_TYPES_DEFINITIONS_
// 1 byte wide pointer for free moving trough memory
typedef char* ptr_t;

// 4096 bytes wide pointer for moving trough blocks
typedef struct {
	int a[1024];
}*block_ptr_t;

#endif

#ifndef MEM_NODE_TYPE_DEFINITION_
#define MEM_NODE_TYPE_DEFINITION_
typedef struct mem_node {
	struct mem_node* next;
}mem_node_t;
#endif



typedef struct buddy_header {

	block_ptr_t mem_start;                 //start addres of memory for allocation
	ptr_t header_start;                    //start addres of buddy header     
	ptr_t header_end;					   //end address of buddy header
	int block_num;                         //total number of blocks for allocation

	mem_node_t* buddies[BUDDY_SIZE];       //array of heads of free block lists
	HANDLE buddy_mutex;

}buddy_header_t;

extern buddy_header_t* b_header;                    //global buddy allocator header

void b_init(void* memstart, int blocknum);          //initialization of buddy allocator from memstart address with blocknum blocks
void * b_alloc(int block_num);                      //allocation of block_num blocks of memory
void b_free(void* addr, int block_num);             //deallocation of block_num blocks of memory starting from addr
static void b_merge(int buddy_index);               //utility function for deallocation
void b_print_state();                               //prints current state of buddies[] array

