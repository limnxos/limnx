/*
 * netagent.c — Resilient TCP network agent for Limnx
 *
 * Demonstrates the AI-native OS stack working end-to-end:
 *  - epoll for multiplexed I/O (listen + multiple clients)
 *  - SA_RESTART for signal resilience
 *  - Inference service registration + health heartbeats
 *  - Supervisor-managed lifecycle (auto-restart on crash)
 *
 * Usage: netagent [port] [max_requests] [crash]
 *   port          - TCP port to listen on (default: 9700)
 *   max_requests  - serve N requests then exit (default: 10, 0=unlimited)
 *   crash         - if "crash", exit with 1 (triggers supervisor restart)
 */
#include "libc/libc.h"

#define MAX_CLIENTS   4
#define BUF_SIZE      256
#define AGENT_PREFIX  "agent: "
#define AGENT_PREFIX_LEN 7

static volatile int got_sigint = 0;

static void sigint_handler(int sig) {
    (void)sig;
    got_sigint = 1;
    sys_sigreturn();
}

static int parse_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

int main(int argc, char **argv) {
    int port = 9700;
    int max_requests = 10;
    int crash_mode = 0;

    if (argc >= 2) port = parse_int(argv[1]);
    if (argc >= 3) max_requests = parse_int(argv[2]);
    if (argc >= 4 && argv[3][0] == 'c') crash_mode = 1;

    printf("[netagent] Starting on port %d (max=%d, crash=%d)\n",
           port, max_requests, crash_mode);

    /* Install SIGINT handler with SA_RESTART — epoll_wait auto-restarts */
    sys_sigaction3(SIGINT, sigint_handler, SA_RESTART);

    /* Create listening TCP socket */
    long listen_conn = sys_tcp_socket();
    if (listen_conn < 0) {
        puts("[netagent] Failed to create TCP socket\n");
        return 1;
    }

    if (sys_tcp_listen(listen_conn, port) < 0) {
        printf("[netagent] Failed to listen on port %d\n", port);
        sys_tcp_close(listen_conn);
        return 1;
    }

    /* Register with inference service registry */
    sys_infer_register("netagent", "/tmp/netagent.sock");
    sys_infer_health(0);

    printf("[netagent] Listening on port %d, registered as 'netagent'\n", port);

    int served = 0;

    /* Main loop: accept connections and serve requests */
    while (max_requests == 0 || served < max_requests) {
        if (got_sigint) {
            printf("[netagent] Caught SIGINT, continuing (SA_RESTART)\n");
            got_sigint = 0;
        }

        /* Report health heartbeat */
        sys_infer_health((long)served);

        /* Accept a client */
        long conn = sys_tcp_accept(listen_conn);
        if (conn < 0) {
            if (conn == -EINTR) continue;  /* interrupted, retry */
            /* Timeout or error — just retry */
            continue;
        }

        /* Read request */
        char buf[BUF_SIZE];
        long n = sys_tcp_recv(conn, buf, BUF_SIZE - AGENT_PREFIX_LEN - 1);
        if (n > 0) {
            /* Build response: "agent: <request>" */
            char resp[BUF_SIZE];
            int ri = 0;
            for (int i = 0; i < AGENT_PREFIX_LEN; i++)
                resp[ri++] = AGENT_PREFIX[i];
            for (int i = 0; i < n && ri < BUF_SIZE - 1; i++)
                resp[ri++] = buf[i];
            resp[ri] = '\0';

            sys_tcp_send(conn, resp, ri);
            served++;
            printf("[netagent] Served request %d: '%s'\n", served, resp);
        }

        sys_tcp_close(conn);
    }

    /* Final health report */
    sys_infer_health(9999);
    sys_tcp_close(listen_conn);

    if (crash_mode) {
        printf("[netagent] Exiting with crash (status 1)\n");
        return 1;
    }

    printf("[netagent] Exiting cleanly after %d requests\n", served);
    return 0;
}
