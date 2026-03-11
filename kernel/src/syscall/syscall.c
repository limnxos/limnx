#define pr_fmt(fmt) "[syscall] " fmt
#include "klog.h"

#include "syscall/syscall.h"
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

/* --- Pipe infrastructure --- */
#define PIPE_BUF_SIZE 4096
#define MAX_PIPES     8

typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    uint8_t  closed_read;
    uint8_t  closed_write;
    uint8_t  used;
    uint32_t read_refs;
    uint32_t write_refs;
} pipe_t;

static pipe_t pipes[MAX_PIPES];

/* --- Shared memory infrastructure --- */
#define MAX_SHM_REGIONS 16

typedef struct shm_region {
    int32_t  key;        /* user-visible key, -1 = unused */
    uint64_t phys_pages[16]; /* physical page addresses (max 16 pages per region) */
    uint32_t num_pages;
    uint32_t ref_count;
} shm_region_t;

shm_region_t shm_table[MAX_SHM_REGIONS];

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

/* --- Helpers --- */

static int validate_user_ptr(uint64_t ptr, uint64_t len) {
    if (ptr == 0 || ptr >= USER_ADDR_MAX)
        return -1;
    if (len > 0 && len > USER_ADDR_MAX - ptr)
        return -1;
    return 0;
}

static int copy_string_from_user(const char *user_src, char *kern_dst,
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
static void resolve_user_path(process_t *proc, const char *path, char *out) {
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
static int fd_is_free(fd_entry_t *e) {
    return e->node == NULL && e->pipe == NULL && e->pty == NULL
        && e->unix_sock == NULL && e->eventfd == NULL
        && e->epoll == NULL && e->uring == NULL
        && e->tcp_conn_idx < 0;
}

/* --- Permission helper --- */

static int check_file_perm(process_t *proc, vfs_node_t *node, uint8_t access) {
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
static int count_open_fds(process_t *proc) {
    int count = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fd_is_free(&proc->fd_table[i]))
            count++;
    }
    return count;
}

/* --- Syscall handlers --- */

static int64_t sys_write(uint64_t buf, uint64_t len,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (len == 0)
        return 0;
    if (validate_user_ptr(buf, len) != 0)
        return -1;

    const char *s = (const char *)buf;
    for (uint64_t i = 0; i < len; i++)
        serial_putc(s[i]);

    return (int64_t)len;
}

static int64_t sys_yield(uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    sched_yield();
    return 0;
}

static int64_t sys_exit(uint64_t status, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (t->process) {
        t->process->exit_status = (int64_t)status;
        /* Close all open file descriptors */
        for (int i = 0; i < MAX_FDS; i++) {
            fd_entry_t *entry = &t->process->fd_table[i];
            if (entry->pipe != NULL) {
                pipe_t *pp = (pipe_t *)entry->pipe;
                if (entry->pipe_write) {
                    if (pp->write_refs > 0) pp->write_refs--;
                    if (pp->write_refs == 0) pp->closed_write = 1;
                } else {
                    if (pp->read_refs > 0) pp->read_refs--;
                    if (pp->read_refs == 0) pp->closed_read = 1;
                }
                if (pp->closed_read && pp->closed_write) pp->used = 0;
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

static int64_t sys_open(uint64_t path_ptr, uint64_t flags,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;
    resolve_user_path(proc, raw_path, path);

    /* Find the file in VFS */
    int node_idx = vfs_open(path);

    /* O_CREAT: create file if it doesn't exist */
    if (node_idx < 0 && (flags & O_CREAT)) {
        node_idx = vfs_create(path);
        if (node_idx < 0)
            return -1;
    }

    if (node_idx < 0)
        return -1;

    /* Capability checks (with token fallback) */
    {
        uint8_t acc = (uint8_t)(flags & O_ACCMODE);
        if ((acc == O_RDONLY || acc == O_RDWR) &&
            !(proc->capabilities & CAP_FS_READ) &&
            !cap_token_check(proc->pid, CAP_FS_READ, path))
            return -EACCES;
        if ((acc == O_WRONLY || acc == O_RDWR) &&
            !(proc->capabilities & CAP_FS_WRITE) &&
            !cap_token_check(proc->pid, CAP_FS_WRITE, path))
            return -EACCES;
    }

    /* Permission checks */
    vfs_node_t *file_node = vfs_get_node(node_idx);
    if (file_node) {
        uint8_t acc = (uint8_t)(flags & O_ACCMODE);
        /* VFS_FLAG_WRITABLE gate applies to all users including root */
        if ((acc == O_WRONLY || acc == O_RDWR) &&
            !(file_node->flags & VFS_FLAG_WRITABLE))
            return -EACCES;
        int perm = check_file_perm(proc, file_node, acc);
        if (perm != 0)
            return perm;
    }

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    /* O_TRUNC: truncate to zero on open */
    if (flags & O_TRUNC) {
        vfs_truncate_node(node_idx, 0);
    }

    /* Store access mode + append flag as low byte */
    uint8_t stored_flags = (uint8_t)(flags & O_ACCMODE);
    if (flags & O_APPEND)
        stored_flags |= 0x04;  /* bit 2 = append */

    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            proc->fd_table[fd].node = vfs_get_node(node_idx);
            proc->fd_table[fd].offset = 0;
            proc->fd_table[fd].pipe = NULL;
            proc->fd_table[fd].pipe_write = 0;
            proc->fd_table[fd].pty = NULL;
            proc->fd_table[fd].pty_is_master = 0;
            proc->fd_table[fd].unix_sock = NULL;
            proc->fd_table[fd].eventfd = NULL;
            proc->fd_table[fd].epoll = NULL;
            proc->fd_table[fd].uring = NULL;
            proc->fd_table[fd].open_flags = stored_flags;
            proc->fd_table[fd].fd_flags = 0;
            return fd;
        }
    }

    return -1;  /* no free fd */
}

static int64_t sys_read(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                          uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;
    if (len == 0)
        return 0;
    if (validate_user_ptr(buf_ptr, len) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];

    /* Pipe read */
    if (entry->pipe != NULL) {
        pipe_t *pp = (pipe_t *)entry->pipe;
        uint8_t *dst = (uint8_t *)buf_ptr;
        uint64_t total = 0;
        while (total < len) {
            if (pp->count > 0) {
                dst[total] = pp->buf[pp->read_pos];
                pp->read_pos = (pp->read_pos + 1) % PIPE_BUF_SIZE;
                pp->count--;
                total++;
            } else if (pp->closed_write) {
                break;  /* EOF — writer closed */
            } else {
                if (total > 0) break;  /* return what we have */
                if (entry->fd_flags & 0x02)  /* O_NONBLOCK */
                    return -EAGAIN;
                if (proc->pending_signals & ~proc->signal_mask)
                    return -EINTR;
                sched_yield();
            }
        }
        return (int64_t)total;
    }

    /* PTY read */
    if (entry->pty != NULL) {
        int pty_idx = pty_index((pty_t *)entry->pty);
        if (pty_idx < 0) return -1;
        int nonblock = (entry->fd_flags & 0x02) ? 1 : 0;
        if (entry->pty_is_master)
            return pty_master_read(pty_idx, (uint8_t *)buf_ptr, (uint32_t)len);
        else
            return pty_slave_read(pty_idx, (uint8_t *)buf_ptr, (uint32_t)len, nonblock);
    }

    /* Unix socket read */
    if (entry->unix_sock != NULL) {
        int nonblock = (entry->fd_flags & 0x02) ? 1 : 0;
        return unix_sock_recv((unix_sock_t *)entry->unix_sock,
                              (uint8_t *)buf_ptr, (uint32_t)len, nonblock);
    }

    /* Eventfd read */
    if (entry->eventfd != NULL) {
        if (len < 8) return -1;
        int nonblock = (entry->fd_flags & 0x02) ? 1 : 0;
        int efd_idx = -1;
        for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
            if (eventfd_get(ei) == (eventfd_t *)entry->eventfd) {
                efd_idx = ei; break;
            }
        }
        if (efd_idx < 0) return -1;
        return eventfd_read(efd_idx, (uint64_t *)buf_ptr, nonblock);
    }

    /* epoll/uring fds are not readable */
    if (entry->epoll != NULL || entry->uring != NULL)
        return -EINVAL;

    if (entry->node == NULL)
        return -1;

    /* Reject read on write-only fd */
    if ((entry->open_flags & O_ACCMODE) == O_WRONLY)
        return -1;

    vfs_node_t *node = entry->node;
    int node_idx = vfs_node_index(node);
    if (node_idx < 0)
        return -1;

    int64_t n = vfs_read(node_idx, entry->offset, (uint8_t *)buf_ptr, len);
    if (n > 0)
        entry->offset += (uint64_t)n;
    return n;
}

static int64_t sys_close(uint64_t fd, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];

    /* Pipe close */
    if (entry->pipe != NULL) {
        pipe_t *pp = (pipe_t *)entry->pipe;
        if (entry->pipe_write) {
            if (pp->write_refs > 0)
                pp->write_refs--;
            if (pp->write_refs == 0)
                pp->closed_write = 1;
        } else {
            if (pp->read_refs > 0)
                pp->read_refs--;
            if (pp->read_refs == 0)
                pp->closed_read = 1;
        }
        /* Free pipe if both ends fully closed */
        if (pp->closed_read && pp->closed_write)
            pp->used = 0;
        entry->pipe = NULL;
        entry->pipe_write = 0;
        return 0;
    }

    /* PTY close */
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
        return 0;
    }

    /* Unix socket close */
    if (entry->unix_sock != NULL) {
        unix_sock_close((unix_sock_t *)entry->unix_sock);
        entry->unix_sock = NULL;
        return 0;
    }

    /* Eventfd close */
    if (entry->eventfd != NULL) {
        for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
            if (eventfd_get(ei) == (eventfd_t *)entry->eventfd) {
                eventfd_close(ei);
                break;
            }
        }
        entry->eventfd = NULL;
        return 0;
    }

    /* epoll close */
    if (entry->epoll != NULL) {
        int ep_idx = epoll_index((epoll_instance_t *)entry->epoll);
        if (ep_idx >= 0)
            epoll_close(ep_idx);
        entry->epoll = NULL;
        return 0;
    }

    /* uring close */
    if (entry->uring != NULL) {
        int ur_idx = uring_index((uring_instance_t *)entry->uring);
        if (ur_idx >= 0)
            uring_close(ur_idx);
        entry->uring = NULL;
        return 0;
    }

    /* TCP fd close — just release the fd, don't close the TCP conn */
    if (entry->tcp_conn_idx >= 0) {
        entry->tcp_conn_idx = -1;
        entry->open_flags = 0;
        entry->fd_flags = 0;
        return 0;
    }

    if (entry->node == NULL)
        return -1;

    entry->node = NULL;
    entry->offset = 0;
    entry->open_flags = 0;
    entry->fd_flags = 0;
    return 0;
}

static int64_t sys_stat(uint64_t path_ptr, uint64_t stat_ptr,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;
    if (validate_user_ptr(stat_ptr, sizeof(vfs_stat_t)) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0)
        return -1;

    /* Copy stat result to user buffer */
    uint8_t *dst = (uint8_t *)stat_ptr;
    const uint8_t *src = (const uint8_t *)&st;
    for (uint64_t i = 0; i < sizeof(vfs_stat_t); i++)
        dst[i] = src[i];

    return 0;
}

static int64_t sys_exec(uint64_t path_ptr, uint64_t argv_ptr,
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
            pipe_t *pp = (pipe_t *)proc->fd_table[i].pipe;
            if (proc->fd_table[i].pipe_write)
                pp->write_refs++;
            else
                pp->read_refs++;
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

/* --- File write syscalls --- */

static int64_t sys_fwrite(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                            uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;
    if (len == 0)
        return 0;
    if (validate_user_ptr(buf_ptr, len) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];

    /* Pipe write */
    if (entry->pipe != NULL) {
        pipe_t *pp = (pipe_t *)entry->pipe;
        const uint8_t *src = (const uint8_t *)buf_ptr;
        uint64_t total = 0;
        while (total < len) {
            if (pp->closed_read)
                return total > 0 ? (int64_t)total : -1;
            if (pp->count < PIPE_BUF_SIZE) {
                pp->buf[pp->write_pos] = src[total];
                pp->write_pos = (pp->write_pos + 1) % PIPE_BUF_SIZE;
                pp->count++;
                total++;
            } else {
                if (entry->fd_flags & 0x02)  /* O_NONBLOCK */
                    return total > 0 ? (int64_t)total : -1;
                if (proc->pending_signals & ~proc->signal_mask)
                    return total > 0 ? (int64_t)total : -EINTR;
                sched_yield();
            }
        }
        return (int64_t)total;
    }

    /* PTY write */
    if (entry->pty != NULL) {
        int pty_idx = pty_index((pty_t *)entry->pty);
        if (pty_idx < 0) return -1;
        if (entry->pty_is_master)
            return pty_master_write(pty_idx, (const uint8_t *)buf_ptr, (uint32_t)len);
        else
            return pty_slave_write(pty_idx, (const uint8_t *)buf_ptr, (uint32_t)len);
    }

    /* Unix socket write */
    if (entry->unix_sock != NULL) {
        int nonblock = (entry->fd_flags & 0x02) ? 1 : 0;
        return unix_sock_send((unix_sock_t *)entry->unix_sock,
                              (const uint8_t *)buf_ptr, (uint32_t)len, nonblock);
    }

    /* Eventfd write */
    if (entry->eventfd != NULL) {
        if (len < 8) return -1;
        int efd_idx = -1;
        for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
            if (eventfd_get(ei) == (eventfd_t *)entry->eventfd) {
                efd_idx = ei; break;
            }
        }
        if (efd_idx < 0) return -1;
        return eventfd_write(efd_idx, *(const uint64_t *)buf_ptr);
    }

    /* epoll/uring fds are not writable */
    if (entry->epoll != NULL || entry->uring != NULL)
        return -EINVAL;

    if (entry->node == NULL)
        return -1;

    /* Reject write on read-only fd */
    if ((entry->open_flags & O_ACCMODE) == O_RDONLY)
        return -1;

    vfs_node_t *node = entry->node;
    int node_idx = vfs_node_index(node);
    if (node_idx < 0)
        return -1;

    /* O_APPEND: always write at end */
    if (entry->open_flags & 0x04)
        entry->offset = node->size;

    int64_t written = vfs_write(node_idx, entry->offset,
                                 (const uint8_t *)buf_ptr, len);

    if (written > 0)
        entry->offset += (uint64_t)written;

    return written;
}

