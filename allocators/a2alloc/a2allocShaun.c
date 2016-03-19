#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include "memlib.h"
#include "mm_thread.h"
#include "malloc.h"

/* *** CONSTANTS *** */

name_t myname = {
     /* team name to be displayed on webpage */
     "Group 19",
     /* Full name of first team member */
     "Shaun Memon",
     /* Email address of first team member */
     "s.memon@utoronto.ca",
     /* Full name of second team member */
     "Binuri Mandula Walpitagamage",
     /* Email address of second team member */
     "binuri.walpitagamage@mail.utoronto.ca"
};

#define MAGICNUM 0xDEADDEAD //to verify data integrity
#define PAGESIZE 4096       //default pagesize
#define MINSLOTSIZECLASS 3  //min sizeclass 2^3 = 8bytes;
#define MINSLOTSIZE 8       //size of smallest superblock slot (2^MINSLOTSIZECLASS)
#define NUMSLOTCLASS 8      //number of sizeclasses, max = 2^(3+8)= 2048bytes 
#define DEBUG 0             //set debug output ON/OFF

//following two macros from provided memlib.c
/* Align pointer to closest page boundary downwards */
#define PAGE_ALIGN(p)    ((void *)(((unsigned long)(p) / page_size) * page_size))
/* Align pointer to closest page boundary upwards */
#define PAGE_ALIGN_UP(p) ((void *)((((unsigned long)(p) + page_size - 1) / page_size) * page_size))


/* *** DATA STRUCTURES USED *** */
typedef unsigned long vaddr_t;


//heap superblock data struct
typedef struct __superblock_t {
    unsigned int magic; //magic number to verify integrity
    int heap_id;        //corresponds to heap (cpu) that superblock belongs to
                        //global heap is 0, 1...P for processor heaps
    int tid;            //thread id of owning thread
    int size_class;     //size of slots in superblock
    int free_slots;     //number of remaining unused slots 
    int total_slots;    //total slots available in superblock (pagesize/2^size_class)

    struct __slot_t *slots;     //pointer to head of linked list to free slots (LIFO ordering)
    struct __superblock_t *next, *prev;     //pointer to next and prev superblocks (doubly linked list)
    pthread_mutex_t lock;                   //superblock lock used in freeing block
} superblock_t;

//per processor memory heaps stuct
typedef struct __heap_t {
    unsigned int magic;     //magic number to verify integrity
    int total_size;        //total bytes allocated to heap
    int allocated_size;         //total bytes in use in heap
    pthread_mutex_t lock;   //lock for heap
    superblock_t *superblock[NUMSLOTCLASS]; //pointers to superblocks for each sizeclass
} heap_t;


//linked list of unused (free) slots within superblock
typedef struct __slot_t {
    struct __slot_t *next;
} slot_t;


/* *** GLOBAL VARIABLES *** */
heap_t *global_heaps=NULL;
int numProcessors;
int page_size;


/* *** HELPER FUNCTIONS *** */

/* alignSize:   aligns requested memory size amounts such that it will 
                end on an 8-byte boundary
 * unsigned in size: amount of memory requested
 * retval: new size of memory that will be aligned to next 8-byte boundary. 
 */
inline unsigned alignSize(unsigned size) 
{
    if (size % 8 != 0) {
        size = size + (8 - size % 8);
    }
    return size;
}


/* tidToProcNum:    take a thread id and map to processor(heap) number using mod #proc
 * TID: thread id
 * retval: thread id mapped to processor/heap number
 */
inline int tidToProcNum(int TID) 
{
    return TID % numProcessors;
}


/* getSizeClass: calculates the index into a heap superblock array
 * sz: size of memory requested
 * retval: index into heap_t superblock array corresponding to sizeclass 
 */
inline int getSizeClass(size_t sz) 
{
    int slotsize = MINSLOTSIZE;
    int slotclass = 0;
    while(sz > slotsize) 
    {
        slotsize = slotsize * 2;
        slotclass++;
    }

    if (DEBUG)
    {
        printf("getSizeClass(%u):%d \n", sz, slotclass);
    }

    return slotclass;
}

