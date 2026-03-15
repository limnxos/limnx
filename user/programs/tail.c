/* tail — display last N lines of file */
#include "../libc/libc.h"

#define MAX_TAIL 4096

int main(int argc, char **argv) {
    int nlines = 10;
    const char *file = (void *)0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'n' && argv[i][2] == '\0') {
            if (i + 1 < argc) nlines = atoi(argv[++i]);
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            nlines = atoi(&argv[i][1]);
        } else {
            file = argv[i];
        }
    }
    if (!file) { printf("usage: tail [-n N] file\n"); return 1; }

    long fd = sys_open(file, 0);
    if (fd < 0) { printf("tail: %s: No such file\n", file); return 1; }

    /* Read entire file into buffer (limited to MAX_TAIL bytes) */
    char buf[MAX_TAIL];
    long total = 0, n;
    while ((n = sys_read(fd, buf + total, MAX_TAIL - total)) > 0) {
        total += n;
        if (total >= MAX_TAIL) break;
    }
    sys_close(fd);

    /* Count newlines from end */
    int lines = 0;
    long start = total;
    for (long i = total - 1; i >= 0; i--) {
        if (buf[i] == '\n') {
            lines++;
            if (lines > nlines) { start = i + 1; break; }
        }
    }
    if (lines <= nlines) start = 0;

    sys_write(buf + start, total - start);
    return 0;
}
