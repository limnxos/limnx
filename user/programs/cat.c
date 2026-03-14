/* cat — concatenate and display files */
#include "../libc/libc.h"

static void cat_file(const char *path, int number_lines) {
    long fd = sys_open(path, 0);
    if (fd < 0) {
        printf("cat: %s: No such file\n", path);
        return;
    }
    char buf[512];
    int line = 1;
    int at_line_start = 1;
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        if (!number_lines) {
            sys_write(buf, n);
        } else {
            for (long i = 0; i < n; i++) {
                if (at_line_start) {
                    printf("%6d\t", line++);
                    at_line_start = 0;
                }
                sys_write(&buf[i], 1);
                if (buf[i] == '\n') at_line_start = 1;
            }
        }
    }
    sys_close(fd);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cat [-n] file [file...]\n");
        return 1;
    }
    int number_lines = 0;
    int start = 1;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n' && argv[1][2] == '\0') {
        number_lines = 1;
        start = 2;
    }
    for (int i = start; i < argc; i++)
        cat_file(argv[i], number_lines);
    return 0;
}