static int64_t sys_create(uint64_t path_ptr, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    /* Check CAP_FS_WRITE (with token fallback) */
    if (!(proc->capabilities & CAP_FS_WRITE) &&
        !cap_token_check(proc->pid, CAP_FS_WRITE, path))
        return -EACCES;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    int node_idx = vfs_create(path);
    if (node_idx < 0)
        return -1;

    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            proc->fd_table[fd].node = vfs_get_node(node_idx);
            proc->fd_table[fd].offset = 0;
            proc->fd_table[fd].pipe = NULL;
            proc->fd_table[fd].pipe_write = 0;
            proc->fd_table[fd].pty = NULL;
            proc->fd_table[fd].pty_is_master = 0;
            proc->fd_table[fd].unix_sock = NULL;
            proc->fd_table[fd].eventfd = NULL;
            proc->fd_table[fd].epoll = NULL;
            proc->fd_table[fd].uring = NULL;
            proc->fd_table[fd].open_flags = O_RDWR;  /* create implies read+write */
            proc->fd_table[fd].fd_flags = 0;
            return fd;
        }
    }

    return -1;
}

static int64_t sys_unlink(uint64_t path_ptr, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    /* Check CAP_FS_WRITE (with token fallback) */
    if (!(proc->capabilities & CAP_FS_WRITE) &&
        !cap_token_check(proc->pid, CAP_FS_WRITE, path))
        return -EACCES;

    return vfs_delete(path);
}

/* --- Mmap syscalls --- */

static int64_t sys_mmap(uint64_t num_pages, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    /* Check memory rlimit */
    if (proc->rlimit_mem_pages > 0 &&
        proc->used_mem_pages + num_pages > proc->rlimit_mem_pages)
        return -ENOMEM;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    /* Allocate contiguous physical pages */
    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0)
        return -1;

    /* Map pages into process address space */
    uint64_t virt = proc->mmap_next_addr;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_virt = virt + i * PAGE_SIZE;

        /* Zero the page */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        for (uint64_t j = 0; j < PAGE_SIZE; j++)
            dst[j] = 0;

        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            /* Cleanup: free all allocated pages */
            for (uint64_t k = 0; k < num_pages; k++)
                pmm_free_page(phys + k * PAGE_SIZE);
            return -1;
        }
    }

    /* Record the mapping */
    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;

    /* Bump next address (+1 guard gap page) */
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;
    proc->used_mem_pages += num_pages;

    return (int64_t)virt;
}

static int64_t sys_munmap(uint64_t virt_addr, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    /* Find the mmap entry */
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (proc->mmap_table[i].used &&
            proc->mmap_table[i].virt_addr == virt_addr) {
            uint32_t npages = proc->mmap_table[i].num_pages;
            uint64_t phys = proc->mmap_table[i].phys_addr;

            if (proc->mmap_table[i].shm_id >= 0) {
                /* Shared memory: don't free phys pages, just decrement ref */
                int32_t sid = proc->mmap_table[i].shm_id;
                if (sid < MAX_SHM_REGIONS && shm_table[sid].ref_count > 0)
                    shm_table[sid].ref_count--;
            } else {
                /* Private mapping: free physical pages */
                for (uint32_t j = 0; j < npages; j++)
                    pmm_free_page(phys + j * PAGE_SIZE);
            }

            proc->mmap_table[i].used = 0;
            proc->mmap_table[i].shm_id = -1;
            if (proc->used_mem_pages >= npages)
                proc->used_mem_pages -= npages;
            return 0;
        }
    }

    return -1;
}

/* --- Socket syscalls --- */

static int64_t sys_socket(uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return net_socket();
}

static int64_t sys_bind(uint64_t sockfd, uint64_t port,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    /* Check CAP_NET_BIND for privileged ports */
    if (port < 1024) {
        thread_t *t = thread_get_current();
        if (t && t->process && !(t->process->capabilities & CAP_NET_BIND))
            return -EACCES;
    }
    return net_bind((int)sockfd, (uint16_t)port);
}

static int64_t sys_sendto(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                           uint64_t dst_ip, uint64_t dst_port) {
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    return net_sendto((int)sockfd, (const void *)buf_ptr, (uint32_t)len,
                      (uint32_t)dst_ip, (uint16_t)dst_port);
}

static int64_t sys_recvfrom(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                             uint64_t src_ip_ptr, uint64_t src_port_ptr) {
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int ret = net_recvfrom((int)sockfd, (void *)buf_ptr, (uint32_t)len,
                           &src_ip, &src_port);
    if (ret >= 0) {
        if (src_ip_ptr && validate_user_ptr(src_ip_ptr, 4) == 0)
            *(uint32_t *)src_ip_ptr = src_ip;
        if (src_port_ptr && validate_user_ptr(src_port_ptr, 2) == 0)
            *(uint16_t *)src_port_ptr = src_port;
    }
    return ret;
}

/* --- New syscalls (Stage 12) --- */

static int64_t sys_getchar(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    char ch;
    /* Try serial first (COM1 via -serial stdio), fall back to PS/2 keyboard */
    thread_t *gc_t = thread_get_current();
    process_t *gc_proc = gc_t ? gc_t->process : NULL;
    while (1) {
        ch = serial_getchar();
        if (ch) return (int64_t)(uint8_t)ch;
        ch = kbd_getchar();
        if (ch) return (int64_t)(uint8_t)ch;
        if (gc_proc && (gc_proc->pending_signals & ~gc_proc->signal_mask))
            return -EINTR;
        sched_yield();
    }
}

static int64_t sys_waitpid(uint64_t pid, uint64_t flags,
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

static int64_t sys_pipe(uint64_t rfd_ptr, uint64_t wfd_ptr,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(rfd_ptr, sizeof(long)) != 0 ||
        validate_user_ptr(wfd_ptr, sizeof(long)) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* fd limit check (pipe needs 2 fds) */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)(count_open_fds(proc) + 2) > proc->rlimit_nfds)
        return -EMFILE;

    /* Find free pipe slot */
    int slot = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    pipe_t *pp = &pipes[slot];
    pp->read_pos = 0;
    pp->write_pos = 0;
    pp->count = 0;
    pp->closed_read = 0;
    pp->closed_write = 0;
    pp->used = 1;
    pp->read_refs = 1;
    pp->write_refs = 1;

    /* Find two free fd slots */
    int rfd = -1, wfd = -1;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            if (rfd < 0) rfd = fd;
            else if (wfd < 0) { wfd = fd; break; }
        }
    }
    if (rfd < 0 || wfd < 0) {
        pp->used = 0;
        return -1;
    }

    /* Set up read end */
    proc->fd_table[rfd].node = NULL;
    proc->fd_table[rfd].offset = 0;
    proc->fd_table[rfd].pipe = (void *)pp;
    proc->fd_table[rfd].pipe_write = 0;
    proc->fd_table[rfd].pty = NULL;
    proc->fd_table[rfd].pty_is_master = 0;
    proc->fd_table[rfd].fd_flags = 0;

    /* Set up write end */
    proc->fd_table[wfd].node = NULL;
    proc->fd_table[wfd].offset = 0;
    proc->fd_table[wfd].pipe = (void *)pp;
    proc->fd_table[wfd].pipe_write = 1;
    proc->fd_table[wfd].pty = NULL;
    proc->fd_table[wfd].pty_is_master = 0;
    proc->fd_table[wfd].fd_flags = 0;

    /* Write fd numbers to user pointers */
    *(long *)rfd_ptr = rfd;
    *(long *)wfd_ptr = wfd;

    return 0;
}

static int64_t sys_pipe2(uint64_t rfd_ptr, uint64_t wfd_ptr,
                          uint64_t flags, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (validate_user_ptr(rfd_ptr, sizeof(long)) != 0 ||
        validate_user_ptr(wfd_ptr, sizeof(long)) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (proc->rlimit_nfds > 0 &&
        (uint32_t)(count_open_fds(proc) + 2) > proc->rlimit_nfds)
        return -EMFILE;

    int slot = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    pipe_t *pp = &pipes[slot];
    pp->read_pos = 0;
    pp->write_pos = 0;
    pp->count = 0;
    pp->closed_read = 0;
    pp->closed_write = 0;
    pp->used = 1;
    pp->read_refs = 1;
    pp->write_refs = 1;

    int rfd = -1, wfd = -1;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            if (rfd < 0) rfd = fd;
            else if (wfd < 0) { wfd = fd; break; }
        }
    }
    if (rfd < 0 || wfd < 0) { pp->used = 0; return -1; }

    proc->fd_table[rfd].node = NULL;
    proc->fd_table[rfd].offset = 0;
    proc->fd_table[rfd].pipe = (void *)pp;
    proc->fd_table[rfd].pipe_write = 0;
    proc->fd_table[rfd].pty = NULL;
    proc->fd_table[rfd].pty_is_master = 0;
    proc->fd_table[rfd].fd_flags = 0;

    proc->fd_table[wfd].node = NULL;
    proc->fd_table[wfd].offset = 0;
    proc->fd_table[wfd].pipe = (void *)pp;
    proc->fd_table[wfd].pipe_write = 1;
    proc->fd_table[wfd].pty = NULL;
    proc->fd_table[wfd].pty_is_master = 0;
    proc->fd_table[wfd].fd_flags = 0;

    if (flags & 0x01) {  /* O_CLOEXEC */
        proc->fd_table[rfd].fd_flags |= 0x01;
        proc->fd_table[wfd].fd_flags |= 0x01;
    }
    if (flags & 0x800) {  /* O_NONBLOCK */
        proc->fd_table[rfd].fd_flags |= 0x02;
        proc->fd_table[wfd].fd_flags |= 0x02;
    }

    *(long *)rfd_ptr = rfd;
    *(long *)wfd_ptr = wfd;
    return 0;
}

static int64_t sys_getpid(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t->process) return -1;
    return (int64_t)t->process->pid;
}

static int64_t sys_fmmap(uint64_t fd, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->node == NULL)
        return -1;

    vfs_node_t *node = entry->node;
    uint64_t file_size = node->size;
    if (file_size == 0)
        return -1;

    /* Allocate mmap pages */
    uint64_t num_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages > MMAP_MAX_PAGES)
        return -1;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    /* Allocate contiguous physical pages */
    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0)
        return -1;

    /* Map pages into process address space and copy file data */
    int node_idx = vfs_node_index(node);
    if (node_idx < 0) {
        for (uint64_t k = 0; k < num_pages; k++)
            pmm_free_page(phys + k * PAGE_SIZE);
        return -1;
    }

    uint64_t virt = proc->mmap_next_addr;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_virt = virt + i * PAGE_SIZE;

        /* Copy file data into physical page via HHDM */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        uint64_t offset = i * PAGE_SIZE;
        uint64_t chunk = file_size - offset;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;

        /* Read through VFS (handles both RAM and disk-backed files) */
        int64_t got = vfs_read(node_idx, offset, dst, chunk);
        if (got < 0) got = 0;
        for (uint64_t j = (uint64_t)got; j < PAGE_SIZE; j++)
            dst[j] = 0;

        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            for (uint64_t k = 0; k < num_pages; k++)
                pmm_free_page(phys + k * PAGE_SIZE);
            return -1;
        }
    }

    /* Record the mapping */
    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;

    return (int64_t)virt;
}

