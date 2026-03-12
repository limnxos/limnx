#include "syscall/syscall_internal.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "serial.h"
#include "idt/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/swap.h"
#include "ipc/uring.h"
#include "ipc/taskgraph.h"
#include "ipc/supervisor.h"
#include "ipc/agent_ns.h"
#include "pty/pty.h"
#include "ipc/unix_sock.h"
#include "ipc/eventfd.h"

int64_t sys_write(uint64_t buf, uint64_t len,
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

int64_t sys_yield(uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    sched_yield();
    return 0;
}

int64_t sys_getchar(uint64_t a1, uint64_t a2,
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

int64_t sys_arch_prctl(uint64_t code, uint64_t addr,
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

/* Supervisor syscalls */

int64_t sys_super_create(uint64_t name_ptr, uint64_t a2,
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

int64_t sys_super_add(uint64_t super_id, uint64_t elf_path_ptr,
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

int64_t sys_super_set_policy(uint64_t super_id, uint64_t policy,
                                     uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return supervisor_set_policy((uint32_t)super_id, (uint8_t)policy);
}

int64_t sys_super_start(uint64_t super_id, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return supervisor_start((uint32_t)super_id);
}

int64_t sys_clock_gettime(uint64_t clockid, uint64_t ts_ptr,
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

int64_t sys_nanosleep(uint64_t ts_ptr, uint64_t a2,
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

int64_t sys_getenv(uint64_t key_ptr, uint64_t val_ptr, uint64_t val_size,
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

int64_t sys_setenv(uint64_t key_ptr, uint64_t val_ptr,
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

int64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms,
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

/* select() fd_set is a 64-bit bitmask (supports fds 0-63) */
typedef struct { uint64_t bits; } fd_set_kern_t;

int64_t sys_select(uint64_t nfds, uint64_t readfds_ptr,
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

int64_t sys_swap_stat(uint64_t stat_ptr, uint64_t a2,
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

int64_t sys_uring_setup(uint64_t entries, uint64_t params_ptr,
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
        uint64_t pflags;
        pipe_lock_acquire(&pflags);
        while (total < len && pp->count > 0) {
            buf[total] = pp->buf[pp->read_pos];
            pp->read_pos = (pp->read_pos + 1) % PIPE_BUF_SIZE;
            pp->count--;
            total++;
        }
        pipe_unlock_release(pflags);
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
        uint64_t pflags;
        pipe_lock_acquire(&pflags);
        while (total < len) {
            if (pp->closed_read) {
                pipe_unlock_release(pflags);
                return total > 0 ? (int64_t)total : -1;
            }
            if (pp->count < PIPE_BUF_SIZE) {
                pp->buf[pp->write_pos] = buf[total];
                pp->write_pos = (pp->write_pos + 1) % PIPE_BUF_SIZE;
                pp->count++;
                total++;
            } else break;
        }
        pipe_unlock_release(pflags);
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

int64_t sys_uring_enter(uint64_t uring_fd, uint64_t sqe_arg,
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

int64_t sys_task_create(uint64_t name_ptr, uint64_t ns_id,
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

int64_t sys_task_depend(uint64_t task_id, uint64_t dep_id,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return taskgraph_depend((uint32_t)task_id, (uint32_t)dep_id, t->process->pid);
}

int64_t sys_task_start(uint64_t task_id, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return taskgraph_start((uint32_t)task_id, t->process->pid);
}

int64_t sys_task_complete(uint64_t task_id, uint64_t result,
                                   uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return taskgraph_complete((uint32_t)task_id, (int32_t)result, t->process->pid);
}

int64_t sys_task_status(uint64_t task_id, uint64_t buf_ptr,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(buf_ptr, sizeof(task_status_t)) != 0) return -EFAULT;
    return taskgraph_status((uint32_t)task_id, (task_status_t *)buf_ptr);
}

int64_t sys_task_wait(uint64_t task_id, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return taskgraph_is_done((uint32_t)task_id) ? 0 : -EAGAIN;
}
