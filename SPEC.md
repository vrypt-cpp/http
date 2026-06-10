# ultrahttp — Architecture & Design Specification

## Directory Tree

```
ultrahttp/
├── CMakeLists.txt
├── include/
│   ├── platform.h          # Compiler attributes, constants, backend selection
│   ├── error.h             # Error code enum
│   ├── mem.h               # Arena + slab allocator interfaces
│   ├── conn.h              # Connection, parser, pool interfaces
│   ├── http.h              # HTTP parser + static response buffers
│   ├── worker.h            # Worker, metrics, server_config
│   ├── server.h            # Server lifecycle
│   ├── epoll_backend.h     # epoll backend (BACKEND_EPOLL guard)
│   └── uring_backend.h     # io_uring backend (BACKEND_URING guard)
└── src/
    ├── mem.c               # mmap-backed arena + slab
    ├── conn.c              # Connection pool
    ├── http.c              # FSM parser + immutable response buffers
    ├── epoll_backend.c     # Edge-triggered epoll event loop
    ├── uring_backend.c     # io_uring multishot-accept event loop
    ├── worker.c            # Worker init, CPU pinning, dispatch
    ├── server.c            # Listener socket, SO_REUSEPORT, thread launch
    └── main.c              # Argument parsing, signal handling, entry point
```

---

## Architecture Specification

### Concurrency Model

Worker-per-core threading. Each worker thread owns an independent epoll
instance (or io_uring ring), an independent connection pool, and an
independent arena. No shared mutable state between workers in the hot path.

`SO_REUSEPORT` is set on the listener socket, enabling the kernel to
distribute `accept()` calls across worker threads without userspace
coordination. Each worker calls `accept4()` directly on the shared fd.

Thread affinity is optional (`-c` flag). When enabled, worker `i` is pinned
to CPU `i % nproc` using `pthread_setaffinity_np`.

### Backend Selection

Compile-time via CMake option `USE_URING`:

- `OFF` (default) → `BACKEND_EPOLL=1` — pure Linux epoll, ET mode
- `ON`            → `BACKEND_URING=1` — io_uring with multishot accept

Both backends share the same connection pool, parser, and response
subsystems. Only the I/O dispatch path differs.

---

## Cache Analysis

### `conn_t` hot-path field layout

```
fd             4 B   used on every syscall
worker_id      4 B   for pool release
state          4 B   branch on every event
parser.*      12 B   FSM position, read/written on every recv
resp_buf       8 B   pointer into .rodata static buffer
resp_len       4 B
resp_sent      4 B
recv_len       4 B
recv_off       4 B
recv_buf    4096 B   embedded; data lands here directly
```

`recv_buf` is embedded in `conn_t`. This eliminates one pointer indirection
and one potential cache miss on every `recv()` call. The tradeoff is a larger
struct (~4140 bytes); since connections are accessed one at a time the larger
footprint does not cause additional cache pressure compared to the
pointer-chasing alternative.

`resp_buf` points to one of three compile-time-constant strings in `.rodata`.
All three fit in under three cache lines total and will be L1-resident
after the first few requests on any core.

### `worker_t` cache layout

```
CACHE_ALIGN  →  64-byte boundary
id, listen_fd, event_fd, cpu_id   16 B  — fit in one line
conn_pool                          CACHE_ALIGN field
arena                              CACHE_ALIGN field
metrics                            CACHE_ALIGN field, never cross-worker written
running (atomic_bool)              own cache line
```

`metrics` fields are per-worker `uint64_t` counters incremented only by the
owning worker thread. No atomics needed; no cache-line sharing.

### `conn_pool_t` cache layout

```
conns        8 B   pointer to flat conn_t array
free_stack   8 B   pointer to flat uint32_t index stack
top          4 B   stack pointer
capacity     4 B
_pad            → fills struct to 64 B
```

`conn_pool_acquire` / `conn_pool_release` are O(1) array-index stack
operations. Hot entries near `top` stay L1-resident.

---

## Memory Layout Analysis

All backing memory is allocated once at startup via `mmap(MAP_POPULATE)`.
`MAP_POPULATE` faults in all pages immediately, eliminating minor-fault
latency spikes during request processing.

### Per-worker allocation (example: 65536 max conns / 2 workers = 32768)

```
conn_t array    32768 × ~4140 B  ≈  128 MB
free_stack      32768 ×    4 B  =    128 KB
arena                            =      4 MB  (reserved scratch)
```

### Zero runtime allocation

`malloc`, `free`, and `realloc` are never called after startup completes.
This eliminates:

- allocator lock contention
- heap fragmentation
- TLS cache overhead (jemalloc/tcmalloc)
- latency spikes from background coalescing

### Arena allocator

`arena_alloc` is a bump pointer with alignment rounding — O(1), no free
list, no fragmentation, no locks. Used only for startup auxiliary
allocations. Per-request state is never arena-allocated.

### Slab allocator

