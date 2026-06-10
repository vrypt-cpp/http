#include "server.h"
#include "worker.h"
#include "http.h"
#include "platform.h"
#include "error.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

int server_create_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (UNLIKELY(fd == -1))
        return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

#ifdef TCP_FASTOPEN
    int qlen = 32;
    setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
#endif

    int sndbuf = 1 << 20;
    int rcvbuf = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = __builtin_bswap16(port);

    union {
        struct sockaddr_in  in;
        struct sockaddr     sa;
    } bind_addr;
    __builtin_memcpy(&bind_addr.in, &addr, sizeof(addr));

    if (UNLIKELY(bind(fd, &bind_addr.sa, sizeof(bind_addr.in)) == -1)) {
        close(fd);
        return -1;
    }

    if (UNLIKELY(listen(fd, BACKLOG) == -1)) {
        close(fd);
        return -1;
    }

    return fd;
}

err_t server_init(server_t *s, uint16_t port, int num_workers, bool pin_cpus)
{
    if (UNLIKELY(num_workers <= 0 || num_workers > MAX_WORKERS))
        return ERR_INVAL;

    http_responses_init();

    __builtin_memset(s, 0, sizeof(*s));

    s->cfg.port                 = port;
    s->cfg.num_workers          = num_workers;
    s->cfg.pin_cpus             = pin_cpus;
    s->cfg.max_conns_per_worker = MAX_CONNECTIONS / (uint32_t)num_workers;
    s->num_workers              = num_workers;

    int listen_fd = server_create_listener(port);
    if (UNLIKELY(listen_fd == -1))
        return ERR_SYSCALL;

    s->cfg.listen_fd = listen_fd;

    for (int i = 0; i < num_workers; i++) {
        err_t err = worker_init(&s->workers[i], &s->cfg, i);
        if (UNLIKELY(err != ERR_OK)) {
            close(listen_fd);
            for (int j = 0; j < i; j++)
                worker_destroy(&s->workers[j]);
            return err;
        }
    }

    return ERR_OK;
}

err_t server_run(server_t *s)
{
    pthread_t tids[MAX_WORKERS];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    for (int i = 0; i < s->num_workers; i++) {
        int ret = pthread_create(&tids[i], &attr,
                                  worker_run, &s->workers[i]);
        if (UNLIKELY(ret != 0)) {
            server_stop(s);
            pthread_attr_destroy(&attr);
            return ERR_SYSCALL;
        }
    }

    pthread_attr_destroy(&attr);

    for (int i = 0; i < s->num_workers; i++)
        pthread_join(tids[i], NULL);

    return ERR_OK;
}

void server_stop(server_t *s)
{
    for (int i = 0; i < s->num_workers; i++)
        atomic_store_explicit(&s->workers[i].running, false,
                              memory_order_relaxed);
}

void server_destroy(server_t *s)
{
    if (s->cfg.listen_fd >= 0) {
        close(s->cfg.listen_fd);
        s->cfg.listen_fd = -1;
    }

    for (int i = 0; i < s->num_workers; i++)
        worker_destroy(&s->workers[i]);
}