static int64_t sys_readdir(uint64_t dir_path_ptr, uint64_t index,
                            uint64_t dirent_ptr, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    char raw_path[MAX_PATH], dir_path[MAX_PATH];
    if (copy_string_from_user((const char *)dir_path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    if (validate_user_ptr(dirent_ptr, sizeof(vfs_dirent_t)) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, dir_path);

    vfs_dirent_t ent;
    if (vfs_readdir(dir_path, (uint32_t)index, &ent) != 0)
        return -1;

    /* Copy result to user buffer */
    uint8_t *dst = (uint8_t *)dirent_ptr;
    const uint8_t *src = (const uint8_t *)&ent;
    for (uint64_t i = 0; i < sizeof(vfs_dirent_t); i++)
        dst[i] = src[i];

    return 0;
}

static int64_t sys_mkdir(uint64_t path_ptr, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    /* Check CAP_FS_WRITE (with token fallback) */
    if (!(proc->capabilities & CAP_FS_WRITE) &&
        !cap_token_check(proc->pid, CAP_FS_WRITE, path))
        return -EACCES;

    int rc = vfs_mkdir(path);
    return (rc >= 0) ? 0 : -1;
}

/* --- Stage 19 syscalls --- */

static int64_t sys_seek(uint64_t fd, uint64_t offset_arg, uint64_t whence,
                          uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->node == NULL)
        return -1;

    vfs_node_t *node = entry->node;
    int64_t new_offset;

    switch (whence) {
    case SEEK_SET:
        new_offset = (int64_t)offset_arg;
        break;
    case SEEK_CUR:
        new_offset = (int64_t)entry->offset + (int64_t)offset_arg;
        break;
    case SEEK_END:
        new_offset = (int64_t)node->size + (int64_t)offset_arg;
        break;
    default:
        return -1;
    }

    if (new_offset < 0 || (uint64_t)new_offset > node->size)
        return -1;

    entry->offset = (uint64_t)new_offset;
    return new_offset;
}

static int64_t sys_truncate(uint64_t path_ptr, uint64_t new_size,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    /* Check CAP_FS_WRITE (with token fallback) */
    if (!(proc->capabilities & CAP_FS_WRITE) &&
        !cap_token_check(proc->pid, CAP_FS_WRITE, path))
        return -EACCES;

    int node_idx = vfs_resolve_path(path);
    if (node_idx < 0)
        return -1;

    return vfs_truncate_node(node_idx, new_size);
}

static int64_t sys_chdir(uint64_t path_ptr, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    int idx = vfs_resolve_path(path);
    if (idx < 0)
        return -1;

    vfs_node_t *node = vfs_get_node(idx);
    if (!node || node->type != VFS_DIRECTORY)
        return -1;

    /* Store resolved absolute path as new cwd */
    uint64_t len = 0;
    while (path[len]) len++;
    if (len >= MAX_PATH) len = MAX_PATH - 1;

    for (uint64_t i = 0; i <= len; i++)
        proc->cwd[i] = path[i];

    /* Remove trailing slash (except for root) */
    if (len > 1 && proc->cwd[len - 1] == '/')
        proc->cwd[len - 1] = '\0';

    return 0;
}

static int64_t sys_getcwd(uint64_t buf_ptr, uint64_t size,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (size == 0)
        return -1;
    if (validate_user_ptr(buf_ptr, size) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    char *dst = (char *)buf_ptr;
    const char *src = proc->cwd;
    uint64_t i = 0;
    while (i < size - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return (int64_t)i;
}

static int64_t sys_fstat(uint64_t fd, uint64_t stat_ptr,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;
    if (validate_user_ptr(stat_ptr, sizeof(vfs_stat_t)) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->node == NULL)
        return -1;

    vfs_node_t *node = entry->node;
    vfs_stat_t st;
    st.size = node->size;
    st.type = node->type;
    st.pad1 = 0;
    st.mode = node->mode;
    st.uid = node->uid;
    st.gid = node->gid;

    /* Copy stat result to user buffer */
    uint8_t *dst = (uint8_t *)stat_ptr;
    const uint8_t *src = (const uint8_t *)&st;
    for (uint64_t i = 0; i < sizeof(vfs_stat_t); i++)
        dst[i] = src[i];

    return 0;
}

static int64_t sys_rename(uint64_t old_path_ptr, uint64_t new_path_ptr,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    char raw_old[MAX_PATH], raw_new[MAX_PATH];
    char old_path[MAX_PATH], new_path[MAX_PATH];
    if (copy_string_from_user((const char *)old_path_ptr, raw_old, MAX_PATH) != 0)
        return -1;
    if (copy_string_from_user((const char *)new_path_ptr, raw_new, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_old, old_path);
    resolve_user_path(proc, raw_new, new_path);

    /* Check CAP_FS_WRITE (with token fallback) */
    if (!(proc->capabilities & CAP_FS_WRITE) &&
        !cap_token_check(proc->pid, CAP_FS_WRITE, old_path))
        return -EACCES;

    return vfs_rename(old_path, new_path);
}

/* --- Stage 24 syscalls --- */

static int64_t sys_dup(uint64_t fd, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *src = &proc->fd_table[fd];
    if (fd_is_free(src))
        return -1;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    /* Find first free fd slot */
    for (int newfd = 0; newfd < MAX_FDS; newfd++) {
        if (fd_is_free(&proc->fd_table[newfd])) {
            proc->fd_table[newfd] = *src;
            proc->fd_table[newfd].fd_flags = 0;  /* dup clears cloexec */
            /* Increment pipe ref count */
            if (src->pipe != NULL) {
                pipe_t *pp = (pipe_t *)src->pipe;
                if (src->pipe_write)
                    pp->write_refs++;
                else
                    pp->read_refs++;
            }
            /* Increment PTY ref count */
            if (src->pty != NULL) {
                pty_t *pt = (pty_t *)src->pty;
                if (src->pty_is_master)
                    pt->master_refs++;
                else
                    pt->slave_refs++;
            }
            /* Increment unix socket ref count */
            if (src->unix_sock != NULL)
                ((unix_sock_t *)src->unix_sock)->refs++;
            /* Increment eventfd ref count */
            if (src->eventfd != NULL) {
                for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
                    if (eventfd_get(ei) == (eventfd_t *)src->eventfd) {
                        eventfd_ref(ei);
                        break;
                    }
                }
            }
            /* Increment epoll ref count */
            if (src->epoll != NULL) {
                int ep_idx = epoll_index((epoll_instance_t *)src->epoll);
                if (ep_idx >= 0) epoll_ref(ep_idx);
            }
            /* Increment uring ref count */
            if (src->uring != NULL) {
                int ur_idx = uring_index((uring_instance_t *)src->uring);
                if (ur_idx >= 0) uring_ref(ur_idx);
            }
            return newfd;
        }
    }

    return -1;  /* no free fd */
}

static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (oldfd >= MAX_FDS || newfd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *src = &proc->fd_table[oldfd];
    if (fd_is_free(src))
        return -1;

    if (oldfd == newfd)
        return (int64_t)newfd;

    /* Close newfd if open */
    fd_entry_t *dst = &proc->fd_table[newfd];
    if (dst->pipe != NULL) {
        pipe_t *pp = (pipe_t *)dst->pipe;
        if (dst->pipe_write) {
            if (pp->write_refs > 0)
                pp->write_refs--;
            if (pp->write_refs == 0)
                pp->closed_write = 1;
        } else {
            if (pp->read_refs > 0)
                pp->read_refs--;
            if (pp->read_refs == 0)
                pp->closed_read = 1;
        }
        if (pp->closed_read && pp->closed_write)
            pp->used = 0;
    }
    if (dst->pty != NULL) {
        int pty_idx = pty_index((pty_t *)dst->pty);
        if (pty_idx >= 0) {
            if (dst->pty_is_master)
                pty_close_master(pty_idx);
            else
                pty_close_slave(pty_idx);
        }
    }
    if (dst->unix_sock != NULL)
        unix_sock_close((unix_sock_t *)dst->unix_sock);
    if (dst->eventfd != NULL) {
        for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
            if (eventfd_get(ei) == (eventfd_t *)dst->eventfd) {
                eventfd_close(ei);
                break;
            }
        }
    }
    if (dst->epoll != NULL) {
        int ep_idx = epoll_index((epoll_instance_t *)dst->epoll);
        if (ep_idx >= 0) epoll_close(ep_idx);
    }
    if (dst->uring != NULL) {
        int ur_idx = uring_index((uring_instance_t *)dst->uring);
        if (ur_idx >= 0) uring_close(ur_idx);
    }

    /* Copy fd entry */
    proc->fd_table[newfd] = *src;
    proc->fd_table[newfd].fd_flags = 0;  /* dup2 clears cloexec */
    /* Increment pipe ref count for new copy */
    if (src->pipe != NULL) {
        pipe_t *pp = (pipe_t *)src->pipe;
        if (src->pipe_write)
            pp->write_refs++;
        else
            pp->read_refs++;
    }
    /* Increment PTY ref count for new copy */
    if (src->pty != NULL) {
        pty_t *pt = (pty_t *)src->pty;
        if (src->pty_is_master)
            pt->master_refs++;
        else
            pt->slave_refs++;
    }
    /* Increment unix socket ref count for new copy */
    if (src->unix_sock != NULL)
        ((unix_sock_t *)src->unix_sock)->refs++;
    /* Increment eventfd ref count for new copy */
    if (src->eventfd != NULL) {
        for (int ei = 0; ei < MAX_EVENTFDS; ei++) {
            if (eventfd_get(ei) == (eventfd_t *)src->eventfd) {
                eventfd_ref(ei);
                break;
            }
        }
    }
    /* Increment epoll ref count for new copy */
    if (src->epoll != NULL) {
        int ep_idx = epoll_index((epoll_instance_t *)src->epoll);
        if (ep_idx >= 0) epoll_ref(ep_idx);
    }
    /* Increment uring ref count for new copy */
    if (src->uring != NULL) {
        int ur_idx = uring_index((uring_instance_t *)src->uring);
        if (ur_idx >= 0) uring_ref(ur_idx);
    }
    return (int64_t)newfd;
}

static int64_t sys_kill(uint64_t pid, uint64_t signal,
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

/* --- fcntl constants --- */
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                           uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (fd_is_free(entry))
        return -1;

    switch (cmd) {
    case F_GETFD:
        return entry->fd_flags & FD_CLOEXEC;
    case F_SETFD:
        entry->fd_flags = (entry->fd_flags & ~FD_CLOEXEC)
                        | (uint8_t)(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
        return (entry->fd_flags & 0x02) ? O_NONBLOCK : 0;
    case F_SETFL:
        if (arg & O_NONBLOCK)
            entry->fd_flags |= 0x02;
        else
            entry->fd_flags &= ~0x02;
        return 0;
    default:
        return -1;
    }
}

/* --- Stage 30 syscalls --- */

static int64_t sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg,
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

static int64_t sys_getpgid(uint64_t pid_arg, uint64_t a2,
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

static int64_t sys_chmod(uint64_t path_ptr, uint64_t mode,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;
    resolve_user_path(proc, raw_path, path);

    /* Permission check: only root or file owner can chmod */
    if (proc->uid != 0) {
        int idx = vfs_resolve_path(path);
        if (idx < 0) return -1;
        vfs_node_t *node = vfs_get_node(idx);
        if (node && node->uid != proc->uid)
            return -EPERM;
    }

    return vfs_chmod(path, (uint16_t)mode);
}

/* --- Shared memory syscalls --- */

static void shm_init(void) {
    for (int i = 0; i < MAX_SHM_REGIONS; i++)
        shm_table[i].key = -1;
}

static int shm_inited = 0;

static int64_t sys_shmget(uint64_t key, uint64_t num_pages,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (!shm_inited) { shm_init(); shm_inited = 1; }

    if (num_pages == 0 || num_pages > 16)
        return -1;

    /* Search for existing key match */
    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        if (shm_table[i].key == (int32_t)key)
            return i;
    }

    /* Allocate new region */
    int slot = -1;
    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        if (shm_table[i].key == -1) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* Allocate physical pages individually */
    for (uint32_t i = 0; i < (uint32_t)num_pages; i++) {
        uint64_t pg = pmm_alloc_page();
        if (pg == 0) {
            /* Free already allocated */
            for (uint32_t j = 0; j < i; j++)
                pmm_free_page(shm_table[slot].phys_pages[j]);
            return -1;
        }
        /* Zero the page */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(pg);
        for (uint64_t j = 0; j < PAGE_SIZE; j++)
            dst[j] = 0;
        shm_table[slot].phys_pages[i] = pg;
    }

    shm_table[slot].key = (int32_t)key;
    shm_table[slot].num_pages = (uint32_t)num_pages;
    shm_table[slot].ref_count = 0;
    return slot;
}

static int64_t sys_shmat(uint64_t shmid, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (!shm_inited) { shm_init(); shm_inited = 1; }

    if (shmid >= MAX_SHM_REGIONS || shm_table[shmid].key == -1)
        return 0;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return 0;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;

    uint32_t npages = shm_table[shmid].num_pages;
    uint64_t virt = proc->mmap_next_addr;

    /* Map each page individually and increment refcount for PTE reference */
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t page_phys = shm_table[shmid].phys_pages[i];
        uint64_t page_virt = virt + (uint64_t)i * PAGE_SIZE;
        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0)
            return 0;
        pmm_ref_inc(page_phys);
    }

    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = shm_table[shmid].phys_pages[0];
    proc->mmap_table[slot].num_pages = npages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = (int32_t)shmid;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    proc->mmap_next_addr = virt + (uint64_t)(npages + 1) * PAGE_SIZE;
    shm_table[shmid].ref_count++;

    return (int64_t)virt;
}

static int64_t sys_shmdt(uint64_t virt_addr, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (!shm_inited) return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (proc->mmap_table[i].used &&
            proc->mmap_table[i].virt_addr == virt_addr &&
            proc->mmap_table[i].shm_id >= 0) {
            int32_t sid = proc->mmap_table[i].shm_id;
            uint32_t npages = proc->mmap_table[i].num_pages;
            if (sid < MAX_SHM_REGIONS && shm_table[sid].ref_count > 0)
                shm_table[sid].ref_count--;
            /* Unmap pages from page table and drop PTE refcount */
            for (uint32_t p = 0; p < npages; p++) {
                uint64_t pv = virt_addr + (uint64_t)p * PAGE_SIZE;
                uint64_t *pte = vmm_get_pte(proc->cr3, pv);
                if (pte && (*pte & PTE_PRESENT)) {
                    uint64_t phys = *pte & PTE_ADDR_MASK;
                    *pte = 0;
                    pmm_free_page(phys);
                }
            }
            proc->mmap_table[i].used = 0;
            proc->mmap_table[i].shm_id = -1;
            return 0;
        }
    }
    return -1;
}

