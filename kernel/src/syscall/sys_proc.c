#include "syscall/syscall_internal.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "proc/elf.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "pty/pty.h"
#include "ipc/unix_sock.h"
#include "ipc/eventfd.h"
#include "ipc/epoll.h"
#include "ipc/uring.h"
#include "ipc/agent_reg.h"
#include "ipc/cap_token.h"
#include "ipc/agent_ns.h"
#include "ipc/infer_svc.h"
#include "ipc/taskgraph.h"
#include "ipc/supervisor.h"
#include "net/tcp.h"
#include "serial.h"

int64_t sys_exit(uint64_t status, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (t->process) {
        t->process->exit_status = (int64_t)status;
        /* Close all open file descriptors */
        for (int i = 0; i < MAX_FDS; i++) {
            fd_entry_t *entry = &t->process->fd_table[i];
            if (entry->pipe != NULL) {
                uint64_t pflags;
                pipe_lock_acquire(&pflags);
                pipe_t *pp = (pipe_t *)entry->pipe;
                if (entry->pipe_write) {
                    if (pp->write_refs > 0) pp->write_refs--;
                    if (pp->write_refs == 0) pp->closed_write = 1;
                } else {
                    if (pp->read_refs > 0) pp->read_refs--;
                    if (pp->read_refs == 0) pp->closed_read = 1;
                }
                if (pp->closed_read && pp->closed_write) pp->used = 0;
                pipe_unlock_release(pflags);
                entry->pipe = NULL;
                entry->pipe_write = 0;
            }
            if (entry->pty != NULL) {
                int pty_idx = pty_index((pty_t *)entry->pty);
                if (pty_idx >= 0) {
                    if (entry->pty_is_master)
                        pty_close_master(pty_idx);
                    else
                        pty_close_slave(pty_idx);
                }
                entry->pty = NULL;
                entry->pty_is_master = 0;
            }
            if (entry->unix_sock != NULL) {
                unix_sock_close((unix_sock_t *)entry->unix_sock);
                entry->unix_sock = NULL;
            }
            if (entry->eventfd != NULL) {
                /* Find index for close */
                for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
                    if (eventfd_get(ei) == (eventfd_t *)entry->eventfd) {
                        eventfd_close(ei);
                        break;
                    }
                }
                entry->eventfd = NULL;
            }
            if (entry->epoll != NULL) {
                int ep_idx = epoll_index((epoll_instance_t *)entry->epoll);
                if (ep_idx >= 0)
                    epoll_close(ep_idx);
                entry->epoll = NULL;
            }
            if (entry->uring != NULL) {
                int ur_idx = uring_index((uring_instance_t *)entry->uring);
                if (ur_idx >= 0)
                    uring_close(ur_idx);
                entry->uring = NULL;
            }
            entry->node = NULL;
            entry->offset = 0;
            entry->open_flags = 0;
            entry->fd_flags = 0;
        }
        /* Close any owned TCP connections */
        for (int i = 0; i < 8; i++) {
            if (t->process->tcp_conns[i]) {
                tcp_close(i);
                t->process->tcp_conns[i] = 0;
            }
        }
        /* Unregister agent entries for dying process */
        agent_unregister_pid(t->process->pid);
        /* Cleanup capability tokens owned by or targeting this process */
        cap_token_cleanup_pid(t->process->pid);
        /* Cleanup agent namespaces owned by this process */
        agent_ns_cleanup_pid(t->process->pid);
        /* Cleanup workflow tasks owned by this process */
        taskgraph_cleanup_pid(t->process->pid);
        /* Decrement namespace process count */
        agent_ns_quota_adjust(t->process->ns_id, NS_QUOTA_PROCS, -1);
        /* Unregister inference services for dying process */
        infer_unregister_pid(t->process->pid);
        /* Cleanup async inference slots owned by dying process */
        infer_async_cleanup_pid(t->process->pid);
        /* Detach shared memory regions */
        {
            uint64_t sflags;
            shm_lock_acquire(&sflags);
            for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
                if (t->process->mmap_table[i].used &&
                    t->process->mmap_table[i].shm_id >= 0) {
                    int32_t sid = t->process->mmap_table[i].shm_id;
                    if (sid < MAX_SHM_REGIONS && shm_table[sid].ref_count > 0)
                        shm_table[sid].ref_count--;
                    t->process->mmap_table[i].used = 0;
                    t->process->mmap_table[i].shm_id = -1;
                }
            }
            shm_unlock_release(sflags);
        }
        /* Free user-space pages (respects COW refcounts) */
        if (t->process->cr3) {
            /* Switch to kernel PML4 before freeing */
            __asm__ volatile ("mov %0, %%cr3" : : "r"(vmm_get_kernel_pml4()) : "memory");
            vmm_free_user_pages(t->process->cr3);
            t->process->cr3 = 0;
        }
        /* Notify supervisors about exit */
        supervisor_on_exit(t->process->pid, (int)status);
        /* Deliver SIGCHLD to parent if parent has a user handler.
         * Only set pending bit — actual delivery happens on kernel→user return.
         * Guard: parent must exist, not exited, and have a real handler (not DFL/IGN).
         * We directly set the pending bit instead of calling process_deliver_signal
         * to avoid any side effects from the signal dispatch switch statement. */
        if (t->process->parent_pid != 0) {
            process_t *parent = process_lookup(t->process->parent_pid);
            if (parent && !parent->exited &&
                parent->sig_handlers[SIGCHLD].sa_handler > SIG_IGN)
                process_deliver_signal(parent, SIGCHLD);
        }
    }
    serial_printf("[proc] Process exited with status %lu\n", status);
    /* CRITICAL: Disable interrupts from here until thread_exit().
     * After cr3 is zeroed above, if a timer interrupt preempts us and the
     * scheduler context-switches to another thread, when we're later
     * re-scheduled, do_switch() would load CR3=0 from process->cr3,
     * causing a triple fault (looks like a hang with -no-reboot).
     * Disabling interrupts ensures we reach thread_exit() atomically,
     * which sets THREAD_DEAD so the scheduler won't re-enqueue us. */
    __asm__ volatile ("cli");
    /* Mark process as exited and wake any waitpid waiter BEFORE thread_exit.
     * Save wait_thread before setting exited=1, because after exited=1 the
     * waiter may kfree the process at any time. */
    if (t->process) {
        thread_t *waiter = t->process->wait_thread;
        t->process->wait_thread = NULL;
        __asm__ volatile ("" ::: "memory");  /* compiler barrier */
        t->process->exited = 1;
        t->process = NULL;
        /* Wake the waiter. sched_wake acquires rq_lock with irqsave,
         * which is safe even with interrupts disabled (cli). */
        if (waiter)
            sched_wake(waiter);
    }
    thread_exit();
    /* Never returns */
    return 0;
}

