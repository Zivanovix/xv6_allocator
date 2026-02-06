#include "types.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"

// Align obj size to a first greater number that is divisible by 8
#define ALIGN(size) (((size) + 7) & ~7)

// metadata about a single slab
typedef struct slab_s {
    struct slab_s* next;
    struct slab_s* prev;
    void* free_slots; // first free slot in a slab
    int used_count; // how many slots are used at the moment
    kmem_cache_t* cache; // pointer to the cache that owns this slab
} slab_t;


struct kmem_cache_s {
    struct spinlock lock;
    char name[16];
    size_t obj_size; // ALIGNED size
    int obj_per_slab;
    int slab_pages;  // Single slab size in pages

    void (*ctor)(void *);
    void (*dtor)(void *);

    slab_t* slabs_full;
    slab_t* slabs_partial;
    slab_t* slabs_free;

    kmem_cache_t* next_cache;
    int error_code;
    int in_use; // flag for static pool of caches
};

#define MAX_CACHES 40
static kmem_cache_t cache_pool[MAX_CACHES];
static kmem_cache_t* cache_chain = 0; // head of all caches list
struct spinlock cache_chain_lock; // Global lock for chace list

void kmem_init(void* space, int block_num) {
    buddy_init(space, block_num);

    initlock(&cache_chain_lock, "kmem_cache_chain");

    for(int i = 0; i < MAX_CACHES; ++i) {
        cache_pool[i].in_use = 0;
        initlock(&cache_pool[i].lock, "cache_lock");
    }
}

kmem_cache_t* kmem_cache_create(const char* name, size_t size, void (*ctor)(void*), void (*dtor)(void*)) {
    kmem_cache_t* cp = 0;
    size_t aligned_size = ALIGN(size);

    acquire(&cache_chain_lock);
    for(int i = 0; i < MAX_CACHES; ++i) {
        if(cache_pool[i].in_use == 0) {
            cp = cache_pool + i;
            cp->in_use = 1;
            break;
        }
    }

    release(&cache_chain_lock);
    if(cp == 0) return 0; // Error! maximum number of caches in system reached

    safestrcpy(cp->name, name, sizeof(cp->name));
    cp->obj_size = aligned_size;
    cp->ctor = ctor;
    cp->dtor = dtor;
    cp->error_code = 0;
    cp->slabs_full = 0;
    cp->slabs_partial = 0;
    cp->slabs_free = 0;

    size_t slab_header_size = ALIGN(sizeof(slab_t));
    size_t min_need = slab_header_size + aligned_size;
    int pages = (min_need + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // size of the slab must be the power of two because of buddy
    int actual_pages = 1;
    while(actual_pages < pages) {
        actual_pages <<= 1;
    }
    cp->slab_pages = actual_pages;

    size_t total_mem = (size_t)actual_pages * BLOCK_SIZE;
    cp->obj_per_slab = (total_mem - slab_header_size) / aligned_size;

    acquire(&cache_chain_lock);
    cp->next_cache = cache_chain;
    cache_chain = cp;
    release(&cache_chain_lock);

    return cp;
}

