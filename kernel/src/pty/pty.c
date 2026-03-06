#include "pty/pty.h"
#include "pty/termios.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "serial.h"

static pty_t ptys[MAX_PTYS];
static int console_pty_idx = -1;

void pty_init(void) {
    for (int i = 0; i < MAX_PTYS; i++)
        ptys[i].used = 0;
    serial_puts("[pty]  PTY subsystem initialized\n");
}

pty_t *pty_get(int idx) {
    if (idx < 0 || idx >= MAX_PTYS) return 0;
    if (!ptys[idx].used) return 0;
    return &ptys[idx];
}

int pty_alloc(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].used) {
            ptys[i].m2s_read_pos = 0;
            ptys[i].m2s_write_pos = 0;
            ptys[i].m2s_count = 0;
            ptys[i].s2m_read_pos = 0;
            ptys[i].s2m_write_pos = 0;
            ptys[i].s2m_count = 0;
            ptys[i].used = 1;
            ptys[i].master_closed = 0;
            ptys[i].slave_closed = 0;
            ptys[i].master_refs = 0;
            ptys[i].slave_refs = 0;
            ptys[i].flags = PTY_ECHO | PTY_ICANON;
            ptys[i].eof_flag = 0;
            ptys[i].fg_pgid = 0;
            ptys[i].winsize.ws_row = 25;
            ptys[i].winsize.ws_col = 80;
            return i;
        }
    }
    return -1;
}

/* Helper: push byte to s2m (slave→master) buffer */
static void s2m_push(pty_t *p, uint8_t ch) {
    if (p->s2m_count < PTY_BUF_SIZE) {
        p->s2m_buf[p->s2m_write_pos] = ch;
        p->s2m_write_pos = (p->s2m_write_pos + 1) % PTY_BUF_SIZE;
        p->s2m_count++;
    }
}

/* Master writes → m2s buffer. Echo goes to s2m. */
int64_t pty_master_write(int idx, const uint8_t *buf, uint32_t len) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return -1;

    pty_t *p = &ptys[idx];
    if (p->slave_closed)
        return -1;

    uint32_t written = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t ch = buf[i];

        /* Control characters in canonical mode */
        if (p->flags & PTY_ICANON) {
            /* ^C (0x03): send SIGINT to foreground group */
            if (ch == 0x03) {
                s2m_push(p, '^'); s2m_push(p, 'C'); s2m_push(p, '\n');
                if (p->fg_pgid)
                    process_kill_group(p->fg_pgid, 2); /* SIGINT */
                /* Discard m2s input buffer */
                p->m2s_read_pos = 0;
                p->m2s_write_pos = 0;
                p->m2s_count = 0;
                written++;
                continue;
            }
            /* ^D (0x04): signal EOF */
            if (ch == 0x04) {
                p->eof_flag = 1;
                written++;
                continue;
            }
            /* ^U (0x15): erase line */
            if (ch == 0x15) {
                while (p->m2s_count > 0) {
                    if (p->m2s_write_pos == 0)
                        p->m2s_write_pos = PTY_BUF_SIZE - 1;
                    else
                        p->m2s_write_pos--;
                    p->m2s_count--;
                    if (p->flags & PTY_ECHO) {
                        s2m_push(p, '\b');
                        s2m_push(p, ' ');
                        s2m_push(p, '\b');
                    }
                }
                written++;
                continue;
            }
            /* ^Z (0x1A): send SIGSTOP to foreground group */
            if (ch == 0x1A) {
                s2m_push(p, '^'); s2m_push(p, 'Z'); s2m_push(p, '\n');
                if (p->fg_pgid)
                    process_kill_group(p->fg_pgid, 19); /* SIGSTOP */
                p->m2s_read_pos = 0;
                p->m2s_write_pos = 0;
                p->m2s_count = 0;
                written++;
                continue;
            }
        }

        /* Backspace handling in canonical mode */
        if ((p->flags & PTY_ICANON) && (ch == 0x7F || ch == 0x08)) {
            if (p->m2s_count > 0) {
                /* Remove last byte from m2s */
                if (p->m2s_write_pos == 0)
                    p->m2s_write_pos = PTY_BUF_SIZE - 1;
                else
                    p->m2s_write_pos--;
                p->m2s_count--;

                /* Echo backspace-space-backspace to s2m */
                if (p->flags & PTY_ECHO) {
                    s2m_push(p, '\b');
                    s2m_push(p, ' ');
                    s2m_push(p, '\b');
                }
            }
            written++;
            continue;
        }

        /* Write to m2s buffer */
        if (p->m2s_count < PTY_BUF_SIZE) {
            p->m2s_buf[p->m2s_write_pos] = ch;
            p->m2s_write_pos = (p->m2s_write_pos + 1) % PTY_BUF_SIZE;
            p->m2s_count++;
            written++;
        } else {
            break;
        }

        /* Echo to s2m */
        if (p->flags & PTY_ECHO) {
            s2m_push(p, ch);
        }
    }

    return (int64_t)written;
}

