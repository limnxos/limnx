#define pr_fmt(fmt) "[syscall] " fmt
#include "klog.h"

#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "proc/process.h"
#include "proc/elf.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "net/net.h"
#include "net/tcp.h"
#include "idt/idt.h"
#include "pty/pty.h"
#include "ipc/unix_sock.h"
#include "ipc/eventfd.h"
#include "ipc/agent_reg.h"
#include "ipc/epoll.h"
#include "ipc/infer_svc.h"
#include "ipc/uring.h"
#include "ipc/cap_token.h"
#include "ipc/agent_ns.h"
#include "sync/futex.h"
#include "ipc/taskgraph.h"
#include "ipc/supervisor.h"
#include "mm/swap.h"
#include "blk/limnfs.h"
#include "smp/percpu.h"
#include "serial.h"

/* MSR addresses */
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Assembly entry point */
extern void syscall_entry(void);

/* --- Shared helpers (non-static, declared in syscall_internal.h) --- */

int validate_user_ptr(uint64_t ptr, uint64_t len) {
    if (ptr == 0 || ptr >= USER_ADDR_MAX)
        return -1;
    if (len > 0 && len > USER_ADDR_MAX - ptr)
        return -1;
    return 0;
}

int copy_string_from_user(const char *user_src, char *kern_dst,
                                  uint64_t max_len) {
    if ((uint64_t)user_src >= USER_ADDR_MAX)
        return -1;

    for (uint64_t i = 0; i < max_len - 1; i++) {
        if ((uint64_t)(user_src + i) >= USER_ADDR_MAX)
            return -1;
        kern_dst[i] = user_src[i];
        if (user_src[i] == '\0')
            return 0;
    }
    kern_dst[max_len - 1] = '\0';
    return 0;
}

