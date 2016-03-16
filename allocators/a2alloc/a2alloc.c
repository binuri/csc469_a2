#include <stdlib.h>


#include "mm_thread.h"
#include "memlib.h"



/* *** DATA STRUCTURES USED *** */
typedef struct sb {
    int heap_id;
    int tid;
    size_t size_class;

    int total_slots;
    int free_slots;

    struct sb *next;
    void *data;

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

/*
 * Given a superblock struct, set the given properties
 *
 */
void set_superblock_properties(superblock_t *sb, int tid, int heap_id, int size_class_index){
    sb->tid = tid;
    sb->heap_id = heap_id;
    sb->size_class = 2 ^ (3 + size_class_index);

    int size_of_struct = alignSize(sizeof(superblock_t));
    sb->total_slots = (mem_pagesize() - size_of_struct) / sb->size_class;
    sb->free_slots = sb->total_slots;

    sb->next = NULL;

    sb->data = (void *)((char *)sb + size_of_struct);
}

/*
 * Given a tid, heap_id and a size class, create an empty superblock with 
 * maximum possible slots for the given size_class and with zero slots
 * allocated.
 */
superblock_t *create_new_superblock(int tid, int heap_id, int size_class_index){
    superblock_t *new_block = (superblock_t *) mem_sbrk(mem_pagesize());

    set_superblock_properties(new_block, tid, heap_id, size_class_index);

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

    if (free_block == NULL){
        free_block = find_superblock_from_global_heap(tid, heap_id, size_class_index);

        // If a free block is still not found, then allocate a new block
        if (free_block == NULL){
            free_block = create_new_superblock(tid, heap_id, size_class_index);
        }
        heap->sized_blocks[size_class_index] = free_block;
    } else {

        while (free_block->tid != getTID() || free_block->free_slots == 0){

            if (free_block->next == NULL) {
                free_block = find_superblock_from_global_heap(tid, heap_id, size_class_index);
                if (free_block == NULL){
                    free_block = create_new_superblock(tid, heap_id, size_class_index);
                }

                free_block->next = heap->sized_blocks[size_class_index];
                heap->sized_blocks[size_class_index] = free_block;

            } else {
                free_block = free_block->next;

            }
        }
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
    superblock_t *free_block = find_free_block(heap, tid, heap_id, size_class_index);
     
    // Set free_block as used... how? bitmap maybe


    // This is wrong, but for the time being.
    return (void*)free_block;
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

        int i;
        for(i = 0; i <= getNumProcessors(); i++){
            heap_t *heap = &heaps[i];

            pthread_mutexattr_t mta;
            pthread_mutexattr_init(&mta);

            pthread_mutex_init(&(heap->lock), &mta);
        }
    }

    /* global heap is at heaps[0] */
    global_heap = heaps;

    return 0;
}


