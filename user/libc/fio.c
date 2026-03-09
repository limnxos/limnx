#include "libc.h"

/* Variadic argument support */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

#define STDIO_BUFSZ 256
#define STDIO_MAX   16

struct _FILE {
    int     fd;
    int     mode;       /* 0=read, 1=write, 2=rw */
    char    buf[STDIO_BUFSZ];
    int     buf_pos;
    int     buf_len;    /* valid bytes in buffer (read mode) */
    int     eof;
    int     error;
    int     used;
};

static FILE _file_slots[STDIO_MAX];

/* Pre-initialized stdin/stdout/stderr */
static int _stdio_inited = 0;

static void _stdio_init(void) {
    if (_stdio_inited) return;
    _stdio_inited = 1;

    /* stdin = fd 0 */
    _file_slots[0].fd = 0;
    _file_slots[0].mode = 0;
    _file_slots[0].buf_pos = 0;
    _file_slots[0].buf_len = 0;
    _file_slots[0].eof = 0;
    _file_slots[0].error = 0;
    _file_slots[0].used = 1;

    /* stdout = fd 1 */
    _file_slots[1].fd = 1;
    _file_slots[1].mode = 1;
    _file_slots[1].buf_pos = 0;
    _file_slots[1].buf_len = 0;
    _file_slots[1].eof = 0;
    _file_slots[1].error = 0;
    _file_slots[1].used = 1;

    /* stderr = fd 2 */
    _file_slots[2].fd = 2;
    _file_slots[2].mode = 1;
    _file_slots[2].buf_pos = 0;
    _file_slots[2].buf_len = 0;
    _file_slots[2].eof = 0;
    _file_slots[2].error = 0;
    _file_slots[2].used = 1;
}

FILE *stdin  = &_file_slots[0];
FILE *stdout = &_file_slots[1];
FILE *stderr = &_file_slots[2];

static FILE *alloc_file(void) {
    _stdio_init();
    for (int i = 3; i < STDIO_MAX; i++) {
        if (!_file_slots[i].used) {
            _file_slots[i].used = 1;
            _file_slots[i].buf_pos = 0;
            _file_slots[i].buf_len = 0;
            _file_slots[i].eof = 0;
            _file_slots[i].error = 0;
            return &_file_slots[i];
        }
    }
    return NULL;
}

FILE *fopen(const char *path, const char *mode) {
    _stdio_init();

    int flags = 0;
    int fmode = 0;

    if (mode[0] == 'r') {
        if (mode[1] == '+') {
            flags = O_RDWR;
            fmode = 2;
        } else {
            flags = O_RDONLY;
            fmode = 0;
        }
    } else if (mode[0] == 'w') {
        if (mode[1] == '+') {
            flags = O_RDWR | O_CREAT | O_TRUNC;
            fmode = 2;
        } else {
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            fmode = 1;
        }
    } else if (mode[0] == 'a') {
        if (mode[1] == '+') {
            flags = O_RDWR | O_CREAT | O_APPEND;
            fmode = 2;
        } else {
            flags = O_WRONLY | O_CREAT | O_APPEND;
            fmode = 1;
        }
    } else {
        return NULL;
    }

    /* Ensure file exists for write/append modes */
    if (flags & O_CREAT)
        sys_create(path);

    long fd = sys_open(path, flags);
    if (fd < 0)
        return NULL;

    /* Truncate if needed */
    if (flags & O_TRUNC)
        sys_truncate(path, 0);

    FILE *fp = alloc_file();
    if (!fp) {
        sys_close(fd);
        return NULL;
    }

    fp->fd = (int)fd;
    fp->mode = fmode;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp || !fp->used)
        return -1;

    fflush(fp);
    sys_close(fp->fd);
    fp->used = 0;
    return 0;
}

int fflush(FILE *fp) {
    if (!fp) return -1;
    if (fp->mode >= 1 && fp->buf_pos > 0) {
        long r = sys_fwrite(fp->fd, fp->buf, fp->buf_pos);
        if (r < 0) fp->error = 1;
        fp->buf_pos = 0;
    }
    return fp->error ? -1 : 0;
}

int fputc(int c, FILE *fp) {
    if (!fp) return -1;
    _stdio_init();

    fp->buf[fp->buf_pos++] = (char)c;
    if (fp->buf_pos >= STDIO_BUFSZ) {
        long r = sys_fwrite(fp->fd, fp->buf, fp->buf_pos);
        if (r < 0) { fp->error = 1; return -1; }
        fp->buf_pos = 0;
    }
    return (unsigned char)c;
}

int fgetc(FILE *fp) {
    if (!fp) return -1;
    _stdio_init();
    if (fp->eof) return -1;

    if (fp->buf_pos >= fp->buf_len) {
        /* Refill buffer */
        long r = sys_read(fp->fd, fp->buf, STDIO_BUFSZ);
        if (r <= 0) {
            fp->eof = 1;
            return -1;
        }
        fp->buf_len = (int)r;
        fp->buf_pos = 0;
    }
    return (unsigned char)fp->buf[fp->buf_pos++];
}

