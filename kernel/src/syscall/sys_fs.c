#include "syscall/syscall_internal.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "mm/vmm.h"
#include "pty/pty.h"
#include "ipc/unix_sock.h"
#include "ipc/eventfd.h"
#include "ipc/epoll.h"
#include "ipc/uring.h"
#include "ipc/cap_token.h"
#include "blk/limnfs.h"
#include "net/tcp.h"

int64_t sys_open(uint64_t path_ptr, uint64_t flags,
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

int64_t sys_read(uint64_t fd, uint64_t buf_ptr, uint64_t len,
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
            uint64_t pflags;
            pipe_lock_acquire(&pflags);
            if (pp->count > 0) {
                dst[total] = pp->buf[pp->read_pos];
                pp->read_pos = (pp->read_pos + 1) % PIPE_BUF_SIZE;
                pp->count--;
                pipe_unlock_release(pflags);
                total++;
            } else if (pp->closed_write) {
                pipe_unlock_release(pflags);
                break;  /* EOF — writer closed */
            } else {
                pipe_unlock_release(pflags);
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
        int efd_idx = eventfd_index((const eventfd_t *)entry->eventfd);
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

/* Shared fd close helper — closes any IPC resource held by an fd entry
 * and resets the entry to empty. Used by sys_close, sys_exit, dup2. */
void fd_close(fd_entry_t *entry) {
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
        int ei = eventfd_index((const eventfd_t *)entry->eventfd);
        if (ei >= 0) eventfd_close(ei);
        entry->eventfd = NULL;
    }
    if (entry->epoll != NULL) {
        int ep_idx = epoll_index((epoll_instance_t *)entry->epoll);
        if (ep_idx >= 0) epoll_close(ep_idx);
        entry->epoll = NULL;
    }
    if (entry->uring != NULL) {
        int ur_idx = uring_index((uring_instance_t *)entry->uring);
        if (ur_idx >= 0) uring_close(ur_idx);
        entry->uring = NULL;
    }
    if (entry->tcp_conn_idx >= 0)
        entry->tcp_conn_idx = -1;
    entry->node = NULL;
    entry->offset = 0;
    entry->open_flags = 0;
    entry->fd_flags = 0;
}

int64_t sys_close(uint64_t fd, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (fd_is_free(entry))
        return -1;

    fd_close(entry);
    return 0;
}

int64_t sys_stat(uint64_t path_ptr, uint64_t stat_ptr,
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

int64_t sys_fwrite(uint64_t fd, uint64_t buf_ptr, uint64_t len,
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
            uint64_t pflags;
            pipe_lock_acquire(&pflags);
            if (pp->closed_read) {
                pipe_unlock_release(pflags);
                return total > 0 ? (int64_t)total : -1;
            }
            if (pp->count < PIPE_BUF_SIZE) {
                pp->buf[pp->write_pos] = src[total];
                pp->write_pos = (pp->write_pos + 1) % PIPE_BUF_SIZE;
                pp->count++;
                pipe_unlock_release(pflags);
                total++;
            } else {
                pipe_unlock_release(pflags);
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
        int efd_idx = eventfd_index((const eventfd_t *)entry->eventfd);
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

int64_t sys_create(uint64_t path_ptr, uint64_t a2,
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

int64_t sys_unlink(uint64_t path_ptr, uint64_t a2,
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

int64_t sys_readdir(uint64_t dir_path_ptr, uint64_t index,
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

int64_t sys_mkdir(uint64_t path_ptr, uint64_t a2,
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

int64_t sys_seek(uint64_t fd, uint64_t offset_arg, uint64_t whence,
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

int64_t sys_truncate(uint64_t path_ptr, uint64_t new_size,
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

int64_t sys_chdir(uint64_t path_ptr, uint64_t a2,
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

int64_t sys_getcwd(uint64_t buf_ptr, uint64_t size,
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

int64_t sys_fstat(uint64_t fd, uint64_t stat_ptr,
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

int64_t sys_rename(uint64_t old_path_ptr, uint64_t new_path_ptr,
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

int64_t sys_chmod(uint64_t path_ptr, uint64_t mode,
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

int64_t sys_fsstat(uint64_t buf_ptr, uint64_t a2,
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
