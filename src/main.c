#include "platform.h"
#include "server.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>

static server_t g_server;

static void sig_handler(int sig)
{
    (void)sig;
    server_stop(&g_server);
}

static int parse_int(const char *s, int lo, int hi, int def)
{
    if (!s) return def;
    char *end;
    long  v = strtol(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi) return def;
    return (int)v;
}

int main(int argc, char *argv[])
{
    uint16_t port     = 8080;
    int      workers  = (int)sysconf(_SC_NPROCESSORS_ONLN);
    bool     pin_cpus = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            int v = parse_int(argv[++i], 1, 65535, 8080);
            port  = (uint16_t)v;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            workers = parse_int(argv[++i], 1, MAX_WORKERS, workers);
        } else if (strcmp(argv[i], "-c") == 0) {
            pin_cpus = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                    "usage: ultrahttp [-p port] [-w workers] [-c]\n"
                    "  -p  TCP port          (default: 8080)\n"
                    "  -w  worker threads    (default: nproc)\n"
                    "  -c  pin workers to CPUs\n");
            return 0;
        }
    }

    struct sigaction sa;
    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    err_t err = server_init(&g_server, port, workers, pin_cpus);
    if (UNLIKELY(err != ERR_OK)) {
        fprintf(stderr, "server_init failed: %d\n", err);
        return 1;
    }

    err = server_run(&g_server);
    server_destroy(&g_server);
    return (err == ERR_OK) ? 0 : 1;
}
