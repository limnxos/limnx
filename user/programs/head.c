/* head — display first N lines of file */
#include "../libc/libc.h"

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
    if (!file) { printf("usage: head [-n N] file\n"); return 1; }

    long fd = sys_open(file, 0);
    if (fd < 0) { printf("head: %s: No such file\n", file); return 1; }

    int lines = 0;
    char buf[512];
    long n;
    while (lines < nlines && (n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n && lines < nlines; i++) {
            sys_write(&buf[i], 1);
            if (buf[i] == '\n') lines++;
        }
    }
    sys_close(fd);
    return 0;
}
