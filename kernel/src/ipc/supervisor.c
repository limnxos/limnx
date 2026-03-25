#define pr_fmt(fmt) "[super] " fmt
#include "klog.h"

#include "ipc/supervisor.h"
#include "proc/process.h"
#include "errno.h"
#include "proc/elf.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "arch/serial.h"
#include "mm/kheap.h"
#include "sync/spinlock.h"
#include "arch/timer.h"
#include "pty/pty.h"

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
            supervisors[i].restart_window_start = 0;
            supervisors[i].restart_count = 0;
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
    proc->daemon = 1;
    proc->ns_id = ch->ns_id;

    /* Set up fd 0/1/2 from the console PTY so child has stdin/stdout */
    int con = pty_get_console();
    if (con >= 0) {
        pty_t *pt = pty_get(con);
        if (pt) {
            for (int fd = 0; fd < 3; fd++) {
                proc->fd_table[fd].node = NULL;
                proc->fd_table[fd].pipe = NULL;
                proc->fd_table[fd].pipe_write = 0;
                proc->fd_table[fd].pty = (void *)pt;
                proc->fd_table[fd].pty_is_master = 0;
                proc->fd_table[fd].unix_sock = NULL;
                proc->fd_table[fd].eventfd = NULL;
                proc->fd_table[fd].epoll = NULL;
                proc->fd_table[fd].uring = NULL;
                proc->fd_table[fd].open_flags = 2; /* O_RDWR */
                proc->fd_table[fd].fd_flags = 0;
                pt->slave_refs++;
            }
        }
    }

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

int supervisor_list(super_info_t *buf, int max_count) {
    uint64_t flags;
    spin_lock_irqsave(&supervisor_lock, &flags);

    int count = 0;
    for (int i = 0; i < MAX_SUPERVISORS && count < max_count; i++) {
        if (!supervisors[i].used) continue;
        supervisor_t *sv = &supervisors[i];
        super_info_t *out = &buf[count];
        out->id = sv->id;
        out->used = 1;
        out->policy = sv->policy;
        out->child_count = sv->child_count;
        out->owner_pid = sv->owner_pid;
        out->restart_count = sv->restart_count;
        for (int j = 0; j < SUPER_NAME_MAX; j++)
            out->name[j] = sv->name[j];
        for (int j = 0; j < MAX_SUPER_CHILDREN; j++)
            out->child_pids[j] = sv->children[j].used ? sv->children[j].pid : 0;
        count++;
    }

    spin_unlock_irqrestore(&supervisor_lock, flags);
    return count;
}

int supervisor_stop(uint32_t super_id, uint64_t caller_pid) {
    uint64_t flags;
    spin_lock_irqsave(&supervisor_lock, &flags);

    if (super_id >= MAX_SUPERVISORS || !supervisors[super_id].used) {
        spin_unlock_irqrestore(&supervisor_lock, flags);
        return -EINVAL;
    }

    supervisor_t *sv = &supervisors[super_id];
    if (sv->owner_pid != caller_pid) {
        spin_unlock_irqrestore(&supervisor_lock, flags);
        return -EACCES;
    }

    /* Mark unused first to prevent supervisor_on_exit from restarting */
    sv->used = 0;

    /* Collect child PIDs to kill */
    uint64_t kill_pids[MAX_SUPER_CHILDREN];
    int kill_count = 0;
    for (int i = 0; i < MAX_SUPER_CHILDREN; i++) {
        if (sv->children[i].used && sv->children[i].pid != 0)
            kill_pids[kill_count++] = sv->children[i].pid;
        sv->children[i].used = 0;
        sv->children[i].pid = 0;
    }
    sv->child_count = 0;

    spin_unlock_irqrestore(&supervisor_lock, flags);

    /* Kill children outside lock */
    for (int i = 0; i < kill_count; i++) {
        process_t *p = process_lookup(kill_pids[i]);
        if (p && !p->exited)
            process_deliver_signal(p, SIGKILL);
    }

    pr_info("Supervisor '%s' stopped (%d children killed)\n",
            sv->name, kill_count);
    return 0;
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

            /* Rate limit: prevent restart storms */
            {
                uint64_t now = arch_timer_get_ticks();
                if (now - sv->restart_window_start > SUPER_RESTART_WINDOW) {
                    sv->restart_count = 0;
                    sv->restart_window_start = now;
                }
                sv->restart_count++;
                if (sv->restart_count > SUPER_MAX_RESTARTS) {
                    pr_err("Restart storm: supervisor '%s' (%u restarts in window), halting restarts\n",
                           sv->name, sv->restart_count);
                    spin_unlock_irqrestore(&supervisor_lock, flags);
                    return;
                }
            }

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
