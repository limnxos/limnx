#define pr_fmt(fmt) "[taskgraph] " fmt
#include "klog.h"

#include "ipc/taskgraph.h"
#include "ipc/cap_token.h"
#include "syscall/syscall.h"
#include "sync/spinlock.h"
#include "errno.h"

static workflow_task_t tasks[MAX_TASKS];
static uint32_t next_task_id = 1;

/* Lock order: tier 5 (subsystem). See klog.h for full hierarchy */
static spinlock_t taskgraph_lock = SPINLOCK_INIT;

static workflow_task_t *find_task(uint32_t id) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].used && tasks[i].id == id)
            return &tasks[i];
    return NULL;
}

int taskgraph_create(const char *name, uint32_t ns_id, uint64_t owner_pid) {
    uint64_t flags;
    spin_lock_irqsave(&taskgraph_lock, &flags);

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
            int id = (int)tasks[i].id;
            spin_unlock_irqrestore(&taskgraph_lock, flags);
            return id;
        }
    }

    spin_unlock_irqrestore(&taskgraph_lock, flags);
    pr_err("task table full\n");
    return -ENOMEM;
}

int taskgraph_depend(uint32_t task_id, uint32_t dep_id,
                     uint64_t caller_pid, uint32_t caller_caps) {
    uint64_t flags;
    spin_lock_irqsave(&taskgraph_lock, &flags);

    workflow_task_t *t = find_task(task_id);
    if (!t) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -ENOENT; }
    if (t->owner_pid != caller_pid) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -EPERM; }
    if (t->status != TASK_PENDING) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -EINVAL; }

    /* Verify dep exists */
    workflow_task_t *dep = find_task(dep_id);
    if (!dep) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -ENOENT; }

    /* Cross-namespace dependency requires CAP_XNS_TASK */
    if (dep->ns_id != t->ns_id) {
        uint32_t dep_ns = dep->ns_id;
        spin_unlock_irqrestore(&taskgraph_lock, flags);

        /* Check capability or token */
        if (!(caller_caps & CAP_XNS_TASK)) {
            char res[16];
            res[0] = 'n'; res[1] = 's'; res[2] = '/';
            /* Convert dep_ns to string */
            if (dep_ns < 10) {
                res[3] = '0' + (char)dep_ns;
                res[4] = '\0';
            } else {
                res[3] = '0' + (char)(dep_ns / 10);
                res[4] = '0' + (char)(dep_ns % 10);
                res[5] = '\0';
            }
            if (!cap_token_check(caller_pid, CAP_XNS_TASK, res))
                return -EPERM;
        }

        /* Re-acquire lock and re-validate */
        spin_lock_irqsave(&taskgraph_lock, &flags);
        t = find_task(task_id);
        dep = find_task(dep_id);
        if (!t || !dep) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -ENOENT; }
        if (t->owner_pid != caller_pid) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -EPERM; }
        if (t->status != TASK_PENDING) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -EINVAL; }
    }

    /* Prevent self-dependency */
    if (task_id == dep_id) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -EINVAL; }

    if (t->dep_count >= MAX_TASK_DEPS) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -ENOMEM; }

    /* Prevent duplicate */
    for (int i = 0; i < t->dep_count; i++) {
        if (t->deps[i] == dep_id) {
            spin_unlock_irqrestore(&taskgraph_lock, flags);
            return 0; /* already added */
        }
    }

    t->deps[t->dep_count++] = dep_id;
    spin_unlock_irqrestore(&taskgraph_lock, flags);
    return 0;
}

int taskgraph_start(uint32_t task_id, uint64_t caller_pid) {
    uint64_t flags;
    spin_lock_irqsave(&taskgraph_lock, &flags);

    workflow_task_t *t = find_task(task_id);
    if (!t) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -ENOENT; }
    if (t->status != TASK_PENDING) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -EINVAL; }

    /* Check all dependencies are DONE */
    for (int i = 0; i < t->dep_count; i++) {
        workflow_task_t *dep = find_task(t->deps[i]);
        if (!dep || dep->status != TASK_DONE) {
            spin_unlock_irqrestore(&taskgraph_lock, flags);
            return -EAGAIN; /* deps not ready */
        }
    }

    t->status = TASK_RUNNING;
    t->worker_pid = caller_pid;
    spin_unlock_irqrestore(&taskgraph_lock, flags);
    return 0;
}

int taskgraph_complete(uint32_t task_id, int32_t result, uint64_t caller_pid) {
    uint64_t flags;
    spin_lock_irqsave(&taskgraph_lock, &flags);

    workflow_task_t *t = find_task(task_id);
    if (!t) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -ENOENT; }
    /* Owner or worker can complete */
    if (t->owner_pid != caller_pid && t->worker_pid != caller_pid) {
        spin_unlock_irqrestore(&taskgraph_lock, flags);
        return -EPERM;
    }

    t->status = (result >= 0) ? TASK_DONE : TASK_FAILED;
    t->result = result;
    spin_unlock_irqrestore(&taskgraph_lock, flags);
    return 0;
}

int taskgraph_status(uint32_t task_id, task_status_t *out) {
    uint64_t flags;
    spin_lock_irqsave(&taskgraph_lock, &flags);

    workflow_task_t *t = find_task(task_id);
    if (!t) { spin_unlock_irqrestore(&taskgraph_lock, flags); return -ENOENT; }

    out->id = t->id;
    out->status = t->status;
    out->dep_count = t->dep_count;
    out->pad[0] = out->pad[1] = 0;
    out->result = t->result;
    for (int i = 0; i < MAX_TASK_DEPS; i++)
        out->deps[i] = t->deps[i];
    for (int i = 0; i < TASK_NAME_MAX; i++)
        out->name[i] = t->name[i];

    spin_unlock_irqrestore(&taskgraph_lock, flags);
    return 0;
}

int taskgraph_is_done(uint32_t task_id) {
    uint64_t flags;
    spin_lock_irqsave(&taskgraph_lock, &flags);

    workflow_task_t *t = find_task(task_id);
    int done = !t || (t->status == TASK_DONE || t->status == TASK_FAILED);

    spin_unlock_irqrestore(&taskgraph_lock, flags);
    return done;
}

void taskgraph_cleanup_pid(uint64_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&taskgraph_lock, &flags);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && tasks[i].owner_pid == pid)
            tasks[i].used = 0;
    }

    spin_unlock_irqrestore(&taskgraph_lock, flags);
}
