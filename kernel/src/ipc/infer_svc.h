#ifndef LIMNX_INFER_SVC_H
#define LIMNX_INFER_SVC_H

#include <stdint.h>

#define MAX_INFER_SERVICES  16
#define INFER_NAME_MAX      32
#define INFER_SOCK_PATH_MAX 108
#define INFER_HEALTH_TIMEOUT 50  /* ticks before considered unhealthy */

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

/* Set the routing policy (INFER_ROUTE_*) */
int  infer_set_policy(uint8_t policy);

/* Get the current routing policy */
uint8_t infer_get_policy(void);

/* Periodic health check: mark services with stale heartbeats as unhealthy.
 * Called from sched_tick. Returns number of services marked unhealthy. */
int  infer_health_check(void);

/* --- Inference request queue --- */

#define INFER_QUEUE_SIZE     16
#define INFER_QUEUE_TIMEOUT  90   /* ticks (~5 seconds at 18Hz) */

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

#endif