/* Resolve user path (prepend cwd if relative) */
void resolve_user_path(process_t *proc, const char *path, char *out) {
    if (path[0] == '/') {
        /* Absolute path — copy as-is */
        int i = 0;
        while (path[i] && i < MAX_PATH - 1) {
            out[i] = path[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    /* Relative path — prepend cwd */
    int pos = 0;
    const char *cwd = proc->cwd;
    while (cwd[pos] && pos < MAX_PATH - 1) {
        out[pos] = cwd[pos];
        pos++;
    }

    /* Add separator if cwd doesn't end with '/' */
    if (pos > 0 && out[pos - 1] != '/' && pos < MAX_PATH - 1)
        out[pos++] = '/';

    /* Append relative path */
    int j = 0;
    while (path[j] && pos < MAX_PATH - 1)
        out[pos++] = path[j++];
    out[pos] = '\0';
}

/* Check if fd slot is free (no node, no pipe, no pty) */
int fd_is_free(const fd_entry_t *e) {
    return e->node == NULL && e->pipe == NULL && e->pty == NULL
        && e->unix_sock == NULL && e->eventfd == NULL
        && e->epoll == NULL && e->uring == NULL
        && e->tcp_conn_idx < 0;
}

/* --- Permission helper --- */

int check_file_perm(const process_t *proc, const vfs_node_t *node, uint8_t access) {
    if (proc->uid == 0) return 0;  /* root bypasses */
    uint16_t perm_bits;
    if (proc->uid == node->uid)
        perm_bits = (node->mode >> 6) & 7;
    else if (proc->gid == node->gid)
        perm_bits = (node->mode >> 3) & 7;
    else
        perm_bits = node->mode & 7;
    if ((access == O_RDONLY || access == O_RDWR) && !(perm_bits & 4))
        return -EACCES;
    if ((access == O_WRONLY || access == O_RDWR) && !(perm_bits & 2))
        return -EACCES;
    return 0;
}

/* Count open fds for a process */
int count_open_fds(process_t *proc) {
    int count = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fd_is_free(&proc->fd_table[i]))
            count++;
    }
    return count;
}

/* poll_check_fd — check readiness of a single fd for given events */
int16_t poll_check_fd(process_t *proc, int fd, int16_t events) {
    int16_t revents = 0;

    if (fd < 0 || fd >= MAX_FDS)
        return POLLERR;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (fd_is_free(entry))
        return POLLERR;

    /* Pipe */
    if (entry->pipe != NULL) {
        uint64_t pflags;
        pipe_lock_acquire(&pflags);
        pipe_t *pp = (pipe_t *)entry->pipe;
        if (!entry->pipe_write) {
            /* Read end */
            if ((events & POLLIN) && pp->count > 0)
                revents |= POLLIN;
            if (pp->closed_write)
                revents |= POLLHUP;
        } else {
            /* Write end */
            if ((events & POLLOUT) && pp->count < PIPE_BUF_SIZE)
                revents |= POLLOUT;
            if (pp->closed_read)
                revents |= POLLERR;
        }
        pipe_unlock_release(pflags);
        return revents;
    }

    /* PTY */
    if (entry->pty != NULL) {
        int pidx = pty_index((pty_t *)entry->pty);
        if (pidx < 0) return POLLERR;
        if ((events & POLLIN) && pty_readable(pidx, entry->pty_is_master))
            revents |= POLLIN;
        if ((events & POLLOUT) && pty_writable(pidx, entry->pty_is_master))
            revents |= POLLOUT;
        return revents;
    }

    /* Unix socket */
    if (entry->unix_sock != NULL) {
        unix_sock_t *us = (unix_sock_t *)entry->unix_sock;
        if (us->state == USOCK_LISTENING) {
            if ((events & POLLIN) && unix_sock_has_backlog(us))
                revents |= POLLIN;
        } else if (us->state == USOCK_CONNECTED) {
            if ((events & POLLIN) && unix_sock_readable(us))
                revents |= POLLIN;
            if ((events & POLLOUT) && unix_sock_writable(us))
                revents |= POLLOUT;
            if (us->peer_closed)
                revents |= POLLHUP;
        }
        return revents;
    }

    /* Eventfd */
    if (entry->eventfd != NULL) {
        int efd_idx = -1;
        for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
            if (eventfd_get(ei) == (eventfd_t *)entry->eventfd) {
                efd_idx = ei; break;
            }
        }
        if (efd_idx >= 0) {
            if ((events & POLLIN) && eventfd_readable(efd_idx))
                revents |= POLLIN;
            if (events & POLLOUT)
                revents |= POLLOUT;
        }
        return revents;
    }

    /* TCP connection fd */
    if (entry->tcp_conn_idx >= 0) {
        int tcp_events = tcp_poll(entry->tcp_conn_idx);
        return (int16_t)(tcp_events & events);
    }

    /* epoll/uring fds — not pollable */
    if (entry->epoll != NULL || entry->uring != NULL)
        return 0;

    /* Regular file — always ready */
    if (entry->node != NULL) {
        if (events & POLLIN) revents |= POLLIN;
        if (events & POLLOUT) revents |= POLLOUT;
        return revents;
    }

    return POLLERR;
}

/* --- Page fault handler --- */

