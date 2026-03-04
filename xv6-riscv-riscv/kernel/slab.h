typedef struct kmem_cache_s kmem_cache_t;

#define BLOCK_SIZE (4096)

typedef unsigned long size_t;


void kmem_init(void *space, int block_num);
void ukmem_init(void* space, int block_num);

kmem_cache_t *kmem_cache_create(const char *name, size_t size, void (*ctor)(void *), void (*dtor)(void *)); // Allocate cache


int kmem_cache_shrink(kmem_cache_t *cachep); // Shrink cache
int ukmem_cache_shrink(kmem_cache_t* cachep);

void *kmem_cache_alloc(kmem_cache_t *cachep); // Allocate one object from cache
void* ukmem_cache_alloc(kmem_cache_t* cachep);

void kmem_cache_free(kmem_cache_t *cachep, void *objp); // Deallocate one object from cache
void ukmem_cache_free(kmem_cache_t* cachep, void* obj);

void *kmalloc(size_t size); // Alloacate one small memory buffer
void* ukmalloc(size_t size);

void buff_kfree(const void *objp); // Deallocate one small memory buffer
void ubuff_kfree(const void* obj);

void kmem_cache_destroy(kmem_cache_t *cachep); // Deallocate cache
void ukmem_cache_destroy(kmem_cache_t* cachep);

void kmem_cache_info(kmem_cache_t *cachep); // Print cache info

