#include <stdlib.h>


#include "mm_thread.h"
#include "memlib.h"



/* *** DATA STRUCTURES USED *** */
typedef struct {
    int heap_id;
    int tid;
    size_t size_class;
    int total_slots;
    int free_slots;
    superblock_t *next;
} superblock_t;


typedef struct {
    int used_bytes;
    int allocated_bytes;
    pthread_mutex_t lock;
    superblock_t *sized_blocks[10];
} heap_t;



/* *** VARIABLES NEEDED *** */
static heap_t *heaps = NULL;
static heap_t *global_heap = NULL;


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

void *mm_malloc(size_t sz)
{

    // For large page sizes, let the OS handle memory allocation for now
    if (sz > mem_pagesize()){
        char *large_block = (char *)malloc(sz);
        return large_block;
    }

    int heap_id = getTID() % getNumProcessors();

    // +1 Since heap[0] is global heap
    heap_t *heap = heaps + heap_id + 1;
    pthread_mutex_lock(&(heap->lock));


    int size_class_index = find_size_class_index(sz);
    superblock_t *free_block = heap->sized_blocks[size_class_index];

    if (free_block == NULL){

        pthread_mutex_lock(&(global_heap->lock));
        free_block = global_heap->sized_blocks[size_class_index];

        if (free_block != NULL){
            global_heap->sized_blocks[size_class_index] = free_block->next;
            
            free_block->next = NULL;
            free_block->tid = getTID();
            free_block->heap_id = heap_id;
            free_block->size_class = 2 ^ (3 + size_class_index);
            free_block->total_slots = 0; //TODO
            free_block->free_slots = 0; //TODO

            //TODO: 8 byte allignment
        }
        pthread_mutex_unlock(&(global_heap->lock));

        // If a free block is still not found, then allocate a new block
        if (free_block == NULL){
            free_block = (superblock_t *) mem_sbrk(mem_pagesize());
            free_block->next = NULL;
            free_block->tid = getTID();
            free_block->heap_id = heap_id;
            free_block->size_class = 2 ^ (3 + size_class_index);
            free_block->total_slots = 0; //TODO
            free_block->free_slots = 0; //TODO

        }
        heap->sized_blocks[size_class_index] = free_block;
    } else {

        while (free_block->tid != getTID() || free_block->free_slots == 0){
            
            if (free_block->next == NULL) {
                pthread_mutex_lock(&(global_heap->lock));

                free_block = global_heap->sized_blocks[size_class_index];
                if (free_block != NULL){
                    global_heap->sized_blocks[size_class_index] = free_block->next;
                    
                    free_block->next = NULL;
                    free_block->tid = getTID();
                    free_block->heap_id = heap_id;
                    free_block->size_class = 2 ^ (3 + size_class_index);
                    free_block->total_slots = 0; //TODO
                    free_block->free_slots = 0; //TODO

                    //TODO: 8 byte allignment
                }
                pthread_mutex_unlock(&(global_heap->lock));

                if (free_block == NULL){
                    free_block = (superblock_t *) mem_sbrk(mem_pagesize());
                    free_block->next = NULL;
                    free_block->tid = getTID();
                    free_block->heap_id = heap_id;
                    free_block->size_class = 2 ^ (3 + size_class_index);
                    free_block->total_slots = 0; //TODO
                    free_block->free_slots = 0; //TODO
                }

                free_block->next = heap->sized_blocks[size_class_index];
                heap->sized_blocks[size_class_index] = free_block;

            } else {
                free_block = free_block->next;

            }
        }
    }

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


