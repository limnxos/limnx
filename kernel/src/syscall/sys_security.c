#include "syscall/syscall_internal.h"
#include "sched/thread.h"
#include "ipc/cap_token.h"
#include "ipc/agent_ns.h"

int64_t sys_getuid(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return (int64_t)t->process->uid;
}

int64_t sys_setuid(uint64_t uid, uint64_t a2,
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

int64_t sys_getgid(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return (int64_t)t->process->gid;
}

int64_t sys_setgid(uint64_t gid, uint64_t a2,
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

int64_t sys_getcap(uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return (int64_t)t->process->capabilities;
}

int64_t sys_setcap(uint64_t pid_arg, uint64_t caps,
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

int64_t sys_getrlimit(uint64_t resource, uint64_t ptr,
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

int64_t sys_setrlimit(uint64_t resource, uint64_t ptr,
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

int64_t sys_seccomp(uint64_t mask, uint64_t strict,
                             uint64_t mask_hi, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    process_t *proc = t->process;

    if (proc->seccomp_mask != 0 || proc->seccomp_mask_hi != 0) {
        /* Already set — can only restrict further (AND) */
        proc->seccomp_mask &= mask;
        proc->seccomp_mask_hi &= mask_hi;
    } else {
        proc->seccomp_mask = mask;
        proc->seccomp_mask_hi = mask_hi;
    }
    if (strict)
        proc->seccomp_strict = 1;
    return 0;
}

int64_t sys_setaudit(uint64_t pid_arg, uint64_t flags,
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

int64_t sys_token_create(uint64_t perms, uint64_t target_pid,
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

int64_t sys_token_revoke(uint64_t token_id, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    return cap_token_revoke((uint32_t)token_id, proc->pid);
}

int64_t sys_token_list(uint64_t buf_ptr, uint64_t max_count,
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

int64_t sys_ns_create(uint64_t name_ptr, uint64_t a2,
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

int64_t sys_ns_join(uint64_t ns_id, uint64_t a2,
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

int64_t sys_token_delegate(uint64_t parent_id, uint64_t target_pid,
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

int64_t sys_ns_setquota(uint64_t ns_id, uint64_t resource,
                                 uint64_t limit, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;
    return agent_ns_set_quota((uint32_t)ns_id, (uint32_t)resource, (uint32_t)limit,
                               t->process->pid, t->process->uid);
}
