/* ps — process listing via /proc */
#include "../libc/libc.h"

typedef struct {
    char name[256];
    unsigned char type;
    unsigned char pad[7];
    unsigned long size;
} ps_dirent_t;

/* Check if string is all digits */
static int is_numeric(const char *s) {
    if (!s[0]) return 0;
    for (int i = 0; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Extract value after "Key:\t" from status text */
static int extract_field(const char *status, const char *key, char *out, int outsize) {
    int klen = 0;
    while (key[klen]) klen++;

    for (int i = 0; status[i]; i++) {
        int match = 1;
        for (int j = 0; j < klen; j++) {
            if (status[i + j] != key[j]) { match = 0; break; }
        }
        if (match && status[i + klen] == ':' && status[i + klen + 1] == '\t') {
            int vi = i + klen + 2;
            int oi = 0;
            while (status[vi] && status[vi] != '\n' && oi < outsize - 1)
                out[oi++] = status[vi++];
            out[oi] = '\0';
            return 0;
        }
    }
    return -1;
}

int main(void) {
    printf("  PID STATE    NAME\n");

    ps_dirent_t ent;
    for (unsigned long idx = 0; ; idx++) {
        if (sys_readdir("/proc", idx, &ent) < 0) break;
        if (!is_numeric(ent.name)) continue;

        /* Read /proc/<pid>/status */
        char path[280];
        int p = 0;
        const char *pfx = "/proc/";
        while (*pfx) path[p++] = *pfx++;
        for (int j = 0; ent.name[j]; j++) path[p++] = ent.name[j];
        const char *sfx = "/status";
        while (*sfx) path[p++] = *sfx++;
        path[p] = '\0';

        long fd = sys_open(path, 0);
        if (fd < 0) continue;
        char buf[1024];
        long n = sys_read(fd, buf, 1023);
        sys_close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        char pid_s[16], state[16], name[32];
        pid_s[0] = state[0] = name[0] = '\0';
        extract_field(buf, "Pid", pid_s, 16);
        extract_field(buf, "State", state, 16);
        extract_field(buf, "Name", name, 32);

        printf("%5s %-8s %s\n", pid_s, state, name);
    }
    return 0;
}
