#include "types.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"

// Align obj size to a first greater number that is divisible by 8
#define ALIGN(size) (((size) + 7) & ~7)
#define NUM_GENERIC_CACHES 13
#define MAX_CACHES 40

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
    size_t obj_size; // ALIGNED size + pointer to next when slot is free
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
    int expanded;
};

kmem_cache_t cache_pool[MAX_CACHES];

kmem_cache_t* cache_chain = 0; // head of all caches list
struct spinlock cache_chain_lock; // Global lock for cache list

kmem_cache_t* generic_caches[NUM_GENERIC_CACHES];

char* generic_cache_names[] = {
    "size-32", "size-64", "size-128", "size-256", "size-512",
    "size-1024", "size-2048", "size-4096", "size-8192", "size-16384",
    "size-32768", "size-65536", "size-131072"
};

int get_generic_cache_index(size_t size) {
    if(size < 32) return -1;
    if(size == 32) return 0;
    int idx = 0;

    size_t current_size = 32;
    while(current_size < size && idx < NUM_GENERIC_CACHES - 1) {
        current_size <<= 1;
        idx++;
    }
    return idx;
}


size_t get_size_from_index(int idx) {
    return (size_t)1 << (5 + idx);
}

void kmem_init(void* space, int block_num) {
    buddy_init(space, block_num);

    initlock(&cache_chain_lock, "kmem_cache_chain");

    for(int i = 0; i < MAX_CACHES; ++i) {
        cache_pool[i].in_use = 0;
        initlock(&cache_pool[i].lock, "cache_lock");
    }

    for(int i = 0; i < NUM_GENERIC_CACHES; ++i) {
        generic_caches[i] = 0;
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
    cp->expanded = 0;

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

    cachep->expanded = 1;
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
        if(slot > (void*)s && slot < (void*)((char*)s + cachep->slab_pages * BLOCK_SIZE)) {
            break;
        }
    }
    if(!s) {
        for (s = cachep->slabs_full; s; s = s->next) {
            if(slot > (void*)s && slot < (void*)((char*)s + cachep->slab_pages * BLOCK_SIZE)) {
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
    s->used_count--;

    if(s->used_count == cachep->obj_per_slab-1) {
        detach_slab(s, &cachep->slabs_full);
        if(s->used_count == 0) {
            push_slab_to_front(s, &cachep->slabs_free);
        }
        else {
            push_slab_to_front(s, &cachep->slabs_partial);
        }
    }
    else if(s->used_count == 0) {
        detach_slab(s, &cachep->slabs_partial);
        //buddy_free(s);
        push_slab_to_front(s, &cachep->slabs_free);
    }

    release(&cachep->lock);
}
/*

void clean_slab_objects(kmem_cache_t* cp, slab_t* s) {
    if (cp->dtor) {
        char* slot_ptr = (char*)s + ALIGN(sizeof(slab_t));
        for(int i = 0; i < cp->obj_per_slab; ++i) {
            cp->dtor((void*)(slot_ptr + 8)); // call dtor on user part of slot
            slot_ptr += cp->obj_size;
        }
    }
}

int kmem_cache_shrink(kmem_cache_t *cachep) {
    if(!cachep) return 0;

    acquire(&cachep->lock);

    if(cachep->expanded) {
        cachep->expanded = 0;
        release(&cachep->lock);
        return 0;
    }
    int ret = 0;

    slab_t* next_slab = 0;
    for(slab_t* s = cachep->slabs_free; s; s = next_slab) {
        next_slab = s->next;
        clean_slab_objects(cachep, s);
        buddy_free(s, cachep->slab_pages);
        ret += cachep->slab_pages;
    }

    cachep->slabs_free = 0;
    release(&cachep->lock);

    return ret;
}

void kmem_cache_destroy(kmem_cache_t* cachep) {
    if(!cachep) return;

    // delete from global cache chain
    acquire(&cache_chain_lock);

    kmem_cache_t **prev = &cache_chain;

    while(*prev && *prev != cachep) {
        prev = &((*prev)->next_cache);
    }
    if(*prev) {
        *prev = cachep->next_cache;
    }
    cachep->next_cache = 0;

    release(&cache_chain_lock);

    acquire(&cachep->lock);

    slab_t* lists[] = {cachep->slabs_full, cachep->slabs_partial, cachep->slabs_free};
    for(int i = 0; i < 3; ++i) {
        slab_t* s = lists[i];
        while(s) {
            slab_t* next_slab = s->next;
            clean_slab_objects(cachep, s);
            buddy_free(s, cachep->slab_pages);
            s = next_slab;
        }
    }

    cachep->in_use = 0;
    release(&cachep->lock);
}
*/
void* kmalloc(size_t size) {
    if (size > (1 << 17)) return 0;

    int idx = get_generic_cache_index(size);

    if (generic_caches[idx] == 0) {
        generic_caches[idx] = kmem_cache_create(generic_cache_names[idx], get_size_from_index(idx), 0, 0);
    }


    if (generic_caches[idx] == 0) return 0;

    return kmem_cache_alloc(generic_caches[idx]);
}

/*
void buff_kfree(const void* obj) {
    if (obj == 0) return;

    //acquire(&cache_chain_lock);
    kmem_cache_t* cp = 0;

    for(int i = 0; i < NUM_GENERIC_CACHES; ++i) {
        if(generic_caches[i]->in_use == 0) continue;
        cp = generic_caches[i];
        acquire(&cp->lock);

        slab_t* lists[] = {cp->slabs_partial, cp->slabs_full};
        for (int i = 0; i < 2; i++) {
            for (slab_t* s = lists[i]; s; s = s->next) {

                char* start = (char*)s;
                char* end = start + (cp->slab_pages * BLOCK_SIZE);

                if ((char*)obj > start && (char*)obj < end) {
                    release(&cp->lock);
                    //release(&cache_chain_lock);
                    kmem_cache_free(cp, (void*)obj);
                    return;
                }
            }
        }

        release(&cp->lock);
        //cp = cp->next_cache;
    }
    release(&cache_chain_lock);
}
*/