/* --- Stage 31 syscalls --- */

static int64_t sys_fork(uint64_t a1, uint64_t a2,
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
            pipe_t *pp = (pipe_t *)proc->fd_table[i].pipe;
            if (proc->fd_table[i].pipe_write)
                pp->write_refs++;
            else
                pp->read_refs++;
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
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (proc->mmap_table[i].used && proc->mmap_table[i].shm_id >= 0) {
            int32_t sid = proc->mmap_table[i].shm_id;
            if (sid < MAX_SHM_REGIONS)
                shm_table[sid].ref_count++;
        }
    }

    /* Schedule the child AFTER all ref counts are incremented.
     * This prevents SMP races where the child exits before ref
     * counts reflect the inherited fds/shm. */
    sched_add(child->main_thread);

    /* Adjust namespace quota after successful fork */
    agent_ns_quota_adjust(proc->ns_id, NS_QUOTA_PROCS, 1);

    return (int64_t)child->pid;
}

static int64_t sys_sigaction(uint64_t signum, uint64_t handler,
                               uint64_t flags, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc || signum >= MAX_SIGNALS) return -1;
    if (signum == SIGKILL || signum == SIGSTOP) return -1;  /* can't catch */

    proc->sig_handlers[signum].sa_handler = handler;
    proc->sig_handlers[signum].sa_flags = (uint32_t)flags;
    return 0;
}

static int64_t sys_sigreturn(uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    uint64_t frame_addr = proc->signal_frame_addr;
    if (frame_addr == 0) return -1;

    uint64_t *frame = (uint64_t *)frame_addr;
    uint64_t orig_rsp    = frame[0];
    uint64_t orig_rip    = frame[1];
    uint64_t orig_rflags = frame[2];
    uint64_t orig_rax    = frame[3];

    proc->signal_frame_addr = 0;

    /* SA_RESTART: re-invoke the interrupted syscall after signal handler */
    if (proc->restart_pending) {
        proc->restart_pending = 0;
        uint64_t snum = proc->restart_syscall_num;
        if (snum < SYS_NR) {
            uint64_t *kstack_top = (uint64_t *)(t->stack_base + t->stack_size);
            kstack_top[-1] = orig_rsp;
            kstack_top[-2] = orig_rip;
            kstack_top[-3] = orig_rflags;
            return syscall_dispatch(snum,
                proc->restart_args[0], proc->restart_args[1],
                proc->restart_args[2], proc->restart_args[3],
                proc->restart_args[4]);
        }
    }

    /* Restore original context — modify kernel stack */
    uint64_t *kstack_top = (uint64_t *)(t->stack_base + t->stack_size);
    kstack_top[-1] = orig_rsp;
    kstack_top[-2] = orig_rip;
    kstack_top[-3] = orig_rflags;

    return (int64_t)orig_rax;
}

static int64_t sys_sigprocmask(uint64_t how, uint64_t new_mask,
                                uint64_t old_mask_ptr, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -1;

    /* Return old mask if pointer provided */
    if (old_mask_ptr) {
        if (validate_user_ptr(old_mask_ptr, sizeof(uint32_t)) != 0)
            return -EFAULT;
        uint64_t *pte = vmm_get_pte(proc->cr3, old_mask_ptr);
        if (pte && (*pte & PTE_PRESENT)) {
            uint64_t phys = (*pte & PTE_ADDR_MASK) + (old_mask_ptr & 0xFFF);
            uint32_t *out = (uint32_t *)PHYS_TO_VIRT(phys);
            *out = proc->signal_mask;
        }
    }

    /* SIGKILL and SIGSTOP cannot be blocked */
    uint32_t safe_mask = (uint32_t)new_mask & ~((1U << SIGKILL) | (1U << SIGSTOP));

    switch (how) {
    case SIG_BLOCK:
        proc->signal_mask |= safe_mask;
        break;
    case SIG_UNBLOCK:
        proc->signal_mask &= ~safe_mask;
        break;
    case SIG_SETMASK:
        proc->signal_mask = safe_mask;
        break;
    default:
        return -1;
    }

    return 0;
}

/* --- Stage 64 syscalls --- */

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static int64_t sys_arch_prctl(uint64_t code, uint64_t addr,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    if (!t) return -1;

    switch (code) {
    case ARCH_SET_FS:
        /* Validate that addr is in user-space */
        if (addr >= USER_ADDR_MAX)
            return -EFAULT;
        t->fs_base = addr;
        /* Apply immediately */
        {
            uint32_t lo = (uint32_t)addr;
            uint32_t hi = (uint32_t)(addr >> 32);
            __asm__ volatile ("wrmsr" : : "c"((uint32_t)0xC0000100), "a"(lo), "d"(hi));
        }
        return 0;
    case ARCH_GET_FS:
        if (t->process && addr) {
            if (validate_user_ptr(addr, sizeof(uint64_t)) != 0)
                return -EFAULT;
            uint64_t *pte = vmm_get_pte(t->process->cr3, addr);
            if (pte && (*pte & PTE_PRESENT)) {
                uint64_t phys = (*pte & PTE_ADDR_MASK) + (addr & 0xFFF);
                uint64_t *out = (uint64_t *)PHYS_TO_VIRT(phys);
                *out = t->fs_base;
            }
        }
        return 0;
    default:
        return -1;
    }
}

/* Forward declaration for poll_check_fd (defined in Stage 34 section) */
static int16_t poll_check_fd(process_t *proc, int fd, int16_t events);

/* select() fd_set is a 64-bit bitmask (supports fds 0-63) */
typedef struct { uint64_t bits; } fd_set_kern_t;

static int64_t sys_select(uint64_t nfds, uint64_t readfds_ptr,
                            uint64_t writefds_ptr, uint64_t timeout_us,
                            uint64_t a5) {
    (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -1;
    if (nfds > MAX_FDS) nfds = MAX_FDS;

    /* Read fd_sets from user space */
    uint64_t rfds = 0, wfds = 0;
    if (readfds_ptr) {
        if (validate_user_ptr(readfds_ptr, sizeof(uint64_t)) != 0)
            return -EFAULT;
        uint64_t *pte = vmm_get_pte(proc->cr3, readfds_ptr);
        if (pte && (*pte & PTE_PRESENT)) {
            uint64_t phys = (*pte & PTE_ADDR_MASK) + (readfds_ptr & 0xFFF);
            rfds = *(uint64_t *)PHYS_TO_VIRT(phys);
        }
    }
    if (writefds_ptr) {
        if (validate_user_ptr(writefds_ptr, sizeof(uint64_t)) != 0)
            return -EFAULT;
        uint64_t *pte = vmm_get_pte(proc->cr3, writefds_ptr);
        if (pte && (*pte & PTE_PRESENT)) {
            uint64_t phys = (*pte & PTE_ADDR_MASK) + (writefds_ptr & 0xFFF);
            wfds = *(uint64_t *)PHYS_TO_VIRT(phys);
        }
    }

    /* Calculate timeout in PIT ticks */
    uint64_t deadline = 0;
    int has_timeout = (timeout_us != (uint64_t)-1);
    if (has_timeout && timeout_us > 0) {
        uint64_t delay_ticks = (timeout_us * 18) / 1000000;
        if (delay_ticks == 0) delay_ticks = 1;
        deadline = pit_get_ticks() + delay_ticks;
    }

    /* Poll loop */
    for (;;) {
        uint64_t r_out = 0, w_out = 0;
        int ready = 0;

        for (uint64_t fd = 0; fd < nfds; fd++) {
            int16_t events = 0;
            if (rfds & (1ULL << fd)) events |= POLLIN;
            if (wfds & (1ULL << fd)) events |= POLLOUT;
            if (!events) continue;

            int16_t revents = poll_check_fd(proc, (int)fd, events);
            if (revents & (POLLIN | POLLERR | POLLHUP))
                if (rfds & (1ULL << fd)) { r_out |= (1ULL << fd); ready++; }
            if (revents & (POLLOUT | POLLERR))
                if (wfds & (1ULL << fd)) { w_out |= (1ULL << fd); ready++; }
        }

        if (ready > 0 || (has_timeout && timeout_us == 0)) {
            /* Write results back (pointers already validated above) */
            if (readfds_ptr) {
                uint64_t *pte = vmm_get_pte(proc->cr3, readfds_ptr);
                if (pte && (*pte & PTE_PRESENT)) {
                    uint64_t phys = (*pte & PTE_ADDR_MASK) + (readfds_ptr & 0xFFF);
                    *(uint64_t *)PHYS_TO_VIRT(phys) = r_out;
                }
            }
            if (writefds_ptr) {
                uint64_t *pte = vmm_get_pte(proc->cr3, writefds_ptr);
                if (pte && (*pte & PTE_PRESENT)) {
                    uint64_t phys = (*pte & PTE_ADDR_MASK) + (writefds_ptr & 0xFFF);
                    *(uint64_t *)PHYS_TO_VIRT(phys) = w_out;
                }
            }
            return ready;
        }

        if (has_timeout && pit_get_ticks() >= deadline)
            return 0;  /* timeout */

        if (proc->pending_signals & ~proc->signal_mask)
            return -EINTR;
        sched_yield();
    }
}

/* --- Supervisor syscalls --- */

static int64_t sys_super_create(uint64_t name_ptr, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    char name_buf[64];
    const char *name = "supervisor";
    if (name_ptr) {
        if (copy_string_from_user((const char *)name_ptr, name_buf, sizeof(name_buf)) != 0)
            return -EFAULT;
        name = name_buf;
    }

    return supervisor_create(t->process->pid, name);
}

static int64_t sys_super_add(uint64_t super_id, uint64_t elf_path_ptr,
                              uint64_t ns_id, uint64_t caps, uint64_t a5) {
    (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    char path_buf[64];
    const char *path = "";
    if (elf_path_ptr) {
        if (copy_string_from_user((const char *)elf_path_ptr, path_buf, sizeof(path_buf)) != 0)
            return -EFAULT;
        path = path_buf;
    }

    return supervisor_add_child((uint32_t)super_id, path, (int64_t)ns_id, caps);
}

static int64_t sys_super_set_policy(uint64_t super_id, uint64_t policy,
                                     uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return supervisor_set_policy((uint32_t)super_id, (uint8_t)policy);
}

static int64_t sys_super_start(uint64_t super_id, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return supervisor_start((uint32_t)super_id);
}

/* --- Stage 32 syscalls --- */

static int64_t sys_openpty(uint64_t master_fd_ptr, uint64_t slave_fd_ptr,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (validate_user_ptr(master_fd_ptr, sizeof(long)) != 0 ||
        validate_user_ptr(slave_fd_ptr, sizeof(long)) != 0)
        return -1;

    int pty_idx = pty_alloc();
    if (pty_idx < 0) return -1;

    pty_t *pt = pty_get(pty_idx);
    if (!pt) return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* Find two free fd slots */
    int mfd = -1, sfd = -1;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            if (mfd < 0) mfd = fd;
            else if (sfd < 0) { sfd = fd; break; }
        }
    }
    if (mfd < 0 || sfd < 0) {
        pt->used = 0;
        return -1;
    }

    /* Set up master fd */
    proc->fd_table[mfd].node = NULL;
    proc->fd_table[mfd].offset = 0;
    proc->fd_table[mfd].pipe = NULL;
    proc->fd_table[mfd].pipe_write = 0;
    proc->fd_table[mfd].pty = (void *)pt;
    proc->fd_table[mfd].pty_is_master = 1;
    proc->fd_table[mfd].unix_sock = NULL;
    proc->fd_table[mfd].eventfd = NULL;
    proc->fd_table[mfd].epoll = NULL;
    proc->fd_table[mfd].uring = NULL;
    proc->fd_table[mfd].open_flags = 0;
    proc->fd_table[mfd].fd_flags = 0;
    pt->master_refs = 1;

    /* Set up slave fd */
    proc->fd_table[sfd].node = NULL;
    proc->fd_table[sfd].offset = 0;
    proc->fd_table[sfd].pipe = NULL;
    proc->fd_table[sfd].pipe_write = 0;
    proc->fd_table[sfd].pty = (void *)pt;
    proc->fd_table[sfd].pty_is_master = 0;
    proc->fd_table[sfd].unix_sock = NULL;
    proc->fd_table[sfd].eventfd = NULL;
    proc->fd_table[sfd].epoll = NULL;
    proc->fd_table[sfd].uring = NULL;
    proc->fd_table[sfd].open_flags = 0;
    proc->fd_table[sfd].fd_flags = 0;
    pt->slave_refs = 1;

    *(long *)master_fd_ptr = mfd;
    *(long *)slave_fd_ptr = sfd;

    return 0;
}

static int64_t sys_tcp_socket(uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    int64_t idx = tcp_socket();
    if (idx >= 0 && idx < 8) {
        thread_t *t = thread_get_current();
        if (t && t->process)
            t->process->tcp_conns[idx] = 1;
    }
    return idx;
}

static int64_t sys_tcp_connect(uint64_t conn_idx, uint64_t ip, uint64_t port,
                                 uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    return tcp_connect((int)conn_idx, (uint32_t)ip, (uint16_t)port);
}

