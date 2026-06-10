#pragma once

#include "platform.h"
#include "worker.h"
#include "error.h"
#include <stdint.h>

typedef struct server {
    server_config_t cfg;
    worker_t        workers[MAX_WORKERS];
    int             num_workers;
} server_t;

err_t   server_init(server_t *s, uint16_t port, int num_workers, bool pin_cpus);
err_t   server_run(server_t *s);
void    server_stop(server_t *s);
void    server_destroy(server_t *s);
int     server_create_listener(uint16_t port);
