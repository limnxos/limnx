#include "ipc/infer_svc.h"
#include "proc/process.h"
#include "sync/spinlock.h"
#include "serial.h"
#include "idt/idt.h"

#ifndef EAGAIN
#define EAGAIN 11
#endif

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
            /* Save name before releasing lock */
            char name[INFER_NAME_MAX];
            str_copy(name, infer_table[i].name, INFER_NAME_MAX);
            spin_unlock_irqrestore(&infer_lock, flags);
            /* Drain queued requests for this service */
            infer_queue_drain(name);
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

/* --- Inference result cache --- */

static infer_cache_entry_t infer_cache[INFER_CACHE_SIZE];
static int8_t  cache_lru_head = -1;
static int8_t  cache_lru_tail = -1;
static uint32_t cache_hits = 0;
static uint32_t cache_misses = 0;
static uint32_t cache_evictions = 0;
static uint32_t cache_ttl = INFER_CACHE_DEFAULT_TTL;

/* FNV-1a hash over name + request bytes */
static uint64_t fnv1a_hash(const char *name, const uint8_t *data, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 0x100000001b3ULL;
    }
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void cache_lru_remove(int idx) {
    infer_cache_entry_t *e = &infer_cache[idx];
    if (e->lru_prev >= 0)
        infer_cache[e->lru_prev].lru_next = e->lru_next;
    else
        cache_lru_head = e->lru_next;

    if (e->lru_next >= 0)
        infer_cache[e->lru_next].lru_prev = e->lru_prev;
    else
        cache_lru_tail = e->lru_prev;

    e->lru_prev = -1;
    e->lru_next = -1;
}

static void cache_lru_push_front(int idx) {
    infer_cache_entry_t *e = &infer_cache[idx];
    e->lru_prev = -1;
    e->lru_next = cache_lru_head;

    if (cache_lru_head >= 0)
        infer_cache[cache_lru_head].lru_prev = (int8_t)idx;
    cache_lru_head = (int8_t)idx;

    if (cache_lru_tail < 0)
        cache_lru_tail = (int8_t)idx;
}

static void mem_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

int infer_cache_lookup(const char *name, const void *req, uint32_t req_len,
                       void *resp_buf, uint32_t resp_max) {
    uint64_t h = fnv1a_hash(name, (const uint8_t *)req, req_len);

    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    for (int i = 0; i < INFER_CACHE_SIZE; i++) {
        if (!infer_cache[i].used) continue;
        if (infer_cache[i].hash != h) continue;
        if (!str_eq(infer_cache[i].name, name)) continue;

        /* Cache hit — copy response */
        uint32_t copy_len = infer_cache[i].resp_len;
        if (copy_len > resp_max) copy_len = resp_max;
        mem_copy(resp_buf, infer_cache[i].response, copy_len);

        /* Move to LRU head */
        cache_lru_remove(i);
        cache_lru_push_front(i);
        cache_hits++;

        spin_unlock_irqrestore(&infer_lock, flags);
        serial_printf("[infer] Cache hit for '%s' (hash %lx)\n", name, h);
        return (int)copy_len;
    }

    cache_misses++;
    spin_unlock_irqrestore(&infer_lock, flags);
    return -1;
}

