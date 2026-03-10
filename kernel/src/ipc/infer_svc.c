#include "ipc/infer_svc.h"
#include "proc/process.h"
#include "sync/spinlock.h"
#include "serial.h"
#include "idt/idt.h"

static infer_service_t infer_table[MAX_INFER_SERVICES];
static spinlock_t infer_lock = SPINLOCK_INIT;

/* Routing state */
static uint8_t  route_policy = INFER_ROUTE_LEAST_LOADED;
static uint32_t rr_counter = 0;

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
            infer_table[i].total_requests = 0;
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
            infer_table[i].total_requests = 0;
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
            infer_table[i].total_requests = 0;
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

int infer_health_check(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    uint32_t now = (uint32_t)pit_get_ticks();
    int unhealthy_count = 0;

    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (!infer_table[i].used) continue;
        if (!infer_table[i].healthy) continue;

        /* Check if heartbeat is stale */
        uint32_t elapsed = now - infer_table[i].last_heartbeat;
        if (elapsed > INFER_HEALTH_TIMEOUT) {
            infer_table[i].healthy = 0;
            unhealthy_count++;
            serial_printf("[infer] Service '%s' (pid %lu) marked unhealthy "
                          "(no heartbeat for %u ticks)\n",
                          infer_table[i].name,
                          infer_table[i].provider_pid, elapsed);
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return unhealthy_count;
}

int infer_route(const char *name) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    /* Collect healthy candidates matching name */
    int candidates[MAX_INFER_SERVICES];
    int count = 0;

    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (!infer_table[i].used) continue;
        if (!str_eq(infer_table[i].name, name)) continue;
        if (!infer_table[i].healthy) continue;

        /* Verify provider process is still alive */
        process_t *p = process_lookup(infer_table[i].provider_pid);
        if (!p || p->exited) {
            infer_table[i].used = 0;
            infer_table[i].healthy = 0;
            continue;
        }

        candidates[count++] = i;
    }

    if (count == 0) {
        spin_unlock_irqrestore(&infer_lock, flags);
        return -1;
    }

    int chosen = -1;

    switch (route_policy) {
    case INFER_ROUTE_ROUND_ROBIN:
        chosen = candidates[rr_counter % count];
        rr_counter++;
        break;

    case INFER_ROUTE_WEIGHTED: {
        /* Inverse-load weighted: lower load = higher weight */
        uint32_t max_load = 0;
        for (int j = 0; j < count; j++) {
            if (infer_table[candidates[j]].load > max_load)
                max_load = infer_table[candidates[j]].load;
        }
        uint32_t total_weight = 0;
        for (int j = 0; j < count; j++)
            total_weight += (max_load - infer_table[candidates[j]].load + 1);

        uint32_t pick = rr_counter % (total_weight ? total_weight : 1);
        rr_counter++;

        uint32_t cumulative = 0;
        for (int j = 0; j < count; j++) {
            cumulative += (max_load - infer_table[candidates[j]].load + 1);
            if (pick < cumulative) { chosen = candidates[j]; break; }
        }
        if (chosen < 0) chosen = candidates[count - 1];
        break;
    }

    case INFER_ROUTE_LEAST_LOADED:
    default: {
        uint32_t best_load = 0xFFFFFFFF;
        for (int j = 0; j < count; j++) {
            if (infer_table[candidates[j]].load < best_load) {
                best_load = infer_table[candidates[j]].load;
                chosen = candidates[j];
            }
        }
        break;
    }
    }

    if (chosen >= 0)
        infer_table[chosen].total_requests++;

    spin_unlock_irqrestore(&infer_lock, flags);
    return chosen;
}

int infer_set_policy(uint8_t policy) {
    if (policy > INFER_ROUTE_WEIGHTED) return -1;
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);
    route_policy = policy;
    rr_counter = 0;
    spin_unlock_irqrestore(&infer_lock, flags);
    return 0;
}

uint8_t infer_get_policy(void) {
    return route_policy;
}
