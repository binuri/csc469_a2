#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>
#include "mm_thread.h"
#include "memlib.h"

#define PAGE_ALIGN(p)    ((void *)(((unsigned long)(p) / mem_pagesize()) * mem_pagesize()))

#define MAGICNUM 0xDEADDEAD

/* *** DATA STRUCTURES USED *** */

typedef struct slot {
    struct slot *next;
} slot_t;

typedef struct sb {
    unsigned magic;
    int heap_id;

    int tid;
    size_t size_class;
    int free_slots;
    int total_slots;

    slot_t *slots;
    struct sb *next, *prev;
    pthread_mutex_t lock; // Used when freeing blocks
} superblock_t;


typedef struct {
    unsigned magic;
    int total_size;
    int allocated_size;
    pthread_mutex_t lock;
    superblock_t *sized_blocks[10];
} heap_t;

/* *** VARIABLES NEEDED *** */
heap_t *heaps = NULL;
heap_t *global_heap = NULL;
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
    sb->prev = NULL;
}

/*
 * Given the pointer to the start of the first slot, initialize a number
 * of total_slots with size size_class
 */
void initialize_slots(superblock_t *block, size_t size_class_index, int total_slots){

    size_t size_class = get_power_of(2, (3 + size_class_index));
    size_t space = alignSize(sizeof(slot_t)) + size_class;


    slot_t *slot = (slot_t *)((char *)block + alignSize(sizeof(superblock_t)));
    block->slots = slot;

    
    int curr_slot = 0;
    while(curr_slot <  (total_slots - 1)){
        slot->next = (slot_t *)((char *)slot + space);
        slot = slot->next;
        curr_slot ++;
    }
    slot->next = NULL;


}

/*
 * Given a tid, heap_id and a size class, create an empty superblock with
 * maximum possible slots for the given size_class and with zero slots
 * allocated.//
 */
superblock_t *create_new_superblock(int tid, int heap_id, int size_class_index){
    superblock_t *new_block = (superblock_t *) mem_sbrk(mem_pagesize());

    set_superblock_properties(new_block, tid, heap_id, size_class_index);

    int pagesize = mem_pagesize();

    int size_of_struct = alignSize(sizeof(superblock_t));

    // For each slot will have a size of size_class + size of slot_t struct
    int total_slots = (pagesize - size_of_struct) /
                        (new_block->size_class + alignSize(sizeof(slot_t)));

    // setup data about the data slots that will be given out
    new_block->free_slots = total_slots;
    new_block->total_slots = total_slots;
    initialize_slots(new_block, size_class_index, total_slots);

     pthread_mutexattr_t sb_lock;
     pthread_mutexattr_init(&sb_lock);
     pthread_mutex_init(&(new_block->lock), &sb_lock);

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
        if (free_block->next != NULL)
            free_block->next->prev = NULL;
        set_superblock_properties(free_block, tid, heap_id, size_class_index);
    }

    
    pthread_mutex_unlock(&(global_heap->lock));

    return free_block;
}

