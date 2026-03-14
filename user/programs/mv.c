/* mv — move/rename files */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: mv old new\n");
        return 1;
    }
    if (sys_rename(argv[1], argv[2]) < 0) {
        printf("mv: cannot rename '%s' to '%s'\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