/* getSuperblockFromGlobalHeap: returns superblock from global heap to be used by processor heap 
 * retval: pointer to available superblock_t
 */
inline superblock_t *getSuperblockFromGlobalHeap(int sizeclass) 
{
    heap_t *heap = global_heaps;

    //aquire global heap lock
    pthread_mutex_lock(&(heap->lock));

    //global heap superblocks can be reused for any size class so they are
    //only stored in index 0
    superblock_t *sb = heap->superblock[sizeclass];

    //if there is a superblock, use it and advance global heap to next superblock 
    if (sb != NULL) {
        heap->superblock[sizeclass] = sb->next;
    }

    if (DEBUG)
    {
        printf("getSuperblockFromGlobalHeap: returning superblock %p from global \
            heap\n", sb);
    }

    //unlock global heap
    pthread_mutex_unlock(&(heap->lock));

    return sb;
}


/* 
 * 
 */
inline void initialize_slots(superblock_t *sb) 
{
    sb->slots = (slot_t *) ((char *) sb + alignSize(sizeof(superblock_t)));


    slot_t *slot = sb->slots;
    int slotsize = sb->size_class;

    if (DEBUG)
    {
        printf("initialize_slots: sb:%p; sb->size_class:%d; firstslot:%p; \n", 
            sb, slotsize, slot);
    }

    int i;
    for ( i = 0; i < sb->total_slots; i++)
    {
        slot->next = (slot_t *) ((char *) slot + sb->size_class);
        slot = slot->next;

        if (DEBUG)
        {
            printf("slot %d: %p;\t", i+1, slot);
        }
    }
    if (DEBUG) printf("\n");
    
    slot->next = NULL;
}

/* *** MAIN HOARD FUNCTIONS *** */

/* mm_init: initialize memory allocator functions and datastuctures
 * retval: 0 on success, -1 otherwise
 */
int mm_init(void) 
{
    if (dseg_lo != NULL && dseg_hi != NULL) {
        //memory functions already initialized

        if (DEBUG) {
            printf("mm_init: Memory functions have already been initialized\n");
        }
        return -1;
    }
    
    //Initialize memory allocater and datastructures
    mem_init();

    //get number of processors which will be the number of heaps + global
    numProcessors = getNumProcessors();
    page_size = (int) getpagesize();
    //beginning of allocated memory is the start of heap data structures 
    global_heaps = mem_sbrk(page_size);
    assert(global_heaps == dseg_lo);

    if (DEBUG) {
        printf("mm_init: initialized memory @%p; numProcessors=%d; pageSize=%d \
            sizeof(heap_t):%u; sizeof(superblock_t):%u \n", 
            (void *)global_heaps, numProcessors, page_size, sizeof(heap_t), sizeof(superblock_t));
    }
    

    //initialize heap datastuctures and locks
    int i;
    for(i = 0; i <= numProcessors + 1; i++){
        heap_t *heap = global_heaps + i;

        heap->magic = MAGICNUM;
        heap->allocated_size = 0;
        heap->total_size = 0;

        //init heap locks
        pthread_mutexattr_t mta;
        pthread_mutexattr_init(&mta);

        pthread_mutex_init(&(heap->lock), &mta);

        //init superblock pointers to NULL
        int block_id;
        for (block_id = 0; block_id < NUMSLOTCLASS; block_id++){

            heap->superblock[block_id] = NULL;
        }

        if (DEBUG)
        {
            printf("mm_init: initialize heap:%d @ addr: %p\n", i, heap);
        }
    }
 
    return 0;
}


/* mm_malloc: out implentation of Hoard memory allocator
 * sz: amount of heap memory requested
 * retval: pointer to heap memory region requested
 */
