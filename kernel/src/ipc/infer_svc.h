#ifndef LIMNX_INFER_SVC_H
#define LIMNX_INFER_SVC_H

#include <stdint.h>

#define MAX_INFER_SERVICES  16
#define INFER_NAME_MAX      32
#define INFER_SOCK_PATH_MAX 108
#define INFER_HEALTH_TIMEOUT 3600 /* ticks before considered unhealthy (~200s at 18Hz) */

/* Routing policies */
#define INFER_ROUTE_LEAST_LOADED  0
#define INFER_ROUTE_ROUND_ROBIN   1
#define INFER_ROUTE_WEIGHTED      2

typedef struct infer_service {
    char     name[INFER_NAME_MAX];
    char     sock_path[INFER_SOCK_PATH_MAX];
    uint64_t provider_pid;
    uint32_t load;              /* current request load (set by daemon) */
    uint32_t last_heartbeat;    /* tick count of last health report */
    uint32_t total_requests;    /* total requests routed to this service */
    uint32_t ns_id;             /* namespace ID (0 = global) */
    uint8_t  healthy;           /* 1 = healthy, 0 = unhealthy/unknown */
    uint8_t  used;
} infer_service_t;

/* Register a service name -> socket path mapping. Adds new entry (allows multiple per name). */
int  infer_register(const char *name, const char *sock_path, uint64_t pid);

/* Lookup a service by name, returns index or -1 */
int  infer_lookup(const char *name);

/* Get service by index */
infer_service_t *infer_get(int idx);

/* Unregister all services owned by a given pid */
void infer_unregister_pid(uint64_t pid);

/* Report health: daemon calls this to set load and mark healthy */
int  infer_health(uint64_t pid, uint32_t load);

/* Route: find the best instance of a named service using current policy.
 * Returns index or -1 if none available. */
int  infer_route(const char *name);

/* Route within a namespace: find best instance scoped to ns_id.
 * ns_id=0 searches global (all namespaces). Returns index or -1. */
int  infer_route_ns(const char *name, uint32_t ns_id);

/* Swap: atomically replace socket path for a service owned by pid.
 * Resets health/load. Returns 0 or -errno. */
int  infer_swap(const char *name, const char *new_sock_path, uint64_t pid);

/* Set the routing policy (INFER_ROUTE_*) */
int  infer_set_policy(uint8_t policy);

/* Get the current routing policy */
uint8_t infer_get_policy(void);

/* Periodic health check: mark services with stale heartbeats as unhealthy.
 * Called from sched_tick. Returns number of services marked unhealthy. */
int  infer_health_check(void);

/* --- Inference result cache --- */

#define INFER_CACHE_SIZE        32
#define INFER_CACHE_RESP_MAX    256
#define INFER_CACHE_DEFAULT_TTL 180  /* ~10 seconds at 18Hz */

/* Cache control sub-commands */
#define INFER_CACHE_FLUSH       0
#define INFER_CACHE_STATS       1
#define INFER_CACHE_SET_TTL     2

typedef struct infer_cache_entry {
    uint64_t hash;
    char     name[INFER_NAME_MAX];
    uint8_t  response[INFER_CACHE_RESP_MAX];
    uint32_t resp_len;
    uint32_t insert_tick;
    int8_t   lru_prev;
    int8_t   lru_next;
    uint8_t  used;
} infer_cache_entry_t;

typedef struct infer_cache_stat {
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t size;
    uint32_t capacity;
    uint32_t ttl;
} infer_cache_stat_t;

int  infer_cache_lookup(const char *name, const void *req, uint32_t req_len,
                        void *resp_buf, uint32_t resp_max);
void infer_cache_insert(const char *name, const void *req, uint32_t req_len,
                        const void *resp, uint32_t resp_len);
void infer_cache_flush(void);
void infer_cache_get_stat(infer_cache_stat_t *stat);
void infer_cache_set_ttl(uint32_t ttl);
void infer_cache_expire(void);

/* --- Inference request queue --- */

#define INFER_QUEUE_SIZE     16
#define INFER_QUEUE_TIMEOUT  36000 /* ticks (~2000s at 18Hz, enough for large model CPU inference) */

typedef struct infer_queue_entry {
    char     name[INFER_NAME_MAX];
    uint64_t caller_pid;
    uint32_t enqueue_tick;
    volatile uint8_t  ready;     /* set to 1 when provider available */
    volatile uint8_t  timed_out; /* set to 1 on timeout */
    uint8_t  used;
} infer_queue_entry_t;