void infer_cache_insert(const char *name, const void *req, uint32_t req_len,
                        const void *resp, uint32_t resp_len) {
    if (resp_len > INFER_CACHE_RESP_MAX) return;

    uint64_t h = fnv1a_hash(name, (const uint8_t *)req, req_len);

    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    /* Check if already cached (dedup) */
    for (int i = 0; i < INFER_CACHE_SIZE; i++) {
        if (infer_cache[i].used && infer_cache[i].hash == h &&
            str_eq(infer_cache[i].name, name)) {
            /* Update existing entry */
            mem_copy(infer_cache[i].response, resp, resp_len);
            infer_cache[i].resp_len = resp_len;
            infer_cache[i].insert_tick = (uint32_t)pit_get_ticks();
            cache_lru_remove(i);
            cache_lru_push_front(i);
            spin_unlock_irqrestore(&infer_lock, flags);
            return;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < INFER_CACHE_SIZE; i++) {
        if (!infer_cache[i].used) { slot = i; break; }
    }

    /* Evict LRU tail if no free slot */
    if (slot < 0 && cache_lru_tail >= 0) {
        slot = cache_lru_tail;
        cache_lru_remove(slot);
        infer_cache[slot].used = 0;
        cache_evictions++;
    }

    if (slot < 0) {
        spin_unlock_irqrestore(&infer_lock, flags);
        return;
    }

    /* Insert */
    infer_cache[slot].hash = h;
    str_copy(infer_cache[slot].name, name, INFER_NAME_MAX);
    mem_copy(infer_cache[slot].response, resp, resp_len);
    infer_cache[slot].resp_len = resp_len;
    infer_cache[slot].insert_tick = (uint32_t)pit_get_ticks();
    infer_cache[slot].used = 1;
    cache_lru_push_front(slot);

    spin_unlock_irqrestore(&infer_lock, flags);
    serial_printf("[infer] Cached response for '%s' (hash %lx, %u bytes)\n",
                  name, h, resp_len);
}

void infer_cache_flush(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    for (int i = 0; i < INFER_CACHE_SIZE; i++) {
        infer_cache[i].used = 0;
        infer_cache[i].lru_prev = -1;
        infer_cache[i].lru_next = -1;
    }
    cache_lru_head = -1;
    cache_lru_tail = -1;

    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_cache_get_stat(infer_cache_stat_t *stat) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    stat->hits = cache_hits;
    stat->misses = cache_misses;
    stat->evictions = cache_evictions;
    stat->capacity = INFER_CACHE_SIZE;
    stat->ttl = cache_ttl;
    stat->size = 0;
    for (int i = 0; i < INFER_CACHE_SIZE; i++) {
        if (infer_cache[i].used) stat->size++;
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_cache_set_ttl(uint32_t ttl) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);
    cache_ttl = ttl;
    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_cache_expire(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    uint32_t now = (uint32_t)pit_get_ticks();

    for (int i = 0; i < INFER_CACHE_SIZE; i++) {
        if (!infer_cache[i].used) continue;
        uint32_t elapsed = now - infer_cache[i].insert_tick;
        if (elapsed > cache_ttl) {
            cache_lru_remove(i);
            infer_cache[i].used = 0;
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}

/* --- Inference request queue --- */

static infer_queue_entry_t infer_queue[INFER_QUEUE_SIZE];
static uint32_t infer_queue_total_queued = 0;
static uint32_t infer_queue_total_timeouts = 0;

int infer_queue_enqueue(const char *name, uint64_t caller_pid) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    for (int i = 0; i < INFER_QUEUE_SIZE; i++) {
        if (!infer_queue[i].used) {
            infer_queue[i].used = 1;
            str_copy(infer_queue[i].name, name, INFER_NAME_MAX);
            infer_queue[i].caller_pid = caller_pid;
            infer_queue[i].enqueue_tick = (uint32_t)pit_get_ticks();
            infer_queue[i].ready = 0;
            infer_queue[i].timed_out = 0;
            infer_queue_total_queued++;
            spin_unlock_irqrestore(&infer_lock, flags);
            serial_printf("[infer] Queued request for '%s' from pid %lu (slot %d)\n",
                          name, caller_pid, i);
            return i;
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return -1;  /* queue full */
}

int infer_queue_check(int slot) {
    if (slot < 0 || slot >= INFER_QUEUE_SIZE) return -1;
    if (!infer_queue[slot].used) return -1;
    if (infer_queue[slot].ready) return 1;
    if (infer_queue[slot].timed_out) return -EAGAIN;
    return 0;  /* still waiting */
}

void infer_queue_remove(int slot) {
    if (slot < 0 || slot >= INFER_QUEUE_SIZE) return;
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);
    infer_queue[slot].used = 0;
    infer_queue[slot].ready = 0;
    infer_queue[slot].timed_out = 0;
    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_queue_drain(const char *name) {
    /* Called when a provider reports health — wake the oldest queued caller */
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    uint32_t oldest_tick = 0xFFFFFFFF;
    int oldest_slot = -1;

    for (int i = 0; i < INFER_QUEUE_SIZE; i++) {
        if (!infer_queue[i].used) continue;
        if (infer_queue[i].ready || infer_queue[i].timed_out) continue;
        if (!str_eq(infer_queue[i].name, name)) continue;

        if (infer_queue[i].enqueue_tick < oldest_tick) {
            oldest_tick = infer_queue[i].enqueue_tick;
            oldest_slot = i;
        }
    }

    if (oldest_slot >= 0) {
        infer_queue[oldest_slot].ready = 1;
        serial_printf("[infer] Draining queue slot %d for '%s' (pid %lu)\n",
                      oldest_slot, name, infer_queue[oldest_slot].caller_pid);
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_queue_expire(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    uint32_t now = (uint32_t)pit_get_ticks();

    for (int i = 0; i < INFER_QUEUE_SIZE; i++) {
        if (!infer_queue[i].used) continue;
        if (infer_queue[i].ready || infer_queue[i].timed_out) continue;

        uint32_t elapsed = now - infer_queue[i].enqueue_tick;
        if (elapsed > INFER_QUEUE_TIMEOUT) {
            infer_queue[i].timed_out = 1;
            infer_queue_total_timeouts++;
            serial_printf("[infer] Queue slot %d timed out for '%s'\n",
                          i, infer_queue[i].name);
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_queue_get_stat(infer_queue_stat_t *stat) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    stat->capacity = INFER_QUEUE_SIZE;
    stat->pending = 0;
    stat->total_queued = infer_queue_total_queued;
    stat->total_timeouts = infer_queue_total_timeouts;

    for (int i = 0; i < INFER_QUEUE_SIZE; i++) {
        if (infer_queue[i].used && !infer_queue[i].ready && !infer_queue[i].timed_out)
            stat->pending++;
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}
