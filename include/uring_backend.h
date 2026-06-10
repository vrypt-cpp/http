#pragma once

#include "platform.h"
#include "worker.h"
#include "error.h"

#if BACKEND_URING

#include <liburing.h>

#define URING_QUEUE_DEPTH   4096
#define URING_FIXED_FILES   MAX_CONNECTIONS

typedef enum uring_op {
    OP_ACCEPT   = 0,
    OP_RECV     = 1,
    OP_SEND     = 2,
    OP_CLOSE    = 3,
} uring_op_t;

typedef struct uring_ctx {
    struct io_uring ring;
    struct iovec   *fixed_bufs;
    uint32_t        pending;
    char            _pad[CACHE_LINE_SIZE
                         - sizeof(struct io_uring)
                         - sizeof(struct iovec*)
                         - sizeof(uint32_t)
                         < CACHE_LINE_SIZE
                         ? 0
                         : (CACHE_LINE_SIZE
                            - (sizeof(struct io_uring)
                               + sizeof(struct iovec*)
                               + sizeof(uint32_t)))
                           % CACHE_LINE_SIZE];
} uring_ctx_t;

err_t   uring_backend_init(worker_t *w, uring_ctx_t *ctx);
void    uring_backend_destroy(uring_ctx_t *ctx);
void    uring_backend_run(worker_t *w, uring_ctx_t *ctx);

#endif