`slab_alloc` / `slab_free` are O(1) index-stack operations on a contiguous
backing array. Available for future extensions (TLS session state, etc.).
The connection pool uses an equivalent mechanism directly.

---

## Lock-Free Design Analysis

### Shared state inventory

The **only** shared state between workers is the OS-level listen socket fd.
The kernel handles `SO_REUSEPORT` distribution internally; no userspace
coordination is required.

### Per-worker exclusive ownership

- `conn_pool_t`       — owned by one worker, never shared
- `arena_t`           — owned by one worker, never shared
- epoll fd            — per-worker
- io_uring ring       — per-worker
- `worker_metrics_t`  — written only by the owning worker

### Atomic usage

`worker_t.running` is a C11 `atomic_bool`. Written once to `false` by the
signal handler; read in the event loop condition with `memory_order_relaxed`.
No ordering guarantee is required: the loop observes the flag within one
200ms epoll timeout window and exits cleanly.

### False-sharing prevention

Every hot structure carries `CACHE_ALIGN` (`__attribute__((aligned(64)))`).
The `worker_t` array is indexed by worker id; adjacent workers occupy
non-overlapping cache lines. `worker_metrics_t` is padded to a full cache
line so no worker's counters share a line with an adjacent worker's.

---

## epoll Execution Flow

```
worker_run()
  └─ epoll_backend_run(w)
       │
       ├─ epoll_add(listen_fd, EPOLLIN|EPOLLET, ptr=NULL)
       │
       └─ loop: epoll_wait(epfd, events[1024], timeout=200ms)
            │
            ├─ ptr == NULL  →  handle_accept(w, epfd)
            │    └─ drain: accept4() until EAGAIN
            │         ├─ TCP_NODELAY on each fd
            │         ├─ conn_pool_acquire()
            │         ├─ conn_reset()
            │         └─ epoll_add(fd, EPOLLIN|EPOLLET|EPOLLRDHUP, conn)
            │
            ├─ EPOLLHUP|EPOLLERR|EPOLLRDHUP  →  close_conn()
            │
            ├─ EPOLLIN  →  handle_read(w, conn, epfd)
            │    └─ drain: recv() until EAGAIN
            │         └─ http_parse(conn)
            │              ├─ incomplete  →  continue drain
            │              └─ complete   →  select resp_buf/resp_len
            │                   └─ epoll_mod(EPOLLOUT|EPOLLET|EPOLLRDHUP)
            │
            └─ EPOLLOUT  →  handle_write(w, conn, epfd)
                 └─ drain: send() until EAGAIN or complete
                      └─ all sent  →  conn_reset()
                           └─ epoll_mod(EPOLLIN|EPOLLET|EPOLLRDHUP)
```

**Edge-triggered discipline:** every fd is registered `EPOLLET`. Accept,
recv, and send loops all drain to `EAGAIN`. Missing a readability edge due to
a partial drain would stall the connection permanently until the next write
produces a spurious re-arm.

---

## io_uring Execution Flow

```
worker_run()
  └─ uring_backend_run(w, ctx)
       │
       ├─ submit_accept(ring, listen_fd)  ← multishot ACCEPT SQE
       │
       └─ loop: io_uring_submit_and_wait(ring, min=1)
            │
            └─ io_uring_for_each_cqe:
                 │
                 ├─ OP_ACCEPT  →  handle_accept_cqe()
                 │    ├─ TCP_NODELAY on new fd
                 │    ├─ conn_pool_acquire()
                 │    ├─ conn_reset()
                 │    ├─ submit_recv(ring, conn)
                 │    └─ if !(flags & IORING_CQE_F_MORE)
                 │            submit_accept(ring, listen_fd)  ← re-arm
                 │
                 ├─ OP_RECV  →  handle_recv_cqe()
                 │    ├─ res <= 0  →  submit_close(ring, conn)
                 │    └─ res > 0  →  http_parse(conn)
                 │         ├─ incomplete  →  submit_recv(ring, conn)
                 │         └─ complete   →  submit_send(ring, conn)
                 │
                 ├─ OP_SEND  →  handle_send_cqe()
                 │    ├─ partial   →  submit_send(ring, conn)
                 │    └─ complete  →  conn_reset() → submit_recv(ring, conn)
                 │
                 └─ OP_CLOSE  →  handle_close_cqe()
                      └─ conn_pool_release()
                 │
                 io_uring_cq_advance(ring, count)  ← single batch CQ advance
```

**Multishot accept:** one `IORING_OP_ACCEPT` with the multishot flag fires
one CQE per accepted connection until the kernel signals
`IORING_CQE_F_MORE = 0`, at which point the accept is re-armed. This
eliminates per-accept SQE submission cost.

**Batched CQ drain:** `io_uring_for_each_cqe` + `io_uring_cq_advance(count)`
processes all available completions before advancing the CQ head once,
minimising ring head write traffic.