int64_t sys_exec(uint64_t path_ptr, uint64_t argv_ptr,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    /* Check CAP_EXEC (with token fallback) */
    if (!(proc->capabilities & CAP_EXEC) &&
        !cap_token_check(proc->pid, CAP_EXEC, path))
        return -EACCES;

    /* Open the file in VFS */
    int node_idx = vfs_open(path);
    if (node_idx < 0)
        return -1;

    /* Check exec permission */
    vfs_node_t *exec_node = vfs_get_node(node_idx);
    if (exec_node && !(exec_node->mode & VFS_PERM_EXEC))
        return -1;

    /* Get file size */
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0)
        return -1;

    if (st.size == 0)
        return -1;

    /* Read entire file into kernel buffer */
    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf)
        return -1;

    int64_t n = vfs_read(node_idx, 0, buf, st.size);
    if (n != (int64_t)st.size) {
        kfree(buf);
        return -1;
    }

    /* Copy argv from parent's user space */
    int argc = 0;
    char argv_buf[PROC_ARGV_BUF_SIZE];
    int buf_pos = 0;

    if (argv_ptr != 0 && validate_user_ptr(argv_ptr, 8) == 0) {
        const char **user_argv = (const char **)argv_ptr;
        while (argc < PROC_MAX_ARGS) {
            /* Validate pointer to the argv entry */
            if (validate_user_ptr((uint64_t)&user_argv[argc], 8) != 0)
                break;
            const char *arg = user_argv[argc];
            if (arg == NULL)
                break;
            /* Copy string from user space */
            char tmp[128];
            if (copy_string_from_user(arg, tmp, sizeof(tmp)) != 0)
                break;
            int slen = 0;
            while (tmp[slen]) slen++;
            if (buf_pos + slen + 1 > PROC_ARGV_BUF_SIZE)
                break;
            for (int j = 0; j <= slen; j++)
                argv_buf[buf_pos + j] = tmp[j];
            buf_pos += slen + 1;
            argc++;
        }
    }

    /* Create process from ELF */
    process_t *child = process_create_from_elf(buf, st.size);
    kfree(buf);

    if (!child)
        return -1;

    /* Set process name from path (strip directory and .elf extension) */
    {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') base = p + 1;
        int ni = 0;
        while (base[ni] && ni < 31) { child->name[ni] = base[ni]; ni++; }
        child->name[ni] = '\0';
        /* Strip .elf suffix */
        if (ni > 4 && child->name[ni-4] == '.' && child->name[ni-3] == 'e' &&
            child->name[ni-2] == 'l' && child->name[ni-1] == 'f')
            child->name[ni-4] = '\0';
    }

    /* Set argv on child */
    child->argc = argc;
    child->argv_buf_len = buf_pos;
    for (int i = 0; i < buf_pos; i++)
        child->argv_buf[i] = argv_buf[i];

    /* Inherit parent's environment */
    child->env_count = proc->env_count;
    child->env_buf_len = proc->env_buf_len;
    for (int i = 0; i < proc->env_buf_len; i++)
        child->env_buf[i] = proc->env_buf[i];

    /* Inherit parent's file descriptors (with FD_CLOEXEC support) */
    for (int i = 0; i < MAX_FDS; i++) {
        if (proc->fd_table[i].fd_flags & FD_CLOEXEC) {
            /* Don't inherit — clear child's entry */
            child->fd_table[i].node = NULL;
            child->fd_table[i].pipe = NULL;
            child->fd_table[i].pipe_write = 0;
            child->fd_table[i].pty = NULL;
            child->fd_table[i].pty_is_master = 0;
            child->fd_table[i].unix_sock = NULL;
            child->fd_table[i].eventfd = NULL;
            child->fd_table[i].epoll = NULL;
            child->fd_table[i].uring = NULL;
            child->fd_table[i].open_flags = 0;
            child->fd_table[i].fd_flags = 0;
            continue;
        }
        child->fd_table[i] = proc->fd_table[i];
        child->fd_table[i].fd_flags = 0;  /* clear cloexec in child */
        /* Increment pipe ref counts for inherited pipe fds */
        if (proc->fd_table[i].pipe != NULL) {
            uint64_t pflags;
            pipe_lock_acquire(&pflags);
            pipe_t *pp = (pipe_t *)proc->fd_table[i].pipe;
            if (proc->fd_table[i].pipe_write)
                pp->write_refs++;
            else
                pp->read_refs++;
            pipe_unlock_release(pflags);
        }
        /* Increment PTY ref counts for inherited pty fds */
        if (proc->fd_table[i].pty != NULL) {
            pty_t *pt = (pty_t *)proc->fd_table[i].pty;
            if (proc->fd_table[i].pty_is_master)
                pt->master_refs++;
            else
                pt->slave_refs++;
        }
        /* Increment unix socket ref counts */
        if (proc->fd_table[i].unix_sock != NULL) {
            unix_sock_t *us = (unix_sock_t *)proc->fd_table[i].unix_sock;
            us->refs++;
        }
        /* Increment eventfd ref counts */
        if (proc->fd_table[i].eventfd != NULL) {
            for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
                if (eventfd_get(ei) == (eventfd_t *)proc->fd_table[i].eventfd) {
                    eventfd_ref(ei);
                    break;
                }
            }
        }
        /* Increment epoll ref counts */
        if (proc->fd_table[i].epoll != NULL) {
            int ep_idx = epoll_index((epoll_instance_t *)proc->fd_table[i].epoll);
            if (ep_idx >= 0) epoll_ref(ep_idx);
        }
        /* Increment uring ref counts */
        if (proc->fd_table[i].uring != NULL) {
            int ur_idx = uring_index((uring_instance_t *)proc->fd_table[i].uring);
            if (ur_idx >= 0) uring_ref(ur_idx);
        }
    }

    /* Schedule the child AFTER all setup (argv, env, fd table) is complete.
     * This prevents SMP races where another CPU runs the child before
     * fd inheritance is done, causing pipe/pty ref count mismatches. */
    sched_add(child->main_thread);

    return (int64_t)child->pid;
}

