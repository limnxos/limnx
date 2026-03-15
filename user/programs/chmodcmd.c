/* chmod — change file mode */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: chmod mode file\n");
        printf("  mode: octal (e.g., 755, 644)\n");
        return 1;
    }
    /* Parse octal mode */
    unsigned long mode = strtoul(argv[1], (void *)0, 8);
    if (mode > 07777) {
        printf("chmod: invalid mode '%s'\n", argv[1]);
        return 1;
    }
    if (sys_chmod(argv[2], mode) < 0) {
        printf("chmod: cannot change mode of '%s'\n", argv[2]);
        return 1;
    }
    return 0;
}
