#pragma once

#include "platform.h"
#include "error.h"
#include <stdint.h>
#include <stddef.h>

typedef enum conn_state {
    CONN_FREE       = 0,
    CONN_READING    = 1,
    CONN_WRITING    = 2,
    CONN_DRAINING   = 3,
} conn_state_t;

typedef enum http_method {
    METHOD_UNKNOWN  = 0,
    METHOD_GET      = 1,
    METHOD_OTHER    = 2,
} http_method_t;

typedef enum http_response {
    RESP_200        = 0,
    RESP_404        = 1,
    RESP_405        = 2,
} http_response_t;

typedef struct parser_state {
    uint32_t        offset;
    uint8_t         step;
    http_method_t   method;
    bool            complete;
    char            _pad[2];
} parser_state_t;

typedef struct conn {
    int             fd;
    int             worker_id;
    conn_state_t    state;
    parser_state_t  parser;

    const uint8_t  *resp_buf;
    uint32_t        resp_len;
    uint32_t        resp_sent;

    uint32_t        recv_len;
    uint32_t        recv_off;

    uint8_t         recv_buf[RECV_BUFFER_SIZE];
} conn_t;

typedef struct conn_pool {
    conn_t     *conns;
    uint32_t   *free_stack;
    uint32_t    top;
    uint32_t    capacity;
    char        _pad[CACHE_LINE_SIZE
                     - sizeof(conn_t*)
                     - sizeof(uint32_t*)
                     - 2*sizeof(uint32_t)];
} CACHE_ALIGN conn_pool_t;

err_t    conn_pool_init(conn_pool_t *pool, uint32_t capacity);
conn_t  *conn_pool_acquire(conn_pool_t *pool);
void     conn_pool_release(conn_pool_t *pool, conn_t *c);
void     conn_pool_destroy(conn_pool_t *pool);
void     conn_reset(conn_t *c);
