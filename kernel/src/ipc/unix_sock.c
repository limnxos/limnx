#define pr_fmt(fmt) "[usock] " fmt
#include "klog.h"

#include "ipc/unix_sock.h"
#include "sched/sched.h"
#include "sync/spinlock.h"
#include "arch/serial.h"
#include "errno.h"

static unix_sock_t unix_socks[MAX_UNIX_SOCKS];

/* Lock order: tier 5 (subsystem). See klog.h for full hierarchy */
static spinlock_t unix_lock = SPINLOCK_INIT;

int unix_sock_alloc(void) {
    uint64_t flags;
    spin_lock_irqsave(&unix_lock, &flags);

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
            us->peer = NULL;
            us->backlog_count = 0;
            for (int j = 0; j < UNIX_SOCK_BACKLOG; j++)
                us->backlog[j] = NULL;
            spin_unlock_irqrestore(&unix_lock, flags);
            return i;
        }
    }

    spin_unlock_irqrestore(&unix_lock, flags);
    pr_err("socket table full\n");
    return -ENOMEM;
}

int unix_sock_bind(int idx, const char *path) {
    uint64_t flags;
    spin_lock_irqsave(&unix_lock, &flags);

    if (idx < 0 || idx >= MAX_UNIX_SOCKS) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EINVAL;
    }
    unix_sock_t *us = &unix_socks[idx];
    if (us->state != USOCK_UNBOUND) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EINVAL;
    }

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
            if (match && *a == *b) {
                spin_unlock_irqrestore(&unix_lock, flags);
                return -EADDRINUSE;
            }
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

    spin_unlock_irqrestore(&unix_lock, flags);
    return 0;
}

int unix_sock_listen(int idx) {
    uint64_t flags;
    spin_lock_irqsave(&unix_lock, &flags);

    if (idx < 0 || idx >= MAX_UNIX_SOCKS) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EINVAL;
    }
    unix_sock_t *us = &unix_socks[idx];
    if (us->state != USOCK_BOUND) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EINVAL;
    }
    us->state = USOCK_LISTENING;

    spin_unlock_irqrestore(&unix_lock, flags);
    return 0;
}

int unix_sock_connect(const char *path) {
    uint64_t flags;
    spin_lock_irqsave(&unix_lock, &flags);

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
    if (listen_idx < 0) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -ECONNREFUSED;
    }

    unix_sock_t *listener = &unix_socks[listen_idx];

    /* Check backlog full */
    if (listener->backlog_count >= UNIX_SOCK_BACKLOG) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EAGAIN;
    }

    /* Allocate client socket (inline to avoid nested lock) */
    int client_idx = -1;
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
            us->peer = NULL;
            us->backlog_count = 0;
            for (int j = 0; j < UNIX_SOCK_BACKLOG; j++)
                us->backlog[j] = NULL;
            client_idx = i;
            break;
        }
    }
    if (client_idx < 0) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -ENOMEM;
    }

    /* Allocate server-side socket (inline to avoid nested lock) */
    int server_idx = -1;
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
            us->peer = NULL;
            us->backlog_count = 0;
            for (int j = 0; j < UNIX_SOCK_BACKLOG; j++)
                us->backlog[j] = NULL;
            server_idx = i;
            break;
        }
    }
    if (server_idx < 0) {
        unix_socks[client_idx].state = USOCK_FREE;
        spin_unlock_irqrestore(&unix_lock, flags);
        return -ENOMEM;
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
        if (listener->backlog[i] == NULL) {
            listener->backlog[i] = server;
            listener->backlog_count++;
            break;
        }
    }

    spin_unlock_irqrestore(&unix_lock, flags);
    return client_idx;
}

