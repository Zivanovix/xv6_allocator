#include "types.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"


#define MAX_ORDER 11

struct buddy_block {
    int is_free;
    int order;
    struct buddy_block *next;
    struct buddy_block *prev;
};

struct buddy_t {
    struct spinlock lock;
    struct buddy_block* freelist[MAX_ORDER + 1];
    struct buddy_block* descriptors; // pointer to array of descriptors (one descriptor for each page)
    void* base_addr; // first page available for allocation
    int total_pages; // total num of available pages
};

struct buddy_t kbuddy;

void push_front(struct buddy_block* desc) {
    desc->prev = 0;
    desc->next = kbuddy.freelist[desc->order];
    if(desc->next != 0) {
        desc->next->prev = desc;
    }
    kbuddy.freelist[desc->order] = desc;
}


void buddy_init(void* space, int num_pages) {
    uint64 start_addr = PGROUNDUP((uint64)space);
    initlock(&kbuddy.lock, "kbuddy");

    uint64 descs_size = sizeof(struct buddy_block) * num_pages;
    uint64 descs_size_pages = PGROUNDUP(descs_size) / PGSIZE;

    kbuddy.descriptors = (struct buddy_block*)start_addr;
    kbuddy.base_addr = (void*)(start_addr + descs_size_pages * PGSIZE);
    kbuddy.total_pages = num_pages - (int)descs_size_pages;

    for(int i = 0; i < kbuddy.total_pages; ++i) {
        kbuddy.descriptors[i].is_free = 0;
        kbuddy.descriptors[i].order = 0;
        kbuddy.descriptors[i].next = 0;
        kbuddy.descriptors[i].prev = 0;
    }

    for(int i = 0; i <= MAX_ORDER; ++i) {
        kbuddy.freelist[i] = 0;
    }


    uint64 current_base = (uint64)kbuddy.base_addr;
    int pages_left = kbuddy.total_pages;

    while(pages_left > 0) {
        int order = MAX_ORDER;
        // find biggest chunk of 2^order blocks/pages we have available
        while((1 << order) > pages_left) {
            order--;
        }

        //count the index of the page in descriptors array that will be the start of the 2^order size free chunk
        int idx = (current_base - (uint64)kbuddy.base_addr) / PGSIZE;

        //update metadata
        struct buddy_block* block = kbuddy.descriptors + idx;
        block->is_free = 1;
        block->order = order;

        // put free chunk in freeList
        push_front(block);

        current_base += (1 << order) * PGSIZE;
        pages_left -= (1 << order);
    }
}


int desc_index(struct buddy_block* desc) {
    return desc - kbuddy.descriptors;
}


struct buddy_block* find_buddy(struct buddy_block* desc) {
    int i = desc_index(desc);
    int buddy_index = i ^ (1 << desc->order);

    if (buddy_index < 0 || buddy_index >= kbuddy.total_pages) {
        return 0;
    }

    return kbuddy.descriptors + buddy_index;
}


void* buddy_alloc(int num_pages) {

    if(num_pages <= 0) return 0;

    int order = MAX_ORDER;
    while((1 << order) >= num_pages) {
        order--;
    }
    ++order;

    int foundorder = -1;

    acquire(&kbuddy.lock);

    for(int i = order; i <= MAX_ORDER; ++i) {
        if(kbuddy.freelist[i] != 0) {
            foundorder = i;
            break;
        }
    }
    if(foundorder == -1) {
        release(&kbuddy.lock);
        return 0;
    }

    struct buddy_block* c = kbuddy.freelist[foundorder];
    //unlink c from list of free chunks of size 2^order pages
    kbuddy.freelist[foundorder] = c->next;
    if(c->next != 0) {
        c->next->prev = 0;
        c->next = 0;
    }
    c->is_free = 0;
    for(int i = foundorder; i > order; --i) {
        c->order--;
        struct buddy_block* bud = find_buddy(c);
        if(bud == 0) {
            release(&kbuddy.lock);
            return 0;
        }
        bud->order = i-1;
        bud->is_free = 1;
        push_front(bud);
    }

    release(&kbuddy.lock);
    return (void*)((uint64)kbuddy.base_addr + (uint64)desc_index(c) * PGSIZE);
}

void buddy_free(void* addr) {
    int idx = ((uint64)addr - (uint64)kbuddy.base_addr) / PGSIZE;

    if (idx < 0 || idx >= kbuddy.total_pages) {
        return;
    }
    struct buddy_block* c = kbuddy.descriptors + idx;
    acquire(&kbuddy.lock);

    c->is_free = 1;

    for(int i = c->order; i < MAX_ORDER; ++i) {
        struct buddy_block* bud = find_buddy(c);
        if(bud == 0 || bud->order != c->order || bud->is_free == 0) {
            push_front(c);
            release(&kbuddy.lock);
            return;
        }
        // Unlink bud
        if(bud->next) {
            bud->next->prev = bud->prev;
        }
        if(bud->prev) {
            bud->prev->next = bud->next;
        }
        else {
            kbuddy.freelist[i] = bud->next;
        }

        bud->next = 0;
        bud->prev = 0;

        if(bud > c) {
            bud->is_free = 0;
            bud->order = 0;
        }
        else {
            c->is_free = 0;
            c->order = 0;
            c = bud;
        }
        c->order++;
        c->is_free = 1;
    }
    push_front(c);
    release(&kbuddy.lock);
}