int page_fault_handler(uint64_t fault_addr, uint64_t err_code,
                       interrupt_frame_t *frame) {
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;  /* Kernel fault */

    process_t *proc = t->process;

    int present = err_code & 1;
    int write   = err_code & 2;
    int user    = err_code & 4;

    if (!user) return -1;  /* Kernel-mode fault */

    if (present && write) {
        /* Page present but not writable — check COW */
        uint64_t *pte = vmm_get_pte(proc->cr3, fault_addr);
        if (pte && (*pte & PTE_COW)) {
            /* If page was not originally writable (mprotect RO), this is a
             * genuine protection fault — don't resolve COW */
            if (!(*pte & PTE_WAS_WRITABLE))
                goto kill;

            uint64_t old_phys = *pte & PTE_ADDR_MASK;
            uint64_t old_flags = *pte & ~PTE_ADDR_MASK;
            uint64_t clean_flags = old_flags & ~(PTE_COW | PTE_WAS_WRITABLE);

            if (pmm_ref_get(old_phys) == 1) {
                /* Last reference — just make writable */
                *pte = old_phys | (clean_flags | PTE_WRITABLE | PTE_PRESENT);
                invlpg_addr(fault_addr & ~0xFFFULL);
                return 0;
            }

            /* Multiple refs — copy the page */
            uint64_t new_phys = pmm_alloc_page();
            if (new_phys == 0) goto kill;

            uint8_t *src = (uint8_t *)PHYS_TO_VIRT(old_phys);
            uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(new_phys);
            for (int i = 0; i < 4096; i++) dst[i] = src[i];

            *pte = new_phys | (clean_flags | PTE_WRITABLE | PTE_PRESENT);
            invlpg_addr(fault_addr & ~0xFFFULL);

            pmm_ref_dec(old_phys);
            return 0;
        }
    }

    /* Demand paging / swap-in: page not present in user mode */
    if (!present && user) {
        /* Check swap entry first */
        uint64_t *pte = vmm_get_pte(proc->cr3, fault_addr);
        if (pte && swap_is_entry(*pte)) {
            if (swap_in(proc->cr3, fault_addr) == 0) {
                invlpg_addr(fault_addr & ~0xFFFULL);
                return 0;
            }
        }

        /* Check if addr is in a demand-paged mmap region */
        uint64_t page_addr = fault_addr & ~0xFFFULL;
        for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
            mmap_entry_t *me = &proc->mmap_table[i];
            if (!me->used || !me->demand) continue;
            uint64_t region_start = me->virt_addr;
            uint64_t region_end = region_start + (uint64_t)me->num_pages * 4096;
            if (page_addr >= region_start && page_addr < region_end) {
                if (me->vfs_node >= 0) {
                    /* File-backed demand page: allocate and fill from file */
                    uint64_t new_phys = pmm_alloc_page();
                    if (new_phys == 0) goto kill;
                    uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(new_phys);

                    uint64_t page_offset_in_region = page_addr - region_start;
                    uint64_t file_off = me->file_offset + page_offset_in_region;
                    int64_t got = vfs_read(me->vfs_node, file_off, dst, 4096);
                    if (got < 0) got = 0;
                    /* Zero-pad remainder (past EOF or short read) */
                    for (int64_t j = got; j < 4096; j++)
                        dst[j] = 0;

                    /* Map read-only (file-backed pages are read-only) */
                    if (vmm_map_page_in(proc->cr3, page_addr, new_phys,
                                        PTE_USER | PTE_NX) == 0) {
                        invlpg_addr(page_addr);
                        return 0;
                    }
                    pmm_free_page(new_phys);
                } else if (demand_page_fault(proc->cr3, page_addr) == 0) {
                    invlpg_addr(page_addr);
                    return 0;
                }
            }
        }
    }

kill:
    /* Detect stack overflow: fault in the guard page below the stack */
    {
        uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
        uint64_t guard_start = stack_bottom - PAGE_SIZE;
        if (fault_addr >= guard_start && fault_addr < stack_bottom) {
            serial_printf("[fault] Stack overflow detected (pid %lu, addr=%lx, rip=%lx)\n",
                proc->pid, fault_addr, frame->rip);
        } else {
            /* Check if fault is in an mmap guard gap */
            int in_guard_gap = 0;
            for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
                mmap_entry_t *me = &proc->mmap_table[i];
                if (!me->used) continue;
                uint64_t region_end = me->virt_addr + (uint64_t)me->num_pages * PAGE_SIZE;
                uint64_t gap_end = region_end + PAGE_SIZE;
                if (fault_addr >= region_end && fault_addr < gap_end) {
                    serial_printf("[fault] Guard page hit (pid %lu, addr=%lx past mmap %lx+%u, rip=%lx)\n",
                        proc->pid, fault_addr, me->virt_addr, me->num_pages, frame->rip);
                    in_guard_gap = 1;
                    break;
                }
            }
            if (!in_guard_gap)
                serial_printf("[fault] Process %lu killed: fault at %lx (err=%lx, rip=%lx)\n",
                    proc->pid, fault_addr, err_code, frame->rip);
        }
    }
    proc->exit_status = -11;  /* SIGSEGV */
    /* Switch to kernel address space BEFORE freeing user pages.
     * Without this, vmm_free_user_pages frees the page tables
     * we're currently running on, risking triple faults. */
    __asm__ volatile ("cli");
    __asm__ volatile ("mov %0, %%cr3" : : "r"(vmm_get_kernel_pml4()) : "memory");
    vmm_free_user_pages(proc->cr3);
    proc->cr3 = 0;
    t->state = THREAD_DEAD;
    schedule();
    return 0;
}

/* --- Dispatch table --- */

