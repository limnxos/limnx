/* umount — unmount filesystem */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: umount mountpoint\n");
        return 1;
    }
    if (sys_umount(argv[1]) < 0) {
        printf("umount: failed to unmount %s\n", argv[1]);
        return 1;
    }
    printf("unmounted %s\n", argv[1]);
    return 0;
}