int unix_sock_accept(int listen_idx) {
    uint64_t flags;
    spin_lock_irqsave(&unix_lock, &flags);

    if (listen_idx < 0 || listen_idx >= MAX_UNIX_SOCKS) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EINVAL;
    }
    unix_sock_t *listener = &unix_socks[listen_idx];
    if (listener->state != USOCK_LISTENING) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EINVAL;
    }

    if (listener->backlog_count == 0) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EAGAIN;
    }

    /* Pop first connection from backlog */
    unix_sock_t *server = NULL;
    for (int i = 0; i < UNIX_SOCK_BACKLOG; i++) {
        if (listener->backlog[i] != NULL) {
            server = listener->backlog[i];
            listener->backlog[i] = NULL;
            listener->backlog_count--;
            break;
        }
    }

    if (!server) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return -EAGAIN;
    }

    /* Return the index of the server socket */
    int result = -EINVAL;
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (&unix_socks[i] == server) {
            result = i;
            break;
        }
    }

    spin_unlock_irqrestore(&unix_lock, flags);
    return result;
}

int unix_sock_send(unix_sock_t *us, const uint8_t *buf, uint32_t len, int nonblock) {
    if (!us || us->state != USOCK_CONNECTED || !us->peer)
        return -ENOTCONN;

    unix_sock_t *peer = us->peer;
    uint32_t total = 0;

    while (total < len) {
        if (peer->peer_closed)
            return total > 0 ? (int)total : -ENOTCONN;

        uint64_t flags;
        spin_lock_irqsave(&unix_lock, &flags);
        /* Copy as much as possible under lock */
        while (total < len && peer->count < UNIX_SOCK_BUF_SZ) {
            peer->buf[peer->wr_pos] = buf[total];
            peer->wr_pos = (peer->wr_pos + 1) % UNIX_SOCK_BUF_SZ;
            peer->count++;
            total++;
        }
        spin_unlock_irqrestore(&unix_lock, flags);

        if (total >= len) break;
        if (total > 0 && peer->count >= UNIX_SOCK_BUF_SZ) return (int)total;
        if (nonblock) return total > 0 ? (int)total : -EAGAIN;
        sched_yield();
    }
    return (int)total;
}

int unix_sock_recv(unix_sock_t *us, uint8_t *buf, uint32_t len, int nonblock) {
    if (!us || (us->state != USOCK_CONNECTED && us->state != USOCK_CLOSED))
        return -ENOTCONN;

    uint32_t total = 0;

    while (total < len) {
        uint64_t flags;
        spin_lock_irqsave(&unix_lock, &flags);
        /* Drain as much as possible under lock */
        while (total < len && us->count > 0) {
            buf[total] = us->buf[us->rd_pos];
            us->rd_pos = (us->rd_pos + 1) % UNIX_SOCK_BUF_SZ;
            us->count--;
            total++;
        }
        int empty = (us->count == 0);
        int peer_gone = us->peer_closed;
        spin_unlock_irqrestore(&unix_lock, flags);

        if (empty) {
            if (peer_gone) break;  /* EOF */
            if (total > 0) break;
            if (nonblock) return -EAGAIN;
            sched_yield();
        }
    }
    return (int)total;
}

void unix_sock_close(unix_sock_t *us) {
    if (!us) return;

    uint64_t flags;
    spin_lock_irqsave(&unix_lock, &flags);

    if (us->refs > 0)
        us->refs--;
    if (us->refs > 0) {
        spin_unlock_irqrestore(&unix_lock, flags);
        return;
    }

    /* Notify peer */
    if (us->peer) {
        us->peer->peer_closed = 1;
        us->peer->peer = NULL;
    }

    us->state = USOCK_FREE;
    us->peer = NULL;
    us->path[0] = '\0';

    spin_unlock_irqrestore(&unix_lock, flags);
}

unix_sock_t *unix_sock_get(int idx) {
    if (idx < 0 || idx >= MAX_UNIX_SOCKS)
        return NULL;
    return &unix_socks[idx];
}

int unix_sock_readable(const unix_sock_t *us) {
    if (!us) return 0;
    return us->count > 0 || us->peer_closed;
}

int unix_sock_writable(const unix_sock_t *us) {
    if (!us) return 0;
    if (!us->peer) return 0;
    return us->peer->count < UNIX_SOCK_BUF_SZ;
}

int unix_sock_has_backlog(const unix_sock_t *us) {
    if (!us) return 0;
    return us->backlog_count > 0;
}