static int64_t sys_tcp_listen(uint64_t conn_idx, uint64_t port,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return tcp_listen((int)conn_idx, (uint16_t)port);
}

static int64_t sys_tcp_accept(uint64_t listen_conn, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    int64_t idx = tcp_accept((int)listen_conn);
    if (idx >= 0 && idx < 8) {
        thread_t *t = thread_get_current();
        if (t && t->process)
            t->process->tcp_conns[idx] = 1;
    }
    return idx;
}

static int64_t sys_tcp_send(uint64_t conn_idx, uint64_t buf_ptr, uint64_t len,
                              uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    return tcp_send((int)conn_idx, (const uint8_t *)buf_ptr, (uint32_t)len);
}

static int64_t sys_tcp_recv(uint64_t conn_idx, uint64_t buf_ptr, uint64_t len,
                              uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    return tcp_recv((int)conn_idx, (uint8_t *)buf_ptr, (uint32_t)len);
}

static int64_t sys_tcp_close(uint64_t conn_idx, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    int64_t r = tcp_close((int)conn_idx);
    if (r == 0 && conn_idx < 8) {
        thread_t *t = thread_get_current();
        if (t && t->process)
            t->process->tcp_conns[conn_idx] = 0;
    }
    return r;
}

static int64_t sys_tcp_setopt(uint64_t conn_idx, uint64_t opt, uint64_t value,
                               uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    /* opt 1 = O_NONBLOCK */
    if (opt == 1)
        return tcp_set_nonblock((int)conn_idx, (int)value);
    return -EINVAL;
}

static int64_t sys_tcp_to_fd(uint64_t conn_idx, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (conn_idx >= MAX_TCP_CONNS) return -EINVAL;
    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -1;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            fd_entry_t *e = &proc->fd_table[fd];
            e->tcp_conn_idx = (int16_t)conn_idx;
            e->open_flags = O_RDWR;
            e->fd_flags = 0;
            return fd;
        }
    }
    return -EMFILE;
}

/* --- Stage 34 syscalls --- */

static int64_t sys_clock_gettime(uint64_t clockid, uint64_t ts_ptr,
                                   uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (clockid != CLOCK_MONOTONIC)
        return -EINVAL;
    if (validate_user_ptr(ts_ptr, sizeof(timespec_t)) != 0)
        return -EFAULT;

    uint64_t ticks = pit_get_ticks();
    timespec_t *ts = (timespec_t *)ts_ptr;
    ts->tv_sec = (int64_t)(ticks / 18);
    ts->tv_nsec = (int64_t)((ticks % 18) * 54945055);
    return 0;
}

static int64_t sys_nanosleep(uint64_t ts_ptr, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (validate_user_ptr(ts_ptr, sizeof(timespec_t)) != 0)
        return -EFAULT;

    const timespec_t *ts = (const timespec_t *)ts_ptr;
    uint64_t delay_ticks = (uint64_t)ts->tv_sec * 18 +
                           (uint64_t)ts->tv_nsec / 54945055;
    if (delay_ticks == 0) delay_ticks = 1;

    uint64_t deadline = pit_get_ticks() + delay_ticks;
    thread_t *ns_t = thread_get_current();
    process_t *ns_proc = ns_t ? ns_t->process : NULL;
    while (pit_get_ticks() < deadline) {
        if (ns_proc && (ns_proc->pending_signals & ~ns_proc->signal_mask))
            return -EINTR;
        sched_yield();
    }
    return 0;
}

static int64_t sys_getenv(uint64_t key_ptr, uint64_t val_ptr, uint64_t val_size,
                            uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    char key[128];
    if (copy_string_from_user((const char *)key_ptr, key, sizeof(key)) != 0)
        return -EFAULT;
    if (val_size > 0 && validate_user_ptr(val_ptr, val_size) != 0)
        return -EFAULT;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -ESRCH;

    int klen = 0;
    while (key[klen]) klen++;

    /* Scan env_buf for "KEY=" match */
    int pos = 0;
    while (pos < proc->env_buf_len) {
        /* Check if this entry starts with key= */
        int match = 1;
        for (int i = 0; i < klen; i++) {
            if (pos + i >= proc->env_buf_len || proc->env_buf[pos + i] != key[i]) {
                match = 0;
                break;
            }
        }
        if (match && pos + klen < proc->env_buf_len && proc->env_buf[pos + klen] == '=') {
            /* Found it — copy value */
            const char *val = &proc->env_buf[pos + klen + 1];
            int vlen = 0;
            while (val[vlen]) vlen++;
            if (val_size > 0) {
                char *dst = (char *)val_ptr;
                int copy = vlen < (int)val_size - 1 ? vlen : (int)val_size - 1;
                for (int i = 0; i < copy; i++)
                    dst[i] = val[i];
                dst[copy] = '\0';
            }
            return (int64_t)vlen;
        }
        /* Skip to next entry */
        while (pos < proc->env_buf_len && proc->env_buf[pos] != '\0') pos++;
        pos++;  /* skip NUL */
    }

    return -ENOENT;
}

static int64_t sys_setenv(uint64_t key_ptr, uint64_t val_ptr,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    char key[128], val[256];
    if (copy_string_from_user((const char *)key_ptr, key, sizeof(key)) != 0)
        return -EFAULT;
    if (copy_string_from_user((const char *)val_ptr, val, sizeof(val)) != 0)
        return -EFAULT;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -ESRCH;

    int klen = 0;
    while (key[klen]) klen++;
    int vlen = 0;
    while (val[vlen]) vlen++;

    /* Remove existing key if present (compact buf) */
    int pos = 0;
    while (pos < proc->env_buf_len) {
        int entry_start = pos;
        /* Check match */
        int match = 1;
        for (int i = 0; i < klen; i++) {
            if (pos + i >= proc->env_buf_len || proc->env_buf[pos + i] != key[i]) {
                match = 0;
                break;
            }
        }
        /* Skip to next entry */
        int entry_end = pos;
        while (entry_end < proc->env_buf_len && proc->env_buf[entry_end] != '\0') entry_end++;
        entry_end++;  /* include NUL */

        if (match && pos + klen < proc->env_buf_len && proc->env_buf[pos + klen] == '=') {
            /* Remove this entry by shifting */
            int remaining = proc->env_buf_len - entry_end;
            for (int i = 0; i < remaining; i++)
                proc->env_buf[entry_start + i] = proc->env_buf[entry_end + i];
            proc->env_buf_len -= (entry_end - entry_start);
            proc->env_count--;
            continue;  /* don't advance pos, re-check from same position */
        }
        pos = entry_end;
    }

    /* Append "KEY=VALUE\0" */
    int needed = klen + 1 + vlen + 1;  /* key + '=' + value + NUL */
    if (proc->env_buf_len + needed > PROC_ENV_BUF_SIZE)
        return -ENOMEM;

    int wp = proc->env_buf_len;
    for (int i = 0; i < klen; i++)
        proc->env_buf[wp++] = key[i];
    proc->env_buf[wp++] = '=';
    for (int i = 0; i < vlen; i++)
        proc->env_buf[wp++] = val[i];
    proc->env_buf[wp++] = '\0';
    proc->env_buf_len = wp;
    proc->env_count++;
    return 0;
}

/* poll helper: check readiness of a single fd */
static int16_t poll_check_fd(process_t *proc, int fd, int16_t events) {
    int16_t revents = 0;

    if (fd < 0 || fd >= MAX_FDS)
        return POLLERR;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (fd_is_free(entry))
        return POLLERR;

    /* Pipe */
    if (entry->pipe != NULL) {
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

static int64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms,
                          uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    if (nfds == 0) return 0;
    if (nfds > MAX_FDS) return -EINVAL;
    if (validate_user_ptr(fds_ptr, nfds * sizeof(pollfd_t)) != 0)
        return -EFAULT;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -ESRCH;

    pollfd_t *fds = (pollfd_t *)fds_ptr;

    /* Calculate deadline */
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        uint64_t delay_ticks = timeout_ms * 18 / 1000;
        if (delay_ticks == 0) delay_ticks = 1;
        deadline = pit_get_ticks() + delay_ticks;
    }

    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            fds[i].revents = poll_check_fd(proc, fds[i].fd, fds[i].events);
            if (fds[i].revents)
                ready++;
        }

        if (ready > 0)
            return (int64_t)ready;

        /* timeout_ms == 0: immediate return */
        if (timeout_ms == 0)
            return 0;

        /* Check deadline for positive timeout */
        if (timeout_ms > 0 && pit_get_ticks() >= deadline)
            return 0;

        if (proc->pending_signals & ~proc->signal_mask)
            return -EINTR;
        sched_yield();
    }
}

/* --- ioctl --- */

static int64_t sys_ioctl(uint64_t fd_num, uint64_t cmd, uint64_t arg,
                          uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *proc = t->process;

    if (fd_num >= MAX_FDS) return -1;
    fd_entry_t *fde = &proc->fd_table[fd_num];

    /* PTY ioctl */
    if (fde->pty) {
        int pidx = pty_index((pty_t *)fde->pty);
        if (pidx < 0) return -1;
        return pty_ioctl(pidx, cmd, arg);
    }

    return -1;  /* unsupported fd type */
}

/* --- Stage 35 syscalls --- */

static int64_t sys_getuid(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return (int64_t)t->process->uid;
}

static int64_t sys_setuid(uint64_t uid, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *proc = t->process;
    if (proc->uid != 0 && !(proc->capabilities & CAP_SETUID))
        return -EPERM;
    proc->uid = (uint16_t)uid;
    /* Root dropping to non-root loses CAP_ALL, keeps inherited caps */
    if (uid != 0 && proc->capabilities == CAP_ALL)
        proc->capabilities = CAP_ALL;  /* keep all caps until explicit setcap */
    return 0;
}

static int64_t sys_getgid(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return (int64_t)t->process->gid;
}

static int64_t sys_setgid(uint64_t gid, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *proc = t->process;
    if (proc->uid != 0 && !(proc->capabilities & CAP_SETUID))
        return -EPERM;
    proc->gid = (uint16_t)gid;
    return 0;
}

static int64_t sys_getcap(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return (int64_t)t->process->capabilities;
}

static int64_t sys_setcap(uint64_t pid_arg, uint64_t caps,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *caller = t->process;

    /* Only root or CAP_SYS_ADMIN can set capabilities */
    if (caller->uid != 0 && !(caller->capabilities & CAP_SYS_ADMIN))
        return -EPERM;

    /* Cannot grant caps the caller doesn't have */
    if (caps & ~caller->capabilities)
        return -EPERM;

    uint64_t pid = pid_arg == 0 ? caller->pid : pid_arg;
    process_t *target = process_lookup(pid);
    if (!target) return -ESRCH;

    target->capabilities = (uint32_t)caps;
    return 0;
}

static int64_t sys_getrlimit(uint64_t resource, uint64_t ptr,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(ptr, sizeof(rlimit_t)) != 0)
        return -EFAULT;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *proc = t->process;
    rlimit_t *rl = (rlimit_t *)ptr;

    switch (resource) {
    case RLIMIT_MEM:
        rl->current = proc->used_mem_pages;
        rl->max = proc->rlimit_mem_pages;
        return 0;
    case RLIMIT_CPU:
        rl->current = proc->main_thread ? proc->main_thread->ticks_used : 0;
        rl->max = proc->rlimit_cpu_ticks;
        return 0;
    case RLIMIT_NFDS:
        rl->current = (uint64_t)count_open_fds(proc);
        rl->max = (uint64_t)proc->rlimit_nfds;
        return 0;
    default:
        return -EINVAL;
    }
}

static int64_t sys_setrlimit(uint64_t resource, uint64_t ptr,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(ptr, sizeof(rlimit_t)) != 0)
        return -EFAULT;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *proc = t->process;

    /* Only root or CAP_SYS_ADMIN */
    if (proc->uid != 0 && !(proc->capabilities & CAP_SYS_ADMIN))
        return -EPERM;

    const rlimit_t *rl = (const rlimit_t *)ptr;
    switch (resource) {
    case RLIMIT_MEM:
        proc->rlimit_mem_pages = rl->max;
        return 0;
    case RLIMIT_CPU:
        proc->rlimit_cpu_ticks = rl->max;
        return 0;
    case RLIMIT_NFDS:
        proc->rlimit_nfds = (uint32_t)rl->max;
        return 0;
    default:
        return -EINVAL;
    }
}

static int64_t sys_seccomp(uint64_t mask, uint64_t strict,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *proc = t->process;

    if (proc->seccomp_mask != 0) {
        /* Already set — can only restrict further (AND) */
        proc->seccomp_mask &= mask;
    } else {
        proc->seccomp_mask = mask;
    }
    if (strict)
        proc->seccomp_strict = 1;
    return 0;
}

static int64_t sys_setaudit(uint64_t pid_arg, uint64_t flags,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *caller = t->process;

    /* Only root or CAP_SYS_ADMIN */
    if (caller->uid != 0 && !(caller->capabilities & CAP_SYS_ADMIN))
        return -EPERM;

    uint64_t pid = pid_arg == 0 ? caller->pid : pid_arg;
    process_t *target = process_lookup(pid);
    if (!target) return -ESRCH;

    target->audit_flags = (uint8_t)flags;
    return 0;
}

/* --- Page fault handler (called from ISR 14 in idt.c) --- */