void *mm_malloc(size_t sz)
{

    sz = alignSize(sz);

    //if the size is bigger than page_size/2 then let the system memory
    //allocator take care of it
    if (sz > page_size/2)
    {
        if (DEBUG)
        {
            printf("mm_malloc: requested size %u > page_size/2, using system malloc\n", sz);
        }
        return malloc(sz);
    }

    //get thread, processor(heap) hash and superblock size class
    int tid = getTID();
    int heap_id = tidToProcNum(tid) + 1;
    int sizeclass = getSizeClass(sz);

    //pointer to processor heap, +1 because heap 0 is global heap, and lock it
    heap_t *heap = global_heaps + heap_id;
    assert(heap->magic == MAGICNUM);

    if(DEBUG)
    {
        printf("mm_malloc(%u): tid:%d, heap_id:%d, sizeclass: %d\n", 
            sz, tid, heap_id, sizeclass);
    }

    pthread_mutex_lock(&(heap->lock));

    superblock_t *block = heap->superblock[sizeclass];

    if (block != NULL)
    {

        if(DEBUG)
        {
            printf("mm_malloc: superblocks found in heap[%d] of sizeclass %d\n",heap_id,sizeclass);
        }
        
        int j = 0;
        while (block != NULL && (block->tid != tid || block->free_slots == 0))
        {
            assert(block->magic == MAGICNUM);
            if(DEBUG)
            {
                printf("mm_malloc: block addr:%p, block->tid:%d, block->free_slots:%d, block->next: %p\n",block, block->tid, block->free_slots, block->next);
                printf("j:%d",j);
            }
            block = block->next;
            j++;

            if (j>10) exit(1);
        }
    }

    //if there is no superblock curresponding to the sizeclass given then check
    //the global heap for any superblocks that can be used and transfer it to the 
    //current heap for use. 
    if (block == NULL) 
    {

        if (DEBUG)
        {
            printf("mm_malloc: no blocks for tid with free slots found, checking global heap\n");
        }

        block = getSuperblockFromGlobalHeap(sizeclass);

        //If there are no global heap superblocks avaiable then allocate memory
        // for a new superblock
        if (block == NULL)
        {
            block = (superblock_t *) mem_sbrk(page_size);

            if (DEBUG)
            {
                printf("mm_malloc: no superblocks on global heap, creating new superblock \
                    @addr %p\n", block);
            }
            //initialize new superblock for the current thread and use it  
            block->magic = MAGICNUM;
            block->size_class = MINSLOTSIZE << (sizeclass);
            block->total_slots = (page_size - sizeof(superblock_t)) / block->size_class;
            block->free_slots = block->total_slots;
            printf("new clock of sizeclass %d and total_slots %d\n", sizeclass, block->total_slots);
            //initialize free slots linked list
            initialize_slots(block);
        }

        block->heap_id = heap_id;
        block->tid = tid;

        heap->total_size += page_size;
    } else 
    {
        if(DEBUG)
        {
            printf("mm_malloc: superblock @ %p with heap_id: %d, tid:%d, size_class:%d \
                freeslot:%d, firstslot@%p\n", block, block->heap_id, block->tid, 
                block->size_class, block->free_slots, block->slots);
        }

        //remove existing block from middle of list
        if (block->prev != NULL) 
        {
            block->prev->next = block->next;
            if (block->next != NULL)
            {
                block->next->prev = block->prev;
            }
        }
    }

    //place block at head of superblock linked list for heap maintaining LIFO ordering
    if (block != heap->superblock[sizeclass])
    {
        block->prev = NULL;
        block->next = heap->superblock[sizeclass];
        if (heap->superblock[sizeclass]) heap->superblock[sizeclass]->prev = block;
        heap->superblock[sizeclass] = block;
    }

    //get freeslot address to passback and update block freelist and heap information
    vaddr_t *ptr= (void *) block->slots;
    block->slots = block->slots->next;

    block->free_slots -= 1;
    heap->allocated_size += block->size_class;

    pthread_mutex_unlock(&(heap->lock));

    return ptr;
}