/* Master reads from s2m buffer (output from slave) */
int64_t pty_master_read(int idx, uint8_t *buf, uint32_t len) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return -1;

    pty_t *p = &ptys[idx];
    uint32_t total = 0;

    while (total < len) {
        if (p->s2m_count > 0) {
            buf[total] = p->s2m_buf[p->s2m_read_pos];
            p->s2m_read_pos = (p->s2m_read_pos + 1) % PTY_BUF_SIZE;
            p->s2m_count--;
            total++;
        } else {
            break;
        }
    }

    return (int64_t)total;
}

/* Helper: check if m2s contains a newline */
static int m2s_has_newline(pty_t *p) {
    uint32_t pos = p->m2s_read_pos;
    for (uint32_t i = 0; i < p->m2s_count; i++) {
        if (p->m2s_buf[pos] == '\n')
            return 1;
        pos = (pos + 1) % PTY_BUF_SIZE;
    }
    return 0;
}

/* Slave reads from m2s buffer. In ICANON mode, waits for newline. */
int64_t pty_slave_read(int idx, uint8_t *buf, uint32_t len, int nonblock) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return -1;

    pty_t *p = &ptys[idx];

    if (p->flags & PTY_ICANON) {
        /* Check for ^D EOF on empty buffer */
        if (p->eof_flag && p->m2s_count == 0) {
            p->eof_flag = 0;
            return 0;
        }

        /* Wait for newline, EOF, or master close */
        int timeout = 50000;
        while (!m2s_has_newline(p) && !p->master_closed && !p->eof_flag) {
            if (nonblock)
                return 0;
            if (--timeout <= 0)
                return 0;
            sched_yield();
        }

        /* EOF with data: return what's in the buffer */
        if (p->eof_flag && p->m2s_count > 0) {
            p->eof_flag = 0;
        }

        /* Read up to and including newline */
        uint32_t total = 0;
        while (total < len && p->m2s_count > 0) {
            uint8_t ch = p->m2s_buf[p->m2s_read_pos];
            p->m2s_read_pos = (p->m2s_read_pos + 1) % PTY_BUF_SIZE;
            p->m2s_count--;
            buf[total++] = ch;
            if (ch == '\n')
                break;
        }
        return (int64_t)total;
    }

    /* Raw mode: read whatever is available */
    int timeout = 50000;
    while (p->m2s_count == 0 && !p->master_closed) {
        if (nonblock)
            return 0;
        if (--timeout <= 0)
            return 0;
        sched_yield();
    }

    uint32_t total = 0;
    while (total < len && p->m2s_count > 0) {
        buf[total] = p->m2s_buf[p->m2s_read_pos];
        p->m2s_read_pos = (p->m2s_read_pos + 1) % PTY_BUF_SIZE;
        p->m2s_count--;
        total++;
    }

    if (total == 0 && p->master_closed)
        return 0;  /* EOF */

    return (int64_t)total;
}

/* Slave writes to s2m buffer */
int64_t pty_slave_write(int idx, const uint8_t *buf, uint32_t len) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return -1;

    pty_t *p = &ptys[idx];
    if (p->master_closed)
        return -1;

    uint32_t written = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (p->s2m_count < PTY_BUF_SIZE) {
            p->s2m_buf[p->s2m_write_pos] = buf[i];
            p->s2m_write_pos = (p->s2m_write_pos + 1) % PTY_BUF_SIZE;
            p->s2m_count++;
            written++;
        } else {
            break;
        }
    }

    return (int64_t)written;
}

