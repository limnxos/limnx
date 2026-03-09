#include "libc.h"

int errno = 0;

const char *strerror(int errnum) {
    switch (errnum) {
    case 0:            return "Success";
    case EPERM:        return "Operation not permitted";
    case ENOENT:       return "No such file or directory";
    case ESRCH:        return "No such process";
    case EINTR:        return "Interrupted system call";
    case EIO:          return "Input/output error";
    case EBADF:        return "Bad file descriptor";
    case EAGAIN:       return "Resource temporarily unavailable";
    case ENOMEM:       return "Cannot allocate memory";
    case EACCES:       return "Permission denied";
    case EFAULT:       return "Bad address";
    case EEXIST:       return "File exists";
    case EINVAL:       return "Invalid argument";
    case EMFILE:       return "Too many open files";
    case ENOSYS:       return "Function not implemented";
    case EADDRINUSE:   return "Address already in use";
    case ENOBUFS:      return "No buffer space available";
    case ENOTCONN:     return "Transport endpoint not connected";
    case ECONNREFUSED: return "Connection refused";
    default:           return "Unknown error";
    }
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++)
        d[i] = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return pa[i] - pb[i];
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

static char *_strtok_state;

char *strtok(char *str, const char *delim) {
    if (str) _strtok_state = str;
    if (!_strtok_state) return NULL;

    /* Skip leading delimiters */
    while (*_strtok_state) {
        const char *d = delim;
        int is_delim = 0;
        while (*d) { if (*_strtok_state == *d) { is_delim = 1; break; } d++; }
        if (!is_delim) break;
        _strtok_state++;
    }
    if (!*_strtok_state) { _strtok_state = NULL; return NULL; }

    char *token = _strtok_state;

    /* Find end of token */
    while (*_strtok_state) {
        const char *d = delim;
        while (*d) {
            if (*_strtok_state == *d) {
                *_strtok_state = '\0';
                _strtok_state++;
                return token;
            }
            d++;
        }
        _strtok_state++;
    }
    _strtok_state = NULL;
    return token;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* --- Number parsing --- */

unsigned long strtoul(const char *s, char **endptr, int base) {
    const char *orig = s;
    while (isspace(*s)) s++;

    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long result = 0;
    int any = 0;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned long)base + (unsigned long)digit;
        any = 1;
        s++;
    }

    if (endptr) *endptr = (char *)(any ? s : orig);
    return neg ? (unsigned long)(-(long)result) : result;
}

long strtol(const char *s, char **endptr, int base) {
    const char *orig = s;
    while (isspace(*s)) s++;

    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }

    /* Detect base if auto */
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long result = 0;
    int any = 0;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned long)base + (unsigned long)digit;
        any = 1;
        s++;
    }

    if (endptr) *endptr = (char *)(any ? s : orig);
    return neg ? -(long)result : (long)result;
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}

/* --- Character classification --- */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* --- Stdlib --- */

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

static void _swap(char *a, char *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        char tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb <= 1) return;

    char *arr = (char *)base;
    /* Partition around last element */
    char *pivot = arr + (nmemb - 1) * size;
    size_t i = 0;
    for (size_t j = 0; j < nmemb - 1; j++) {
        if (compar(arr + j * size, pivot) <= 0) {
            _swap(arr + i * size, arr + j * size, size);
            i++;
        }
    }
    _swap(arr + i * size, pivot, size);

    /* Recurse on partitions */
    qsort(arr, i, size, compar);
    if (i + 1 < nmemb)
        qsort(arr + (i + 1) * size, nmemb - i - 1, size, compar);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *arr = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, arr + mid * size);
        if (cmp == 0) return (void *)(arr + mid * size);
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}