static syscall_fn_t syscall_table[SYS_NR] = {
    [SYS_WRITE]    = sys_write,
    [SYS_YIELD]    = sys_yield,
    [SYS_EXIT]     = sys_exit,
    [SYS_OPEN]     = sys_open,
    [SYS_READ]     = sys_read,
    [SYS_CLOSE]    = sys_close,
    [SYS_STAT]     = sys_stat,
    [SYS_EXEC]     = sys_exec,
    [SYS_SOCKET]   = sys_socket,
    [SYS_BIND]     = sys_bind,
    [SYS_SENDTO]   = sys_sendto,
    [SYS_RECVFROM] = sys_recvfrom,
    [SYS_FWRITE]   = sys_fwrite,
    [SYS_CREATE]   = sys_create,
    [SYS_UNLINK]   = sys_unlink,
    [SYS_MMAP]     = sys_mmap,
    [SYS_MUNMAP]   = sys_munmap,
    [SYS_GETCHAR]  = sys_getchar,
    [SYS_WAITPID]  = sys_waitpid,
    [SYS_PIPE]     = sys_pipe,
    [SYS_GETPID]   = sys_getpid,
    [SYS_FMMAP]    = sys_fmmap,
    [SYS_READDIR]  = sys_readdir,
    [SYS_MKDIR]    = sys_mkdir,
    [SYS_SEEK]     = sys_seek,
    [SYS_TRUNCATE] = sys_truncate,
    [SYS_CHDIR]    = sys_chdir,
    [SYS_GETCWD]   = sys_getcwd,
    [SYS_FSTAT]    = sys_fstat,
    [SYS_RENAME]   = sys_rename,
    [SYS_DUP]      = sys_dup,
    [SYS_DUP2]     = sys_dup2,
    [SYS_KILL]     = sys_kill,
    [SYS_FCNTL]    = sys_fcntl,
    [SYS_SETPGID]  = sys_setpgid,
    [SYS_GETPGID]  = sys_getpgid,
    [SYS_CHMOD]    = sys_chmod,
    [SYS_SHMGET]   = sys_shmget,
    [SYS_SHMAT]    = sys_shmat,
    [SYS_SHMDT]    = sys_shmdt,
    [SYS_FORK]     = sys_fork,
    [SYS_SIGACTION]  = sys_sigaction,
    [SYS_SIGRETURN]  = sys_sigreturn,
    [SYS_OPENPTY]    = sys_openpty,
    [SYS_TCP_SOCKET] = sys_tcp_socket,
    [SYS_TCP_CONNECT]= sys_tcp_connect,
    [SYS_TCP_LISTEN] = sys_tcp_listen,
    [SYS_TCP_ACCEPT] = sys_tcp_accept,
    [SYS_TCP_SEND]   = sys_tcp_send,
    [SYS_TCP_RECV]   = sys_tcp_recv,
    [SYS_TCP_CLOSE]  = sys_tcp_close,
    [SYS_IOCTL]      = sys_ioctl,
    [SYS_CLOCK_GETTIME] = sys_clock_gettime,
    [SYS_NANOSLEEP]  = sys_nanosleep,
    [SYS_GETENV]     = sys_getenv,
    [SYS_SETENV]     = sys_setenv,
    [SYS_POLL]       = sys_poll,
    [SYS_GETUID]     = sys_getuid,
    [SYS_SETUID]     = sys_setuid,
    [SYS_GETGID]     = sys_getgid,
    [SYS_SETGID]     = sys_setgid,
    [SYS_GETCAP]     = sys_getcap,
    [SYS_SETCAP]     = sys_setcap,
    [SYS_GETRLIMIT]  = sys_getrlimit,
    [SYS_SETRLIMIT]  = sys_setrlimit,
    [SYS_SECCOMP]    = sys_seccomp,
    [SYS_SETAUDIT]   = sys_setaudit,
    [SYS_UNIX_SOCKET]  = sys_unix_socket,
    [SYS_UNIX_BIND]    = sys_unix_bind,
    [SYS_UNIX_LISTEN]  = sys_unix_listen,
    [SYS_UNIX_ACCEPT]  = sys_unix_accept,
    [SYS_UNIX_CONNECT] = sys_unix_connect,
    [SYS_AGENT_REGISTER] = sys_agent_register,
    [SYS_AGENT_LOOKUP] = sys_agent_lookup,
    [SYS_EVENTFD]      = sys_eventfd,
    [SYS_EPOLL_CREATE]   = sys_epoll_create,
    [SYS_EPOLL_CTL]      = sys_epoll_ctl,
    [SYS_EPOLL_WAIT]     = sys_epoll_wait,
    [SYS_SWAP_STAT]      = sys_swap_stat,
    [SYS_INFER_REGISTER] = sys_infer_register,
    [SYS_INFER_REQUEST]  = sys_infer_request,
    [SYS_URING_SETUP]    = sys_uring_setup,
    [SYS_URING_ENTER]    = sys_uring_enter,
    [SYS_MMAP2]          = sys_mmap2,
    [SYS_TOKEN_CREATE]   = sys_token_create,
    [SYS_TOKEN_REVOKE]   = sys_token_revoke,
    [SYS_TOKEN_LIST]     = sys_token_list,
    [SYS_NS_CREATE]      = sys_ns_create,
    [SYS_NS_JOIN]        = sys_ns_join,
    [SYS_PROCINFO]       = sys_procinfo,
    [SYS_FSSTAT]         = sys_fsstat,
    [SYS_TASK_CREATE]    = sys_task_create,
    [SYS_TASK_DEPEND]    = sys_task_depend,
    [SYS_TASK_START]     = sys_task_start,
    [SYS_TASK_COMPLETE]  = sys_task_complete,
    [SYS_TASK_STATUS]    = sys_task_status,
    [SYS_TASK_WAIT]      = sys_task_wait,
    [SYS_TOKEN_DELEGATE] = sys_token_delegate,
    [SYS_NS_SETQUOTA]    = sys_ns_setquota,
    [SYS_INFER_HEALTH]   = sys_infer_health,
    [SYS_INFER_ROUTE]    = sys_infer_route,
    [SYS_AGENT_SEND]     = sys_agent_send,
    [SYS_AGENT_RECV]     = sys_agent_recv,
    [SYS_FUTEX_WAIT]     = sys_futex_wait,
    [SYS_FUTEX_WAKE]     = sys_futex_wake,
    [SYS_MMAP_FILE]      = sys_mmap_file,
    [SYS_MPROTECT]       = sys_mprotect,
    [SYS_MMAP_GUARD]     = sys_mmap_guard,
    [SYS_SIGPROCMASK]    = sys_sigprocmask,
    [SYS_ARCH_PRCTL]     = sys_arch_prctl,
    [SYS_SELECT]         = sys_select,
    [SYS_SUPER_CREATE]   = sys_super_create,
    [SYS_SUPER_ADD]      = sys_super_add,
    [SYS_SUPER_SET_POLICY] = sys_super_set_policy,
    [SYS_PIPE2]            = sys_pipe2,
    [SYS_SUPER_START]      = sys_super_start,
    [SYS_TCP_SETOPT]       = sys_tcp_setopt,
    [SYS_TCP_TO_FD]        = sys_tcp_to_fd,
    [SYS_INFER_SET_POLICY] = sys_infer_set_policy,
    [SYS_INFER_QUEUE_STAT] = sys_infer_queue_stat,
    [SYS_INFER_CACHE_CTRL] = sys_infer_cache_ctrl,
    [SYS_INFER_SUBMIT]     = sys_infer_submit,
    [SYS_INFER_POLL]       = sys_infer_poll,
    [SYS_INFER_RESULT]     = sys_infer_result,
};

