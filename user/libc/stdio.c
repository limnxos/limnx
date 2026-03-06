#include "libc.h"

/* Variadic argument support — minimal implementation for freestanding */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

int puts(const char *s) {
    size_t len = strlen(s);
    sys_write(s, len);
    sys_write("\n", 1);
    return (int)len + 1;
}

static void flush_buf(char *buf, int *pos) {
    if (*pos > 0) {
        sys_write(buf, (unsigned long)*pos);
        *pos = 0;
    }
}

static void emit(char *buf, int *pos, char c) {
    buf[*pos] = c;
    (*pos)++;
    if (*pos >= 1024)
        flush_buf(buf, pos);
}

static void emit_str(char *buf, int *pos, const char *s) {
    while (*s)
        emit(buf, pos, *s++);
}

/* Render unsigned long to tmp buffer, return length */
static int render_uint(char *tmp, unsigned long val) {
    int i = 0;
    if (val == 0) {
        tmp[0] = '0';
        return 1;
    }
    char rev[20];
    while (val > 0) {
        rev[i++] = '0' + (int)(val % 10);
        val /= 10;
    }
    for (int j = 0; j < i; j++)
        tmp[j] = rev[i - 1 - j];
    return i;
}

/* Render unsigned long as hex to tmp buffer, return length */
static int render_hex(char *tmp, unsigned long val) {
    const char *hex = "0123456789abcdef";
    int i = 0;
    if (val == 0) {
        tmp[0] = '0';
        return 1;
    }
    char rev[16];
    while (val > 0) {
        rev[i++] = hex[val & 0xF];
        val >>= 4;
    }
    for (int j = 0; j < i; j++)
        tmp[j] = rev[i - 1 - j];
    return i;
}

/* Emit a rendered string with padding (width, left-align, zero-pad) */
static void emit_padded(char *buf, int *pos, const char *rendered, int len,
                         int width, int left_align, char pad_char) {
    if (left_align) {
        for (int i = 0; i < len; i++)
            emit(buf, pos, rendered[i]);
        for (int i = len; i < width; i++)
            emit(buf, pos, ' ');
    } else {
        for (int i = len; i < width; i++)
            emit(buf, pos, pad_char);
        for (int i = 0; i < len; i++)
            emit(buf, pos, rendered[i]);
    }
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char buf[1024];
    int pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            emit(buf, &pos, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse flags */
        int left_align = 0;
        int zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        /* Left-align overrides zero-pad */
        if (left_align) zero_pad = 0;

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse precision */
        int has_precision = 0;
        int precision = 0;
        if (*fmt == '.') {
            fmt++;
            has_precision = 1;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        char tmp[32];
        int len;
        char pad_char = zero_pad ? '0' : ' ';

        switch (*fmt) {
        case 'd': {
            long val = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            int neg = 0;
            unsigned long uval;
            if (val < 0) {
                neg = 1;
                uval = (unsigned long)(-val);
            } else {
                uval = (unsigned long)val;
            }
            /* Render digits */
            char digits[20];
            int dlen = render_uint(digits, uval);
            /* Build full string: sign + digits */
            int tlen = 0;
            if (neg) {
                if (zero_pad && !left_align) {
                    /* Sign before zero padding */
                    emit(buf, &pos, '-');
                    emit_padded(buf, &pos, digits, dlen, width - 1, 0, '0');
                } else {
                    tmp[tlen++] = '-';
                    for (int i = 0; i < dlen; i++) tmp[tlen++] = digits[i];
                    emit_padded(buf, &pos, tmp, tlen, width, left_align, ' ');
                }
            } else {
                emit_padded(buf, &pos, digits, dlen, width, left_align, pad_char);
            }
            break;
        }
        case 'u': {
            unsigned long val = is_long ? va_arg(ap, unsigned long)
                                        : (unsigned long)va_arg(ap, unsigned int);
            len = render_uint(tmp, val);
            emit_padded(buf, &pos, tmp, len, width, left_align, pad_char);
            break;
        }
        case 'x': {
            unsigned long val = is_long ? va_arg(ap, unsigned long)
                                        : (unsigned long)va_arg(ap, unsigned int);
            len = render_hex(tmp, val);
            emit_padded(buf, &pos, tmp, len, width, left_align, pad_char);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            emit_padded(buf, &pos, s, slen, width, left_align, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit(buf, &pos, c);
            break;
        }
        case 'p': {
            unsigned long val = (unsigned long)va_arg(ap, void *);
            emit_str(buf, &pos, "0x");
            len = render_hex(tmp, val);
            emit_padded(buf, &pos, tmp, len, width, left_align, pad_char);
            break;
        }
        case '%':
            emit(buf, &pos, '%');
            break;
        case 'f': {
            double val = va_arg(ap, double);
            int neg = 0;
            if (val < 0) {
                neg = 1;
                val = -val;
            }
            int fprec = has_precision ? precision : 4;
            unsigned long integer = (unsigned long)val;
            double frac = val - (double)integer;

            /* Build into tmp: [-]integer.fraction */
            int tlen = 0;
            if (neg) tmp[tlen++] = '-';

            /* Integer part */
            char ipart[20];
            int ilen = render_uint(ipart, integer);
            for (int i = 0; i < ilen; i++) tmp[tlen++] = ipart[i];

            /* Decimal part */
            if (fprec > 0) {
                tmp[tlen++] = '.';
                for (int i = 0; i < fprec && tlen < 30; i++) {
                    frac *= 10.0;
                    int digit = (int)frac;
                    if (digit > 9) digit = 9;
                    tmp[tlen++] = '0' + digit;
                    frac -= digit;
                }
            }
            emit_padded(buf, &pos, tmp, tlen, width, left_align, ' ');
            break;
        }
        default:
            emit(buf, &pos, '%');
            if (is_long)
                emit(buf, &pos, 'l');
            emit(buf, &pos, *fmt);
            break;
        }
        fmt++;
    }

    flush_buf(buf, &pos);
    va_end(ap);
    return pos;
}
