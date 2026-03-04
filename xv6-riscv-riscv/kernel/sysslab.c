#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "slab.h"
#include "proc.h"


extern pagetable_t kernel_pagetable;

uint64
sys_kmem_init(void)
{
    struct proc *p = myproc();
    uint64 space;
    int block_num;
    argaddr(0, &space);
    argint(1, &block_num);


    if(mirror_user_pagetable(p->pagetable, kernel_pagetable, p->sz) < 0){
        return -1;
    }

    ukmem_init((void*)space, block_num);
    return 0;
}

uint64
sys_kmem_cache_create(void)
{
    char name[32];
    uint64 size;
    uint64 ctor, dtor;

    argstr(0, name, 32);
    argaddr(1, &size);
    argaddr(2, &ctor);
    argaddr(3, &dtor);

    return (uint64)kmem_cache_create(name, size, (void (*)(void*))ctor, (void (*)(void*))dtor);
}

uint64
sys_kmem_cache_alloc(void)
{
    uint64 cachep;
    argaddr(0, &cachep);
    return (uint64)ukmem_cache_alloc((kmem_cache_t*)cachep);
}

uint64
sys_kmem_cache_free(void)
{
    uint64 cachep, objp;
    argaddr(0, &cachep);
    argaddr(1, &objp);
    ukmem_cache_free((kmem_cache_t*)cachep, (void*)objp);
    return 0;
}

uint64
sys_kmalloc(void)
{
    uint64 size;
    argaddr(0, &size);
    return (uint64)ukmalloc(size);
}

uint64
sys_kfree(void)
{
    uint64 objp;
    argaddr(0, &objp);
    ubuff_kfree((void*)objp);
    return 0;
}

uint64
sys_kmem_cache_destroy(void)
{
    uint64 cachep;
    argaddr(0, &cachep);
    ukmem_cache_destroy((kmem_cache_t*)cachep);
    return 0;
}

uint64
sys_kmem_cache_info(void)
{
    uint64 cachep;
    argaddr(0, &cachep);
    kmem_cache_info((kmem_cache_t*)cachep);
    return 0;
}

uint64
sys_kmem_cache_shrink(void)
{
    uint64 cachep;
    argaddr(0, &cachep);
    return ukmem_cache_shrink((kmem_cache_t*)cachep);
}