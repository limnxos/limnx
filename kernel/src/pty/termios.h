#ifndef LIMNX_TERMIOS_H
#define LIMNX_TERMIOS_H

#include <stdint.h>

/* Linux-compatible termios struct (matches musl/glibc layout) */
#define NCCS 32

typedef struct {
    uint32_t c_iflag;       /* input flags */
    uint32_t c_oflag;       /* output flags */
    uint32_t c_cflag;       /* control flags */
    uint32_t c_lflag;       /* local flags: ECHO, ICANON, ISIG, ... */
    uint8_t  c_line;        /* line discipline */
    uint8_t  c_cc[NCCS];    /* control characters */
    uint32_t c_ispeed;      /* input baud rate */
    uint32_t c_ospeed;      /* output baud rate */
} termios_t;

typedef struct {
    uint16_t ws_row;    /* rows (default 24) */
    uint16_t ws_col;    /* columns (default 80) */
    uint16_t ws_xpixel; /* unused */
    uint16_t ws_ypixel; /* unused */
} winsize_t;

/* c_lflag bits (Linux values) */
#define TERMIOS_ISIG    0x0001
#define TERMIOS_ICANON  0x0002
#define TERMIOS_ECHO    0x0008
#define TERMIOS_ECHOE   0x0010
#define TERMIOS_ECHOK   0x0020
#define TERMIOS_ECHONL  0x0040
#define TERMIOS_IEXTEN  0x8000

/* c_iflag bits */
#define TERMIOS_ICRNL   0x0100
#define TERMIOS_IXON    0x0400

/* c_oflag bits */
#define TERMIOS_OPOST   0x0001
#define TERMIOS_ONLCR   0x0004

/* Control characters */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSUSP    10

/* ioctl commands for terminals */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x540F

#endif
