#pragma once

#include "platform.h"
#include "worker.h"
#include "error.h"

#if BACKEND_EPOLL

err_t   epoll_backend_init(worker_t *w);
void    epoll_backend_destroy(worker_t *w);
void    epoll_backend_run(worker_t *w);

#endif
