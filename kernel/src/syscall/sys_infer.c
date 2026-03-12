#include "syscall/syscall_internal.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "ipc/infer_svc.h"
#include "ipc/unix_sock.h"
#include "ipc/cap_token.h"
#include "ipc/eventfd.h"
#include "ipc/agent_ns.h"

int64_t sys_infer_register(uint64_t name_ptr, uint64_t path_ptr,
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

int64_t sys_infer_request(uint64_t name_ptr, uint64_t req_buf,
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

int64_t sys_infer_health(uint64_t load, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return infer_health(t->process->pid, (uint32_t)load);
}

int64_t sys_infer_route(uint64_t name_ptr, uint64_t a2,
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

int64_t sys_infer_set_policy(uint64_t policy, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return infer_set_policy((uint8_t)policy);
}

int64_t sys_infer_queue_stat(uint64_t stat_ptr, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (validate_user_ptr(stat_ptr, sizeof(infer_queue_stat_t)) != 0)
        return -EFAULT;

    infer_queue_stat_t *out = (infer_queue_stat_t *)stat_ptr;
    infer_queue_get_stat(out);
    return 0;
}

int64_t sys_infer_cache_ctrl(uint64_t cmd, uint64_t arg_ptr,
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

int64_t sys_infer_submit(uint64_t name_ptr, uint64_t req_buf,
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

int64_t sys_infer_poll(uint64_t request_id, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    return infer_async_poll((int)request_id, proc->pid);
}

int64_t sys_infer_result(uint64_t request_id, uint64_t resp_buf,
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

int64_t sys_infer_swap(uint64_t name_ptr, uint64_t path_ptr,
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

    /* Must be owner of the service or have CAP_INFER */
    if (!(proc->capabilities & CAP_INFER) &&
        !cap_token_check(proc->pid, CAP_INFER, name))
        return -EACCES;

    return infer_swap(name, path, proc->pid);
}
