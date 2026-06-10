#include "conn.h"
#include "platform.h"
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>

err_t conn_pool_init(conn_pool_t *pool, uint32_t capacity)
{
    size_t conn_sz  = sizeof(conn_t) * capacity;
    size_t stack_sz = sizeof(uint32_t) * capacity;

    conn_t *conns = mmap(NULL, conn_sz,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                         -1, 0);
    if (UNLIKELY(conns == MAP_FAILED))
        return ERR_NOMEM;

    uint32_t *stack = mmap(NULL, stack_sz,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0);
    if (UNLIKELY(stack == MAP_FAILED)) {
        munmap(conns, conn_sz);
        return ERR_NOMEM;
    }

    pool->conns      = conns;
    pool->free_stack = stack;
    pool->capacity   = capacity;
    pool->top        = capacity;

    for (uint32_t i = 0; i < capacity; i++) {
        stack[i]       = i;
        conns[i].state = CONN_FREE;
        conns[i].fd    = -1;
    }

    return ERR_OK;
}

conn_t *conn_pool_acquire(conn_pool_t *pool)
{
    if (UNLIKELY(pool->top == 0))
        return NULL;
    uint32_t idx = pool->free_stack[--pool->top];
    return &pool->conns[idx];
}

void conn_pool_release(conn_pool_t *pool, conn_t *c)
{
    uint32_t idx = (uint32_t)(c - pool->conns);
    pool->free_stack[pool->top++] = idx;
}

void conn_pool_destroy(conn_pool_t *pool)
{
    if (pool->conns) {
        munmap(pool->conns, sizeof(conn_t) * pool->capacity);
        munmap(pool->free_stack, sizeof(uint32_t) * pool->capacity);
        pool->conns      = NULL;
        pool->free_stack = NULL;
        pool->top        = 0;
        pool->capacity   = 0;
    }
}

void conn_reset(conn_t *c)
{
    c->state            = CONN_READING;
    c->recv_len         = 0;
    c->recv_off         = 0;
    c->resp_buf         = NULL;
    c->resp_len         = 0;
    c->resp_sent        = 0;
    c->parser.offset    = 0;
    c->parser.step      = 0;
    c->parser.method    = METHOD_UNKNOWN;
    c->parser.complete  = false;
}
