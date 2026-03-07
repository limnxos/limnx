#include "ipc/taskgraph.h"

static workflow_task_t tasks[MAX_TASKS];
static uint32_t next_task_id = 1;

static workflow_task_t *find_task(uint32_t id) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used && tasks[i].id == id)
            return &tasks[i];
    return (void *)0;
}

int taskgraph_create(const char *name, uint32_t ns_id, uint64_t owner_pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].used) {
            tasks[i].used = 1;
            tasks[i].id = next_task_id++;
            tasks[i].owner_pid = owner_pid;
            tasks[i].worker_pid = 0;
            tasks[i].ns_id = ns_id;
            tasks[i].dep_count = 0;
            tasks[i].status = TASK_PENDING;
            tasks[i].result = 0;
            int j = 0;
            if (name) {
                for (; j < TASK_NAME_MAX - 1 && name[j]; j++)
                    tasks[i].name[j] = name[j];
            }
            tasks[i].name[j] = '\0';
            for (int k = 0; k < MAX_TASK_DEPS; k++)
                tasks[i].deps[k] = 0;
            return (int)tasks[i].id;
        }
    }
    return -12; /* -ENOMEM */
}

int taskgraph_depend(uint32_t task_id, uint32_t dep_id, uint64_t caller_pid) {
    workflow_task_t *t = find_task(task_id);
    if (!t) return -2; /* -ENOENT */
    if (t->owner_pid != caller_pid) return -1; /* -EPERM */
    if (t->status != TASK_PENDING) return -22; /* -EINVAL: can't add deps after start */

    /* Verify dep exists */
    workflow_task_t *dep = find_task(dep_id);
    if (!dep) return -2;

    /* Check same namespace */
    if (dep->ns_id != t->ns_id) return -1;

    /* Prevent self-dependency */
    if (task_id == dep_id) return -22;

    if (t->dep_count >= MAX_TASK_DEPS) return -12;

    /* Prevent duplicate */
    for (int i = 0; i < t->dep_count; i++)
        if (t->deps[i] == dep_id) return 0; /* already added */

    t->deps[t->dep_count++] = dep_id;
    return 0;
}

int taskgraph_start(uint32_t task_id, uint64_t caller_pid) {
    workflow_task_t *t = find_task(task_id);
    if (!t) return -2;
    if (t->status != TASK_PENDING) return -22; /* already started */

    /* Check all dependencies are DONE */
    for (int i = 0; i < t->dep_count; i++) {
        workflow_task_t *dep = find_task(t->deps[i]);
        if (!dep || dep->status != TASK_DONE)
            return -11; /* -EAGAIN: deps not ready */
    }

    t->status = TASK_RUNNING;
    t->worker_pid = caller_pid;
    return 0;
}

int taskgraph_complete(uint32_t task_id, int32_t result, uint64_t caller_pid) {
    workflow_task_t *t = find_task(task_id);
    if (!t) return -2;
    /* Owner or worker can complete */
    if (t->owner_pid != caller_pid && t->worker_pid != caller_pid)
        return -1;

    t->status = (result >= 0) ? TASK_DONE : TASK_FAILED;
    t->result = result;
    return 0;
}

int taskgraph_status(uint32_t task_id, task_status_t *out) {
    workflow_task_t *t = find_task(task_id);
    if (!t) return -2;

    out->id = t->id;
    out->status = t->status;
    out->dep_count = t->dep_count;
    out->pad[0] = out->pad[1] = 0;
    out->result = t->result;
    for (int i = 0; i < MAX_TASK_DEPS; i++)
        out->deps[i] = t->deps[i];
    for (int i = 0; i < TASK_NAME_MAX; i++)
        out->name[i] = t->name[i];
    return 0;
}

int taskgraph_is_done(uint32_t task_id) {
    workflow_task_t *t = find_task(task_id);
    if (!t) return 1; /* nonexistent = done */
    return (t->status == TASK_DONE || t->status == TASK_FAILED);
}

void taskgraph_cleanup_pid(uint64_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && tasks[i].owner_pid == pid)
            tasks[i].used = 0;
    }
}
