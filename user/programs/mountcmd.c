/* mount — mount filesystem */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: mount -t fstype mountpoint\n");
        printf("  e.g., mount -t tmpfs /mnt\n");
        return 1;
    }
    const char *fstype = (void *)0;
    const char *path = (void *)0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            fstype = argv[++i];
        } else {
            path = argv[i];
        }
    }
    if (!fstype || !path) {
        printf("mount: missing fstype or mountpoint\n");
        return 1;
    }
    if (sys_mount(path, fstype) < 0) {
        printf("mount: failed to mount %s at %s\n", fstype, path);
        return 1;
    }
    printf("mounted %s at %s\n", fstype, path);
    return 0;
}