void transfer_superblock_size_stats(int from_heapid, int to_heapid, superblock_t *block_transferred){
    if (block_transferred != NULL){
        size_t total_allocated_size_of_superblock = block_transferred->size_class * 
                                (block_transferred->total_slots - block_transferred->free_slots);


        heap_t *from_heap = heaps + from_heapid;
        from_heap->allocated_size -= total_allocated_size_of_superblock;
        from_heap->total_size -= block_transferred->total_slots * block_transferred->size_class;

        heap_t *to_heap = heaps + to_heapid;
        to_heap->allocated_size += total_allocated_size_of_superblock;
        to_heap->total_size += block_transferred->total_slots * block_transferred->size_class;
    }

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
            printf("Iterating through the free blocks\n");
            // If heap does not have a sufficient superblock, search global heap
            if (free_block->next == NULL) {
                if (verbose)
                    printf("No superblock in the heap has a free slot\n");

                free_block = find_superblock_from_global_heap(tid, heap_id, size_class_index);
                if (free_block == NULL){
                    if (verbose)
                        printf("Could not find block in global heap either so creating new supoerblock.\n");

                    // Create  a new superblock and update the heap's total used space count
                    // Heap's allocated space count is not updated since free block newly created
                    free_block = create_new_superblock(tid, heap_id, size_class_index);
                    heap->total_size += free_block->size_class * free_block->total_slots;
                } else {
                    // Subtract superblock stats from global heap and add to current heap
                    transfer_superblock_size_stats(0, heap_id, free_block);
                }

                // Add the free block to the current heap
                free_block->next = heap->sized_blocks[size_class_index];
                if (free_block->next != NULL){
                    free_block->next->prev = free_block;
                }
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
            // Create  a new superblock and update the heap's total used space count
            // Heap's allocated space count is not updated since free block newly created
            free_block = create_new_superblock(tid, heap_id, size_class_index);
            heap->total_size += free_block->size_class * free_block->total_slots;            
                
        } else {
            // Subtract superblock stats from global heap and add to current heap
            transfer_superblock_size_stats(0, heap_id, free_block);
        }

        heap->sized_blocks[size_class_index] = free_block;

    }

    return free_block;

}

void *mm_malloc(size_t sz)
{

    // For large page sizes, let the OS handle memory allocation for now
    if (sz > mem_pagesize()/2){
        char *large_block = (char *)malloc(sz);
        return large_block;
    }

    // Use the current thread to get the heap to be used
    int tid = getTID();
    int heap_id = (tid % getNumProcessors()) + 1;

    // +1 Since heap[0] is global heap
    heap_t *heap = heaps + heap_id;
    pthread_mutex_lock(&(heap->lock));

    int size_class_index = find_size_class_index(sz);
    if (verbose)
        printf("Size class index for requested block : %d\n", size_class_index);

    superblock_t *free_block = find_free_block(heap, tid, heap_id, size_class_index);

    // Now we have a free block whose first slot has memory
    slot_t *free_slot = free_block->slots;

    // Update the supreblock to indicate that a single data slot will be allocated
    free_block->slots = free_slot->next;

    free_block->free_slots = free_block->free_slots - 1;

    // Update the heap to indicate that a block of data is allocated
    heap->allocated_size += free_block->size_class;

    pthread_mutex_unlock(&(heap->lock));


    // This is wrong, but for the time being.
    return (void*)((char *)free_slot + sizeof(slot_t));
}


/* 
 * Transfer a block that is msot empty from the heap with heap_id to the global
 * heap
 */
void transfer_mostly_empty_superblock_to_global_heap(int heap_id){


}

