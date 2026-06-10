#include "mem.h"
#include "platform.h"
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

err_t arena_init(arena_t *a, size_t capacity)
{
    void *mem = mmap(NULL, capacity,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                     -1, 0);
    if (UNLIKELY(mem == MAP_FAILED))
        return ERR_NOMEM;

    a->base     = mem;
    a->offset   = 0;
    a->capacity = capacity;
    return ERR_OK;
}

void *arena_alloc(arena_t *a, size_t size, size_t align)
{
    size_t aligned = (a->offset + (align - 1)) & ~(align - 1);
    if (UNLIKELY(aligned + size > a->capacity))
        return NULL;
    void *ptr   = a->base + aligned;
    a->offset   = aligned + size;
    return ptr;
}

void arena_reset(arena_t *a)
{
    a->offset = 0;
}

void arena_destroy(arena_t *a)
{
    if (a->base) {
        munmap(a->base, a->capacity);
        a->base     = NULL;
        a->offset   = 0;
        a->capacity = 0;
    }
}

err_t slab_init(slab_pool_t *p, size_t obj_size, uint32_t count)
{
    size_t total = obj_size * count;
    void  *mem   = mmap(NULL, total,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1, 0);
    if (UNLIKELY(mem == MAP_FAILED))
        return ERR_NOMEM;

    void **fl = mmap(NULL, sizeof(void*) * count,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (UNLIKELY(fl == MAP_FAILED)) {
        munmap(mem, total);
        return ERR_NOMEM;
    }

    p->backing  = mem;
    p->free_list = fl;
    p->obj_size = obj_size;
    p->capacity = count;
    p->count    = count;

    for (uint32_t i = 0; i < count; i++)
        fl[i] = (uint8_t*)mem + (size_t)i * obj_size;

    return ERR_OK;
}

void *slab_alloc(slab_pool_t *p)
{
    if (UNLIKELY(p->count == 0))
        return NULL;
    return p->free_list[--p->count];
}

void slab_free(slab_pool_t *p, void *obj)
{
    p->free_list[p->count++] = obj;
}

void slab_destroy(slab_pool_t *p)
{
    if (p->backing) {
        munmap(p->backing, p->obj_size * p->capacity);
        munmap(p->free_list, sizeof(void*) * p->capacity);
        p->backing   = NULL;
        p->free_list = NULL;
        p->count     = 0;
        p->capacity  = 0;
    }
}
