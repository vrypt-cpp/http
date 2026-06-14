#include "worker.h"
#include "server.h"
#include "platform.h"
#include "conn.h"
#include "http.h"
#include "error.h"

#if BACKEND_EPOLL
#include "epoll_backend.h"
#endif

#if BACKEND_URING
#include "uring_backend.h"
#endif

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

err_t worker_init(worker_t *w, const server_config_t *cfg, int id)
{
    w->id         = id;
    w->event_fd   = -1;
    w->cpu_id     = cfg->pin_cpus ? id : -1;

    int lfd = server_create_listener(cfg->port);
    if (UNLIKELY(lfd == -1))
        return ERR_SYSCALL;
    w->listen_fd = lfd;

    atomic_store_explicit(&w->running, true, memory_order_relaxed);

    __builtin_memset(&w->metrics, 0, sizeof(w->metrics));

    err_t err = conn_pool_init(&w->conn_pool, cfg->max_conns_per_worker);
    if (UNLIKELY(err != ERR_OK))
        return err;

#if BACKEND_EPOLL
    err = epoll_backend_init(w);
    if (UNLIKELY(err != ERR_OK)) {
        conn_pool_destroy(&w->conn_pool);
        return err;
    }
#endif

    return ERR_OK;
}

void worker_destroy(worker_t *w)
{
#if BACKEND_EPOLL
    epoll_backend_destroy(w);
#endif

    conn_pool_destroy(&w->conn_pool);
    if (w->listen_fd >= 0) {
        close(w->listen_fd);
        w->listen_fd = -1;
    }
}

void *worker_run(void *arg)
{
    worker_t *w = (worker_t*)arg;

    if (w->cpu_id >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(w->cpu_id % (int)sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

#if BACKEND_EPOLL
    epoll_backend_run(w);
#endif

#if BACKEND_URING
    uring_ctx_t ctx;
    if (uring_backend_init(w, &ctx) == ERR_OK) {
        uring_backend_run(w, &ctx);
        uring_backend_destroy(&ctx);
    }
#endif

    return NULL;
}
