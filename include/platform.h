#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#define CACHE_LINE_SIZE     64
#define CACHE_ALIGN         __attribute__((aligned(CACHE_LINE_SIZE)))
#define PACKED              __attribute__((packed))
#define LIKELY(x)           __builtin_expect(!!(x), 1)
#define UNLIKELY(x)         __builtin_expect(!!(x), 0)
#define PREFETCH_R(p)       __builtin_prefetch((p), 0, 3)
#define PREFETCH_W(p)       __builtin_prefetch((p), 1, 3)
#define FORCE_INLINE        __attribute__((always_inline)) static inline
#define COLD                __attribute__((cold))
#define HOT                 __attribute__((hot))
#define UNREACHABLE()       __builtin_unreachable()
#define BARRIER()           __asm__ volatile("" ::: "memory")
#define PAD_TO(s, a)        char _pad[((a) - ((s) % (a))) % (a)]

#define MAX_WORKERS         128
#define MAX_CONNECTIONS     65536
#define BACKLOG             65535
#define RECV_BUFFER_SIZE    4096
#define MAX_EVENTS          1024
#define RING_SIZE           4096

#ifndef BACKEND_URING
#define BACKEND_URING 0
#endif

#ifndef BACKEND_EPOLL
#define BACKEND_EPOLL 1
#endif

#if BACKEND_URING && BACKEND_EPOLL
#error "Select exactly one backend"
#endif

#if !BACKEND_URING && !BACKEND_EPOLL
#undef  BACKEND_EPOLL
#define BACKEND_EPOLL 1
#endif
