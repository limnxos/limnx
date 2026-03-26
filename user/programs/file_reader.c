/*
 * file_reader.c — Tool: read a file and write contents to stdout
 *
 * stdin: file path (one line)
 * stdout: file contents
 * exit 0 on success, 1 on error
 *
 * Used by tool_dispatch for sandboxed file reading.
 * Should be launched with CAP_FS_READ only.
 */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    char path[256];
    int len = 0;

    /* Read path from argv or stdin */
    if (argc >= 2) {
        const char *s = argv[1];
        while (*s && len < 255) path[len++] = *s++;
    } else {
        /* Read from stdin */
        long n = sys_read(0, path, 255);
        if (n <= 0) {
            sys_write("[file_reader] no path\n", 22);
            return 1;
        }
        len = (int)n;
        /* Strip trailing newline */
        while (len > 0 && (path[len-1] == '\n' || path[len-1] == '\r'))
            len--;
    }
    path[len] = '\0';

    if (len == 0) {
        sys_write("[file_reader] empty path\n", 25);
        return 1;
    }

    long fd = sys_open(path, 0);
    if (fd < 0) {
        printf("[file_reader] cannot open: %s\n", path);
        return 1;
    }

    char buf[512];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        sys_fwrite(1, buf, (unsigned long)n);
    }

    sys_close(fd);
    return 0;
}
