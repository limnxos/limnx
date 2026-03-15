/* whoami — print current user name */
#include "../libc/libc.h"

int main(void) {
    char user[64];
    long ret = sys_getenv("USER", user, sizeof(user));
    if (ret >= 0) {
        printf("%s\n", user);
    } else {
        /* Fallback: look up uid in /etc/passwd */
        long uid = sys_getuid();
        long fd = sys_open("/etc/passwd", 0);
        if (fd < 0) { printf("uid %ld\n", uid); return 0; }
        char buf[2048];
        long n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n <= 0) { printf("uid %ld\n", uid); return 0; }
        buf[n] = '\0';

        char *lstart = buf;
        for (long i = 0; i <= n; i++) {
            if (buf[i] == '\n' || buf[i] == '\0') {
                buf[i] = '\0';
                /* Parse uid field (3rd field) */
                int field = 0, colon_count = 0;
                char name[64] = {0};
                int ni = 0;
                for (int j = 0; lstart[j]; j++) {
                    if (lstart[j] == ':') {
                        colon_count++;
                        if (colon_count == 1) { name[ni] = '\0'; }
                        if (colon_count == 2) {
                            /* Next field is uid */
                            int u = atoi(&lstart[j+1]);
                            if (u == (int)uid) {
                                printf("%s\n", name);
                                return 0;
                            }
                            break;
                        }
                    } else if (colon_count == 0 && ni < 63) {
                        name[ni++] = lstart[j];
                    }
                }
                lstart = &buf[i + 1];
            }
        }
        printf("uid %ld\n", uid);
    }
    return 0;
}
