#ifndef LIMNX_TASKGRAPH_H
#define LIMNX_TASKGRAPH_H

#include <stdint.h>

#define MAX_TASKS      32
#define MAX_TASK_DEPS   4
#define TASK_NAME_MAX  32

/* Task status */
#define TASK_PENDING  0
#define TASK_RUNNING  1
#define TASK_DONE     2
#define TASK_FAILED   3

typedef struct workflow_task {
    uint32_t id;
    uint64_t owner_pid;                 /* orchestrator */
    uint64_t worker_pid;                /* assigned worker (0 = unassigned) */
    char     name[TASK_NAME_MAX];
    uint32_t ns_id;
    uint32_t deps[MAX_TASK_DEPS];       /* task IDs this depends on */
    uint8_t  dep_count;
    uint8_t  status;
    int32_t  result;
    uint8_t  used;
} workflow_task_t;

/* User-visible task status struct (32 bytes) */
typedef struct task_status {
    uint32_t id;
    uint8_t  status;        /* TASK_PENDING/RUNNING/DONE/FAILED */
    uint8_t  dep_count;
    uint8_t  pad[2];
    int32_t  result;
    uint32_t deps[MAX_TASK_DEPS];
    char     name[TASK_NAME_MAX];
} task_status_t;

/* Create a task. Returns task_id or -errno. */
int  taskgraph_create(const char *name, uint32_t ns_id, uint64_t owner_pid);

/* Add dependency: task_id depends on dep_id.
 * Cross-namespace deps require CAP_XNS_TASK. Returns 0 or -errno. */
int  taskgraph_depend(uint32_t task_id, uint32_t dep_id,
                      uint64_t caller_pid, uint32_t caller_caps);

/* Mark task as RUNNING (fails if deps not DONE). Returns 0 or -errno. */
int  taskgraph_start(uint32_t task_id, uint64_t caller_pid);

/* Mark task as DONE (result>=0) or FAILED (result<0). Returns 0 or -errno. */
int  taskgraph_complete(uint32_t task_id, int32_t result, uint64_t caller_pid,
                        uint32_t caller_ns_id);

/* Query task status. Returns 0 or -errno. */
int  taskgraph_status(uint32_t task_id, task_status_t *out);

/* Check if task is finished (DONE or FAILED). Returns 1 if done. */
int  taskgraph_is_done(uint32_t task_id);

/* Cleanup tasks owned by pid. */
void taskgraph_cleanup_pid(uint64_t pid);

#endif
