#include <stdlib.h>
#include "memlib.h"
#include "mm_thread.h"
#include "malloc.h"

/* *** DATA STRUCTURES USED *** */
typedef struct {
    int used_bytes;
    int allocated_bytes;
    pthread_mutex_t mta_lock;
} heap_t;


/* *** VARIABLES NEEDED *** */
static heap_t *heaps = NULL;
static heap_t *global_heap = NULL;

void *mm_malloc(size_t sz)
{
    (void)sz; /* Avoid warning about unused variable */
    return NULL;
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

        int i = 0;
        for (i = 0; i <= getNumProcessors(); i++){
            heap_t *heap = &heaps[i];

            pthread_mutexattr_t mta;
            pthread_mutexattr_init(&mta);

            pthread_mutex_init(&(heap->mta_lock), &mta);
        }
    }

    /* global heap is at heaps[0] */
    global_heap = heaps;

    return 0;
}


