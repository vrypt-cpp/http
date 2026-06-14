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