int64_t sys_fork(uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* Namespace process quota check */
    if (!agent_ns_quota_check(proc->ns_id, NS_QUOTA_PROCS, 1))
        return -ENOMEM;

    /* Read parent's saved user context from kernel stack.
     * Layout (from syscall_entry.asm, high to low):
     *   kstack_top[-1] = user RSP
     *   kstack_top[-2] = user RIP (RCX)
     *   kstack_top[-3] = user RFLAGS (R11)
     *   kstack_top[-4] = RBP
     *   kstack_top[-5] = RBX
     *   kstack_top[-6] = R12
     *   kstack_top[-7] = R13
     *   kstack_top[-8] = R14
     *   kstack_top[-9] = R15
     */
    uint64_t *kstack_top = (uint64_t *)(t->stack_base + t->stack_size);
    fork_context_t ctx = {
        .rip    = kstack_top[-2],
        .rsp    = kstack_top[-1],
        .rflags = kstack_top[-3],
        .rbp    = kstack_top[-4],
        .rbx    = kstack_top[-5],
        .r12    = kstack_top[-6],
        .r13    = kstack_top[-7],
        .r14    = kstack_top[-8],
        .r15    = kstack_top[-9],
    };

    process_t *child = process_fork(proc, &ctx);
    if (!child) return -1;

    /* Increment pipe, PTY, unix_sock, eventfd, epoll, uring ref counts for inherited fds */
    for (int i = 0; i < MAX_FDS; i++) {
        if (proc->fd_table[i].pipe != NULL) {
            uint64_t pflags;
            pipe_lock_acquire(&pflags);
            pipe_t *pp = (pipe_t *)proc->fd_table[i].pipe;
            if (proc->fd_table[i].pipe_write)
                pp->write_refs++;
            else
                pp->read_refs++;
            pipe_unlock_release(pflags);
        }
        if (proc->fd_table[i].pty != NULL) {
            pty_t *pt = (pty_t *)proc->fd_table[i].pty;
            if (proc->fd_table[i].pty_is_master)
                pt->master_refs++;
            else
                pt->slave_refs++;
        }
        if (proc->fd_table[i].unix_sock != NULL)
            ((unix_sock_t *)proc->fd_table[i].unix_sock)->refs++;
        if (proc->fd_table[i].eventfd != NULL) {
            for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
                if (eventfd_get(ei) == (eventfd_t *)proc->fd_table[i].eventfd) {
                    eventfd_ref(ei);
                    break;
                }
            }
        }
        if (proc->fd_table[i].epoll != NULL) {
            int ep_idx = epoll_index((epoll_instance_t *)proc->fd_table[i].epoll);
            if (ep_idx >= 0) epoll_ref(ep_idx);
        }
        if (proc->fd_table[i].uring != NULL) {
            int ur_idx = uring_index((uring_instance_t *)proc->fd_table[i].uring);
            if (ur_idx >= 0) uring_ref(ur_idx);
        }
    }

    /* Increment shm ref counts */
    {
        uint64_t sflags;
        shm_lock_acquire(&sflags);
        for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
            if (proc->mmap_table[i].used && proc->mmap_table[i].shm_id >= 0) {
                int32_t sid = proc->mmap_table[i].shm_id;
                if (sid < MAX_SHM_REGIONS)
                    shm_table[sid].ref_count++;
            }
        }
        shm_unlock_release(sflags);
    }

    /* Schedule the child AFTER all ref counts are incremented.
     * This prevents SMP races where the child exits before ref
     * counts reflect the inherited fds/shm. */
    sched_add(child->main_thread);

    /* Adjust namespace quota after successful fork */
    agent_ns_quota_adjust(proc->ns_id, NS_QUOTA_PROCS, 1);

    return (int64_t)child->pid;
}