void mm_free(void *ptr)
{
    
    // Find the superblock that the freed block belongs to
    superblock_t *sb =  (superblock_t *)(PAGE_ALIGN(ptr));
    
    // Get the lock superblock and then the heap to which it belongs to
    pthread_mutex_lock(&(sb->lock));

    heap_t *heap = heaps + sb->heap_id;
    pthread_mutex_lock(&(heap->lock));
    
    // Deallocate the block from the superblock
    slot_t *deallocated_block = (slot_t *)((char *)ptr - sizeof(slot_t));
    deallocated_block->next = sb->slots;
    sb->slots = deallocated_block;

    // update the superblock slot count
    sb->free_slots += 1;
    
    // Update the heap data
    heap->allocated_size -= sb->size_class;

    // If the superblock is not in the global heap, then we have to see if superblocks
    // from the heap can be transferred to the global heap    
    if (sb->heap_id != 0 && sb->tid == getTID()){
        if (sb->free_slots == sb->total_slots){
            
            size_t size_class_i = find_size_class_index(sb->size_class);
            
            if (sb->prev == NULL){
                heap->sized_blocks[size_class_i] = sb->next;
            } else {
                sb->prev->next = sb->next;
            }
    
            if (sb->next != NULL){
                sb->next->prev = sb->prev;
            }

            sb->prev = NULL;
            sb->next = global_heap->sized_blocks[size_class_i];
            if (sb->next != NULL){
                sb->next->prev = sb;
            }
            global_heap->sized_blocks[size_class_i] = sb;
            
            transfer_superblock_size_stats(sb->heap_id, 0, sb);
            

        }    
    
        // If 7/8ths of the heaps blocks are not in use and has more than 9
        // superblocks worth of free memeory on the heap
        //if (heap->allocated_size < heap->total_size - (9 * mem_pagesize()) && 
        //    heap->allocated_size < (0.125 * heap->total_size)){
        //    transfer_mostly_empty_superblock_to_global_heap(sb->heap_id);
        //}
    }

    
    pthread_mutex_unlock(&(heap->lock));
    pthread_mutex_unlock(&(sb->lock));

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
        for(i = 0; i <= getNumProcessors()+1; i++){
            heap_t *heap = &heaps[i];

            pthread_mutexattr_t mta;
            pthread_mutexattr_init(&mta);

            pthread_mutex_init(&(heap->lock), &mta);

            heap->total_size = 0;
            heap->allocated_size = 0;

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
    printf("The location of the malloced ptr : %ld\n", (uintptr_t)ptr);
    // Brand new superblock must be created for sizeclass of 2 for
    // whichever heap.
    int heapid = (getTID() % getNumProcessors()) + 1;
    heap_t *heap = heaps + heapid;
    printf ("TEST: Allocated 1 superblock to heap %d by thread %d\n", heapid, getTID());
    if (heap->sized_blocks[1]){
        superblock_t *sb = heap->sized_blocks[1];
        printf("The total slots available in the superblock : %d\n", sb->total_slots);
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }
    mm_free(ptr);
    printf ("TEST: Freed one slot in superblock to heap %d by thread %d\n", heapid, getTID());
    if (heap->sized_blocks[1]){
        superblock_t *sb = heap->sized_blocks[1];
        printf("The total slots available in the superblock : %d\n", sb->total_slots);
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }
   printf ("\nTEST: Allocating a slot from the same supoerblock to heap %d by thread %d \n", heapid, getTID());
    ptr = mm_malloc(16);
    if (heap->sized_blocks[1]){
        superblock_t *sb = heap->sized_blocks[1];
        printf("Total slots available in teh superblock: %d\n", sb->total_slots);
        printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
        printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
        printf ("ERROR: A superblock did not get allocated");
    }
   
   
    printf("Number of pages heap after heapspace + 1 superblocks : %lu \n", ((uintptr_t)(void *)dseg_hi + 1 - (uintptr_t)(void *)dseg_lo)/mem_pagesize());
    int test_num = 0;
    while (test_num < 4) {
        printf ("\nTEST: Allocating a slot from the same supoerblock to heap %d by thread %d \n", heapid, getTID());
        ptr = mm_malloc(1000);
        if (heap->sized_blocks[7]){
            superblock_t *sb = heap->sized_blocks[7];
            printf("HEAP total space available: %d\n", heap->total_size);
            printf("HEAP allocated_space: %d\n", heap->allocated_size);
            printf("Total number of slots available in the superblock : %d\n", sb->total_slots);
            printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
            printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
        } else {
            printf ("ERROR: A superblock did not get allocated");
        }
        test_num ++;
    }
    mm_free(ptr);
    if (heap->sized_blocks[7]){
            superblock_t *sb = heap->sized_blocks[7];
            printf("\n\nAFTER_FREEEE-> HEAP total space available: %d\n", heap->total_size);
            printf("HEAP allocated_space: %d\n", heap->allocated_size);
            printf("Total number of slots available in the superblock : %d\n", sb->total_slots);
            printf ("Number of slots available after first allocation : %d\n", sb->free_slots);
            printf("Heap id %d and tid %d with size class %zu\n", sb->heap_id, sb->tid, sb->size_class);
    } else {
            printf ("ERROR: A superblock did not get allocated");
    }
    printf("Number of pages heap after heapspace + 4 superblocks : %lu \n", ((uintptr_t)(void *)dseg_hi + 1 - (uintptr_t)(void *)dseg_lo)/mem_pagesize());
    return 0;
}
