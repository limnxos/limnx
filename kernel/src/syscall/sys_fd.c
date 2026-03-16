#include "syscall/syscall_internal.h"
#include "sched/thread.h"
#include "pty/pty.h"
#include "ipc/unix_sock.h"
#include "ipc/eventfd.h"
#include "ipc/epoll.h"
#include "ipc/uring.h"

int64_t sys_pipe(uint64_t pipefd_ptr, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (validate_user_ptr(pipefd_ptr, 2 * sizeof(int)) != 0)
        return -EFAULT;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* fd limit check (pipe needs 2 fds) */
    if (proc->rlimit_nfds > 0 &&
        (uint32_t)(count_open_fds(proc) + 2) > proc->rlimit_nfds)
        return -EMFILE;

    /* Find free pipe slot */
    uint64_t pflags;
    pipe_lock_acquire(&pflags);
    int slot = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { pipe_unlock_release(pflags); return -1; }

    pipe_t *pp = &pipes[slot];
    pp->read_pos = 0;
    pp->write_pos = 0;
    pp->count = 0;
    pp->closed_read = 0;
    pp->closed_write = 0;
    pp->used = 1;
    pp->read_refs = 1;
    pp->write_refs = 1;
    pipe_unlock_release(pflags);

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

    /* Linux ABI: write two int32 to pipefd[0], pipefd[1] */
    int *pipefd = (int *)pipefd_ptr;
    pipefd[0] = rfd;
    pipefd[1] = wfd;

    return 0;
}

int64_t sys_pipe2(uint64_t pipefd_ptr, uint64_t flags,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    /* Linux ABI: pipefd_ptr points to int[2] */
    if (validate_user_ptr(pipefd_ptr, 2 * sizeof(int)) != 0)
        return -EFAULT;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    if (proc->rlimit_nfds > 0 &&
        (uint32_t)(count_open_fds(proc) + 2) > proc->rlimit_nfds)
        return -EMFILE;

    uint64_t pflags;
    pipe_lock_acquire(&pflags);
    int slot = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) { slot = i; break; }
    }
    if (slot < 0) { pipe_unlock_release(pflags); return -1; }

    pipe_t *pp = &pipes[slot];
    pp->read_pos = 0;
    pp->write_pos = 0;
    pp->count = 0;
    pp->closed_read = 0;
    pp->closed_write = 0;
    pp->used = 1;
    pp->read_refs = 1;
    pp->write_refs = 1;
    pipe_unlock_release(pflags);

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

    if (flags & O_CLOEXEC) {
        proc->fd_table[rfd].fd_flags |= FD_CLOEXEC;
        proc->fd_table[wfd].fd_flags |= FD_CLOEXEC;
    }
    if (flags & O_NONBLOCK) {
        proc->fd_table[rfd].fd_flags |= 0x02;
        proc->fd_table[wfd].fd_flags |= 0x02;
    }

    /* Linux ABI: write two int32 to pipefd[0], pipefd[1] */
    int *pipefd = (int *)pipefd_ptr;
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

int64_t sys_dup(uint64_t fd, uint64_t a2,
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
                uint64_t pflags;
                pipe_lock_acquire(&pflags);
                pipe_t *pp = (pipe_t *)src->pipe;
                if (src->pipe_write)
                    pp->write_refs++;
                else
                    pp->read_refs++;
                pipe_unlock_release(pflags);
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
                int ei = eventfd_index((const eventfd_t *)src->eventfd);
                if (ei >= 0) eventfd_ref(ei);
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

int64_t sys_dup2(uint64_t oldfd, uint64_t newfd,
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

    /* Close newfd if open (unified via fd_close) */
    fd_entry_t *dst = &proc->fd_table[newfd];
    if (!fd_is_free(dst))
        fd_close(dst);

    /* Copy fd entry */
    proc->fd_table[newfd] = *src;
    proc->fd_table[newfd].fd_flags = 0;  /* dup2 clears cloexec */
    /* Increment pipe ref count for new copy */
    if (src->pipe != NULL) {
        uint64_t pflags;
        pipe_lock_acquire(&pflags);
        pipe_t *pp = (pipe_t *)src->pipe;
        if (src->pipe_write)
            pp->write_refs++;
        else
            pp->read_refs++;
        pipe_unlock_release(pflags);
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

int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
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
    case F_GETFL: {
        uint32_t fl = entry->open_flags;
        if (entry->fd_flags & 0x02) fl |= O_NONBLOCK;
        return (int64_t)fl;
    }
    case F_SETFL:
        if (arg & O_NONBLOCK)
            entry->fd_flags |= 0x02;
        else
            entry->fd_flags &= ~0x02;
        if (arg & O_APPEND)
            entry->open_flags |= O_APPEND;
        else
            entry->open_flags &= ~O_APPEND;
        return 0;
    case 0: /* F_DUPFD */
    {
        int min_fd = (int)arg;
        for (int nfd = min_fd; nfd < MAX_FDS; nfd++) {
            if (fd_is_free(&proc->fd_table[nfd])) {
                proc->fd_table[nfd] = *entry;
                /* Increment refs for PTY */
                if (entry->pty) {
                    pty_t *pt = (pty_t *)entry->pty;
                    if (entry->pty_is_master) pt->master_refs++;
                    else pt->slave_refs++;
                }
                proc->fd_table[nfd].fd_flags = 0; /* F_DUPFD clears CLOEXEC */
                return nfd;
            }
        }
        return -EMFILE;
    }
    default:
        return -1;
    }
}

int64_t sys_openpty(uint64_t master_fd_ptr, uint64_t slave_fd_ptr,
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

int64_t sys_ioctl(uint64_t fd_num, uint64_t cmd, uint64_t arg,
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

    return -ENOTTY;  /* not a terminal */
}