typedef struct infer_queue_stat {
    uint32_t capacity;
    uint32_t pending;
    uint32_t total_queued;
    uint32_t total_timeouts;
} infer_queue_stat_t;

/* Enqueue a request — returns queue slot index, or -1 if full */
int  infer_queue_enqueue(const char *name, uint64_t caller_pid);

/* Check if a queued entry is ready or timed out */
int  infer_queue_check(int slot);

/* Remove a queue entry */
void infer_queue_remove(int slot);

/* Drain: wake queued callers matching a service name (called from infer_health) */
void infer_queue_drain(const char *name);

/* Expire timed-out queue entries */
void infer_queue_expire(void);

/* Get queue statistics */
void infer_queue_get_stat(infer_queue_stat_t *stat);

/* --- Inference batching --- */

#define INFER_BATCH_SIZE       4
#define INFER_BATCH_REQ_MAX    256
#define INFER_BATCH_RESP_MAX   256
#define INFER_BATCH_WINDOW     2    /* ticks to wait for batch to fill */

typedef struct infer_batch_entry {
    uint8_t  request[INFER_BATCH_REQ_MAX];
    uint32_t req_len;
    uint8_t  response[INFER_BATCH_RESP_MAX];
    uint32_t resp_len;
    uint64_t caller_pid;
    volatile uint8_t ready;   /* 0=waiting, 1=response ready, 2=error */
} infer_batch_entry_t;

typedef struct infer_batch {
    char     name[INFER_NAME_MAX];
    char     sock_path[INFER_SOCK_PATH_MAX];
    infer_batch_entry_t entries[INFER_BATCH_SIZE];
    uint32_t count;
    uint32_t start_tick;
    volatile uint8_t active;   /* 1 = collecting, 0 = idle */
} infer_batch_t;

typedef struct infer_batch_stat {
    uint32_t total_batches;
    uint32_t total_requests;
    uint32_t batches_of_1;
    uint32_t batches_of_2plus;
} infer_batch_stat_t;

int  infer_batch_submit(const char *name, const char *sock_path,
                         const void *req, uint32_t req_len,
                         void *resp_buf, uint32_t resp_max,
                         uint64_t caller_pid);
void infer_batch_get_stat(infer_batch_stat_t *stat);

/* --- Async inference --- */

#define INFER_ASYNC_SIZE       16
#define INFER_ASYNC_REQ_MAX    256
#define INFER_ASYNC_RESP_MAX   256
#define INFER_ASYNC_TIMEOUT    90   /* ticks (~5 seconds at 18Hz) */

/* Async slot states */
#define INFER_ASYNC_FREE       0
#define INFER_ASYNC_PENDING    1
#define INFER_ASYNC_READY      2
#define INFER_ASYNC_ERROR      3

typedef struct infer_async_entry {
    char     name[INFER_NAME_MAX];
    uint8_t  request[INFER_ASYNC_REQ_MAX];
    uint32_t req_len;
    uint8_t  response[INFER_ASYNC_RESP_MAX];
    uint32_t resp_len;
    uint64_t owner_pid;
    int32_t  eventfd_idx;     /* -1 = no notification */
    int32_t  error_code;      /* errno when state == ERROR */
    uint32_t submit_tick;
    uint32_t ns_id;           /* namespace scope for routing (0 = global) */
    volatile uint8_t state;   /* INFER_ASYNC_* */
} infer_async_entry_t;

/* Submit an async inference request. Returns request_id (>0) or negative error. */
int  infer_async_submit(const char *name, const void *req, uint32_t req_len,
                         uint64_t owner_pid, int32_t eventfd_idx);

/* Poll async request status. Returns INFER_ASYNC_* state or -EINVAL. */
int  infer_async_poll(int request_id, uint64_t caller_pid);

/* Retrieve result and free slot. Returns bytes copied or negative error. */
int  infer_async_result(int request_id, uint64_t caller_pid,
                         void *resp_buf, uint32_t resp_max);

/* Process one pending async request (called by worker thread). Returns 1 if work done. */
int  infer_async_process_one(void);

/* Expire timed-out async requests (called from sched_tick). */
void infer_async_expire(void);

/* Cleanup async slots owned by a dying process. */
void infer_async_cleanup_pid(uint64_t pid);

/* Start the async worker kernel thread. */
void infer_async_start_worker(void);

#endif
