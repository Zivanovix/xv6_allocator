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
    cp->obj_size = aligned_size + 8;
    cp->ctor = ctor;
    cp->dtor = dtor;
    cp->error_code = 0;
    cp->slabs_full = 0;
    cp->slabs_partial = 0;
    cp->slabs_free = 0;

    size_t slab_header_size = ALIGN(sizeof(slab_t));
    size_t min_need = slab_header_size + cp->obj_size;
    int pages = (min_need + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // size of the slab must be the power of two because of buddy
    int actual_pages = 1;
    while(actual_pages < pages) {
        actual_pages <<= 1;
    }
    cp->slab_pages = actual_pages;

    size_t total_mem = (size_t)actual_pages * BLOCK_SIZE;
    cp->obj_per_slab = (total_mem - slab_header_size) / cp->obj_size;

    acquire(&cache_chain_lock);
    cp->next_cache = cache_chain;
    cache_chain = cp;
    release(&cache_chain_lock);

    return cp;
}

void push_slab_to_front(slab_t* s, slab_t** list) {
    s->prev = 0;
    s->next = *list;
    if(s->next) {
        s->next->prev = s;
    }
    *list = s;
}

void pop_slab(slab_t** list) {
    slab_t* first = *list;
    *list = first->next;
    if(first->next) {
        first->next->prev = 0;
    }
    first->next = 0;
}

void detach_slab(slab_t* s, slab_t** list) {
    if(s->next) {
        s->next->prev = s->prev;
    }
    if(s->prev) {
        s->prev->next = s->next;
    }
    else {
        *list = s->next;
    }
    s->next = 0;
    s->prev = 0;
}

slab_t* alloc_new_slab(kmem_cache_t* cachep) {
    void* mem = buddy_alloc(cachep->slab_pages);

    if(mem == 0) return 0;

    slab_t* s = (slab_t*)mem;
    s->cache = cachep;
    s->used_count = 0;

    void* slot_start = (char*)s + ALIGN(sizeof(slab_t));
    s->free_slots = slot_start;

    for(int i = 0; i < cachep->obj_per_slab; ++i) {
        void* user_space = (char*)slot_start + 8;
        if(cachep->ctor) {
            cachep->ctor(user_space);
        }
        void* next = (i < cachep->obj_per_slab-1) ? (char*)slot_start + cachep->obj_size : 0;
        *(void**)slot_start = next;
        slot_start = next;
    }
    return s;
}

void* kmem_cache_alloc(kmem_cache_t* cachep) {
    void* ret = 0;
    slab_t* s = 0;
    int freeSlabUsed = 0;
    acquire(&cachep->lock);

    if(cachep->slabs_partial != 0) {
        s = cachep->slabs_partial;
    }
    else if(cachep->slabs_free != 0) {
        s = cachep->slabs_free;
        freeSlabUsed = 1;
    }
    else {
        s = alloc_new_slab(cachep);
        push_slab_to_front(s, &cachep->slabs_partial);
    }
    if(s == 0) {
        return 0;
    }
    ret = s->free_slots;
    s->free_slots = *(void**)ret;
    s->used_count++;

    if(freeSlabUsed == 1) {
        pop_slab(&cachep->slabs_free);
        if(s->used_count != cachep->obj_per_slab) {
            push_slab_to_front(s, &cachep->slabs_partial);
        }
    }
    if(s->used_count == cachep->obj_per_slab) {
        if(!freeSlabUsed) {
            pop_slab(&cachep->slabs_partial);
        }
        push_slab_to_front(s, &cachep->slabs_full);
    }


    release(&cachep->lock);

    return (void*)((char*)ret + 8);
}


void kmem_cache_free(kmem_cache_t* cachep, void* obj) {
    if(!cachep || !obj) return;

    void* slot = (char*)obj - 8;
    acquire(&cachep->lock);

    // find a slab which holds this object
    slab_t* s = 0;

    for (s = cachep->slabs_partial; s; s = s->next) {
        if(slot >= (void*)s && slot < (void*)((char*)s + cachep->slab_pages * BLOCK_SIZE)) {
            break;
        }
    }
    if(!s) {
        for (s = cachep->slabs_partial; s; s = s->next) {
            if(slot >= (void*)s && slot < (void*)((char*)s + cachep->slab_pages * BLOCK_SIZE)) {
                break;
            }
        }
    }

    if(!s) {
        release(&cachep->lock);
        return; // Error: object does not belong to this cache
    }

    // return slot back to the free list in slab
    *(void**)slot = s->free_slots;
    s->free_slots = (void*)slot;

    if(s->used_count == cachep->obj_per_slab) {
        detach_slab(s, &cachep->slabs_full);
        push_slab_to_front(s, &cachep->slabs_partial);
    }
    else if(s->used_count == 1) {
        detach_slab(s, &cachep->slabs_partial);
        push_slab_to_front(s, &cachep->slabs_free);
    }
    s->used_count--;

    release(&cachep->lock);
}