/* cp — copy files */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: cp src dst\n");
        return 1;
    }
    long src = sys_open(argv[1], 0);
    if (src < 0) {
        printf("cp: cannot open '%s'\n", argv[1]);
        return 1;
    }
    /* Create destination (O_CREAT | O_RDWR) */
    long dst = sys_open(argv[2], 0x100 | 2);
    if (dst < 0) {
        printf("cp: cannot create '%s'\n", argv[2]);
        sys_close(src);
        return 1;
    }
    /* Truncate destination to 0 */
    sys_truncate(argv[2], 0);

    char buf[4096];
    long n;
    while ((n = sys_read(src, buf, sizeof(buf))) > 0) {
        sys_fwrite(dst, buf, n);
    }
    sys_close(src);
    sys_close(dst);
    return 0;
}
