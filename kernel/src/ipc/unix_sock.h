#ifndef LIMNX_UNIX_SOCK_H
#define LIMNX_UNIX_SOCK_H

#include <stdint.h>

#define MAX_UNIX_SOCKS    16
#define UNIX_SOCK_BUF_SZ  4096
#define UNIX_SOCK_PATH_MAX 108
#define UNIX_SOCK_BACKLOG  4

/* Socket states */
#define USOCK_FREE       0
#define USOCK_UNBOUND    1
#define USOCK_BOUND      2
#define USOCK_LISTENING  3
#define USOCK_CONNECTED  4
#define USOCK_CLOSED     5

typedef struct unix_sock {
    uint8_t  state;
    uint8_t  peer_closed;
    uint32_t refs;
    char     path[UNIX_SOCK_PATH_MAX];

    /* Ring buffer for incoming data */
    uint8_t  buf[UNIX_SOCK_BUF_SZ];
    uint32_t rd_pos;
    uint32_t wr_pos;
    uint32_t count;

    /* Peer pointer (for CONNECTED state) */
    struct unix_sock *peer;

    /* Backlog for LISTENING sockets */
    struct unix_sock *backlog[UNIX_SOCK_BACKLOG];
    uint32_t backlog_count;
} unix_sock_t;

/* Allocate a new socket, returns index or -1 */
int  unix_sock_alloc(void);

/* Bind socket to a path, returns 0 or -errno */
int  unix_sock_bind(int idx, const char *path);

/* Set socket to listening mode, returns 0 or -errno */
int  unix_sock_listen(int idx);

/* Connect to a listening socket by path, returns client socket index or -errno */
int  unix_sock_connect(const char *path);

/* Accept a connection from a listening socket, returns server-side socket index or -errno */
int  unix_sock_accept(int listen_idx);

/* Send data to peer, returns bytes written or -errno */
int  unix_sock_send(unix_sock_t *us, const uint8_t *buf, uint32_t len, int nonblock);

/* Receive data from own buffer, returns bytes read or -errno */
int  unix_sock_recv(unix_sock_t *us, uint8_t *buf, uint32_t len, int nonblock);

/* Close and decrement refs */
void unix_sock_close(unix_sock_t *us);

/* Get socket by index */
unix_sock_t *unix_sock_get(int idx);

/* Check readability (data available or peer_closed) */
int  unix_sock_readable(const unix_sock_t *us);

/* Check writability (peer has space) */
int  unix_sock_writable(const unix_sock_t *us);

/* Check if listening socket has pending connections */
int  unix_sock_has_backlog(const unix_sock_t *us);

#endif
