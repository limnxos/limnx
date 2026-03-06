#ifndef LIMNX_TERMIOS_H
#define LIMNX_TERMIOS_H

#include <stdint.h>

/* Simplified termios (POSIX-inspired, minimal subset) */
typedef struct {
    uint32_t c_iflag;   /* input flags (unused for now) */
    uint32_t c_oflag;   /* output flags (unused for now) */
    uint32_t c_cflag;   /* control flags (unused for now) */
    uint32_t c_lflag;   /* local flags: ECHO, ICANON */
} termios_t;

typedef struct {
    uint16_t ws_row;    /* rows (default 25) */
    uint16_t ws_col;    /* columns (default 80) */
} winsize_t;

/* c_lflag bits */
#define TERMIOS_ECHO   (1 << 0)
#define TERMIOS_ICANON (1 << 1)

/* ioctl commands for terminals */
#define TCGETS     0x5401   /* get terminal attributes */
#define TCSETS     0x5402   /* set terminal attributes */
#define TIOCGWINSZ 0x5413   /* get window size */
#define TIOCSWINSZ 0x5414   /* set window size */

#endif
