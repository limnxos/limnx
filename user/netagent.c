/*
 * netagent.c — Epoll-driven TCP network agent for Limnx
 *
 * Demonstrates the AI-native OS stack working end-to-end:
 *  - epoll for multiplexed I/O (listen + multiple concurrent clients)
 *  - O_NONBLOCK TCP sockets for non-blocking accept/recv
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
#define LISTENER_DATA 0xFFFF
#define EPOLL_TIMEOUT 500

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

typedef struct {
    int  conn_idx;   /* TCP connection index, -1 if slot free */
    int  fd;         /* fd from tcp_to_fd, -1 if not assigned */
} client_slot_t;

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

    /* Set listener to nonblock */
    sys_tcp_setopt(listen_conn, 1 /* TCP_OPT_NONBLOCK */, 1);

    /* Create fd for listener so epoll can monitor it */
    long listen_fd = sys_tcp_to_fd(listen_conn);
    if (listen_fd < 0) {
        puts("[netagent] Failed to create listener fd\n");
        sys_tcp_close(listen_conn);
        return 1;
    }

    /* Create epoll instance */
    long epfd = sys_epoll_create(0);
    if (epfd < 0) {
        puts("[netagent] Failed to create epoll\n");
        sys_tcp_close(listen_conn);
        return 1;
    }

    /* Add listener fd to epoll */
    epoll_event_t ev;
    ev.events = EPOLLIN;
    ev.data = LISTENER_DATA;
    sys_epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    /* Register with inference service registry */
    sys_infer_register("netagent", "/tmp/netagent.sock");
    sys_infer_health(0);

    printf("[netagent] Listening on port %d (epoll), registered as 'netagent'\n", port);

    /* Client tracking */
    client_slot_t clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].conn_idx = -1;
        clients[i].fd = -1;
    }

    int served = 0;
    epoll_event_t events[MAX_CLIENTS + 1];

    /* Main epoll event loop */
    while (max_requests == 0 || served < max_requests) {
        if (got_sigint) {
            printf("[netagent] Caught SIGINT, continuing (SA_RESTART)\n");
            got_sigint = 0;
        }

        /* Report health heartbeat */
        sys_infer_health((long)served);

        long nev = sys_epoll_wait(epfd, events, MAX_CLIENTS + 1, EPOLL_TIMEOUT);
        if (nev < 0) {
            if (nev == -EINTR) continue;
            continue;
        }

        if (nev == 0) continue; /* timeout, loop for heartbeat */

        for (long i = 0; i < nev; i++) {
            if (events[i].data == LISTENER_DATA) {
                /* Listener event — accept all pending connections */
                while (1) {
                    long conn = sys_tcp_accept(listen_conn);
                    if (conn < 0) break; /* EAGAIN or error */

                    /* Find a free client slot */
                    int slot = -1;
                    for (int s = 0; s < MAX_CLIENTS; s++) {
                        if (clients[s].conn_idx < 0) { slot = s; break; }
                    }

                    if (slot < 0) {
                        /* No slots — reject */
                        sys_tcp_close(conn);
                        continue;
                    }

                    /* Set client to nonblock and create fd for epoll */
                    sys_tcp_setopt(conn, 1 /* TCP_OPT_NONBLOCK */, 1);
                    long cfd = sys_tcp_to_fd(conn);
                    if (cfd < 0) {
                        sys_tcp_close(conn);
                        continue;
                    }

                    clients[slot].conn_idx = (int)conn;
                    clients[slot].fd = (int)cfd;

                    /* Add client fd to epoll */
                    epoll_event_t cev;
                    cev.events = EPOLLIN;
                    cev.data = (uint64_t)slot;
                    sys_epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                /* Client event */
                int slot = (int)events[i].data;
                if (slot < 0 || slot >= MAX_CLIENTS) continue;
                if (clients[slot].conn_idx < 0) continue;

                int conn = clients[slot].conn_idx;
                int cfd = clients[slot].fd;

                char buf[BUF_SIZE];
                long n = sys_tcp_recv(conn, buf, BUF_SIZE - AGENT_PREFIX_LEN - 1);

                if (n > 0) {
                    /* Build response: "agent: <request>" */
                    char resp[BUF_SIZE];
                    int ri = 0;
                    for (int j = 0; j < AGENT_PREFIX_LEN; j++)
                        resp[ri++] = AGENT_PREFIX[j];
                    for (int j = 0; j < n && ri < BUF_SIZE - 1; j++)
                        resp[ri++] = buf[j];
                    resp[ri] = '\0';

                    sys_tcp_send(conn, resp, ri);
                    served++;
                    printf("[netagent] Served request %d: '%s'\n", served, resp);
                }

                /* Done with client — remove from epoll, close */
                sys_epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, (void *)0);
                sys_close(cfd);
                sys_tcp_close(conn);
                clients[slot].conn_idx = -1;
                clients[slot].fd = -1;
            }
        }
    }

    /* Final health report */
    sys_infer_health(9999);

    /* Clean up remaining clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].conn_idx >= 0) {
            sys_epoll_ctl(epfd, EPOLL_CTL_DEL, clients[i].fd, (void *)0);
            sys_close(clients[i].fd);
            sys_tcp_close(clients[i].conn_idx);
        }
    }

    sys_close(epfd);
    sys_close(listen_fd);
    sys_tcp_close(listen_conn);

    if (crash_mode) {
        printf("[netagent] Exiting with crash (status 1)\n");
        return 1;
    }

    printf("[netagent] Exiting cleanly after %d requests\n", served);
    return 0;
}
