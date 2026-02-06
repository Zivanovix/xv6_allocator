#include "types.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"


// Align obj size to a first greater number that is divisible by 8
#define ALIGIN(size) (((size) + 7) & ~7)

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
    int in_use; // flag for static pool of cahces
};

#define MAX_CACHES 40
static kmem_cache_t cache_pool[MAX_CACHES];
static kmem_cache_t* cache_chain = 0; // head of all caches list


void kmem_init(void* space, int block_num) {
    buddy_init(space, block_num);

    initlock(&cache_chain.lock, "kmem_cache_chain");

    for(int i = 0; i < MAX_CACHES; ++i) {
        cache_pool[i].in_use = 0;
        initlock(&cache_pool[i].lock, "cache_lock");
    }
}