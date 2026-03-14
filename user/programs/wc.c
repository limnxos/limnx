/* wc — word, line, character count */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    int show_lines = 0, show_words = 0, show_chars = 0;
    const char *file = (void *)0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'l') show_lines = 1;
                else if (argv[i][j] == 'w') show_words = 1;
                else if (argv[i][j] == 'c') show_chars = 1;
            }
        } else {
            file = argv[i];
        }
    }

    /* Default: show all */
    if (!show_lines && !show_words && !show_chars) {
        show_lines = show_words = show_chars = 1;
    }

    if (!file) {
        printf("usage: wc [-lwc] file\n");
        return 1;
    }

    long fd = sys_open(file, 0);
    if (fd < 0) {
        printf("wc: cannot open '%s'\n", file);
        return 1;
    }

    long lines = 0, words = 0, chars = 0;
    int in_word = 0;
    char buf[4096];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            chars++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
                in_word = 0;
            } else {
                if (!in_word) words++;
                in_word = 1;
            }
        }
    }
    sys_close(fd);

    if (show_lines) printf("%7ld ", lines);
    if (show_words) printf("%7ld ", words);
    if (show_chars) printf("%7ld ", chars);
    printf("%s\n", file);
    return 0;
}
