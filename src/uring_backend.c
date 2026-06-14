#include "platform.h"

#if BACKEND_URING

#include "uring_backend.h"
#include "conn.h"
#include "http.h"
#include "worker.h"

#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

#define SQE_USER_DATA(op, conn) \
    (((uint64_t)(uintptr_t)(conn)) | ((uint64_t)(op) << 48))

#define USER_DATA_OP(ud)    ((uring_op_t)(((ud) >> 48) & 0xFFFF))
#define USER_DATA_CONN(ud)  ((conn_t*)(uintptr_t)((ud) & 0x0000FFFFFFFFFFFF))

FORCE_INLINE struct io_uring_sqe *get_sqe(struct io_uring *ring)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (UNLIKELY(!sqe)) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

FORCE_INLINE void submit_accept(struct io_uring *ring, int listen_fd)
{
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (UNLIKELY(!sqe)) return;
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
    sqe->user_data = SQE_USER_DATA(OP_ACCEPT, NULL);
}

FORCE_INLINE void submit_recv(struct io_uring *ring, conn_t *c)
{
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (UNLIKELY(!sqe)) return;
    uint32_t space = RECV_BUFFER_SIZE - c->recv_len;
    io_uring_prep_recv(sqe, c->fd,
                       c->recv_buf + c->recv_len,
                       space, 0);
    sqe->user_data = SQE_USER_DATA(OP_RECV, c);
}

FORCE_INLINE void submit_send(struct io_uring *ring, conn_t *c)
{
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (UNLIKELY(!sqe)) return;
    io_uring_prep_send(sqe, c->fd,
                       c->resp_buf + c->resp_sent,
                       c->resp_len - c->resp_sent,
                       MSG_NOSIGNAL);
    sqe->user_data = SQE_USER_DATA(OP_SEND, c);
}

FORCE_INLINE void submit_close(struct io_uring *ring, conn_t *c,
                               worker_t *w)
{
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (UNLIKELY(!sqe)) {
        close(c->fd);
        c->fd = -1;
        conn_pool_release(&w->conn_pool, c);
        return;
    }
    io_uring_prep_close(sqe, c->fd);
    sqe->user_data = SQE_USER_DATA(OP_CLOSE, c);
}

HOT static void handle_accept_cqe(worker_t *w, struct io_uring *ring,
                                   struct io_uring_cqe *cqe)
{
    int fd = cqe->res;
    if (UNLIKELY(fd < 0)) {
        if (fd != -EAGAIN && fd != -EWOULDBLOCK)
            w->metrics.errors_total++;
        return;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    conn_t *c = conn_pool_acquire(&w->conn_pool);
    if (UNLIKELY(!c)) {
        close(fd);
        w->metrics.errors_total++;
        return;
    }

    conn_reset(c);
    c->fd        = fd;
    c->worker_id = w->id;

    w->metrics.accepts_total++;
    submit_recv(ring, c);
}

HOT static void handle_recv_cqe(worker_t *w, struct io_uring *ring,
                                  conn_t *c, int res)
{
    if (UNLIKELY(res <= 0)) {
        submit_close(ring, c, w);
        return;
    }

    c->recv_len += (uint32_t)res;
    http_response_t resp = http_parse(c);

    if (!c->parser.complete) {
        if (UNLIKELY(c->recv_len == RECV_BUFFER_SIZE)) {
            submit_close(ring, c, w);
            return;
        }
        submit_recv(ring, c);
        return;
    }

    w->metrics.requests_total++;

    if (resp == RESP_200) {
        c->resp_buf = g_responses.buf_200;
        c->resp_len = g_responses.len_200;
    } else if (resp == RESP_404) {
        c->resp_buf = g_responses.buf_404;
        c->resp_len = g_responses.len_404;
    } else {
        c->resp_buf = g_responses.buf_405;
        c->resp_len = g_responses.len_405;
    }

    c->resp_sent = 0;
    c->state     = CONN_WRITING;
    submit_send(ring, c);
}

HOT static void handle_send_cqe(worker_t *w, struct io_uring *ring,
                                  conn_t *c, int res)
{
    if (UNLIKELY(res < 0)) {
        submit_close(ring, c, w);
        return;
    }

    c->resp_sent += (uint32_t)res;
    w->metrics.bytes_sent += (uint64_t)res;

    if (c->resp_sent < c->resp_len) {
        submit_send(ring, c);
        return;
    }

    conn_reset(c);
    submit_recv(ring, c);
}

HOT static void handle_close_cqe(worker_t *w, conn_t *c)
{
    c->fd    = -1;
    c->state = CONN_FREE;
    conn_pool_release(&w->conn_pool, c);
}

err_t uring_backend_init(worker_t *w, uring_ctx_t *ctx)
{
    struct io_uring_params params;
    __builtin_memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;

    int ret = io_uring_queue_init_params(URING_QUEUE_DEPTH,
                                          &ctx->ring, &params);
    if (ret < 0) {
        params.flags = 0;
        ret = io_uring_queue_init(URING_QUEUE_DEPTH, &ctx->ring, 0);
        if (ret < 0)
            return ERR_SYSCALL;
    }

    ctx->pending = 0;
    (void)w;
    return ERR_OK;
}

void uring_backend_destroy(uring_ctx_t *ctx)
{
    io_uring_queue_exit(&ctx->ring);
}

HOT void uring_backend_run(worker_t *w, uring_ctx_t *ctx)
{
    struct io_uring      *ring = &ctx->ring;
    struct io_uring_cqe  *cqe;

    submit_accept(ring, w->listen_fd);
    io_uring_submit(ring);

    while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
        int ret = io_uring_submit_and_wait(ring, 1);
        if (UNLIKELY(ret < 0 && ret != -EINTR))
            break;

        uint32_t head;
        uint32_t count = 0;

        io_uring_for_each_cqe(ring, head, cqe) {
            uint64_t ud   = cqe->user_data;
            uring_op_t op = USER_DATA_OP(ud);
            conn_t    *c  = USER_DATA_CONN(ud);
            int        res = cqe->res;

            switch (op) {
            case OP_ACCEPT:
                handle_accept_cqe(w, ring, cqe);
                if (!(cqe->flags & IORING_CQE_F_MORE))
                    submit_accept(ring, w->listen_fd);
                break;
            case OP_RECV:
                handle_recv_cqe(w, ring, c, res);
                break;
            case OP_SEND:
                handle_send_cqe(w, ring, c, res);
                break;
            case OP_CLOSE:
                handle_close_cqe(w, c);
                break;
            }
            count++;
        }

        io_uring_cq_advance(ring, count);
    }
}

#endif
