#include "platform.h"

#if BACKEND_EPOLL

#include "epoll_backend.h"
#include "conn.h"
#include "http.h"
#include "worker.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define EPOLL_MAX_EVENTS    MAX_EVENTS

FORCE_INLINE err_t set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (UNLIKELY(flags == -1))
        return ERR_SYSCALL;
    if (UNLIKELY(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1))
        return ERR_SYSCALL;
    return ERR_OK;
}

FORCE_INLINE err_t epoll_add(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = ptr;
    if (UNLIKELY(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1))
        return ERR_SYSCALL;
    return ERR_OK;
}

FORCE_INLINE err_t epoll_mod(int epfd, int fd, uint32_t events, void *ptr)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = ptr;
    if (UNLIKELY(epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1))
        return ERR_SYSCALL;
    return ERR_OK;
}

FORCE_INLINE void close_conn(worker_t *w, conn_t *c, int epfd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd    = -1;
    c->state = CONN_FREE;
    conn_pool_release(&w->conn_pool, c);
}

HOT static void handle_accept(worker_t *w, int epfd)
{
    while (1) {
        union {
            struct sockaddr_storage storage;
            struct sockaddr         sa;
        } addr_u;
        socklen_t addrlen = sizeof(addr_u.storage);

        int fd = accept4(w->listen_fd, &addr_u.sa,
                         &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (UNLIKELY(fd == -1)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            return;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        conn_t *c = conn_pool_acquire(&w->conn_pool);
        if (UNLIKELY(!c)) {
            close(fd);
            w->metrics.errors_total++;
            continue;
        }

        conn_reset(c);
        c->fd        = fd;
        c->worker_id = w->id;

        if (UNLIKELY(epoll_add(epfd, fd,
                               EPOLLIN | EPOLLET | EPOLLRDHUP,
                               c) != ERR_OK)) {
            close(fd);
            conn_pool_release(&w->conn_pool, c);
            w->metrics.errors_total++;
            continue;
        }

        w->metrics.accepts_total++;
    }
}

HOT static void handle_read(worker_t *w, conn_t *c, int epfd)
{
    while (1) {
        uint32_t space = RECV_BUFFER_SIZE - c->recv_len;
        if (UNLIKELY(space == 0)) {
            close_conn(w, c, epfd);
            return;
        }

        ssize_t n = recv(c->fd,
                         c->recv_buf + c->recv_len,
                         space, 0);
        if (n > 0) {
            c->recv_len += (uint32_t)n;

            http_response_t resp = http_parse(c);

            if (c->parser.complete) {
                w->metrics.requests_total++;

                if (resp == RESP_200) {
                    c->resp_buf  = g_responses.buf_200;
                    c->resp_len  = g_responses.len_200;
                } else if (resp == RESP_404) {
                    c->resp_buf  = g_responses.buf_404;
                    c->resp_len  = g_responses.len_404;
                } else {
                    c->resp_buf  = g_responses.buf_405;
                    c->resp_len  = g_responses.len_405;
                }

                c->resp_sent = 0;
                c->state     = CONN_WRITING;

                epoll_mod(epfd, c->fd,
                          EPOLLOUT | EPOLLET | EPOLLRDHUP,
                          c);
                return;
            }
            continue;
        }

        if (n == 0) {
            close_conn(w, c, epfd);
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;

        close_conn(w, c, epfd);
        return;
    }
}

HOT static void handle_write(worker_t *w, conn_t *c, int epfd)
{
    while (c->resp_sent < c->resp_len) {
        ssize_t n = send(c->fd,
                         c->resp_buf + c->resp_sent,
                         c->resp_len - c->resp_sent,
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->resp_sent += (uint32_t)n;
            w->metrics.bytes_sent += (uint64_t)n;
            continue;
        }

        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR)
                continue;
            close_conn(w, c, epfd);
            return;
        }
    }

    conn_reset(c);
    c->state = CONN_READING;
    epoll_mod(epfd, c->fd,
              EPOLLIN | EPOLLET | EPOLLRDHUP,
              c);
}

err_t epoll_backend_init(worker_t *w)
{
    w->event_fd = epoll_create1(EPOLL_CLOEXEC);
    if (UNLIKELY(w->event_fd == -1))
        return ERR_SYSCALL;
    return ERR_OK;
}

void epoll_backend_destroy(worker_t *w)
{
    if (w->event_fd >= 0) {
        close(w->event_fd);
        w->event_fd = -1;
    }
}

HOT void epoll_backend_run(worker_t *w)
{
    int    epfd   = w->event_fd;
    struct epoll_event events[EPOLL_MAX_EVENTS];

    conn_t *sentinel = NULL;
    if (UNLIKELY(epoll_add(epfd, w->listen_fd,
                           EPOLLIN | EPOLLET,
                           sentinel) != ERR_OK))
        return;

    while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
        int n = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 200);

        if (UNLIKELY(n < 0)) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            uint32_t ev    = events[i].events;
            conn_t  *c     = events[i].data.ptr;

            if (UNLIKELY(c == sentinel)) {
                handle_accept(w, epfd);
                continue;
            }

            if (UNLIKELY(ev & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))) {
                close_conn(w, c, epfd);
                continue;
            }

            if (ev & EPOLLIN) {
                PREFETCH_R(c->recv_buf);
                handle_read(w, c, epfd);
            } else if (ev & EPOLLOUT) {
                PREFETCH_R(c->resp_buf);
                handle_write(w, c, epfd);
            }
        }
    }
}

#endif
