#include "ipc/infer_svc.h"
#include "proc/process.h"
#include "sync/spinlock.h"
#include "serial.h"
#include "idt/idt.h"

static infer_service_t infer_table[MAX_INFER_SERVICES];
static spinlock_t infer_lock = SPINLOCK_INIT;

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
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    /* Check for existing entry with same pid — replace */
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && infer_table[i].provider_pid == pid) {
            str_copy(infer_table[i].name, name, INFER_NAME_MAX);
            str_copy(infer_table[i].sock_path, sock_path, INFER_SOCK_PATH_MAX);
            infer_table[i].load = 0;
            infer_table[i].healthy = 0;
            infer_table[i].last_heartbeat = 0;
            spin_unlock_irqrestore(&infer_lock, flags);
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
            infer_table[i].load = 0;
            infer_table[i].healthy = 0;
            infer_table[i].last_heartbeat = 0;
            spin_unlock_irqrestore(&infer_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return -1;
}

int infer_lookup(const char *name) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && str_eq(infer_table[i].name, name)) {
            spin_unlock_irqrestore(&infer_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&infer_lock, flags);
    return -1;
}

infer_service_t *infer_get(int idx) {
    if (idx < 0 || idx >= MAX_INFER_SERVICES) return (void *)0;
    if (!infer_table[idx].used) return (void *)0;
    return &infer_table[idx];
}

void infer_unregister_pid(uint64_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && infer_table[i].provider_pid == pid) {
            infer_table[i].used = 0;
            infer_table[i].name[0] = '\0';
            infer_table[i].sock_path[0] = '\0';
            infer_table[i].provider_pid = 0;
            infer_table[i].load = 0;
            infer_table[i].healthy = 0;
            infer_table[i].last_heartbeat = 0;
        }
    }
    spin_unlock_irqrestore(&infer_lock, flags);
}

int infer_health(uint64_t pid, uint32_t load) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && infer_table[i].provider_pid == pid) {
            infer_table[i].load = load;
            infer_table[i].healthy = 1;
            infer_table[i].last_heartbeat = (uint32_t)pit_get_ticks();
            spin_unlock_irqrestore(&infer_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&infer_lock, flags);
    return -1;  /* pid not registered */
}

int infer_route(const char *name) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    int best = -1;
    uint32_t best_load = 0xFFFFFFFF;

    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (!infer_table[i].used) continue;
        if (!str_eq(infer_table[i].name, name)) continue;
        if (!infer_table[i].healthy) continue;

        /* Verify provider process is still alive (handles SIGKILL case) */
        process_t *p = process_lookup(infer_table[i].provider_pid);
        if (!p || p->exited) {
            /* Provider died — auto-unregister */
            infer_table[i].used = 0;
            infer_table[i].healthy = 0;
            continue;
        }

        if (infer_table[i].load < best_load) {
            best_load = infer_table[i].load;
            best = i;
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return best;
}
