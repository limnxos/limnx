/* grep — search for pattern in files */
#include "../libc/libc.h"

static int match_nocase(const char *hay, const char *needle) {
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j]) {
            char h = hay[i + j], n = needle[j];
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) break;
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int ignore_case = 0;
    int show_linenum = 0;
    const char *pattern = (void *)0;
    const char *file = (void *)0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'i') ignore_case = 1;
                else if (argv[i][j] == 'n') show_linenum = 1;
            }
        } else if (!pattern) {
            pattern = argv[i];
        } else {
            file = argv[i];
        }
    }
    if (!pattern || !file) {
        printf("usage: grep [-in] pattern file\n");
        return 1;
    }

    long fd = sys_open(file, 0);
    if (fd < 0) { printf("grep: %s: No such file\n", file); return 1; }

    char buf[4096];
    long total = 0, n;
    while ((n = sys_read(fd, buf + total, sizeof(buf) - total - 1)) > 0) {
        total += n;
        if (total >= (long)sizeof(buf) - 1) break;
    }
    sys_close(fd);
    buf[total] = '\0';

    int found = 0, linenum = 1;
    char *lstart = buf;
    for (long i = 0; i <= total; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            int matched = ignore_case ? match_nocase(lstart, pattern)
                                       : (strstr(lstart, pattern) != (void *)0);
            if (matched) {
                if (show_linenum) printf("%d:", linenum);
                printf("%s\n", lstart);
                found = 1;
            }
            buf[i] = saved;
            lstart = &buf[i + 1];
            linenum++;
        }
    }

    return found ? 0 : 1;
}
