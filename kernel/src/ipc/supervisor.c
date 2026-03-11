#define pr_fmt(fmt) "[super] " fmt
#include "klog.h"

#include "ipc/supervisor.h"
#include "proc/process.h"
#include "proc/elf.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "serial.h"
#include "mm/kheap.h"
#include "sync/spinlock.h"

static supervisor_t supervisors[MAX_SUPERVISORS];

/* Lock order: tier 5 (subsystem). See klog.h for full hierarchy */
static spinlock_t supervisor_lock = SPINLOCK_INIT;

void supervisor_init(void) {
    for (int i = 0; i < MAX_SUPERVISORS; i++) {
        supervisors[i].used = 0;
        supervisors[i].id = (uint32_t)i;
    }
}

int supervisor_create(uint64_t owner_pid, const char *name) {
    uint64_t flags;
    spin_lock_irqsave(&supervisor_lock, &flags);

    for (int i = 0; i < MAX_SUPERVISORS; i++) {
        if (!supervisors[i].used) {
            supervisors[i].used = 1;
            supervisors[i].owner_pid = owner_pid;
            supervisors[i].policy = SUPER_ONE_FOR_ONE;
            supervisors[i].child_count = 0;
            for (int j = 0; j < SUPER_NAME_MAX - 1 && name[j]; j++)
                supervisors[i].name[j] = name[j];
            supervisors[i].name[SUPER_NAME_MAX - 1] = '\0';
            for (int j = 0; j < MAX_SUPER_CHILDREN; j++)
                supervisors[i].children[j].used = 0;
            spin_unlock_irqrestore(&supervisor_lock, flags);
            return i;
        }
    }

    spin_unlock_irqrestore(&supervisor_lock, flags);
    return -1;
}

int supervisor_add_child(uint32_t super_id, const char *elf_path,
                         int64_t ns_id, uint64_t caps) {
    uint64_t flags;
    spin_lock_irqsave(&supervisor_lock, &flags);

    if (super_id >= MAX_SUPERVISORS || !supervisors[super_id].used) {
        spin_unlock_irqrestore(&supervisor_lock, flags);
        return -1;
    }

    supervisor_t *sv = &supervisors[super_id];
    for (int i = 0; i < MAX_SUPER_CHILDREN; i++) {
        if (!sv->children[i].used) {
            sv->children[i].used = 1;
            sv->children[i].ns_id = ns_id;
            sv->children[i].caps = caps;
            sv->children[i].pid = 0;
            for (int j = 0; j < 63 && elf_path[j]; j++)
                sv->children[i].elf_path[j] = elf_path[j];
            sv->children[i].elf_path[63] = '\0';
            sv->child_count++;
            spin_unlock_irqrestore(&supervisor_lock, flags);
            return i;
        }
    }

    spin_unlock_irqrestore(&supervisor_lock, flags);
    return -1;
}

int supervisor_set_policy(uint32_t super_id, uint8_t policy) {
    uint64_t flags;
    spin_lock_irqsave(&supervisor_lock, &flags);

    if (super_id >= MAX_SUPERVISORS || !supervisors[super_id].used) {
        spin_unlock_irqrestore(&supervisor_lock, flags);
        return -1;
    }
    if (policy > SUPER_ONE_FOR_ALL) {
        spin_unlock_irqrestore(&supervisor_lock, flags);
        return -1;
    }
    supervisors[super_id].policy = policy;

    spin_unlock_irqrestore(&supervisor_lock, flags);
    return 0;
}

/* Load ELF from VFS path and create a process */
static process_t *supervisor_load_elf(const char *path) {
    int node_idx = vfs_open(path);
    if (node_idx < 0) return NULL;

    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0) return NULL;

    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf) return NULL;

    int64_t n = vfs_read(node_idx, 0, buf, st.size);
    if (n != (int64_t)st.size) { kfree(buf); return NULL; }

    process_t *proc = process_create_from_elf(buf, st.size);
    kfree(buf);
    if (proc) {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') base = p + 1;
        int ni = 0;
        while (base[ni] && ni < 31) { proc->name[ni] = base[ni]; ni++; }
        proc->name[ni] = '\0';
    }
    return proc;
}

