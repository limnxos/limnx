#include "ipc/infer_svc.h"
#include "serial.h"

static infer_service_t infer_table[MAX_INFER_SERVICES];

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int infer_register(const char *name, const char *sock_path, uint64_t pid) {
    /* Check for existing entry with same name — replace */
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && str_eq(infer_table[i].name, name)) {
            str_copy(infer_table[i].sock_path, sock_path, INFER_SOCK_PATH_MAX);
            infer_table[i].provider_pid = pid;
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (!infer_table[i].used) {
            infer_table[i].used = 1;
            str_copy(infer_table[i].name, name, INFER_NAME_MAX);
            str_copy(infer_table[i].sock_path, sock_path, INFER_SOCK_PATH_MAX);
            infer_table[i].provider_pid = pid;
            return 0;
        }
    }

    return -1;
}

int infer_lookup(const char *name) {
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && str_eq(infer_table[i].name, name))
            return i;
    }
    return -1;
}

infer_service_t *infer_get(int idx) {
    if (idx < 0 || idx >= MAX_INFER_SERVICES) return (void *)0;
    if (!infer_table[idx].used) return (void *)0;
    return &infer_table[idx];
}

void infer_unregister_pid(uint64_t pid) {
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && infer_table[i].provider_pid == pid) {
            infer_table[i].used = 0;
            infer_table[i].name[0] = '\0';
            infer_table[i].sock_path[0] = '\0';
            infer_table[i].provider_pid = 0;
        }
    }
}
