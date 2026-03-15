/* chown — change file owner/group */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: chown uid:gid file\n");
        printf("  e.g., chown 0:0 /etc/passwd\n");
        return 1;
    }
    /* Parse uid:gid */
    int uid = -1, gid = -1;
    const char *s = argv[1];
    uid = atoi(s);
    while (*s && *s != ':') s++;
    if (*s == ':') gid = atoi(s + 1);
    else gid = uid;

    if (sys_chown(argv[2], uid, gid) < 0) {
        printf("chown: cannot change owner of '%s'\n", argv[2]);
        return 1;
    }
    return 0;
}