/* Signal delivery is now per-CPU via percpu_t (GS-relative in asm).
 * We access it through percpu_get() in C code. */

int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (num >= SYS_NR)
        return -1;

    /* Seccomp filtering */
    thread_t *st = thread_get_current();
    if (st && st->process && st->process->seccomp_mask != 0 &&
        num != SYS_EXIT && num != SYS_SIGRETURN &&
        num < 64 && !(st->process->seccomp_mask & (1ULL << num))) {
        if (st->process->seccomp_strict) {
            process_deliver_signal(st->process, SIGKILL);
            return -EACCES;
        }
        return -EACCES;
    }

    int64_t result = syscall_table[num](arg1, arg2, arg3, arg4, arg5);

    /* Audit logging */
    if (st && st->process && st->process->audit_flags) {
        process_t *ap = st->process;
        if (ap->audit_flags & AUDIT_SYSCALL)
            serial_printf("[AUDIT pid=%lu uid=%u] syscall %lu = %ld\n",
                          ap->pid, ap->uid, num, result);
        if ((ap->audit_flags & AUDIT_SECURITY) &&
            (result == -EACCES || result == -EPERM))
            serial_printf("[AUDIT pid=%lu uid=%u] DENIED syscall %lu = %ld\n",
                          ap->pid, ap->uid, num, result);
        if ((ap->audit_flags & AUDIT_EXEC) &&
            (num == SYS_EXEC || num == SYS_FORK || num == SYS_EXIT))
            serial_printf("[AUDIT pid=%lu uid=%u] %s = %ld\n",
                          ap->pid, ap->uid,
                          num == SYS_EXEC ? "exec" : num == SYS_FORK ? "fork" : "exit",
                          result);
        if ((ap->audit_flags & AUDIT_FILE) &&
            (num == SYS_OPEN || num == SYS_CREATE || num == SYS_UNLINK))
            serial_printf("[AUDIT pid=%lu uid=%u] file_op %lu = %ld\n",
                          ap->pid, ap->uid, num, result);
    }

    /* Check for pending signals after syscall */
    thread_t *t = thread_get_current();
    if (t && t->process) {
        process_t *proc = t->process;
        for (int sig = 1; sig < MAX_SIGNALS; sig++) {
            if (!(proc->pending_signals & (1U << sig))) continue;
            /* Skip masked (blocked) signals — they stay pending */
            if (proc->signal_mask & (1U << sig)) continue;
            proc->pending_signals &= ~(1U << sig);

            /* Re-pend from queue if there are duplicates queued */
            signal_queue_t *sq = &proc->sig_queue;
            for (int qi = 0; qi < sq->count; qi++) {
                int idx = (sq->head + qi) % SIG_QUEUE_SIZE;
                if (sq->signum[idx] == sig) {
                    /* Remove this entry by shifting */
                    for (int j = qi; j < sq->count - 1; j++) {
                        int from = (sq->head + j + 1) % SIG_QUEUE_SIZE;
                        int to = (sq->head + j) % SIG_QUEUE_SIZE;
                        sq->signum[to] = sq->signum[from];
                    }
                    sq->count--;
                    proc->pending_signals |= (1U << sig);  /* re-pend */
                    break;
                }
            }

            uint64_t handler = proc->sig_handlers[sig].sa_handler;
            if (handler == SIG_IGN) continue;
            if (handler == SIG_DFL) {
                /* SIGCHLD and SIGCONT default action is ignore */
                if (sig == SIGCHLD || sig == SIGCONT) continue;
                /* Default: kill */
                proc->exit_status = -(int64_t)sig;
                serial_printf("[proc] Process %lu terminated (signal %d)\n",
                    proc->pid, sig);
                if (proc->cr3) {
                    __asm__ volatile ("mov %0, %%cr3" : : "r"(vmm_get_kernel_pml4()) : "memory");
                    vmm_free_user_pages(proc->cr3);
                    proc->cr3 = 0;
                }
                /* Disable interrupts before thread_exit to prevent
                 * preemption with cr3=0 (same fix as sys_exit). */
                __asm__ volatile ("cli");
                proc->exited = 1;
                t->process = NULL;
                thread_exit();
            }

            /* User handler: set up signal frame */
            uint64_t *kstack_top = (uint64_t *)(t->stack_base + t->stack_size);
            uint64_t orig_rip = kstack_top[-2];
            uint64_t orig_rsp = kstack_top[-1];
            uint64_t orig_rflags = kstack_top[-3];

            /* Build signal frame on user stack */
            uint64_t frame_rsp = orig_rsp - 32;
            frame_rsp &= ~0xFULL;  /* 16-byte align */

            /* Write frame via HHDM (user stack is mapped in process address space) */
            uint64_t *pte = vmm_get_pte(proc->cr3, frame_rsp);
            if (!pte || !(*pte & PTE_PRESENT)) break;  /* stack not mapped */

            /* Handle COW: if page is COW, copy it before writing signal frame */
            if (*pte & PTE_COW) {
                uint64_t old_phys = *pte & PTE_ADDR_MASK;
                uint64_t old_flags = *pte & ~PTE_ADDR_MASK;
                uint64_t clean = old_flags & ~(PTE_COW | PTE_WAS_WRITABLE);
                if (pmm_ref_get(old_phys) == 1) {
                    *pte = old_phys | (clean | PTE_WRITABLE | PTE_PRESENT);
                } else {
                    uint64_t new_phys = pmm_alloc_page();
                    if (new_phys == 0) break;
                    uint8_t *src = (uint8_t *)PHYS_TO_VIRT(old_phys);
                    uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(new_phys);
                    for (int i = 0; i < 4096; i++) dst[i] = src[i];
                    *pte = new_phys | (clean | PTE_WRITABLE | PTE_PRESENT);
                    pmm_ref_dec(old_phys);
                }
                invlpg_addr(frame_rsp & ~0xFFFULL);
                /* Re-read pte after COW */
                pte = vmm_get_pte(proc->cr3, frame_rsp);
                if (!pte || !(*pte & PTE_PRESENT)) break;
            }

            uint64_t stack_phys = (*pte & PTE_ADDR_MASK) + (frame_rsp & 0xFFF);
            uint64_t *frame = (uint64_t *)PHYS_TO_VIRT(stack_phys);

            frame[0] = orig_rsp;
            frame[1] = orig_rip;
            frame[2] = orig_rflags;
            frame[3] = (uint64_t)result;

            proc->signal_frame_addr = frame_rsp;

            /* SA_RESTART: save syscall info if result was -EINTR */
            if (result == -EINTR &&
                (proc->sig_handlers[sig].sa_flags & SA_RESTART)) {
                proc->restart_pending = 1;
                proc->restart_syscall_num = num;
                proc->restart_args[0] = arg1;
                proc->restart_args[1] = arg2;
                proc->restart_args[2] = arg3;
                proc->restart_args[3] = arg4;
                proc->restart_args[4] = arg5;
            } else {
                proc->restart_pending = 0;
            }

            /* Redirect SYSRET to handler via per-CPU data.
             * The asm stub overrides RCX (→RIP) and RSP after pops. */
            percpu_t *pc = percpu_get();
            pc->signal_handler_rip = handler;
            pc->signal_frame_rsp = frame_rsp;
            pc->signal_deliver_rdi = (uint64_t)sig;
            pc->signal_deliver_pending = 1;

            break;  /* deliver one signal at a time */
        }
    }

    return result;
}