**SQ poll (`IORING_SETUP_SQPOLL`):** attempted at init with a 2s idle
timeout. When the kernel supports it, SQEs are consumed by a kernel thread
without `io_uring_enter` syscalls. Falls back to normal submission if
unavailable.

---

## HTTP Parser — Performance Rationale

Single-pass, branch-predictable byte scanner. After a few hundred requests
the branch predictor saturates on the fast path (steps 0→1→…→DONE), so each
byte costs approximately one cycle in the predicted case.

Operates directly on the embedded `recv_buf`. No intermediate
representation, no string copy, no tokenization, no allocation. Partial-parse
state (`step`, `method`) is stored in `conn_t` adjacent to the buffer,
keeping both in the same cache line on continuation.

Fast-path recognition: the first byte is checked against `'G'`. Any other
byte sets `STEP_ERROR` with one branch, skipping all further method parsing.
A secondary check on the first byte identifies known method prefixes
(`P`, `D`, `H`, `O`) for 405 vs 404 discrimination.

---

## Response Generation — Performance Rationale

Three response strings are compile-time constants in `.rodata`. They are
never written after link time. All three fit in under three cache lines total
and are shared read-only across all workers without any synchronisation.

All `snprintf`, `strcat`, `sprintf`, and dynamic formatting is absent. Response
dispatch is a pointer assignment and a length copy — two scalar stores.

---

## Network Stack — Performance Rationale

**`TCP_NODELAY`** — disables Nagle's algorithm. Without it, the kernel waits
up to 200ms to coalesce small writes. With it, each response is sent
immediately.

**`SO_REUSEPORT`** — eliminates the accept lock. Without it, all workers
compete on a single kernel accept queue mutex. With it, each worker has its
own queue.

**`accept4(SOCK_NONBLOCK|SOCK_CLOEXEC)`** — sets both flags atomically at
accept time, avoiding a `fcntl` round-trip per connection.

**`TCP_FASTOPEN`** — reduces setup latency for repeat clients by allowing
payload in the SYN, eliminating one RTT on connection establishment.

**1MB socket buffers** — prevents receiver-side stalling under burst traffic.
At 1 Gbps, 1 MB covers ~8ms of burst, sufficient to absorb kernel scheduling
jitter.

---

## Scalability Analysis

### Horizontal (more cores)

Each worker is fully independent. Adding a worker adds one thread, one epoll
fd, one connection pool, and one arena. No central lock, no shared queue, no
coordination. Throughput scales linearly with core count until NIC saturation.

`SO_REUSEPORT` distributes connections via the kernel's 4-tuple hash.
Connection affinity is preserved for the duration of each connection.

### Vertical (more connections per worker)

The connection pool is a flat pre-allocated array. `acquire` and `release`
are array-index stack operations. At 32K connections the free stack occupies
~128 KB, well within L2 on any modern server.

### Latency tail

With ET epoll and full drain loops:
- P99 latency bounded by event batch size (1024 events per `epoll_wait`)
- No head-of-line blocking between connections
- No convoy effects from lock acquisition
- 200ms epoll timeout has zero impact under load; epoll returns immediately
  when events are ready

With io_uring SQPOLL the ring poller runs continuously while the CPU is
busy, eliminating kernel–userspace transitions at high load. At moderate
load `io_uring_submit_and_wait` provides natural back-pressure.

---

## Build Instructions

### Release — epoll backend (default)

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### Release — io_uring backend

```sh
mkdir build-uring && cd build-uring
cmake .. -DCMAKE_BUILD_TYPE=Release -DUSE_URING=ON
cmake --build . -j$(nproc)
# requires liburing-dev
```

### Debug + sanitizers

```sh
mkdir build-dbg && cd build-dbg
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

### PGO workflow

```sh
# Phase 1: instrument
mkdir build-pgo1 && cd build-pgo1
cmake .. -DCMAKE_BUILD_TYPE=PGOInstrument
cmake --build . -j$(nproc)

# Run under representative load (wrk, hey, etc.)
./ultrahttp -p 8080 -w $(nproc) &
wrk -t$(nproc) -c1000 -d30s http://localhost:8080/
kill %1

# Phase 2: optimise with profile data
cd ..
mkdir build-pgo2 && cd build-pgo2
cmake .. -DCMAKE_BUILD_TYPE=PGOUse
cmake --build . -j$(nproc)
```

### Run

```
./ultrahttp [-p port] [-w workers] [-c]

  -p   TCP port            (default: 8080)
  -w   worker thread count (default: nproc)
  -c   pin each worker to its corresponding CPU
```

---

## Benchmark Recommendation

```sh
# throughput
wrk -t$(nproc) -c$(( $(nproc) * 100 )) -d30s http://localhost:8080/

# fixed-rate latency histogram
wrk2 -t$(nproc) -c$(( $(nproc) * 100 )) -d30s -R500000 http://localhost:8080/

# connection concurrency
hey -n 1000000 -c 10000 http://localhost:8080/
```