int pty_index(pty_t *p) {
    if (!p) return -1;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (&ptys[i] == p)
            return i;
    }
    return -1;
}

void pty_close_master(int idx) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return;

    pty_t *p = &ptys[idx];
    if (p->master_refs > 0)
        p->master_refs--;
    if (p->master_refs == 0)
        p->master_closed = 1;

    if (p->master_closed && p->slave_closed)
        p->used = 0;
}

void pty_close_slave(int idx) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return;

    pty_t *p = &ptys[idx];
    if (p->slave_refs > 0)
        p->slave_refs--;
    if (p->slave_refs == 0)
        p->slave_closed = 1;

    if (p->master_closed && p->slave_closed)
        p->used = 0;
}

/* --- ioctl --- */

int pty_ioctl(int idx, uint64_t cmd, uint64_t arg) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return -1;

    pty_t *p = &ptys[idx];

    switch (cmd) {
    case TCGETS: {
        termios_t *t = (termios_t *)arg;
        if (!t) return -1;
        t->c_iflag = 0;
        t->c_oflag = 0;
        t->c_cflag = 0;
        t->c_lflag = 0;
        if (p->flags & PTY_ECHO)   t->c_lflag |= TERMIOS_ECHO;
        if (p->flags & PTY_ICANON) t->c_lflag |= TERMIOS_ICANON;
        return 0;
    }
    case TCSETS: {
        const termios_t *t = (const termios_t *)arg;
        if (!t) return -1;
        p->flags = 0;
        if (t->c_lflag & TERMIOS_ECHO)   p->flags |= PTY_ECHO;
        if (t->c_lflag & TERMIOS_ICANON) p->flags |= PTY_ICANON;
        return 0;
    }
    case TIOCGWINSZ: {
        winsize_t *ws = (winsize_t *)arg;
        if (!ws) return -1;
        ws->ws_row = p->winsize.ws_row;
        ws->ws_col = p->winsize.ws_col;
        return 0;
    }
    case TIOCSWINSZ: {
        const winsize_t *ws = (const winsize_t *)arg;
        if (!ws) return -1;
        p->winsize.ws_row = ws->ws_row;
        p->winsize.ws_col = ws->ws_col;
        return 0;
    }
    case TIOCSPGRP: {
        p->fg_pgid = arg;
        return 0;
    }
    case TIOCGPGRP: {
        return (int)p->fg_pgid;
    }
    default:
        return -1;
    }
}

/* --- Console PTY --- */

int pty_create_console(void) {
    int idx = pty_alloc();
    if (idx < 0) return -1;
    console_pty_idx = idx;
    ptys[idx].master_refs = 1;  /* kernel holds master */
    serial_printf("[pty]  Console PTY created (PTY %d)\n", idx);
    return idx;
}

int pty_get_console(void) {
    return console_pty_idx;
}

/* Called from keyboard ISR to feed a character into the console PTY master */
void pty_console_input(char ch) {
    if (console_pty_idx < 0) return;
    uint8_t c = (uint8_t)ch;
    pty_master_write(console_pty_idx, &c, 1);
}

/* Poll helpers */
int pty_readable(int idx, int is_master) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return 0;
    pty_t *p = &ptys[idx];
    if (is_master) {
        return p->s2m_count > 0;
    } else {
        /* Slave: canonical needs newline or EOF or master close */
        if (p->flags & PTY_ICANON)
            return m2s_has_newline(p) || p->master_closed || p->eof_flag;
        return p->m2s_count > 0 || p->master_closed;
    }
}

int pty_writable(int idx, int is_master) {
    if (idx < 0 || idx >= MAX_PTYS || !ptys[idx].used)
        return 0;
    pty_t *p = &ptys[idx];
    if (is_master)
        return p->m2s_count < PTY_BUF_SIZE;
    else
        return p->s2m_count < PTY_BUF_SIZE;
}