static inline void invlpg_addr(uint64_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

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
    vmm_free_user_pages(proc->cr3);
    proc->cr3 = 0;
    t->state = THREAD_DEAD;
    schedule();
    return 0;
}

/* --- Stage 36 syscalls: Agent Communication --- */

static int64_t sys_unix_socket(uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    int idx = unix_sock_alloc();
    if (idx < 0) return -1;

    unix_sock_t *us = unix_sock_get(idx);
    if (!us) return -1;

    /* Find free fd */
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            proc->fd_table[fd].node = NULL;
            proc->fd_table[fd].pipe = NULL;
            proc->fd_table[fd].pipe_write = 0;
            proc->fd_table[fd].pty = NULL;
            proc->fd_table[fd].pty_is_master = 0;
            proc->fd_table[fd].unix_sock = (void *)us;
            proc->fd_table[fd].eventfd = NULL;
            proc->fd_table[fd].epoll = NULL;
            proc->fd_table[fd].uring = NULL;
            proc->fd_table[fd].open_flags = O_RDWR;
            proc->fd_table[fd].fd_flags = 0;
            return fd;
        }
    }
    unix_sock_close(us);
    return -EMFILE;
}

static int64_t sys_unix_bind(uint64_t fd, uint64_t path_ptr,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS) return -EINVAL;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->unix_sock == NULL) return -EINVAL;

    char raw_path[UNIX_SOCK_PATH_MAX], path[UNIX_SOCK_PATH_MAX];
    if (copy_string_from_user((const char *)path_ptr, raw_path, UNIX_SOCK_PATH_MAX) != 0)
        return -EFAULT;
    resolve_user_path(proc, raw_path, path);

    unix_sock_t *us = (unix_sock_t *)entry->unix_sock;
    /* Find index */
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (unix_sock_get(i) == us)
            return unix_sock_bind(i, path);
    }
    return -EINVAL;
}

static int64_t sys_unix_listen(uint64_t fd, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS) return -EINVAL;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->unix_sock == NULL) return -EINVAL;

    unix_sock_t *us = (unix_sock_t *)entry->unix_sock;
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (unix_sock_get(i) == us)
            return unix_sock_listen(i);
    }
    return -EINVAL;
}

static int64_t sys_unix_accept(uint64_t fd, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS) return -EINVAL;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->unix_sock == NULL) return -EINVAL;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    unix_sock_t *us = (unix_sock_t *)entry->unix_sock;
    int listen_idx = -1;
    for (int i = 0; i < MAX_UNIX_SOCKS; i++) {
        if (unix_sock_get(i) == us) { listen_idx = i; break; }
    }
    if (listen_idx < 0) return -EINVAL;

    /* Busy-wait for a connection */
    int server_idx;
    for (;;) {
        server_idx = unix_sock_accept(listen_idx);
        if (server_idx >= 0) break;
        if (entry->fd_flags & 0x02)  /* O_NONBLOCK */
            return -EAGAIN;
        if (proc->pending_signals & ~proc->signal_mask)
            return -EINTR;
        sched_yield();
    }

    unix_sock_t *server = unix_sock_get(server_idx);
    if (!server) return -EINVAL;

    /* Find free fd for accepted socket */
    for (int nfd = 0; nfd < MAX_FDS; nfd++) {
        if (fd_is_free(&proc->fd_table[nfd])) {
            proc->fd_table[nfd].node = NULL;
            proc->fd_table[nfd].pipe = NULL;
            proc->fd_table[nfd].pipe_write = 0;
            proc->fd_table[nfd].pty = NULL;
            proc->fd_table[nfd].pty_is_master = 0;
            proc->fd_table[nfd].unix_sock = (void *)server;
            proc->fd_table[nfd].eventfd = NULL;
            proc->fd_table[nfd].epoll = NULL;
            proc->fd_table[nfd].uring = NULL;
            proc->fd_table[nfd].open_flags = O_RDWR;
            proc->fd_table[nfd].fd_flags = 0;
            return nfd;
        }
    }
    unix_sock_close(server);
    return -EMFILE;
}

static int64_t sys_unix_connect(uint64_t path_ptr, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    char raw_path[UNIX_SOCK_PATH_MAX], path[UNIX_SOCK_PATH_MAX];
    if (copy_string_from_user((const char *)path_ptr, raw_path, UNIX_SOCK_PATH_MAX) != 0)
        return -EFAULT;
    resolve_user_path(proc, raw_path, path);

    int client_idx = unix_sock_connect(path);
    if (client_idx < 0) return client_idx;

    unix_sock_t *client = unix_sock_get(client_idx);
    if (!client) return -EINVAL;

    /* Find free fd for client socket */
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            proc->fd_table[fd].node = NULL;
            proc->fd_table[fd].pipe = NULL;
            proc->fd_table[fd].pipe_write = 0;
            proc->fd_table[fd].pty = NULL;
            proc->fd_table[fd].pty_is_master = 0;
            proc->fd_table[fd].unix_sock = (void *)client;
            proc->fd_table[fd].eventfd = NULL;
            proc->fd_table[fd].epoll = NULL;
            proc->fd_table[fd].uring = NULL;
            proc->fd_table[fd].open_flags = O_RDWR;
            proc->fd_table[fd].fd_flags = 0;
            return fd;
        }
    }
    unix_sock_close(client);
    return -EMFILE;
}

static int64_t sys_agent_register(uint64_t name_ptr, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    char name[AGENT_NAME_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, AGENT_NAME_MAX) != 0)
        return -EFAULT;

    return agent_register_ns(name, proc->pid, proc->ns_id);
}

static int64_t sys_agent_lookup(uint64_t name_ptr, uint64_t pid_out_ptr,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    char name[AGENT_NAME_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, AGENT_NAME_MAX) != 0)
        return -EFAULT;

    uint64_t pid = 0;
    int ret = agent_lookup_ns(name, &pid, proc->ns_id);
    if (ret != 0) return -1;

    if (pid_out_ptr != 0) {
        if (validate_user_ptr(pid_out_ptr, sizeof(uint64_t)) != 0)
            return -EFAULT;
        *(uint64_t *)pid_out_ptr = pid;
    }
    return 0;
}

static int64_t sys_eventfd(uint64_t flags, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    int idx = eventfd_alloc((uint32_t)flags);
    if (idx < 0) return -1;

    eventfd_t *efd = eventfd_get(idx);
    if (!efd) return -1;

    /* Find free fd */
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            proc->fd_table[fd].node = NULL;
            proc->fd_table[fd].pipe = NULL;
            proc->fd_table[fd].pipe_write = 0;
            proc->fd_table[fd].pty = NULL;
            proc->fd_table[fd].pty_is_master = 0;
            proc->fd_table[fd].unix_sock = NULL;
            proc->fd_table[fd].eventfd = (void *)efd;
            proc->fd_table[fd].epoll = NULL;
            proc->fd_table[fd].uring = NULL;
            proc->fd_table[fd].open_flags = O_RDWR;
            proc->fd_table[fd].fd_flags = 0;
            /* If EFD_NONBLOCK set, mark fd as nonblock */
            if (flags & EFD_NONBLOCK)
                proc->fd_table[fd].fd_flags |= 0x02;
            return fd;
        }
    }
    eventfd_close(idx);
    return -EMFILE;
}

/* --- Stage 37 syscalls: Agent Scale --- */

static int64_t sys_epoll_create(uint64_t flags, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)flags; (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    int idx = epoll_create_instance();
    if (idx < 0) return -ENOMEM;

    epoll_instance_t *ep = epoll_get(idx);
    if (!ep) return -ENOMEM;

    /* Find free fd */
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            proc->fd_table[fd].node = NULL;
            proc->fd_table[fd].pipe = NULL;
            proc->fd_table[fd].pipe_write = 0;
            proc->fd_table[fd].pty = NULL;
            proc->fd_table[fd].pty_is_master = 0;
            proc->fd_table[fd].unix_sock = NULL;
            proc->fd_table[fd].eventfd = NULL;
            proc->fd_table[fd].epoll = (void *)ep;
            proc->fd_table[fd].uring = NULL;
            proc->fd_table[fd].open_flags = 0;
            proc->fd_table[fd].fd_flags = 0;
            return fd;
        }
    }
    epoll_close(idx);
    return -EMFILE;
}

static int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd,
                               uint64_t event_ptr, uint64_t a5) {
    (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (epfd >= MAX_FDS) return -EBADF;
    fd_entry_t *ep_entry = &proc->fd_table[epfd];
    if (ep_entry->epoll == NULL) return -EINVAL;

    int ep_idx = epoll_index((epoll_instance_t *)ep_entry->epoll);
    if (ep_idx < 0) return -EINVAL;

    epoll_event_t ev = {0, 0};
    if (op != EPOLL_CTL_DEL) {
        if (validate_user_ptr(event_ptr, sizeof(epoll_event_t)) != 0)
            return -EFAULT;
        const epoll_event_t *user_ev = (const epoll_event_t *)event_ptr;
        ev.events = user_ev->events;
        ev.data = user_ev->data;
    }

    return epoll_ctl(ep_idx, (int)op, (int)fd, &ev);
}

static int64_t sys_epoll_wait(uint64_t epfd, uint64_t events_ptr,
                                uint64_t max_events, uint64_t timeout_ms,
                                uint64_t a5) {
    (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (epfd >= MAX_FDS) return -EBADF;
    if (max_events == 0) return 0;
    if (validate_user_ptr(events_ptr, max_events * sizeof(epoll_event_t)) != 0)
        return -EFAULT;

    fd_entry_t *ep_entry = &proc->fd_table[epfd];
    if (ep_entry->epoll == NULL) return -EINVAL;

    epoll_instance_t *ep = (epoll_instance_t *)ep_entry->epoll;
    epoll_event_t *out = (epoll_event_t *)events_ptr;

    /* Calculate deadline */
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        uint64_t delay_ticks = timeout_ms * 18 / 1000;
        if (delay_ticks == 0) delay_ticks = 1;
        deadline = pit_get_ticks() + delay_ticks;
    }

    for (;;) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < EPOLL_MAX_FDS && count < max_events; i++) {
            if (ep->interests[i].fd < 0) continue;
            int16_t revents = poll_check_fd(proc, ep->interests[i].fd,
                                            (int16_t)ep->interests[i].events);
            if (revents) {
                out[count].events = (uint32_t)revents;
                out[count].data = ep->interests[i].data;
                count++;
            }
        }
        if (count > 0)
            return (int64_t)count;

        if (timeout_ms == 0)
            return 0;

        if (timeout_ms > 0 && pit_get_ticks() >= deadline)
            return 0;

        if (proc->pending_signals & ~proc->signal_mask)
            return -EINTR;
        sched_yield();
    }
}

static int64_t sys_swap_stat(uint64_t stat_ptr, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (validate_user_ptr(stat_ptr, 8) != 0)
        return -EFAULT;

    uint32_t total, used;
    swap_stat(&total, &used);

    uint32_t *out = (uint32_t *)stat_ptr;
    out[0] = total;
    out[1] = used;
    return 0;
}

static int64_t sys_infer_register(uint64_t name_ptr, uint64_t path_ptr,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    char name[INFER_NAME_MAX], path[INFER_SOCK_PATH_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, INFER_NAME_MAX) != 0)
        return -EFAULT;
    if (copy_string_from_user((const char *)path_ptr, path, INFER_SOCK_PATH_MAX) != 0)
        return -EFAULT;

    /* Require CAP_INFER to register as a provider */
    if (!(proc->capabilities & CAP_INFER) &&
        !cap_token_check(proc->pid, CAP_INFER, name))
        return -EACCES;

    return infer_register(name, path, proc->pid);
}

static int64_t sys_infer_request(uint64_t name_ptr, uint64_t req_buf,
                                   uint64_t req_len, uint64_t resp_buf,
                                   uint64_t resp_len) {
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    char name[INFER_NAME_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, INFER_NAME_MAX) != 0)
        return -EFAULT;
    if (req_len > 0 && validate_user_ptr(req_buf, req_len) != 0)
        return -EFAULT;
    if (resp_len > 0 && validate_user_ptr(resp_buf, resp_len) != 0)
        return -EFAULT;

    /* Require CAP_INFER to make inference requests */
    if (!(proc->capabilities & CAP_INFER) &&
        !cap_token_check(proc->pid, CAP_INFER, name))
        return -EACCES;

    /* Cache check: skip if req_len == 0 (side-effect request) */
    if (req_len > 0 && resp_len > 0) {
        int cached = infer_cache_lookup(name, (const void *)req_buf,
                                         (uint32_t)req_len,
                                         (void *)resp_buf, (uint32_t)resp_len);
        if (cached > 0) return cached;
    }

    /* Route to best available service (load-balanced) */
    int svc_idx = infer_route(name);
    if (svc_idx < 0) {
        /* No provider available — queue the request and wait */
        int slot = infer_queue_enqueue(name, proc->pid);
        if (slot < 0) return -ENOBUFS;  /* queue full */

        /* Block until provider becomes available or timeout */
        for (;;) {
            int status = infer_queue_check(slot);
            if (status == 1) {
                /* Provider available — retry route */
                infer_queue_remove(slot);
                svc_idx = infer_route(name);
                if (svc_idx < 0) return -ENOENT;
                break;
            }
            if (status == -EAGAIN) {
                /* Timed out */
                infer_queue_remove(slot);
                return -EAGAIN;
            }
            if (proc->pending_signals & ~proc->signal_mask) {
                infer_queue_remove(slot);
                return -EINTR;
            }
            sched_yield();
        }
    }

    infer_service_t *svc = infer_get(svc_idx);
    if (!svc) return -ENOENT;

    /* Connect to service via unix socket (internal, no fd_table entry) */
    int client_idx = unix_sock_connect(svc->sock_path);
    if (client_idx < 0) return -ECONNREFUSED;

    unix_sock_t *client = unix_sock_get(client_idx);
    if (!client) return -ECONNREFUSED;

    /* Send request */
    if (req_len > 0) {
        int sent = unix_sock_send(client, (const uint8_t *)req_buf,
                                  (uint32_t)req_len, 0);
        if (sent < 0) {
            unix_sock_close(client);
            return sent;
        }
    }

    /* Receive response (yield-based wait, max 1000 iterations) */
    int received = 0;
    if (resp_len > 0) {
        for (int attempt = 0; attempt < 1000; attempt++) {
            received = unix_sock_recv(client, (uint8_t *)resp_buf,
                                      (uint32_t)resp_len, 1);
            if (received > 0) break;
            if (client->peer_closed) break;
            sched_yield();
        }
    }

    unix_sock_close(client);

    /* Cache the response for future lookups */
    if (received > 0 && req_len > 0 &&
        (uint32_t)received <= INFER_CACHE_RESP_MAX) {
        infer_cache_insert(name, (const void *)req_buf, (uint32_t)req_len,
                           (const void *)resp_buf, (uint32_t)received);
    }

    return received > 0 ? received : -ENOENT;
}

