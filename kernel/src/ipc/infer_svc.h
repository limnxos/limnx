#ifndef LIMNX_INFER_SVC_H
#define LIMNX_INFER_SVC_H

#include <stdint.h>

#define MAX_INFER_SERVICES  8
#define INFER_NAME_MAX      32
#define INFER_SOCK_PATH_MAX 108
#define INFER_HEALTH_TIMEOUT 50  /* ticks before considered unhealthy */

typedef struct infer_service {
    char     name[INFER_NAME_MAX];
    char     sock_path[INFER_SOCK_PATH_MAX];
    uint64_t provider_pid;
    uint32_t load;              /* current request load (set by daemon) */
    uint32_t last_heartbeat;    /* tick count of last health report */
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

/* Route: find the healthiest, lowest-load instance of a named service.
 * Returns index or -1 if none available. */
int  infer_route(const char *name);

/* Periodic health check: mark services with stale heartbeats as unhealthy.
 * Called from sched_tick. Returns number of services marked unhealthy. */
int  infer_health_check(void);

#endif
