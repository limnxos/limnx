/* env/printenv — list environment variables */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    /* If arguments given, look up specific variable */
    if (argc > 1) {
        char val[256];
        for (int i = 1; i < argc; i++) {
            long ret = sys_getenv(argv[i], val, sizeof(val));
            if (ret >= 0)
                printf("%s=%s\n", argv[i], val);
        }
        return 0;
    }

    /* List all environment variables via SYS_ENVIRON */
    char buf[4096];
    long n = sys_environ(buf, sizeof(buf));
    if (n <= 0) {
        printf("(no environment)\n");
        return 0;
    }

    /* env_buf is NUL-separated "KEY=VALUE\0KEY=VALUE\0" */
    long pos = 0;
    while (pos < n) {
        if (buf[pos] == '\0') { pos++; continue; }
        printf("%s\n", &buf[pos]);
        while (pos < n && buf[pos] != '\0') pos++;
        pos++;  /* skip NUL */
    }
    return 0;
}