size_t fread(void *buf, size_t size, size_t count, FILE *fp) {
    if (!fp || size == 0 || count == 0) return 0;
    _stdio_init();

    size_t total = size * count;
    size_t done = 0;
    char *dst = (char *)buf;

    while (done < total) {
        int c = fgetc(fp);
        if (c < 0) break;
        dst[done++] = (char)c;
    }

    return done / size;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *fp) {
    if (!fp || size == 0 || count == 0) return 0;
    _stdio_init();

    size_t total = size * count;
    const char *src = (const char *)buf;

    for (size_t i = 0; i < total; i++) {
        if (fputc(src[i], fp) < 0)
            return i / size;
    }
    return count;
}

char *fgets(char *buf, int size, FILE *fp) {
    if (!fp || !buf || size <= 0) return NULL;
    _stdio_init();

    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c < 0) {
            if (i == 0) return NULL;  /* EOF with no data */
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

int fputs(const char *s, FILE *fp) {
    if (!fp || !s) return -1;
    while (*s) {
        if (fputc(*s++, fp) < 0)
            return -1;
    }
    return 0;
}

int feof(FILE *fp) {
    return fp ? fp->eof : 1;
}

int ferror(FILE *fp) {
    return fp ? fp->error : 1;
}

int fseek(FILE *fp, long offset, int whence) {
    if (!fp) return -1;
    fflush(fp);
    /* Discard read buffer */
    fp->buf_pos = 0;
    fp->buf_len = 0;
    fp->eof = 0;
    long r = sys_seek(fp->fd, offset, whence);
    return (r < 0) ? -1 : 0;
}

int fileno(FILE *fp) {
    return fp ? fp->fd : -1;
}

/* --- fprintf: formatted output to FILE --- */

/* Render unsigned long to tmp, return length */
static int _render_uint(char *tmp, unsigned long val) {
    if (val == 0) { tmp[0] = '0'; return 1; }
    char rev[20];
    int i = 0;
    while (val > 0) { rev[i++] = '0' + (int)(val % 10); val /= 10; }
    for (int j = 0; j < i; j++) tmp[j] = rev[i - 1 - j];
    return i;
}

static int _render_hex(char *tmp, unsigned long val) {
    const char *hex = "0123456789abcdef";
    if (val == 0) { tmp[0] = '0'; return 1; }
    char rev[16];
    int i = 0;
    while (val > 0) { rev[i++] = hex[val & 0xF]; val >>= 4; }
    for (int j = 0; j < i; j++) tmp[j] = rev[i - 1 - j];
    return i;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    if (!fp) return -1;
    _stdio_init();

    va_list ap;
    va_start(ap, fmt);

    /* Format into a local buffer, then fwrite */
    char out[1024];
    int pos = 0;

    while (*fmt && pos < 1020) {
        if (*fmt != '%') {
            out[pos++] = *fmt++;
            continue;
        }
        fmt++;

        /* Parse flags */
        int left_align = 0, zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        if (left_align) zero_pad = 0;

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        char tmp[32];
        int len;
        char pad = zero_pad ? '0' : ' ';

        switch (*fmt) {
        case 'd': {
            long val = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long uval;
            int neg = 0;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); }
            else uval = (unsigned long)val;
            len = _render_uint(tmp + (neg ? 1 : 0), uval);
            if (neg) { tmp[0] = '-'; len++; }
            /* Pad */
            for (int i = len; i < width && pos < 1020; i++) out[pos++] = pad;
            for (int i = 0; i < len && pos < 1020; i++) out[pos++] = tmp[i];
            break;
        }
        case 'u': {
            unsigned long val = is_long ? va_arg(ap, unsigned long)
                                        : (unsigned long)va_arg(ap, unsigned int);
            len = _render_uint(tmp, val);
            for (int i = len; i < width && pos < 1020; i++) out[pos++] = pad;
            for (int i = 0; i < len && pos < 1020; i++) out[pos++] = tmp[i];
            break;
        }
        case 'x': {
            unsigned long val = is_long ? va_arg(ap, unsigned long)
                                        : (unsigned long)va_arg(ap, unsigned int);
            len = _render_hex(tmp, val);
            for (int i = len; i < width && pos < 1020; i++) out[pos++] = pad;
            for (int i = 0; i < len && pos < 1020; i++) out[pos++] = tmp[i];
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            if (!left_align)
                for (int i = slen; i < width && pos < 1020; i++) out[pos++] = ' ';
            for (int i = 0; i < slen && pos < 1020; i++) out[pos++] = s[i];
            if (left_align)
                for (int i = slen; i < width && pos < 1020; i++) out[pos++] = ' ';
            break;
        }
        case 'c':
            out[pos++] = (char)va_arg(ap, int);
            break;
        case '%':
            out[pos++] = '%';
            break;
        default:
            out[pos++] = '%';
            if (is_long && pos < 1020) out[pos++] = 'l';
            if (pos < 1020) out[pos++] = *fmt;
            break;
        }
        fmt++;
    }

    /* Write the formatted output via fwrite */
    if (pos > 0)
        fwrite(out, 1, pos, fp);
    fflush(fp);

    va_end(ap);
    return pos;
}
