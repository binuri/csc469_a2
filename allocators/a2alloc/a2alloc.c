#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>
#include "mm_thread.h"
#include "memlib.h"



/* *** DATA STRUCTURES USED *** */

typedef struct slot {
    struct slot *next;
} slot_t;

typedef struct sb {
    int heap_id;
    int tid;
    size_t size_class;

    int free_slots;
    slot_t *slots;

    struct sb *next;

} superblock_t;


typedef struct {
    int total_bytes;
    int allocated_bytes;
    pthread_mutex_t lock;
    superblock_t *sized_blocks[10];
} heap_t;

/* *** VARIABLES NEEDED *** */
static heap_t *heaps = NULL;
static heap_t *global_heap = NULL;
int verbose = 1;


/* allignSize: Returns a positive integer that is the smallest
 * multiple of 8 that is larger or equal to size.
 */
size_t alignSize(int size){
    /*orrr (size%8) + (8 - size%8) + ((size - (size%8))*/
    return (size + 7) & ~7;
}

/*
 * Returns the index of the size classes that will fit the
 * given size
 */
int find_size_class_index(size_t size){

    int i = 0;

    // bounds worst-case internal fragmentation within
    // a block to a factor of b
    int b = 2;

    // 8 bytes is smallest block that is mallocd
    size_t block_size = 8;

    while (size > block_size){
        i ++;
        block_size = block_size * b;
    }

    return i;
}

int get_power_of(int base, int exp){
    int res = 1;
    while (exp){
        res = res * base;
        exp --;
    }

    return res;
}

/*
 * Given a superblock struct, set the given properties
 *
 */
void set_superblock_properties(superblock_t *sb, int tid, int heap_id, int size_class_index){
    sb->tid = tid;
    sb->heap_id = heap_id;

    sb->size_class = get_power_of(2, (3 + size_class_index));

    sb->next = NULL;
}

/*
 * Given the pointer to the start of the first slot, initialize a number
 * of total_slots with size size_class
 */
void initialize_slots(superblock_t *block, size_t size_class_index, int total_slots){

    int slot;
    size_t size_class = get_power_of(2, (3 + size_class_index));
    size_t space = alignSize(sizeof(slot_t)) + size_class;

    block->slots = (slot_t *)((char*)block + space);
    slot_t *curr_slot = block->slots;
    for (slot = 0; slot < total_slots - 1; slot++){
        curr_slot->next = (slot_t *)((char *)curr_slot + space);
        curr_slot = curr_slot->next;
    }

    curr_slot->next = NULL;
}

/*
 * Given a tid, heap_id and a size class, create an empty superblock with
 * maximum possible slots for the given size_class and with zero slots
 * allocated.
 */
superblock_t *create_new_superblock(int tid, int heap_id, int size_class_index){
    superblock_t *new_block = (superblock_t *) mem_sbrk(mem_pagesize());

    set_superblock_properties(new_block, tid, heap_id, size_class_index);

    int pagesize = mem_pagesize();

    int size_of_struct = alignSize(sizeof(superblock_t));

    // For each slot will have a size of size_class + size of slot_t struct
    int total_slots = (pagesize - size_of_struct) /
                        (new_block->size_class + alignSize(sizeof(slot_t)));

    new_block->free_slots = total_slots;
    initialize_slots(new_block, size_class_index, total_slots);

    return new_block;
}

/*
 * Finds a supoerblock from the global heap for the given size and initializes
 * its properties
 */
superblock_t *find_superblock_from_global_heap(int tid, int heap_id, int size_class_index){

    pthread_mutex_lock(&(global_heap->lock));
    superblock_t *free_block = global_heap->sized_blocks[size_class_index];

    if (free_block != NULL){
        global_heap->sized_blocks[size_class_index] = free_block->next;
        set_superblock_properties(free_block, tid, heap_id, size_class_index);
    }
    pthread_mutex_unlock(&(global_heap->lock));

    return free_block;
}


/*
 * Find a free block of the given size_class
 */
