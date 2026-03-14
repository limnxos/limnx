#include "syscall/syscall_internal.h"
#include "arch/syscall_arch.h"
#include "arch/percpu.h"
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
#include "ipc/pubsub.h"
#include "net/tcp.h"
#include "arch/serial.h"
#include "arch/cpu.h"
#include "arch/paging.h"

/* Shared process termination — called from sys_exit and page fault kill path.
 * Performs full cleanup: FDs, TCP, agents, caps, namespaces, SHM, signals, etc.
 * Does NOT return. */
void process_terminate(thread_t *t, int64_t status) {
    if (t->process) {
        t->process->exit_status = status;
        /* Close all open file descriptors */
        for (int i = 0; i < MAX_FDS; i++)
            fd_close(&t->process->fd_table[i]);
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
        /* Cleanup pub/sub topics and subscriptions */
        pubsub_cleanup_pid(t->process->pid);
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
                    if (sid < MAX_SHM_REGIONS && shm_table[sid].ref_count > 0) {
                        shm_table[sid].ref_count--;
                        /* Free SHM pages when last reference drops */
                        if (shm_table[sid].ref_count == 0) {
                            for (uint32_t pi = 0; pi < shm_table[sid].num_pages; pi++) {
                                if (shm_table[sid].phys_pages[pi]) {
                                    pmm_free_page(shm_table[sid].phys_pages[pi]);
                                    shm_table[sid].phys_pages[pi] = 0;
                                }
                            }
                            shm_table[sid].key = -1;
                            shm_table[sid].num_pages = 0;
                        }
                    }
                    t->process->mmap_table[i].used = 0;
                    t->process->mmap_table[i].shm_id = -1;
                }
            }
            shm_unlock_release(sflags);
        }
        /* Re-parent orphaned children to init (pid 1) */
        process_reparent_children(t->process->pid, 1);
        /* Free user-space pages (respects COW refcounts) */
        if (t->process->cr3) {
            /* Switch to kernel PML4 before freeing */
            arch_switch_address_space(vmm_get_kernel_pml4());
            vmm_free_user_pages(t->process->cr3);
            t->process->cr3 = 0;
        }
        /* Notify supervisors about exit */
        supervisor_on_exit(t->process->pid, (int)status);
        /* Deliver SIGCHLD to parent */
        if (t->process->parent_pid != 0) {
            process_t *parent = process_lookup(t->process->parent_pid);
            if (parent && !parent->exited &&
                parent->sig_handlers[SIGCHLD].sa_handler > SIG_IGN)
                process_deliver_signal(parent, SIGCHLD);
        }
    }
    serial_printf("[proc] Process terminated with status %ld\n", status);
    /* CRITICAL: Disable interrupts from here until thread_exit().
     * After cr3 is zeroed above, if a timer interrupt preempts us and the
     * scheduler context-switches to another thread, when we're later
     * re-scheduled, do_switch() would load CR3=0 from process->cr3,
     * causing a triple fault (looks like a hang with -no-reboot).
     * Disabling interrupts ensures we reach thread_exit() atomically,
     * which sets THREAD_DEAD so the scheduler won't re-enqueue us. */
    arch_irq_disable();
    /* Mark process as exited and wake any waitpid waiter BEFORE thread_exit.
     * Save wait_thread before setting exited=1, because after exited=1 the
     * waiter may kfree the process at any time. */
    if (t->process) {
        thread_t *waiter = t->process->wait_thread;
        t->process->wait_thread = NULL;
        arch_memory_barrier();
        t->process->exited = 1;
        t->process = NULL;
        /* Wake the waiter. sched_wake acquires rq_lock with irqsave,
         * which is safe even with interrupts disabled (cli). */
        if (waiter)
            sched_wake(waiter);
    }
    thread_exit();
}

