#ifndef LIMNX_PTY_H
#define LIMNX_PTY_H

#include <stdint.h>
#include "pty/termios.h"

#define MAX_PTYS       8
#define PTY_BUF_SIZE   4096

#define PTY_ECHO       (1 << 0)   /* echo input back to output */
#define PTY_ICANON     (1 << 1)   /* canonical mode (line-buffered) */

typedef struct pty {
    /* Master→Slave buffer: master writes here, slave reads */
    uint8_t  m2s_buf[PTY_BUF_SIZE];
    uint32_t m2s_read_pos;
    uint32_t m2s_write_pos;
    uint32_t m2s_count;

    /* Slave→Master buffer: slave writes here, master reads */
    uint8_t  s2m_buf[PTY_BUF_SIZE];
    uint32_t s2m_read_pos;
    uint32_t s2m_write_pos;
    uint32_t s2m_count;

    uint8_t  used;
    uint8_t  master_closed;
    uint8_t  slave_closed;
    uint8_t  eof_flag;    /* ^D received, slave read returns 0 */
    uint32_t master_refs;
    uint32_t slave_refs;
    uint32_t flags;   /* PTY_ECHO | PTY_ICANON */
    uint64_t fg_pgid; /* foreground process group */
    winsize_t winsize; /* terminal window size */
} pty_t;

/* ioctl commands for foreground process group */
#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x5411

void    pty_init(void);
int     pty_alloc(void);
int64_t pty_master_write(int idx, const uint8_t *buf, uint32_t len);
int64_t pty_master_read(int idx, uint8_t *buf, uint32_t len);
int64_t pty_slave_read(int idx, uint8_t *buf, uint32_t len, int nonblock);
int64_t pty_slave_write(int idx, const uint8_t *buf, uint32_t len);
void    pty_close_master(int idx);
void    pty_close_slave(int idx);
pty_t  *pty_get(int idx);
int     pty_index(pty_t *p);
int     pty_ioctl(int idx, uint64_t cmd, uint64_t arg);
int     pty_create_console(void);
int     pty_get_console(void);
void    pty_console_input(char ch);
int     pty_readable(int idx, int is_master);
int     pty_writable(int idx, int is_master);

#endif
