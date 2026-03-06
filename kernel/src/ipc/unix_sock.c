#include "ipc/unix_sock.h"
#include "sched/sched.h"
#include "serial.h"

/* errno codes */
#define EADDRINUSE  98
#define ENOTCONN   107
#define ECONNREFUSED 111
#define EINVAL      22
#define EAGAIN      11
#define ENOMEM      12

static unix_sock_t unix_socks[MAX_UNIX_SOCKS];

int unix_sock_alloc(void) {
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (unix_socks[i].state == USOCK_FREE) {
            unix_sock_t *us = &unix_socks[i];
            us->state = USOCK_UNBOUND;
            us->peer_closed = 0;
            us->refs = 1;
            us->path[0] = '\0';
            us->rd_pos = 0;
            us->wr_pos = 0;
            us->count = 0;
            us->peer = (void *)0;
            us->backlog_count = 0;
            for (int j = 0; j < UNIX_SOCK_BACKLOG; j++)
                us->backlog[j] = (void *)0;
            return i;
        }
    }
    return -ENOMEM;
}

int unix_sock_bind(int idx, const char *path) {
    if (idx < 0 || idx >= MAX_UNIX_SOCKS)
        return -EINVAL;
    unix_sock_t *us = &unix_socks[idx];
    if (us->state != USOCK_UNBOUND)
        return -EINVAL;

    /* Check for duplicate path */
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (i == idx) continue;
        if (unix_socks[i].state >= USOCK_BOUND && unix_socks[i].state <= USOCK_LISTENING) {
            /* Compare paths */
            const char *a = unix_socks[i].path;
            const char *b = path;
            int match = 1;
            while (*a && *b) {
                if (*a != *b) { match = 0; break; }
                a++; b++;
            }
            if (match && *a == *b)
                return -EADDRINUSE;
        }
    }

    /* Copy path */
    int i = 0;
    while (path[i] && i < UNIX_SOCK_PATH_MAX - 1) {
        us->path[i] = path[i];
        i++;
    }
    us->path[i] = '\0';
    us->state = USOCK_BOUND;
    return 0;
}

int unix_sock_listen(int idx) {
    if (idx < 0 || idx >= MAX_UNIX_SOCKS)
        return -EINVAL;
    unix_sock_t *us = &unix_socks[idx];
    if (us->state != USOCK_BOUND)
        return -EINVAL;
    us->state = USOCK_LISTENING;
    return 0;
}

int unix_sock_connect(const char *path) {
    /* Find listening socket with matching path */
    int listen_idx = -1;
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (unix_socks[i].state == USOCK_LISTENING) {
            const char *a = unix_socks[i].path;
            const char *b = path;
            int match = 1;
            while (*a && *b) {
                if (*a != *b) { match = 0; break; }
                a++; b++;
            }
            if (match && *a == *b) {
                listen_idx = i;
                break;
            }
        }
    }
    if (listen_idx < 0)
        return -ECONNREFUSED;

    unix_sock_t *listener = &unix_socks[listen_idx];

    /* Check backlog full */
    if (listener->backlog_count >= UNIX_SOCK_BACKLOG)
        return -EAGAIN;

    /* Allocate client socket */
    int client_idx = unix_sock_alloc();
    if (client_idx < 0) return client_idx;

    /* Allocate server-side socket */
    int server_idx = unix_sock_alloc();
    if (server_idx < 0) {
        unix_socks[client_idx].state = USOCK_FREE;
        return server_idx;
    }

    unix_sock_t *client = &unix_socks[client_idx];
    unix_sock_t *server = &unix_socks[server_idx];

    /* Link as peers */
    client->state = USOCK_CONNECTED;
    server->state = USOCK_CONNECTED;
    client->peer = server;
    server->peer = client;

    /* Add server socket to listener's backlog */
    for (int i = 0; i < UNIX_SOCK_BACKLOG; i++) {
        if (listener->backlog[i] == (void *)0) {
            listener->backlog[i] = server;
            listener->backlog_count++;
            break;
        }
    }

    return client_idx;
}

int unix_sock_accept(int listen_idx) {
    if (listen_idx < 0 || listen_idx >= MAX_UNIX_SOCKS)
        return -EINVAL;
    unix_sock_t *listener = &unix_socks[listen_idx];
    if (listener->state != USOCK_LISTENING)
        return -EINVAL;

    if (listener->backlog_count == 0)
        return -EAGAIN;

    /* Pop first connection from backlog */
    unix_sock_t *server = (void *)0;
    for (int i = 0; i < UNIX_SOCK_BACKLOG; i++) {
        if (listener->backlog[i] != (void *)0) {
            server = listener->backlog[i];
            listener->backlog[i] = (void *)0;
            listener->backlog_count--;
            break;
        }
    }

    if (!server)
        return -EAGAIN;

    /* Return the index of the server socket */
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (&unix_socks[i] == server)
            return i;
    }
    return -EINVAL;
}

int unix_sock_send(unix_sock_t *us, const uint8_t *buf, uint32_t len, int nonblock) {
    if (!us || us->state != USOCK_CONNECTED || !us->peer)
        return -ENOTCONN;

    unix_sock_t *peer = us->peer;
    uint32_t total = 0;

    while (total < len) {
        if (peer->peer_closed)
            return total > 0 ? (int)total : -ENOTCONN;

        if (peer->count < UNIX_SOCK_BUF_SZ) {
            peer->buf[peer->wr_pos] = buf[total];
            peer->wr_pos = (peer->wr_pos + 1) % UNIX_SOCK_BUF_SZ;
            peer->count++;
            total++;
        } else {
            if (total > 0) return (int)total;
            if (nonblock) return -EAGAIN;
            sched_yield();
        }
    }
    return (int)total;
}

int unix_sock_recv(unix_sock_t *us, uint8_t *buf, uint32_t len, int nonblock) {
    if (!us || (us->state != USOCK_CONNECTED && us->state != USOCK_CLOSED))
        return -ENOTCONN;

    uint32_t total = 0;

    while (total < len) {
        if (us->count > 0) {
            buf[total] = us->buf[us->rd_pos];
            us->rd_pos = (us->rd_pos + 1) % UNIX_SOCK_BUF_SZ;
            us->count--;
            total++;
        } else if (us->peer_closed) {
            break;  /* EOF */
        } else {
            if (total > 0) break;
            if (nonblock) return -EAGAIN;
            sched_yield();
        }
    }
    return (int)total;
}

void unix_sock_close(unix_sock_t *us) {
    if (!us) return;
    if (us->refs > 0)
        us->refs--;
    if (us->refs > 0) return;

    /* Notify peer */
    if (us->peer) {
        us->peer->peer_closed = 1;
        us->peer->peer = (void *)0;
    }

    us->state = USOCK_FREE;
    us->peer = (void *)0;
    us->path[0] = '\0';
}

unix_sock_t *unix_sock_get(int idx) {
    if (idx < 0 || idx >= MAX_UNIX_SOCKS)
        return (void *)0;
    return &unix_socks[idx];
}

int unix_sock_readable(unix_sock_t *us) {
    if (!us) return 0;
    return us->count > 0 || us->peer_closed;
}

int unix_sock_writable(unix_sock_t *us) {
    if (!us) return 0;
    if (!us->peer) return 0;
    return us->peer->count < UNIX_SOCK_BUF_SZ;
}

int unix_sock_has_backlog(unix_sock_t *us) {
    if (!us) return 0;
    return us->backlog_count > 0;
}