void syscall_set_kernel_stack(uint64_t rsp) {
    /* Update per-CPU kernel_rsp (read by syscall_entry.asm via gs:0).
     * Read MSR_GS_BASE to get percpu pointer if available,
     * otherwise fall back to BSP (early boot before syscall_init). */
    uint64_t gs_base = rdmsr(0xC0000101);
    if (gs_base) {
        ((percpu_t *)gs_base)->kernel_rsp = rsp;
    } else {
        percpu_array[0].kernel_rsp = rsp;
    }
}

void syscall_init(void) {
    /* Enable SYSCALL/SYSRET in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);  /* bit 0 = SCE (Syscall Enable) */

    /*
     * STAR MSR:
     *   [47:32] = kernel CS base for SYSCALL (0x08)
     *             SYSCALL loads CS=0x08, SS=0x10
     *   [63:48] = user CS base for SYSRET (0x10)
     *             SYSRET loads CS=0x10+16=0x20 (|3=0x23), SS=0x10+8=0x18 (|3=0x1B)
     */
    uint64_t star = (0x0010ULL << 48) | (0x0008ULL << 32);
    wrmsr(MSR_STAR, star);

    /* LSTAR = syscall entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* SFMASK = bits to clear in RFLAGS on SYSCALL (clear IF to disable interrupts) */
    wrmsr(MSR_SFMASK, 0x200);

    /* Set up BSP per-CPU data early so SWAPGS works from the first SYSCALL.
     * smp_init() will re-initialize this more fully later. */
    percpu_array[0].self = (uint64_t)&percpu_array[0];
    percpu_array[0].cpu_id = 0;
    percpu_array[0].signal_deliver_pending = 0;
    percpu_array[0].signal_deliver_rdi = 0;
    percpu_array[0].signal_handler_rip = 0;
    percpu_array[0].signal_frame_rsp = 0;

    /* Set GS.base directly for kernel mode — percpu_get() reads gs:16.
     * KERNEL_GS_BASE starts as 0; process_enter_usermode will set it
     * to percpu before SYSRETQ (so SWAPGS in syscall_entry works). */
#define MSR_GS_BASE        0xC0000101
    wrmsr(MSR_GS_BASE, (uint64_t)&percpu_array[0]);

    pr_info("STAR=%lx LSTAR=%lx\n", star, (uint64_t)syscall_entry);
    pr_info("SYSCALL/SYSRET initialized (SWAPGS ready)\n");
}