/* Restart a single child spec by loading its ELF.
 * MUST be called WITHOUT supervisor_lock held (does I/O). */
static void supervisor_restart_child(supervisor_t *sv, int child_idx) {
    super_child_t *ch = &sv->children[child_idx];
    if (!ch->used) return;

    process_t *proc = supervisor_load_elf(ch->elf_path);
    if (!proc) {
        pr_err("Failed to load: %s\n", ch->elf_path);
        return;
    }

    proc->parent_pid = sv->owner_pid;
    proc->capabilities = (uint32_t)ch->caps;
    ch->pid = proc->pid;
    sched_add(proc->main_thread);
    pr_info("Restarted %s as pid %lu\n",
                  ch->elf_path, proc->pid);
}

int supervisor_start(uint32_t super_id) {
    uint64_t flags;
    spin_lock_irqsave(&supervisor_lock, &flags);

    if (super_id >= MAX_SUPERVISORS || !supervisors[super_id].used) {
        spin_unlock_irqrestore(&supervisor_lock, flags);
        return -1;
    }
    supervisor_t *sv = &supervisors[super_id];

    /* Collect children that need starting */
    int to_start[MAX_SUPER_CHILDREN];
    int start_count = 0;
    for (int i = 0; i < MAX_SUPER_CHILDREN; i++) {
        if (sv->children[i].used && sv->children[i].pid == 0)
            to_start[start_count++] = i;
    }

    spin_unlock_irqrestore(&supervisor_lock, flags);

    /* Do I/O (ELF loading) outside the lock */
    int launched = 0;
    for (int i = 0; i < start_count; i++) {
        supervisor_restart_child(sv, to_start[i]);
        launched++;
    }
    return launched;
}

void supervisor_on_exit(uint64_t pid, int exit_status) {
    if (exit_status == 0) return;  /* clean exit, no restart */

    for (int i = 0; i < MAX_SUPERVISORS; i++) {
        uint64_t flags;
        spin_lock_irqsave(&supervisor_lock, &flags);

        if (!supervisors[i].used) {
            spin_unlock_irqrestore(&supervisor_lock, flags);
            continue;
        }
        supervisor_t *sv = &supervisors[i];

        for (int j = 0; j < MAX_SUPER_CHILDREN; j++) {
            if (!sv->children[j].used) continue;
            if (sv->children[j].pid != pid) continue;

            pr_warn("Child pid %lu crashed (status %d), policy=%d\n",
                    pid, exit_status, sv->policy);

            if (sv->policy == SUPER_ONE_FOR_ONE) {
                spin_unlock_irqrestore(&supervisor_lock, flags);
                supervisor_restart_child(sv, j);
                return;
            } else {
                /* ONE_FOR_ALL: kill others, then restart all */
                /* Collect pids to kill and children to restart */
                uint64_t kill_pids[MAX_SUPER_CHILDREN];
                int kill_count = 0;
                int restart[MAX_SUPER_CHILDREN];
                int restart_count = 0;

                for (int k = 0; k < MAX_SUPER_CHILDREN; k++) {
                    if (sv->children[k].used && sv->children[k].pid != pid) {
                        kill_pids[kill_count++] = sv->children[k].pid;
                    }
                    if (sv->children[k].used) {
                        restart[restart_count++] = k;
                    }
                }

                spin_unlock_irqrestore(&supervisor_lock, flags);

                /* Kill outside lock */
                for (int k = 0; k < kill_count; k++) {
                    process_t *other = process_lookup(kill_pids[k]);
                    if (other && !other->exited)
                        process_deliver_signal(other, SIGKILL);
                }
                /* Restart outside lock */
                for (int k = 0; k < restart_count; k++) {
                    supervisor_restart_child(sv, restart[k]);
                }
                return;
            }
        }

        spin_unlock_irqrestore(&supervisor_lock, flags);
    }
}