int64_t sys_waitpid(uint64_t pid, uint64_t flags,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    process_t *child = process_lookup(pid);
    if (!child)
        return -1;
    if ((flags & WNOHANG) && !child->exited)
        return 0;
    thread_t *wt = thread_get_current();
    process_t *wproc = wt ? wt->process : NULL;
    while (!child->exited) {
        /* Check for interrupting signals, but filter out SIGCHLD —
         * waitpid is logically waiting for child state changes, so
         * SIGCHLD should not cause -EINTR here (matches POSIX). */
        if (wproc) {
            uint32_t interruptible = (wproc->pending_signals & ~wproc->signal_mask)
                                     & ~(1U << SIGCHLD);
            if (interruptible)
                return -EINTR;
        }
        /* Register as waiter and block instead of spin-yielding.
         * Double-check pattern: set wait_thread, barrier, re-check exited. */
        child->wait_thread = wt;
        __asm__ volatile ("" ::: "memory");  /* compiler barrier */
        if (child->exited) {
            child->wait_thread = NULL;
            break;
        }
        sched_block(wt);
        child->wait_thread = NULL;
        /* Re-check signals after wake */
        if (wproc) {
            uint32_t interruptible = (wproc->pending_signals & ~wproc->signal_mask)
                                     & ~(1U << SIGCHLD);
            if (interruptible)
                return -EINTR;
        }
    }
    int64_t status = child->exit_status;
    process_unregister(pid);
    kfree(child);
    return status;
}

