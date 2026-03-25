#define pr_fmt(fmt) "[infer] " fmt
#include "klog.h"

#include "ipc/infer_svc.h"
#include "ipc/unix_sock.h"
#include "ipc/eventfd.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "sync/spinlock.h"
#include "arch/serial.h"
#include "arch/timer.h"
#include "errno.h"
#include "kutil.h"

static infer_service_t infer_table[MAX_INFER_SERVICES];
/*
 * Lock ordering: infer_lock is at level 4 (subsystem).
 * Must NOT hold sched_lock, pmm_lock, or kheap_lock when acquiring.
 * Does NOT call kmalloc or pmm_alloc while held.
 * Copy data under lock, release, then do I/O (unix socket send).
 */
static spinlock_t infer_lock = SPINLOCK_INIT;

/* Routing state */
static uint8_t  route_policy = INFER_ROUTE_LEAST_LOADED;
static uint32_t rr_counter = 0;


int infer_register(const char *name, const char *sock_path, uint64_t pid) {
    /* Resolve caller's namespace */
    uint32_t ns_id = 0;
    process_t *caller = process_lookup(pid);
    if (caller) ns_id = caller->ns_id;

    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    /* Check for existing entry with same pid — replace */
    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (infer_table[i].used && infer_table[i].provider_pid == pid) {
            str_copy(infer_table[i].name, name, INFER_NAME_MAX);
            str_copy(infer_table[i].sock_path, sock_path, INFER_SOCK_PATH_MAX);
            infer_table[i].ns_id = ns_id;
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
            infer_table[i].ns_id = ns_id;
            infer_table[i].load = 0;
            infer_table[i].healthy = 0;
            infer_table[i].last_heartbeat = 0;
            infer_table[i].total_requests = 0;
            spin_unlock_irqrestore(&infer_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return -ENOSPC;
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
    return -ENOENT;
}

infer_service_t *infer_get(int idx) {
    if (idx < 0 || idx >= MAX_INFER_SERVICES) return NULL;
    if (!infer_table[idx].used) return NULL;
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
            infer_table[i].ns_id = 0;
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
            infer_table[i].last_heartbeat = (uint32_t)arch_timer_get_ticks();
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
    return -ESRCH;  /* pid not registered */
}

int infer_health_check(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    uint32_t now = (uint32_t)arch_timer_get_ticks();
    int unhealthy_count = 0;

    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (!infer_table[i].used) continue;
        if (!infer_table[i].healthy) continue;

        /* Check if heartbeat is stale */
        uint32_t elapsed = now - infer_table[i].last_heartbeat;
        if (elapsed > INFER_HEALTH_TIMEOUT) {
            infer_table[i].healthy = 0;
            unhealthy_count++;
            pr_warn("Service '%s' (pid %lu) marked unhealthy "
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
        return -ENOENT;
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
    if (policy > INFER_ROUTE_WEIGHTED) return -EINVAL;
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

int infer_swap(const char *name, const char *new_sock_path, uint64_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (!infer_table[i].used) continue;
        if (!str_eq(infer_table[i].name, name)) continue;
        if (infer_table[i].provider_pid != pid) continue;

        /* Atomic path replacement */
        str_copy(infer_table[i].sock_path, new_sock_path, INFER_SOCK_PATH_MAX);
        infer_table[i].load = 0;
        infer_table[i].healthy = 0;
        infer_table[i].last_heartbeat = 0;

        spin_unlock_irqrestore(&infer_lock, flags);
        pr_info("Swapped '%s' (pid %lu) → '%s'\n", name, pid, new_sock_path);
        return 0;
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return -ENOENT;
}

int infer_route_ns(const char *name, uint32_t ns_id) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    /* Collect healthy candidates matching name and namespace */
    int candidates[MAX_INFER_SERVICES];
    int count = 0;

    for (int i = 0; i < MAX_INFER_SERVICES; i++) {
        if (!infer_table[i].used) continue;
        if (!str_eq(infer_table[i].name, name)) continue;
        if (!infer_table[i].healthy) continue;

        /* Namespace filter: ns_id=0 means search all */
        if (ns_id != 0 && infer_table[i].ns_id != ns_id) continue;

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
        return -ENOENT;
    }

    /* Use same routing policy as infer_route */
    int chosen = -1;

    switch (route_policy) {
    case INFER_ROUTE_ROUND_ROBIN:
        chosen = candidates[rr_counter % count];
        rr_counter++;
        break;

    case INFER_ROUTE_WEIGHTED: {
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
        pr_info("Cache hit for '%s' (hash %lx)\n", name, h);
        return (int)copy_len;
    }

    cache_misses++;
    spin_unlock_irqrestore(&infer_lock, flags);
    return -ENOENT;
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
            infer_cache[i].insert_tick = (uint32_t)arch_timer_get_ticks();
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
    infer_cache[slot].insert_tick = (uint32_t)arch_timer_get_ticks();
    infer_cache[slot].used = 1;
    cache_lru_push_front(slot);

    spin_unlock_irqrestore(&infer_lock, flags);
    pr_info("Cached response for '%s' (hash %lx, %u bytes)\n",
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

    uint32_t now = (uint32_t)arch_timer_get_ticks();

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
            infer_queue[i].enqueue_tick = (uint32_t)arch_timer_get_ticks();
            infer_queue[i].ready = 0;
            infer_queue[i].timed_out = 0;
            infer_queue_total_queued++;
            spin_unlock_irqrestore(&infer_lock, flags);
            pr_info("Queued request for '%s' from pid %lu (slot %d)\n",
                    name, caller_pid, i);
            return i;
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return -ENOBUFS;  /* queue full */
}

int infer_queue_check(int slot) {
    if (slot < 0 || slot >= INFER_QUEUE_SIZE) return -EINVAL;
    if (!infer_queue[slot].used) return -EINVAL;
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
        pr_info("Draining queue slot %d for '%s' (pid %lu)\n",
                oldest_slot, name, infer_queue[oldest_slot].caller_pid);
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_queue_expire(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    uint32_t now = (uint32_t)arch_timer_get_ticks();

    for (int i = 0; i < INFER_QUEUE_SIZE; i++) {
        if (!infer_queue[i].used) continue;
        if (infer_queue[i].ready || infer_queue[i].timed_out) continue;

        uint32_t elapsed = now - infer_queue[i].enqueue_tick;
        if (elapsed > INFER_QUEUE_TIMEOUT) {
            infer_queue[i].timed_out = 1;
            infer_queue_total_timeouts++;
            pr_warn("Queue slot %d timed out for '%s'\n",
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

/* --- Async inference --- */

static infer_async_entry_t infer_async[INFER_ASYNC_SIZE];

int infer_async_submit(const char *name, const void *req, uint32_t req_len,
                        uint64_t owner_pid, int32_t eventfd_idx) {
    if (req_len > INFER_ASYNC_REQ_MAX) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    for (int i = 0; i < INFER_ASYNC_SIZE; i++) {
        if (infer_async[i].state == INFER_ASYNC_FREE) {
            str_copy(infer_async[i].name, name, INFER_NAME_MAX);
            if (req_len > 0)
                mem_copy(infer_async[i].request, req, req_len);
            infer_async[i].req_len = req_len;
            infer_async[i].resp_len = 0;
            infer_async[i].owner_pid = owner_pid;
            infer_async[i].eventfd_idx = eventfd_idx;
            infer_async[i].error_code = 0;
            infer_async[i].submit_tick = (uint32_t)arch_timer_get_ticks();
            infer_async[i].ns_id = 0;
            {
                process_t *op = process_lookup(owner_pid);
                if (op) infer_async[i].ns_id = op->ns_id;
            }
            infer_async[i].state = INFER_ASYNC_PENDING;

            spin_unlock_irqrestore(&infer_lock, flags);
            pr_info("Async submit slot %d for '%s' (pid %lu)\n",
                    i, name, owner_pid);
            return i + 1;  /* request IDs are 1-based */
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
    return -ENOBUFS;
}

int infer_async_poll(int request_id, uint64_t caller_pid) {
    int idx = request_id - 1;
    if (idx < 0 || idx >= INFER_ASYNC_SIZE) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    infer_async_entry_t *e = &infer_async[idx];
    if (e->state == INFER_ASYNC_FREE || e->owner_pid != caller_pid) {
        spin_unlock_irqrestore(&infer_lock, flags);
        return -EINVAL;
    }

    int state = e->state;
    int err = e->error_code;
    spin_unlock_irqrestore(&infer_lock, flags);

    if (state == INFER_ASYNC_ERROR) return err ? -err : -ENOENT;
    return state; /* PENDING=1, READY=2 */
}

int infer_async_result(int request_id, uint64_t caller_pid,
                        void *resp_buf, uint32_t resp_max) {
    int idx = request_id - 1;
    if (idx < 0 || idx >= INFER_ASYNC_SIZE) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    infer_async_entry_t *e = &infer_async[idx];
    if (e->state == INFER_ASYNC_FREE || e->owner_pid != caller_pid) {
        spin_unlock_irqrestore(&infer_lock, flags);
        return -EINVAL;
    }

    if (e->state == INFER_ASYNC_PENDING) {
        spin_unlock_irqrestore(&infer_lock, flags);
        return -EAGAIN;
    }

    if (e->state == INFER_ASYNC_ERROR) {
        int err = e->error_code;
        e->state = INFER_ASYNC_FREE;
        spin_unlock_irqrestore(&infer_lock, flags);
        return err ? -err : -ENOENT;
    }

    /* READY — copy response and free slot */
    uint32_t copy_len = e->resp_len;
    if (copy_len > resp_max) copy_len = resp_max;
    if (copy_len > 0)
        mem_copy(resp_buf, e->response, copy_len);

    e->state = INFER_ASYNC_FREE;
    spin_unlock_irqrestore(&infer_lock, flags);
    return (int)copy_len;
}

int infer_async_process_one(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    /* Find oldest PENDING entry */
    int best = -1;
    uint32_t oldest_tick = 0xFFFFFFFF;
    for (int i = 0; i < INFER_ASYNC_SIZE; i++) {
        if (infer_async[i].state != INFER_ASYNC_PENDING) continue;
        if (infer_async[i].submit_tick < oldest_tick) {
            oldest_tick = infer_async[i].submit_tick;
            best = i;
        }
    }

    if (best < 0) {
        spin_unlock_irqrestore(&infer_lock, flags);
        return 0;  /* no work */
    }

    /* Copy out what we need, release lock for I/O */
    infer_async_entry_t *e = &infer_async[best];
    char name[INFER_NAME_MAX];
    uint8_t req_copy[INFER_ASYNC_REQ_MAX];
    uint32_t req_len = e->req_len;
    int32_t efd = e->eventfd_idx;
    uint32_t async_ns_id = e->ns_id;
    str_copy(name, e->name, INFER_NAME_MAX);
    if (req_len > 0)
        mem_copy(req_copy, e->request, req_len);

    spin_unlock_irqrestore(&infer_lock, flags);

    /* Try to route (namespace-aware) */
    int svc_idx = infer_route_ns(name, async_ns_id);
    if (svc_idx < 0) {
        /* No provider — check timeout */
        uint32_t now = (uint32_t)arch_timer_get_ticks();
        uint32_t elapsed = now - oldest_tick;
        if (elapsed > INFER_ASYNC_TIMEOUT) {
            spin_lock_irqsave(&infer_lock, &flags);
            if (infer_async[best].state == INFER_ASYNC_PENDING) {
                infer_async[best].state = INFER_ASYNC_ERROR;
                infer_async[best].error_code = EAGAIN;
                pr_warn("Async slot %d timed out for '%s'\n",
                        best, name);
            }
            spin_unlock_irqrestore(&infer_lock, flags);
            if (efd >= 0) eventfd_write(efd, 1);
            return 1;
        }
        return 0;  /* not timed out yet, try again later */
    }

    infer_service_t *svc = infer_get(svc_idx);
    if (!svc) {
        spin_lock_irqsave(&infer_lock, &flags);
        if (infer_async[best].state == INFER_ASYNC_PENDING) {
            infer_async[best].state = INFER_ASYNC_ERROR;
            infer_async[best].error_code = ENOENT;
        }
        spin_unlock_irqrestore(&infer_lock, flags);
        if (efd >= 0) eventfd_write(efd, 1);
        return 1;
    }

    /* Connect to provider via unix socket */
    int client_idx = unix_sock_connect(svc->sock_path);
    if (client_idx < 0) {
        spin_lock_irqsave(&infer_lock, &flags);
        if (infer_async[best].state == INFER_ASYNC_PENDING) {
            infer_async[best].state = INFER_ASYNC_ERROR;
            infer_async[best].error_code = ECONNREFUSED;
        }
        spin_unlock_irqrestore(&infer_lock, flags);
        if (efd >= 0) eventfd_write(efd, 1);
        return 1;
    }

    unix_sock_t *client = unix_sock_get(client_idx);
    if (!client) {
        spin_lock_irqsave(&infer_lock, &flags);
        if (infer_async[best].state == INFER_ASYNC_PENDING) {
            infer_async[best].state = INFER_ASYNC_ERROR;
            infer_async[best].error_code = ECONNREFUSED;
        }
        spin_unlock_irqrestore(&infer_lock, flags);
        if (efd >= 0) eventfd_write(efd, 1);
        return 1;
    }

    /* Send request */
    if (req_len > 0) {
        int sent = unix_sock_send(client, req_copy, req_len, 0);
        if (sent < 0) {
            unix_sock_close(client);
            spin_lock_irqsave(&infer_lock, &flags);
            if (infer_async[best].state == INFER_ASYNC_PENDING) {
                infer_async[best].state = INFER_ASYNC_ERROR;
                infer_async[best].error_code = EIO;
            }
            spin_unlock_irqrestore(&infer_lock, flags);
            if (efd >= 0) eventfd_write(efd, 1);
            return 1;
        }
    }

    /* Receive response — blocking recv (yields internally until data or peer close) */
    uint8_t resp_tmp[INFER_ASYNC_RESP_MAX];
    int received = unix_sock_recv(client, resp_tmp, INFER_ASYNC_RESP_MAX, 0);

    unix_sock_close(client);

    /* Store result */
    spin_lock_irqsave(&infer_lock, &flags);
    if (infer_async[best].state == INFER_ASYNC_PENDING) {
        if (received > 0) {
            uint32_t rlen = (uint32_t)received;
            if (rlen > INFER_ASYNC_RESP_MAX) rlen = INFER_ASYNC_RESP_MAX;
            mem_copy(infer_async[best].response, resp_tmp, rlen);
            infer_async[best].resp_len = rlen;
            infer_async[best].state = INFER_ASYNC_READY;

            /* Cache the response */
            spin_unlock_irqrestore(&infer_lock, flags);
            if (req_len > 0 && rlen <= INFER_CACHE_RESP_MAX)
                infer_cache_insert(name, req_copy, req_len, resp_tmp, rlen);
        } else {
            infer_async[best].state = INFER_ASYNC_ERROR;
            infer_async[best].error_code = ENOENT;
            spin_unlock_irqrestore(&infer_lock, flags);
        }
    } else {
        spin_unlock_irqrestore(&infer_lock, flags);
    }

    /* Signal eventfd */
    if (efd >= 0) eventfd_write(efd, 1);

    pr_info("Async slot %d completed for '%s' (%d bytes)\n",
            best, name, received);
    return 1;
}

void infer_async_expire(void) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    uint32_t now = (uint32_t)arch_timer_get_ticks();
    for (int i = 0; i < INFER_ASYNC_SIZE; i++) {
        if (infer_async[i].state != INFER_ASYNC_PENDING) continue;
        uint32_t elapsed = now - infer_async[i].submit_tick;
        if (elapsed > INFER_ASYNC_TIMEOUT) {
            infer_async[i].state = INFER_ASYNC_ERROR;
            infer_async[i].error_code = EAGAIN;
            if (infer_async[i].eventfd_idx >= 0)
                eventfd_write(infer_async[i].eventfd_idx, 1);
            pr_warn("Async slot %d expired for '%s'\n",
                    i, infer_async[i].name);
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}

void infer_async_cleanup_pid(uint64_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&infer_lock, &flags);

    for (int i = 0; i < INFER_ASYNC_SIZE; i++) {
        if (infer_async[i].state != INFER_ASYNC_FREE &&
            infer_async[i].owner_pid == pid) {
            infer_async[i].state = INFER_ASYNC_FREE;
        }
    }

    spin_unlock_irqrestore(&infer_lock, flags);
}

static void infer_async_worker_fn(void) {
    pr_info("Async worker thread started\n");
    for (;;) {
        int did_work = infer_async_process_one();
        if (!did_work) {
            /* No pending work — yield several times to avoid busy-spinning */
            for (int i = 0; i < 5; i++) sched_yield();
        }
    }
}

void infer_async_start_worker(void) {
    thread_t *t = thread_create(infer_async_worker_fn, 0);
    if (t) {
        sched_add(t);
        pr_info("Async worker thread created\n");
    }
}
