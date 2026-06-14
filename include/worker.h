#pragma once

#include "platform.h"
#include "conn.h"
#include "mem.h"
#include "error.h"
#include <stdint.h>
#include <stdatomic.h>

typedef struct worker_metrics {
    uint64_t    requests_total;
    uint64_t    accepts_total;
    uint64_t    errors_total;
    uint64_t    bytes_sent;
    char        _pad[CACHE_LINE_SIZE - 4*sizeof(uint64_t)];
} CACHE_ALIGN worker_metrics_t;

typedef struct worker {
    int             id;
    int             listen_fd;
    int             event_fd;
    int             cpu_id;

    conn_pool_t     conn_pool;
    worker_metrics_t metrics;

    atomic_bool     running;
    char            _pad[CACHE_LINE_SIZE - sizeof(atomic_bool)];
} CACHE_ALIGN worker_t;

typedef struct server_config {
    uint16_t    port;
    int         num_workers;
    bool        pin_cpus;
    int         listen_fd;
    uint32_t    max_conns_per_worker;
} server_config_t;

err_t   worker_init(worker_t *w, const server_config_t *cfg, int id);
void    worker_destroy(worker_t *w);
void   *worker_run(void *arg);