int64_t sys_getpid(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t->process) return -1;
    return (int64_t)t->process->pid;
}

int64_t sys_kill(uint64_t pid, uint64_t signal,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *caller = t ? t->process : NULL;

    /* Negative pid: kill process group */
    if ((int64_t)pid < 0) {
        uint64_t pgid = (uint64_t)(-(int64_t)pid);
        return process_kill_group(pgid, (int)signal);
    }

    process_t *target = process_lookup(pid);
    if (!target)
        return -1;

    /* Permission check: non-root cross-uid kill needs CAP_KILL */
    if (caller && caller->uid != 0 && caller->uid != target->uid &&
        !(caller->capabilities & CAP_KILL))
        return -EPERM;

    return process_deliver_signal(target, (int)signal);
}

int64_t sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *caller = t->process;
    if (!caller) return -1;

    uint64_t pid = pid_arg == 0 ? caller->pid : pid_arg;
    uint64_t pgid = pgid_arg == 0 ? pid : pgid_arg;
    process_t *proc = process_lookup(pid);
    if (!proc) return -1;
    proc->pgid = pgid;
    return 0;
}

int64_t sys_getpgid(uint64_t pid_arg, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *caller = t->process;
    if (!caller) return -1;

    uint64_t pid = pid_arg == 0 ? caller->pid : pid_arg;
    process_t *proc = process_lookup(pid);
    if (!proc) return -1;
    return (int64_t)proc->pgid;
}

int64_t sys_procinfo(uint64_t index, uint64_t buf_ptr,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(buf_ptr, 56) != 0) return -1;

    /* Find the index-th active process */
    extern process_t *proc_table_get(int idx);
    int count = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        process_t *p = proc_table_get(i);
        if (!p || p->exited) continue;
        if (count == (int)index) {
            uint8_t *out = (uint8_t *)buf_ptr;
            *(uint64_t *)(out + 0)  = p->pid;
            *(uint64_t *)(out + 8)  = p->parent_pid;
            /* state: 0=running, 1=stopped */
            uint32_t state = 0;
            if (p->main_thread && p->main_thread->state == THREAD_STOPPED)
                state = 1;
            *(uint32_t *)(out + 16) = state;
            *(uint16_t *)(out + 20) = p->uid;
            *(uint16_t *)(out + 22) = p->gid;
            for (int j = 0; j < 32; j++)
                out[24 + j] = (uint8_t)p->name[j];
            return 0;
        }
        count++;
    }
    return -1;  /* no more processes */
}