static int64_t sys_uring_setup(uint64_t entries, uint64_t params_ptr,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)params_ptr; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* fd limit check */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)count_open_fds(proc) >= proc->rlimit_nfds)
        return -EMFILE;

    int idx = uring_create((uint32_t)entries);
    if (idx < 0) return -ENOMEM;

    uring_instance_t *ur = uring_get(idx);
    if (!ur) return -ENOMEM;

    /* Find free fd */
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            proc->fd_table[fd].node = NULL;
            proc->fd_table[fd].pipe = NULL;
            proc->fd_table[fd].pipe_write = 0;
            proc->fd_table[fd].pty = NULL;
            proc->fd_table[fd].pty_is_master = 0;
            proc->fd_table[fd].unix_sock = NULL;
            proc->fd_table[fd].eventfd = NULL;
            proc->fd_table[fd].epoll = NULL;
            proc->fd_table[fd].uring = (void *)ur;
            proc->fd_table[fd].open_flags = 0;
            proc->fd_table[fd].fd_flags = 0;
            return fd;
        }
    }
    uring_close(idx);
    return -EMFILE;
}

/* uring helper: perform a read operation on an fd (reuses sys_read logic) */
static int64_t uring_do_read(process_t *proc, int fd, uint8_t *buf, uint32_t len) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    fd_entry_t *entry = &proc->fd_table[fd];
    if (fd_is_free(entry)) return -EBADF;

    /* Pipe read */
    if (entry->pipe != NULL) {
        pipe_t *pp = (pipe_t *)entry->pipe;
        uint64_t total = 0;
        while (total < len && pp->count > 0) {
            buf[total] = pp->buf[pp->read_pos];
            pp->read_pos = (pp->read_pos + 1) % PIPE_BUF_SIZE;
            pp->count--;
            total++;
        }
        return (int64_t)total;
    }

    /* PTY read */
    if (entry->pty != NULL) {
        int pty_idx = pty_index((pty_t *)entry->pty);
        if (pty_idx < 0) return -1;
        if (entry->pty_is_master)
            return pty_master_read(pty_idx, buf, len);
        else
            return pty_slave_read(pty_idx, buf, len, 1);
    }

    /* Unix socket */
    if (entry->unix_sock != NULL)
        return unix_sock_recv((unix_sock_t *)entry->unix_sock, buf, len, 1);

    /* Regular file */
    if (entry->node != NULL) {
        int node_idx = vfs_node_index(entry->node);
        if (node_idx < 0) return -1;
        int64_t n = vfs_read(node_idx, entry->offset, buf, len);
        if (n > 0) entry->offset += (uint64_t)n;
        return n;
    }

    return -EBADF;
}

/* uring helper: perform a write operation on an fd (reuses sys_fwrite logic) */
static int64_t uring_do_write(process_t *proc, int fd, const uint8_t *buf, uint32_t len) {
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    fd_entry_t *entry = &proc->fd_table[fd];
    if (fd_is_free(entry)) return -EBADF;

    /* Pipe write */
    if (entry->pipe != NULL) {
        pipe_t *pp = (pipe_t *)entry->pipe;
        uint64_t total = 0;
        while (total < len) {
            if (pp->closed_read) return total > 0 ? (int64_t)total : -1;
            if (pp->count < PIPE_BUF_SIZE) {
                pp->buf[pp->write_pos] = buf[total];
                pp->write_pos = (pp->write_pos + 1) % PIPE_BUF_SIZE;
                pp->count++;
                total++;
            } else break;
        }
        return (int64_t)total;
    }

    /* PTY write */
    if (entry->pty != NULL) {
        int pty_idx = pty_index((pty_t *)entry->pty);
        if (pty_idx < 0) return -1;
        if (entry->pty_is_master)
            return pty_master_write(pty_idx, buf, len);
        else
            return pty_slave_write(pty_idx, buf, len);
    }

    /* Unix socket */
    if (entry->unix_sock != NULL)
        return unix_sock_send((unix_sock_t *)entry->unix_sock, buf, len, 1);

    /* Regular file */
    if (entry->node != NULL) {
        int node_idx = vfs_node_index(entry->node);
        if (node_idx < 0) return -1;
        if (entry->open_flags & 0x04)
            entry->offset = entry->node->size;
        int64_t written = vfs_write(node_idx, entry->offset, buf, len);
        if (written > 0) entry->offset += (uint64_t)written;
        return written;
    }

    return -EBADF;
}

static int64_t sys_uring_enter(uint64_t uring_fd, uint64_t sqe_arg,
                                 uint64_t count_arg, uint64_t cqe_arg,
                                 uint64_t a5) {
    (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (uring_fd >= MAX_FDS) return -EBADF;
    fd_entry_t *ur_entry = &proc->fd_table[uring_fd];
    if (ur_entry->uring == NULL) return -EINVAL;

    uring_instance_t *ur = (uring_instance_t *)ur_entry->uring;

    /* Convention: arg1=uring_fd, arg2=sqe_ptr, arg3=count, arg4=cqe_ptr */
    uint64_t sqe_ptr = sqe_arg;
    uint32_t count = (uint32_t)count_arg;
    uint64_t cqe_ptr = cqe_arg;

    if (count == 0) return 0;
    if (count > ur->max_entries) count = ur->max_entries;

    if (validate_user_ptr(sqe_ptr, count * sizeof(uring_sqe_t)) != 0)
        return -EFAULT;
    if (validate_user_ptr(cqe_ptr, count * sizeof(uring_cqe_t)) != 0)
        return -EFAULT;

    uring_sqe_t *sqes = (uring_sqe_t *)sqe_ptr;
    uring_cqe_t *cqes = (uring_cqe_t *)cqe_ptr;

    uint32_t completed = 0;
    for (uint32_t i = 0; i < count; i++) {
        uring_sqe_t *sqe = &sqes[i];
        uring_cqe_t *cqe = &cqes[i];
        cqe->user_data = sqe->user_data;
        cqe->flags = 0;
        cqe->reserved = 0;

        switch (sqe->opcode) {
        case IORING_OP_NOP:
            cqe->res = 0;
            break;
        case IORING_OP_READ:
            if (validate_user_ptr(sqe->addr, sqe->len) != 0)
                cqe->res = -EFAULT;
            else
                cqe->res = (int32_t)uring_do_read(proc, sqe->fd,
                    (uint8_t *)sqe->addr, sqe->len);
            break;
        case IORING_OP_WRITE:
            if (validate_user_ptr(sqe->addr, sqe->len) != 0)
                cqe->res = -EFAULT;
            else
                cqe->res = (int32_t)uring_do_write(proc, sqe->fd,
                    (const uint8_t *)sqe->addr, sqe->len);
            break;
        case IORING_OP_POLL_ADD:
            cqe->res = (int32_t)poll_check_fd(proc, sqe->fd,
                (int16_t)(sqe->len & 0xFFFF));
            break;
        default:
            cqe->res = -EINVAL;
            break;
        }
        completed++;
    }

    return (int64_t)completed;
}

static int64_t sys_mmap2(uint64_t num_pages, uint64_t mmap_flags,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -EINVAL;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* Check memory rlimit */
    if (proc->rlimit_mem_pages > 0 &&
        proc->used_mem_pages + num_pages > proc->rlimit_mem_pages)
        return -ENOMEM;

    /* Namespace memory quota check */
    if (!agent_ns_quota_check(proc->ns_id, NS_QUOTA_MEM_PAGES, (uint32_t)num_pages))
        return -ENOMEM;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOMEM;

    uint64_t virt = proc->mmap_next_addr;

    if (mmap_flags & MMAP_DEMAND) {
        /* Demand-paged: reserve address space but don't allocate physical pages */
        proc->mmap_table[slot].virt_addr = virt;
        proc->mmap_table[slot].phys_addr = 0;
        proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
        proc->mmap_table[slot].used = 1;
        proc->mmap_table[slot].shm_id = -1;
        proc->mmap_table[slot].demand = 1;
        proc->mmap_table[slot].vfs_node = -1;
        proc->mmap_table[slot].file_offset = 0;
        proc->mmap_next_addr = virt + (num_pages + 1) * 4096;
        proc->used_mem_pages += num_pages;
        return (int64_t)virt;
    }

    /* Eager allocation — same as sys_mmap */
    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0) return -ENOMEM;

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * 4096;
        uint64_t page_virt = virt + i * 4096;
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        for (int j = 0; j < 4096; j++) dst[j] = 0;
        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0)
            return -ENOMEM;
    }

    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].demand = 0;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    proc->mmap_next_addr = virt + (num_pages + 1) * 4096;
    proc->used_mem_pages += num_pages;
    return (int64_t)virt;
}

/* --- Capability tokens --- */

static int64_t sys_token_create(uint64_t perms, uint64_t target_pid,
                                uint64_t resource_ptr, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    char resource[TOKEN_PATH_MAX];
    resource[0] = '\0';
    if (resource_ptr != 0) {
        if (copy_string_from_user((const char *)resource_ptr, resource, TOKEN_PATH_MAX) != 0)
            return -EFAULT;
    }

    return cap_token_create(proc->pid, proc->capabilities,
                            (uint32_t)perms, target_pid, resource);
}

static int64_t sys_token_revoke(uint64_t token_id, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    return cap_token_revoke((uint32_t)token_id, proc->pid);
}

static int64_t sys_token_list(uint64_t buf_ptr, uint64_t max_count,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (max_count == 0 || buf_ptr == 0) return 0;
    if (max_count > 32) max_count = 32;

    if (validate_user_ptr(buf_ptr, max_count * sizeof(token_info_t)) != 0)
        return -EFAULT;

    return cap_token_list(proc->pid, (token_info_t *)buf_ptr, (int)max_count);
}

/* --- Agent namespaces --- */

static int64_t sys_ns_create(uint64_t name_ptr, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* Only root or CAP_SYS_ADMIN can create namespaces */
    if (proc->uid != 0 && !(proc->capabilities & CAP_SYS_ADMIN))
        return -EPERM;

    char name[NS_NAME_MAX];
    name[0] = '\0';
    if (name_ptr != 0) {
        if (copy_string_from_user((const char *)name_ptr, name, NS_NAME_MAX) != 0)
            return -EFAULT;
    }

    return agent_ns_create(name, proc->pid);
}

static int64_t sys_ns_join(uint64_t ns_id, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    int ret = agent_ns_join((uint32_t)ns_id, proc->pid, proc->uid);
    if (ret == 0)
        proc->ns_id = (uint32_t)ns_id;
    return ret;
}

/* --- SYS_PROCINFO: enumerate active processes --- */
/* User-space struct layout: pid(8), parent_pid(8), state(4), uid(2), gid(2), name(32) = 56 bytes */
static int64_t sys_procinfo(uint64_t index, uint64_t buf_ptr,
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

/* --- SYS_FSSTAT: filesystem statistics --- */
/* User-space struct layout: total_blocks(4), free_blocks(4), total_inodes(4),
 *                           free_inodes(4), block_size(4), mounted(4) = 24 bytes */
static int64_t sys_fsstat(uint64_t buf_ptr, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(buf_ptr, 24) != 0) return -1;

    uint32_t *out = (uint32_t *)buf_ptr;
    if (limnfs_mounted()) {
        extern int limnfs_get_stats(uint32_t *total, uint32_t *free_blk,
                                     uint32_t *total_ino, uint32_t *free_ino);
        uint32_t tb, fb, ti, fi;
        limnfs_get_stats(&tb, &fb, &ti, &fi);
        out[0] = tb;
        out[1] = fb;
        out[2] = ti;
        out[3] = fi;
        out[4] = 4096;  /* LIMNFS_BLOCK_SIZE */
        out[5] = 1;     /* mounted */
    } else {
        for (int i = 0; i < 6; i++) out[i] = 0;
    }
    return 0;
}