superblock_t *find_free_block(heap_t *heap, int tid, int heap_id, int size_class_index){

    superblock_t *free_block = heap->sized_blocks[size_class_index];

    if (free_block != NULL) {
        if (verbose){
            printf("Supoerblocks for the size class %zu exits in the heap for TID %d with %d free_slots\n",
                free_block->size_class, free_block->tid, free_block->free_slots);
        }

        // Scan heap's list of superblocks
        while (free_block->tid != tid || free_block->free_slots == 0){

            // If heap does not have a sufficient superblock, search global heap
            if (free_block->next == NULL) {
                if (verbose)
                    printf("No superblock in the heap has a free slot\n");

                free_block = find_superblock_from_global_heap(tid, heap_id, size_class_index);
                if (free_block == NULL){
                    if (verbose)
                        printf("Could not find block in global heap either so creating new supoerblock.\n");
                    free_block = create_new_superblock(tid, heap_id, size_class_index);
                }

                free_block->next = heap->sized_blocks[size_class_index];
                heap->sized_blocks[size_class_index] = free_block;

            } else {
                free_block = free_block->next;

            }
        }
        printf("\n");
    } else {
        free_block = find_superblock_from_global_heap(tid, heap_id, size_class_index);

        // If a free block is still not found, then allocate a new block
        if (free_block == NULL){
            free_block = create_new_superblock(tid, heap_id, size_class_index);
        }
        heap->sized_blocks[size_class_index] = free_block;

    }

    return free_block;

}

void *mm_malloc(size_t sz)
{

    // For large page sizes, let the OS handle memory allocation for now
    if (sz > mem_pagesize()){
        char *large_block = (char *)malloc(sz);
        return large_block;
    }

    // Use the current thread to get the heap to be used
    int tid = getTID();
    int heap_id = tid % getNumProcessors();

    // +1 Since heap[0] is global heap
    heap_t *heap = heaps + heap_id + 1;
    pthread_mutex_lock(&(heap->lock));

    int size_class_index = find_size_class_index(sz);
    if (verbose)
        printf("Size class index for requested block : %d\n", size_class_index);

    superblock_t *free_block = find_free_block(heap, tid, heap_id, size_class_index);

    // Now we have a free block whose first slot has memory
    slot_t *free_slot = free_block->slots;

    free_block->slots = free_slot->next;
    free_block->free_slots = free_block->free_slots - 1;

    pthread_mutex_unlock(&(heap->lock));


    // This is wrong, but for the time being.
    return (void*)((char *)free_slot + sizeof(slot_t));
}

void mm_free(void *ptr)
{
    (void)ptr; /* Avoid warning about unused variable */
}


int mm_init(void)
{

    /* initalize memory if dseg_lo/hi is not set */
    if (dseg_lo == NULL && dseg_hi == NULL) {
        int success = mem_init();

        if (success == -1){
            return success;
        }


        heaps = (heap_t *)mem_sbrk(mem_pagesize());
        if (!heaps){
            return -1;
        }

        if (verbose) {
            printf("PAGESIZE : %d\n",  mem_pagesize());
            printf("mm_init : Current heap size : %lu \n", (uintptr_t)(void *)dseg_hi + 1 - (uintptr_t)(void *)dseg_lo);
        }

        int i;
        for(i = 0; i <= getNumProcessors(); i++){
            heap_t *heap = &heaps[i];

            pthread_mutexattr_t mta;
            pthread_mutexattr_init(&mta);

            pthread_mutex_init(&(heap->lock), &mta);

            int block_id;
            for (block_id = 0; block_id < 10; block_id++){

                heap->sized_blocks[block_id] = NULL;
            }
        }
    }

    /* global heap is at heaps[0] */
    global_heap = heaps;

    return 0;
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

    // Brand new superblock must be created for sizeclass of 2 for
    // whichever heap.
    int heapid = getTID() % getNumProcessors();
    heap_t *heap = heaps + heapid + 1;
    printf ("TEST: Allocated 1 superblock to heap %d by thread %d\n", heapid, getTID());
    if (heap->sized_blocks[1]){
        superblock_t *sb = heap->sized_blocks[1];
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }

   printf ("\nTEST: Allocating a slot from the same supoerblock to heap %d by thread %d \n", heapid, getTID());
    ptr = mm_malloc(16);
    if (heap->sized_blocks[1]){
        superblock_t *sb = heap->sized_blocks[1];
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }
   
   
    printf("Number of pages heap after heapspace + 1 superblocks : %lu \n", ((uintptr_t)(void *)dseg_hi + 1 - (uintptr_t)(void *)dseg_lo)/mem_pagesize());


    int test_num = 0;
    while (test_num < 7) {
        printf ("\nTEST: Allocating a slot from the same supoerblock to heap %d by thread %d \n", heapid, getTID());
        ptr = mm_malloc(1000);
        if (heap->sized_blocks[7]){
            superblock_t *sb = heap->sized_blocks[7];
            printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
            printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
        } else {
            printf ("ERROR: A superblock did not get allocated");
        }
        test_num ++;
    }

    printf("Number of pages heap after heapspace + 4 superblocks : %lu \n", ((uintptr_t)(void *)dseg_hi + 1 - (uintptr_t)(void *)dseg_lo)/mem_pagesize());


    return 0;
}
