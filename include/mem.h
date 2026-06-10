#pragma once

#include "platform.h"
#include "error.h"
#include <stddef.h>
#include <stdint.h>

typedef struct arena {
    uint8_t    *base;
    size_t      offset;
    size_t      capacity;
    char        _pad[CACHE_LINE_SIZE - sizeof(uint8_t*) - 2*sizeof(size_t)];
} CACHE_ALIGN arena_t;

typedef struct slab_pool {
    void      **free_list;
    uint32_t    count;
    uint32_t    capacity;
    size_t      obj_size;
    uint8_t    *backing;
    char        _pad[CACHE_LINE_SIZE
                     - sizeof(void**)
                     - 2*sizeof(uint32_t)
                     - sizeof(size_t)
                     - sizeof(uint8_t*)];
} CACHE_ALIGN slab_pool_t;

err_t   arena_init(arena_t *a, size_t capacity);
void   *arena_alloc(arena_t *a, size_t size, size_t align);
void    arena_reset(arena_t *a);
void    arena_destroy(arena_t *a);

err_t   slab_init(slab_pool_t *p, size_t obj_size, uint32_t count);
void   *slab_alloc(slab_pool_t *p);
void    slab_free(slab_pool_t *p, void *obj);
void    slab_destroy(slab_pool_t *p);