/* --- SYS_TASK_CREATE: create workflow task --- */
static int64_t sys_task_create(uint64_t name_ptr, uint64_t ns_id,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    char name[TASK_NAME_MAX];
    if (name_ptr) {
        if (copy_string_from_user((const char *)name_ptr, name, TASK_NAME_MAX) != 0)
            return -EFAULT;
    } else {
        name[0] = '\0';
    }

    if (!agent_ns_valid((uint32_t)ns_id))
        return -EINVAL;

    return taskgraph_create(name, (uint32_t)ns_id, t->process->pid);
}

/* --- SYS_TASK_DEPEND: add dependency --- */
static int64_t sys_task_depend(uint64_t task_id, uint64_t dep_id,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return taskgraph_depend((uint32_t)task_id, (uint32_t)dep_id, t->process->pid);
}

/* --- SYS_TASK_START: start task (check deps) --- */
static int64_t sys_task_start(uint64_t task_id, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return taskgraph_start((uint32_t)task_id, t->process->pid);
}

/* --- SYS_TASK_COMPLETE: mark task done/failed --- */
static int64_t sys_task_complete(uint64_t task_id, uint64_t result,
                                   uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return taskgraph_complete((uint32_t)task_id, (int32_t)result, t->process->pid);
}

/* --- SYS_TASK_STATUS: query task --- */
static int64_t sys_task_status(uint64_t task_id, uint64_t buf_ptr,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(buf_ptr, sizeof(task_status_t)) != 0) return -EFAULT;
    return taskgraph_status((uint32_t)task_id, (task_status_t *)buf_ptr);
}

/* --- SYS_TASK_WAIT: poll if task finished --- */
static int64_t sys_task_wait(uint64_t task_id, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return taskgraph_is_done((uint32_t)task_id) ? 0 : -EAGAIN;
}

/* --- SYS_TOKEN_DELEGATE: create sub-token --- */
static int64_t sys_token_delegate(uint64_t parent_id, uint64_t target_pid,
                                    uint64_t perms, uint64_t resource_ptr,
                                    uint64_t a5) {
    (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    char resource[TOKEN_PATH_MAX];
    resource[0] = '\0';
    if (resource_ptr) {
        if (copy_string_from_user((const char *)resource_ptr, resource, TOKEN_PATH_MAX) != 0)
            return -EFAULT;
    }

    return cap_token_delegate((uint32_t)parent_id, t->process->pid,
                               target_pid, (uint32_t)perms, resource);
}

/* --- SYS_NS_SETQUOTA: set namespace quota --- */
static int64_t sys_ns_setquota(uint64_t ns_id, uint64_t resource,
                                 uint64_t limit, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return agent_ns_set_quota((uint32_t)ns_id, (uint32_t)resource, (uint32_t)limit,
                               t->process->pid, t->process->uid);
}

/* --- SYS_INFER_HEALTH: daemon reports heartbeat + load --- */
static int64_t sys_infer_health(uint64_t load, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return infer_health(t->process->pid, (uint32_t)load);
}

/* --- SYS_INFER_ROUTE: find best service instance by name --- */
static int64_t sys_infer_route(uint64_t name_ptr, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    char name[INFER_NAME_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, INFER_NAME_MAX) != 0)
        return -EFAULT;

    int idx = infer_route(name);
    if (idx < 0) return -ENOENT;

    /* Return the provider_pid so caller knows which instance was chosen */
    infer_service_t *svc = infer_get(idx);
    if (!svc) return -ENOENT;
    return (int64_t)svc->provider_pid;
}

/* --- SYS_INFER_SET_POLICY: set inference routing policy --- */
static int64_t sys_infer_set_policy(uint64_t policy, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return infer_set_policy((uint8_t)policy);
}

static int64_t sys_infer_queue_stat(uint64_t stat_ptr, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (validate_user_ptr(stat_ptr, sizeof(infer_queue_stat_t)) != 0)
        return -EFAULT;

    infer_queue_stat_t *out = (infer_queue_stat_t *)stat_ptr;
    infer_queue_get_stat(out);
    return 0;
}

static int64_t sys_infer_cache_ctrl(uint64_t cmd, uint64_t arg_ptr,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    switch (cmd) {
    case INFER_CACHE_FLUSH:
        infer_cache_flush();
        return 0;
    case INFER_CACHE_STATS:
        if (validate_user_ptr(arg_ptr, sizeof(infer_cache_stat_t)) != 0)
            return -EFAULT;
        infer_cache_get_stat((infer_cache_stat_t *)arg_ptr);
        return 0;
    case INFER_CACHE_SET_TTL:
        infer_cache_set_ttl((uint32_t)arg_ptr);
        return 0;
    default:
        return -EINVAL;
    }
}

/* --- SYS_INFER_SUBMIT: submit async inference request --- */
static int64_t sys_infer_submit(uint64_t name_ptr, uint64_t req_buf,
                                  uint64_t req_len, uint64_t eventfd_idx,
                                  uint64_t a5) {
    (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    char name[INFER_NAME_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, INFER_NAME_MAX) != 0)
        return -EFAULT;
    if (req_len > 0 && validate_user_ptr(req_buf, req_len) != 0)
        return -EFAULT;

    /* Require CAP_INFER */
    if (!(proc->capabilities & CAP_INFER) &&
        !cap_token_check(proc->pid, CAP_INFER, name))
        return -EACCES;

    /* Resolve eventfd file descriptor to raw eventfd index */
    int32_t efd_idx = -1;
    int64_t efd_arg = (int64_t)eventfd_idx;
    if (efd_arg >= 0 && efd_arg < MAX_FDS) {
        fd_entry_t *efd_entry = &proc->fd_table[efd_arg];
        if (efd_entry->eventfd != NULL) {
            for (int i = 0; i < MAX_EVENTFDS; i++) {
                if (eventfd_get(i) == (eventfd_t *)efd_entry->eventfd) {
                    efd_idx = i;
                    break;
                }
            }
        }
    }

    return infer_async_submit(name, (const void *)req_buf, (uint32_t)req_len,
                               proc->pid, efd_idx);
}

/* --- SYS_INFER_POLL: poll async inference request status --- */
static int64_t sys_infer_poll(uint64_t request_id, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    return infer_async_poll((int)request_id, proc->pid);
}

/* --- SYS_INFER_RESULT: retrieve async inference result --- */
static int64_t sys_infer_result(uint64_t request_id, uint64_t resp_buf,
                                  uint64_t resp_len, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (resp_len > 0 && validate_user_ptr(resp_buf, resp_len) != 0)
        return -EFAULT;

    return infer_async_result((int)request_id, proc->pid,
                               (void *)resp_buf, (uint32_t)resp_len);
}

/* --- SYS_AGENT_SEND: send message to named agent with optional token delegation --- */
static int64_t sys_agent_send(uint64_t name_ptr, uint64_t msg_buf,
                                uint64_t msg_len, uint64_t token_id, uint64_t a5) {
    (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    char name[AGENT_NAME_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, AGENT_NAME_MAX) != 0)
        return -EFAULT;
    if (msg_len > AGENT_MSG_MAX) msg_len = AGENT_MSG_MAX;
    if (msg_len > 0 && validate_user_ptr(msg_buf, msg_len) != 0)
        return -EFAULT;

    return agent_send(name, proc->ns_id, proc->pid, proc->capabilities,
                      (const uint8_t *)msg_buf, (uint32_t)msg_len,
                      (uint32_t)token_id);
}

/* --- SYS_AGENT_RECV: receive message from own mailbox --- */
static int64_t sys_agent_recv(uint64_t msg_buf, uint64_t msg_len,
                                uint64_t sender_pid_ptr, uint64_t token_id_ptr,
                                uint64_t a5) {
    (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (msg_len > 0 && validate_user_ptr(msg_buf, msg_len) != 0)
        return -EFAULT;
    if (sender_pid_ptr && validate_user_ptr(sender_pid_ptr, sizeof(uint64_t)) != 0)
        return -EFAULT;
    if (token_id_ptr && validate_user_ptr(token_id_ptr, sizeof(uint32_t)) != 0)
        return -EFAULT;

    uint64_t sender_pid = 0;
    uint32_t token_id = 0;
    int r = agent_recv(proc->pid, &sender_pid, &token_id,
                       (uint8_t *)msg_buf, (uint32_t)msg_len);
    if (r >= 0) {
        if (sender_pid_ptr) *(uint64_t *)sender_pid_ptr = sender_pid;
        if (token_id_ptr) *(uint32_t *)token_id_ptr = token_id;
    }
    return r;
}

/* --- Futex --- */

static int64_t sys_futex_wait(uint64_t uaddr, uint64_t expected,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    if (validate_user_ptr(uaddr, sizeof(uint32_t)) != 0) return -EFAULT;
    return futex_wait(t->process->pid, (uint32_t *)uaddr, (uint32_t)expected);
}

static int64_t sys_futex_wake(uint64_t uaddr, uint64_t max_wake,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    if (validate_user_ptr(uaddr, sizeof(uint32_t)) != 0) return -EFAULT;
    if (max_wake == 0) max_wake = 1;
    return futex_wake(t->process->pid, (uint32_t *)uaddr, (uint32_t)max_wake);
}

/* --- File-backed mmap --- */

static int64_t sys_mmap_file(uint64_t fd, uint64_t offset,
                              uint64_t num_pages, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (fd >= MAX_FDS || num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->node == NULL) return -1;

    vfs_node_t *node = entry->node;
    int node_idx = vfs_node_index(node);
    if (node_idx < 0) return -1;

    /* Find free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) { slot = i; break; }
    }
    if (slot < 0) return -12;  /* -ENOMEM */

    uint64_t virt = proc->mmap_next_addr;

    /* Reserve virtual address space — no physical pages allocated */
    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = 0;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].demand = 1;
    proc->mmap_table[slot].vfs_node = (int32_t)node_idx;
    proc->mmap_table[slot].file_offset = offset;
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;

    return (int64_t)virt;
}

/* --- mprotect --- */

static inline void flush_page(uint64_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static int64_t sys_mprotect(uint64_t virt_addr, uint64_t num_pages,
                             uint64_t prot, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -EINVAL;

    if (virt_addr & 0xFFF) return -EINVAL;  /* not page-aligned */
    if (num_pages == 0) return -EINVAL;

    uint64_t range_end = virt_addr + num_pages * PAGE_SIZE;

    /* Validate: entire range must be within a single mmap entry */
    int found = 0;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) continue;
        uint64_t entry_start = proc->mmap_table[i].virt_addr;
        uint64_t entry_end = entry_start + (uint64_t)proc->mmap_table[i].num_pages * PAGE_SIZE;
        if (virt_addr >= entry_start && range_end <= entry_end) {
            found = 1;
            break;
        }
    }
    if (!found) return -EINVAL;

    /* Convert prot flags to PTE flags */
    uint64_t new_flags = PTE_PRESENT | PTE_USER;
    if (prot == PROT_NONE) {
        new_flags = PTE_USER;  /* no PTE_PRESENT — page inaccessible */
    } else {
        if (prot & PROT_WRITE)
            new_flags |= PTE_WRITABLE;
        if (!(prot & PROT_EXEC))
            new_flags |= PTE_NX;
    }

    /* Walk PTEs and update flags */
    for (uint64_t pg = 0; pg < num_pages; pg++) {
        uint64_t va = virt_addr + pg * PAGE_SIZE;
        uint64_t *pte = vmm_get_pte(proc->cr3, va);
        if (!pte) continue;  /* demand page not yet faulted — skip */
        uint64_t old = *pte;
        if (!(old & PTE_PRESENT) && !(old & PTE_SWAP)) continue;  /* not mapped yet */

        uint64_t phys = old & PTE_ADDR_MASK;
        /* Preserve COW bit if set (don't grant write on COW page via mprotect) */
        uint64_t cow = old & PTE_COW;
        uint64_t final_flags = new_flags | cow;
        if (cow) {
            if (final_flags & PTE_WRITABLE) {
                /* COW page: don't actually make writable yet — keep COW semantics.
                 * But mark WAS_WRITABLE so COW handler knows to grant write later. */
                final_flags &= ~PTE_WRITABLE;
                final_flags |= PTE_WAS_WRITABLE;
            } else {
                /* Making COW page read-only: clear WAS_WRITABLE so COW handler
                 * knows this is a genuine protection fault */
                final_flags &= ~PTE_WAS_WRITABLE;
            }
        }

        *pte = phys | final_flags;
        flush_page(va);
    }

    return 0;
}

/* --- mmap_guard: allocate region with explicit trailing guard page --- */

static int64_t sys_mmap_guard(uint64_t num_pages, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -EINVAL;

    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -EINVAL;

    if (proc->rlimit_mem_pages > 0 &&
        proc->used_mem_pages + num_pages > proc->rlimit_mem_pages)
        return -ENOMEM;

    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) { slot = i; break; }
    }
    if (slot < 0) return -ENOMEM;

    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0) return -ENOMEM;

    uint64_t virt = proc->mmap_next_addr;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_virt = virt + i * PAGE_SIZE;
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        for (uint64_t j = 0; j < PAGE_SIZE; j++) dst[j] = 0;
        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            for (uint64_t k = 0; k < num_pages; k++)
                pmm_free_page(phys + k * PAGE_SIZE);
            return -ENOMEM;
        }
    }

    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].demand = 0;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    /* Skip 1 guard page after the usable region */
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;
    proc->used_mem_pages += num_pages;

    return (int64_t)virt;
}

/* Syscall dispatch table */
typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

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