int64_t sys_exit(uint64_t status, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    thread_t *t = thread_get_current();
    process_terminate(t, (int64_t)status);
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

    /* Capture setuid/setgid bits before we lose exec_node access */
    uint16_t exec_mode = exec_node ? exec_node->mode : 0;
    uint16_t exec_uid = exec_node ? exec_node->uid : 0;
    uint16_t exec_gid = exec_node ? exec_node->gid : 0;

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

    /* Apply setuid/setgid bits from executable */
    if (exec_mode & VFS_MODE_SETUID) {
        child->euid = exec_uid;
        child->suid = exec_uid;
    }
    if (exec_mode & VFS_MODE_SETGID) {
        child->egid = exec_gid;
        child->sgid = exec_gid;
    }

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
            {
                int ei = eventfd_index((const eventfd_t *)proc->fd_table[i].eventfd);
                if (ei >= 0) eventfd_ref(ei);
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

int64_t sys_execve(uint64_t path_ptr, uint64_t argv_ptr,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* Copy path from user space (before we destroy the address space) */
    char raw_path[MAX_PATH], path[MAX_PATH];
    if (copy_string_from_user((const char *)path_ptr, raw_path, MAX_PATH) != 0)
        return -EFAULT;
    resolve_user_path(proc, raw_path, path);

    /* Check CAP_EXEC */
    if (!(proc->capabilities & CAP_EXEC) &&
        !cap_token_check(proc->pid, CAP_EXEC, path))
        return -EACCES;

    /* Open and read the ELF file */
    int node_idx = vfs_open(path);
    if (node_idx < 0)
        return -ENOENT;

    vfs_node_t *exec_node = vfs_get_node(node_idx);
    if (exec_node && !(exec_node->mode & VFS_PERM_EXEC))
        return -EACCES;

    /* Capture setuid/setgid bits before we lose exec_node access */
    uint16_t exec_mode = exec_node ? exec_node->mode : 0;
    uint16_t exec_uid = exec_node ? exec_node->uid : 0;
    uint16_t exec_gid = exec_node ? exec_node->gid : 0;

    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.size == 0)
        return -ENOENT;

    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf)
        return -ENOMEM;

    int64_t n = vfs_read(node_idx, 0, buf, st.size);
    if (n != (int64_t)st.size) {
        kfree(buf);
        return -EIO;
    }

    /* Copy argv from user space (before we destroy the address space) */
    int argc = 0;
    char argv_buf[PROC_ARGV_BUF_SIZE];
    int buf_pos = 0;

    if (argv_ptr != 0 && validate_user_ptr(argv_ptr, 8) == 0) {
        const char **user_argv = (const char **)argv_ptr;
        while (argc < PROC_MAX_ARGS) {
            if (validate_user_ptr((uint64_t)&user_argv[argc], 8) != 0)
                break;
            const char *arg = user_argv[argc];
            if (arg == NULL)
                break;
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

    /* Load ELF into a new address space */
    elf_load_result_t result;
    if (elf_load(buf, st.size, &result) != 0) {
        kfree(buf);
        return -ENOEXEC;
    }
    kfree(buf);

    /* Map new user stack in the new address space */
    uint64_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t i = 0; i < stack_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            vmm_free_user_pages(result.cr3);
            return -ENOMEM;
        }
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(phys);
        for (uint64_t j = 0; j < PAGE_SIZE; j++)
            dst[j] = 0;
        if (vmm_map_page_in(result.cr3, stack_bottom + i * PAGE_SIZE, phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            vmm_free_user_pages(result.cr3);
            return -ENOMEM;
        }
    }

    /* === POINT OF NO RETURN === */

    /* Close FD_CLOEXEC file descriptors */
    for (int i = 0; i < MAX_FDS; i++) {
        if (proc->fd_table[i].fd_flags & FD_CLOEXEC)
            fd_close(&proc->fd_table[i]);
        else
            proc->fd_table[i].fd_flags = 0;  /* clear cloexec in surviving fds */
    }

    /* Close TCP connections (not inherited across exec) */
    for (int i = 0; i < 8; i++) {
        if (proc->tcp_conns[i]) {
            tcp_close(i);
            proc->tcp_conns[i] = 0;
        }
    }

    /* Detach shared memory regions */
    {
        uint64_t sflags;
        shm_lock_acquire(&sflags);
        for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
            if (proc->mmap_table[i].used &&
                proc->mmap_table[i].shm_id >= 0) {
                int32_t sid = proc->mmap_table[i].shm_id;
                if (sid < MAX_SHM_REGIONS && shm_table[sid].ref_count > 0) {
                    shm_table[sid].ref_count--;
                    if (shm_table[sid].ref_count == 0) {
                        for (uint32_t pi = 0; pi < shm_table[sid].num_pages; pi++) {
                            if (shm_table[sid].phys_pages[pi]) {
                                pmm_free_page(shm_table[sid].phys_pages[pi]);
                                shm_table[sid].phys_pages[pi] = 0;
                            }
                        }
                        shm_table[sid].key = -1;
                        shm_table[sid].num_pages = 0;
                    }
                }
                proc->mmap_table[i].used = 0;
                proc->mmap_table[i].shm_id = -1;
            }
        }
        shm_unlock_release(sflags);
    }

    /* Free old address space */
    uint64_t old_cr3 = proc->cr3;
    arch_switch_address_space(vmm_get_kernel_pml4());
    vmm_free_user_pages(old_cr3);

    /* Install new address space */
    proc->cr3 = result.cr3;
    proc->user_entry = result.entry;
    proc->user_stack_top = USER_STACK_TOP;

    /* Reset mmap table */
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        proc->mmap_table[i].used = 0;
        proc->mmap_table[i].shm_id = -1;
        proc->mmap_table[i].demand = 0;
    }
    proc->mmap_next_addr = MMAP_REGION_BASE;
    proc->used_mem_pages = 0;

    /* Set process name from path */
    {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') base = p + 1;
        int ni = 0;
        while (base[ni] && ni < 31) { proc->name[ni] = base[ni]; ni++; }
        proc->name[ni] = '\0';
        if (ni > 4 && proc->name[ni-4] == '.' && proc->name[ni-3] == 'e' &&
            proc->name[ni-2] == 'l' && proc->name[ni-1] == 'f')
            proc->name[ni-4] = '\0';
    }

    /* Set argv */
    proc->argc = argc;
    proc->argv_buf_len = buf_pos;
    for (int i = 0; i < buf_pos; i++)
        proc->argv_buf[i] = argv_buf[i];

    /* Reset signals: handlers to SIG_DFL (except SIG_IGN stays) */
    proc->pending_signals = 0;
    proc->signal_mask = 0;
    for (int i = 0; i < MAX_SIGNALS; i++) {
        if (proc->sig_handlers[i].sa_handler != SIG_IGN) {
            proc->sig_handlers[i].sa_handler = SIG_DFL;
            proc->sig_handlers[i].sa_flags = 0;
        }
    }
    proc->sig_queue.head = 0;
    proc->sig_queue.tail = 0;
    proc->sig_queue.count = 0;
    proc->signal_depth = 0;
    proc->restart_pending = 0;

    /* Apply setuid/setgid bits from executable */
    if (exec_mode & VFS_MODE_SETUID) {
        proc->euid = exec_uid;
        proc->suid = exec_uid;
    }
    if (exec_mode & VFS_MODE_SETGID) {
        proc->egid = exec_gid;
        proc->sgid = exec_gid;
    }

    /* Reset security state */
    proc->seccomp_mask = 0;
    proc->seccomp_mask_hi = 0;
    proc->seccomp_strict = 0;

    /* Set up argv on user stack and enter usermode.
     * This mirrors process_enter_usermode() logic. */
    arch_switch_address_space(proc->cr3);
    syscall_set_kernel_stack(t->stack_base + t->stack_size);

    uint64_t user_rsp = proc->user_stack_top;
    uint64_t user_rdi = 0;
    uint64_t user_rsi = 0;

    if (proc->argc > 0) {
        uint64_t sp = proc->user_stack_top;
        uint64_t str_addrs[PROC_MAX_ARGS];
        int idx = 0;
        int ii = 0;
        while (ii < proc->argv_buf_len && idx < proc->argc) {
            int slen = 0;
            while (ii + slen < proc->argv_buf_len && proc->argv_buf[ii + slen] != '\0')
                slen++;
            slen++;
            sp -= (uint64_t)slen;
            char *dst = (char *)sp;
            for (int j = 0; j < slen; j++)
                dst[j] = proc->argv_buf[ii + j];
            str_addrs[idx] = sp;
            idx++;
            ii += slen;
        }
        sp &= ~7ULL;
        sp -= (uint64_t)(idx + 1) * 8;
        uint64_t *argv_arr = (uint64_t *)sp;
        for (int j = 0; j < idx; j++)
            argv_arr[j] = str_addrs[j];
        argv_arr[idx] = 0;
        sp &= ~0xFULL;
        user_rsp = sp;
        user_rdi = (uint64_t)idx;
        user_rsi = (uint64_t)argv_arr;
    }

    arch_prepare_usermode_return();
    arch_enter_usermode(proc->user_entry, user_rsp, user_rdi, user_rsi);
    /* Never reached */
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

    /* Read parent's saved user context from kernel stack. */
    uint64_t *kstack_top = (uint64_t *)(t->stack_base + t->stack_size);
    fork_context_t ctx;