void mm_free(void *ptr)
{
    //if ptr is out of our range meaning it is either in error or was allocated by
    //system malloc due to size constraint
    if ((char *) ptr < dseg_lo || (char *) ptr > dseg_hi)
    {
        free(ptr);
        return;
    }

    //get superblock from beginning of page
    superblock_t *sb = PAGE_ALIGN(ptr);

    assert(sb->magic == MAGICNUM);

    //extract heap_id and get pointer to head and lock it
    int heap_id = sb->heap_id;
    heap_t *heap = global_heaps + heap_id;
    assert(heap->magic == MAGICNUM);

    pthread_mutex_lock(&(heap->lock));

    //put freelist node at head of superblock freelist linked list to maintain LIFO ordering
    ((slot_t *) ptr)->next = sb->slots;
    sb->slots = (slot_t *) ptr;

    sb->free_slots += 1;
    heap->allocated_size -= sb->size_class;

    //unlock heap and return
    pthread_mutex_unlock(&(heap->lock));
    return;
}



int main( int argc, const char* argv[] )
{
    int i;
    for( i = 0; i < 10; i++ )
    {
        printf( "Iteration %d\n", i );
    }
    mm_init();
    void *ptr = mm_malloc(16);
    printf("The location of the malloced ptr : %ld\n", (uintptr_t)ptr);
    // Brand new superblock must be created for sizeclass of 2 for
    // whichever heap.
    int heapid = (getTID() % getNumProcessors()) + 1;
    heap_t *heap = global_heaps + heapid;
    printf ("TEST: Allocated 1 superblock to heap %d by thread %d\n", heapid, getTID());
    if (heap->superblock[1]){
        superblock_t *sb = heap->superblock[1];
        printf("The total slots available in the superblock : %d\n", sb->total_slots);
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %u\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }
    mm_free(ptr);
    printf ("TEST: Freed one slot in superblock to heap %d by thread %d\n", heapid, getTID());
    if (heap->superblock[1]){
        superblock_t *sb = heap->superblock[1];
        printf("The total slots available in the superblock : %d\n", sb->total_slots);
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %u\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }
   printf ("\nTEST: Allocating a slot from the same supoerblock to heap %d by thread %d \n", heapid, getTID());
    ptr = mm_malloc(16);
    if (heap->superblock[1]){
        superblock_t *sb = heap->superblock[1];
        printf("Total slots available in teh superblock: %d\n", sb->total_slots);
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %u\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }
   
   
    printf("Number of pages heap after heapspace + 1 superblocks : %u \n", ((uintptr_t)(void *)dseg_hi + 1 - (uintptr_t)(void *)dseg_lo)/mem_pagesize());
    int test_num = 0;
    while (test_num < 4) {
        printf ("\nTEST: Allocating a slot from the same supoerblock to heap %d by thread %d \n", heapid, getTID());
        ptr = mm_malloc(1000);
        if (heap->superblock[7]){
            superblock_t *sb = heap->superblock[7];
            printf("HEAP total space available: %d\n", heap->total_size);
            printf("HEAP allocated_space: %d\n", heap->allocated_size);
            printf("Total number of slots available in the superblock : %d\n", sb->total_slots);
            printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
            printf("Heap id %d and tid %d with size class %u\n", sb->heap_id, sb->tid, sb->size_class);
        } else {
            printf ("ERROR: A superblock did not get allocated");
        }
        test_num ++;
    }
    mm_free(ptr);
    if (heap->superblock[7]){
            superblock_t *sb = heap->superblock[7];
            printf("\n\nAFTER_FREEEE-> HEAP total space available: %d\n", heap->total_size);
            printf("HEAP allocated_space: %d\n", heap->allocated_size);
            printf("Total number of slots available in the superblock : %d\n", sb->total_slots);
            printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
            printf("Heap id %d and tid %d with size class %u\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
            printf ("ERROR: A superblock did not get allocated");
    }
    printf("Number of pages heap after heapspace + 4 superblocks : %u \n", ((uintptr_t)(void *)dseg_hi + 1 - (uintptr_t)(void *)dseg_lo)/mem_pagesize());
    return 0;
}
