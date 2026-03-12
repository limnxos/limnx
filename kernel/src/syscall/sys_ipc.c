#include "syscall/syscall_internal.h"
#include "sched/thread.h"
#include "sched/sched.h"
#include "ipc/unix_sock.h"
#include "ipc/eventfd.h"
#include "ipc/agent_reg.h"
#include "ipc/epoll.h"
#include "ipc/cap_token.h"
#include "ipc/pubsub.h"
#include "sync/futex.h"
#include "idt/idt.h"

int64_t sys_unix_socket(uint64_t a1, uint64_t a2,
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

int64_t sys_unix_bind(uint64_t fd, uint64_t path_ptr,
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

int64_t sys_unix_listen(uint64_t fd, uint64_t a2,
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

int64_t sys_unix_accept(uint64_t fd, uint64_t a2,
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

int64_t sys_unix_connect(uint64_t path_ptr, uint64_t a2,
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

int64_t sys_agent_register(uint64_t name_ptr, uint64_t a2,
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

int64_t sys_agent_lookup(uint64_t name_ptr, uint64_t pid_out_ptr,
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

int64_t sys_eventfd(uint64_t flags, uint64_t a2,
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

int64_t sys_epoll_create(uint64_t flags, uint64_t a2,
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

int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd,
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

int64_t sys_epoll_wait(uint64_t epfd, uint64_t events_ptr,
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

int64_t sys_agent_send(uint64_t name_ptr, uint64_t msg_buf,
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

int64_t sys_agent_recv(uint64_t msg_buf, uint64_t msg_len,
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

int64_t sys_futex_wait(uint64_t uaddr, uint64_t expected,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    if (validate_user_ptr(uaddr, sizeof(uint32_t)) != 0) return -EFAULT;
    return futex_wait(t->process->pid, (uint32_t *)uaddr, (uint32_t)expected);
}

int64_t sys_futex_wake(uint64_t uaddr, uint64_t max_wake,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    if (validate_user_ptr(uaddr, sizeof(uint32_t)) != 0) return -EFAULT;
    if (max_wake == 0) max_wake = 1;
    return futex_wake(t->process->pid, (uint32_t *)uaddr, (uint32_t)max_wake);
}

/* --- Pub/Sub --- */

int64_t sys_topic_create(uint64_t name_ptr, uint64_t ns_id,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    char name[TOPIC_NAME_MAX];
    if (copy_string_from_user((const char *)name_ptr, name, TOPIC_NAME_MAX) != 0)
        return -EFAULT;

    return pubsub_topic_create(name, (uint32_t)ns_id, t->process->pid);
}

int64_t sys_topic_subscribe(uint64_t topic_id, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    return pubsub_subscribe((uint32_t)topic_id, t->process->pid,
                            t->process->ns_id, t->process->capabilities);
}

int64_t sys_topic_publish(uint64_t topic_id, uint64_t buf_ptr,
                                   uint64_t len, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    if (len > TOPIC_MSG_MAX) return -EMSGSIZE;
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -EFAULT;

    /* Copy data from user space to kernel buffer */
    uint8_t kbuf[TOPIC_MSG_MAX];
    const uint8_t *src = (const uint8_t *)buf_ptr;
    for (uint32_t i = 0; i < (uint32_t)len; i++)
        kbuf[i] = src[i];

    return pubsub_publish((uint32_t)topic_id, t->process->pid,
                          t->process->ns_id, t->process->capabilities,
                          kbuf, (uint32_t)len);
}

int64_t sys_topic_recv(uint64_t topic_id, uint64_t buf_ptr,
                                uint64_t max_len, uint64_t pub_pid_ptr, uint64_t a5) {
    (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;

    if (max_len > TOPIC_MSG_MAX) max_len = TOPIC_MSG_MAX;
    if (max_len > 0 && validate_user_ptr(buf_ptr, max_len) != 0)
        return -EFAULT;

    uint8_t kbuf[TOPIC_MSG_MAX];
    uint64_t pub_pid = 0;
    int rc = pubsub_recv((uint32_t)topic_id, t->process->pid,
                         &pub_pid, kbuf, (uint32_t)max_len);
    if (rc < 0) return rc;

    /* Copy data to user space */
    uint8_t *dst = (uint8_t *)buf_ptr;
    for (int i = 0; i < rc; i++)
        dst[i] = kbuf[i];

    /* Optionally return publisher pid */
    if (pub_pid_ptr && validate_user_ptr(pub_pid_ptr, sizeof(uint64_t)) == 0) {
        *(uint64_t *)pub_pid_ptr = pub_pid;
    }

    return rc;
}