#if defined(__x86_64__)
    /* Layout (from syscall_entry.asm, high to low):
     *   kstack_top[-1] = user RSP
     *   kstack_top[-2] = user RIP (RCX)
     *   kstack_top[-3] = user RFLAGS (R11)
     *   kstack_top[-4] = RBP .. kstack_top[-9] = R15 */
    ctx.rip    = kstack_top[-2];
    ctx.rsp    = kstack_top[-1];
    ctx.rflags = kstack_top[-3];
    ctx.rbp    = kstack_top[-4];
    ctx.rbx    = kstack_top[-5];
    ctx.r12    = kstack_top[-6];
    ctx.r13    = kstack_top[-7];
    ctx.r14    = kstack_top[-8];
    ctx.r15    = kstack_top[-9];
#elif defined(__aarch64__)
    /* Read user context directly from the exception frame saved by
     * vectors.S SAVE_CONTEXT. The frame pointer is stored per-CPU by
     * arm64_sync_handler — avoids dependence on SP == kstack_top. */
    {
        extern uint64_t *arm64_exception_frame[];
        uint64_t *frame = arm64_exception_frame[percpu_get()->cpu_id];
        /* SAVE_CONTEXT layout (arm64_frame_t / interrupt_frame_t):
         *   [frame+0..240]  = x0-x30
         *   [frame+248]     = elr_el1
         *   [frame+256]     = spsr_el1
         *   [frame+264]     = sp_el0 */
        ctx.elr  = *(uint64_t *)((uint8_t *)frame + 248);
        ctx.sp   = *(uint64_t *)((uint8_t *)frame + 264);
        ctx.spsr = *(uint64_t *)((uint8_t *)frame + 256);
        /* Callee-saved regs from SAVE_CONTEXT: x19-x29 */
        ctx.x19  = frame[19];
        ctx.x20  = frame[20];
        ctx.x21  = frame[21];
        ctx.x22  = frame[22];
        ctx.x23  = frame[23];
        ctx.x24  = frame[24];
        ctx.x25  = frame[25];
        ctx.x26  = frame[26];
        ctx.x27  = frame[27];
        ctx.x28  = frame[28];
        ctx.x29  = frame[29];
    }
#endif

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
            {
                int ei = eventfd_index((const eventfd_t *)proc->fd_table[i].eventfd);
                if (ei >= 0) eventfd_ref(ei);
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
        arch_memory_barrier();
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
    if (validate_user_ptr(buf_ptr, 64) != 0) return -1;

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
            out[56] = p->daemon;
            /* bytes 57-63 reserved */
            for (int j = 57; j < 64; j++)
                out[j] = 0;
            return 0;
        }
        count++;
    }
    return -1;  /* no more processes */
}
