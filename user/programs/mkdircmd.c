/* mkdir — create directories */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: mkdir dir [dir...]\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_mkdir(argv[i]) < 0) {
            printf("mkdir: cannot create '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
