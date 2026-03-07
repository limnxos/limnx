#include "proc/process.h"
#include "proc/elf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kheap.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "serial.h"

static uint64_t next_pid = 1;
static process_t *proc_table[MAX_PROCS];

void process_register(process_t *proc) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i] == NULL) {
            proc_table[i] = proc;
            return;
        }
    }
}

process_t *process_lookup(uint64_t pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i] && proc_table[i]->pid == pid)
            return proc_table[i];
    }
    return NULL;
}

void process_unregister(uint64_t pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i] && proc_table[i]->pid == pid) {
            proc_table[i] = NULL;
            return;
        }
    }
}

/*
 * Kernel-thread entry point for a user-space process.
 * Switches to the process's address space and jumps to ring 3 via SYSRETQ.
 */
static void process_enter_usermode(void) {
    thread_t *t = thread_get_current();
    process_t *proc = t->process;

    /* Set the kernel stack for SYSCALL re-entry */
    syscall_set_kernel_stack(t->stack_base + t->stack_size);

    /* Load the process's address space */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(proc->cr3) : "memory");

    uint64_t user_rsp = proc->user_stack_top;
    uint64_t user_rdi = 0;  /* argc */
    uint64_t user_rsi = 0;  /* argv */

    if (proc->argc > 0) {
        /*
         * Set up argv on the user stack:
         *   1. Copy packed strings from argv_buf
         *   2. Write argv[] pointer array (NULL-terminated)
         *   3. Align RSP to 16 bytes
         */
        uint64_t sp = proc->user_stack_top;

        /* 1. Copy strings onto stack, record start of each */
        uint64_t str_addrs[PROC_MAX_ARGS];
        int idx = 0;
        int i = 0;
        while (i < proc->argv_buf_len && idx < proc->argc) {
            int slen = 0;
            while (i + slen < proc->argv_buf_len && proc->argv_buf[i + slen] != '\0')
                slen++;
            slen++;  /* include NUL */
            sp -= (uint64_t)slen;
            /* Copy string to user stack */
            char *dst = (char *)sp;
            for (int j = 0; j < slen; j++)
                dst[j] = proc->argv_buf[i + j];
            str_addrs[idx] = sp;
            idx++;
            i += slen;
        }

        /* 2. Align sp to 8-byte boundary for pointer array */
        sp &= ~7ULL;

        /* 3. Write argv[] pointer array (NULL-terminated) */
        sp -= (uint64_t)(idx + 1) * 8;  /* space for idx pointers + NULL */
        uint64_t *argv_arr = (uint64_t *)sp;
        for (int j = 0; j < idx; j++)
            argv_arr[j] = str_addrs[j];
        argv_arr[idx] = 0;  /* NULL terminator */

        /* 4. Align RSP to 16 bytes */
        sp &= ~0xFULL;

        user_rsp = sp;
        user_rdi = (uint64_t)idx;       /* argc */
        user_rsi = (uint64_t)argv_arr;  /* argv */
    }

    /*
     * Fix GS state before entering user mode via SYSRETQ.
     *
     * In kernel mode, GS.base = percpu (set by syscall_init/smp_init).
     * For SYSRETQ, we need: GS.base = 0 (user), KERNEL_GS_BASE = percpu.
     * This ensures the next SWAPGS in syscall_entry correctly loads percpu.
     *
     * Read current GS.base; if non-zero (percpu), move it to KERNEL_GS_BASE
     * and clear GS.base.
     */
    {
        uint32_t gs_lo, gs_hi;
        __asm__ volatile ("rdmsr" : "=a"(gs_lo), "=d"(gs_hi) : "c"((uint32_t)0xC0000101));
        if (gs_lo || gs_hi) {
            /* Move percpu pointer from GS.base → KERNEL_GS_BASE */
            __asm__ volatile ("wrmsr" : : "c"((uint32_t)0xC0000102), "a"(gs_lo), "d"(gs_hi));
            __asm__ volatile ("wrmsr" : : "c"((uint32_t)0xC0000101), "a"((uint32_t)0), "d"((uint32_t)0));
        }
    }

    /*
     * Jump to ring 3 via SYSRETQ:
     *   RCX = user RIP (entry point)
     *   R11 = user RFLAGS (IF=1 to enable interrupts)
     *   RSP = user stack top
     *   RDI = argc, RSI = argv
     */
    __asm__ volatile (
        "mov %0, %%rcx\n"
        "mov %1, %%rsp\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov $0x202, %%r11\n"
        "sysretq"
        :
        : "r"(proc->user_entry), "r"(user_rsp),
          "r"(user_rdi), "r"(user_rsi)
        : "rcx", "r11", "rdi", "rsi", "memory"
    );
}

process_t *process_create(const uint8_t *code, uint64_t code_size) {
    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    if (!proc)
        return NULL;

    proc->pid = next_pid++;
    proc->user_entry = USER_CODE_BASE;
    proc->user_stack_top = USER_STACK_TOP;

    /* Set parent PID from calling process (0 if kernel) */
    thread_t *caller = thread_get_current();
    proc->parent_pid = (caller && caller->process) ? caller->process->pid : 0;
    proc->exit_status = 0;
    proc->exited = 0;

    /* Initialize process group — inherit parent's pgid */
    {
        process_t *parent = process_lookup(proc->parent_pid);
        proc->pgid = parent ? parent->pgid : proc->pid;
        /* Inherit uid/gid/caps from parent, or root defaults */
        if (parent) {
            proc->uid = parent->uid;
            proc->gid = parent->gid;
            proc->capabilities = parent->capabilities;
        } else {
            proc->uid = 0;
            proc->gid = 0;
            proc->capabilities = CAP_ALL;
        }
    }

    /* Initialize security fields */
    proc->rlimit_mem_pages = 0;
    proc->rlimit_cpu_ticks = 0;
    proc->rlimit_nfds = 0;
    proc->used_mem_pages = 0;
    proc->seccomp_mask = 0;
    proc->seccomp_strict = 0;
    proc->audit_flags = 0;

    /* Initialize file descriptor table */
    for (int i = 0; i < MAX_FDS; i++) {
        proc->fd_table[i].node = (void *)0;
        proc->fd_table[i].offset = 0;
        proc->fd_table[i].pipe = (void *)0;
        proc->fd_table[i].pipe_write = 0;
        proc->fd_table[i].pty = (void *)0;
        proc->fd_table[i].pty_is_master = 0;
        proc->fd_table[i].unix_sock = (void *)0;
        proc->fd_table[i].eventfd = (void *)0;
        proc->fd_table[i].epoll = (void *)0;
        proc->fd_table[i].uring = (void *)0;
        proc->fd_table[i].open_flags = 0;
        proc->fd_table[i].fd_flags = 0;
    }

    /* Initialize mmap table */
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        proc->mmap_table[i].used = 0;
        proc->mmap_table[i].shm_id = -1;
        proc->mmap_table[i].demand = 0;
    }
    proc->mmap_next_addr = MMAP_REGION_BASE;

    /* Initialize working directory */
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';

    /* Initialize signals */
    proc->pending_signals = 0;
    for (int i = 0; i < MAX_SIGNALS; i++)
        proc->sig_handlers[i].sa_handler = SIG_DFL;
    proc->signal_frame_addr = 0;

    /* Initialize argv */
    proc->argc = 0;
    proc->argv_buf_len = 0;

    /* Initialize env */
    proc->env_count = 0;
    proc->env_buf_len = 0;

    /* Initialize TCP connection tracking */
    for (int i = 0; i < 8; i++)
        proc->tcp_conns[i] = 0;

    /* Create a new address space (PML4 with kernel upper-half cloned) */
    proc->cr3 = vmm_create_address_space();
    if (proc->cr3 == 0) {
        serial_puts("[proc] ERROR: failed to create address space\n");
        kfree(proc);
        return NULL;
    }

    /* Map user code pages */
    uint64_t code_pages = (code_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < code_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            serial_puts("[proc] ERROR: out of memory for code\n");
            kfree(proc);
            return NULL;
        }

        /* Copy code bytes into the physical page via HHDM */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(phys);
        uint64_t offset = i * PAGE_SIZE;
        uint64_t chunk = code_size - offset;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;

        for (uint64_t j = 0; j < chunk; j++)
            dst[j] = code[offset + j];
        /* Zero remaining bytes in the page */
        for (uint64_t j = chunk; j < PAGE_SIZE; j++)
            dst[j] = 0;

        /* Map into user address space: readable + writable + executable + user */
        uint64_t virt = USER_CODE_BASE + offset;
        if (vmm_map_page_in(proc->cr3, virt, phys, PTE_USER | PTE_WRITABLE) != 0) {
            serial_puts("[proc] ERROR: failed to map code page\n");
            kfree(proc);
            return NULL;
        }
    }

    /* Map user stack pages (grows down from USER_STACK_TOP) */
    uint64_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t i = 0; i < stack_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            serial_puts("[proc] ERROR: out of memory for stack\n");
            kfree(proc);
            return NULL;
        }

        /* Zero the stack page */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(phys);
        for (uint64_t j = 0; j < PAGE_SIZE; j++)
            dst[j] = 0;

        uint64_t virt = stack_bottom + i * PAGE_SIZE;
        if (vmm_map_page_in(proc->cr3, virt, phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            serial_puts("[proc] ERROR: failed to map stack page\n");
            kfree(proc);
            return NULL;
        }
    }

    /* Create a kernel thread that will transition to user mode */
    thread_t *t = thread_create(process_enter_usermode, 0);
    if (!t) {
        serial_puts("[proc] ERROR: failed to create thread\n");
        kfree(proc);
        return NULL;
    }

    t->process = proc;
    proc->main_thread = t;
    process_register(proc);
    /* NOTE: caller must call sched_add(proc->main_thread) after setup */

    serial_printf("[proc] Created process %lu (cr3=%lx, entry=%lx)\n",
        proc->pid, proc->cr3, proc->user_entry);

    return proc;
}

process_t *process_create_from_elf(const uint8_t *elf, uint64_t size) {
    elf_load_result_t result;
    if (elf_load(elf, size, &result) != 0)
        return NULL;

    process_t *proc = (process_t *)kmalloc(sizeof(process_t));
    if (!proc)
        return NULL;

    proc->pid = next_pid++;
    proc->user_entry = result.entry;
    proc->user_stack_top = USER_STACK_TOP;
    proc->cr3 = result.cr3;

    /* Set parent PID from calling process (0 if kernel) */
    thread_t *caller = thread_get_current();
    proc->parent_pid = (caller && caller->process) ? caller->process->pid : 0;
    proc->exit_status = 0;
    proc->exited = 0;

    /* Initialize process group — inherit parent's pgid */
    {
        process_t *parent = process_lookup(proc->parent_pid);
        proc->pgid = parent ? parent->pgid : proc->pid;
        /* Inherit uid/gid/caps from parent, or root defaults */
        if (parent) {
            proc->uid = parent->uid;
            proc->gid = parent->gid;
            proc->capabilities = parent->capabilities;
        } else {
            proc->uid = 0;
            proc->gid = 0;
            proc->capabilities = CAP_ALL;
        }
    }

    /* Initialize security fields */
    proc->rlimit_mem_pages = 0;
    proc->rlimit_cpu_ticks = 0;
    proc->rlimit_nfds = 0;
    proc->used_mem_pages = 0;
    proc->seccomp_mask = 0;
    proc->seccomp_strict = 0;
    proc->audit_flags = 0;

    /* Initialize file descriptor table */
    for (int i = 0; i < MAX_FDS; i++) {
        proc->fd_table[i].node = (void *)0;
        proc->fd_table[i].offset = 0;
        proc->fd_table[i].pipe = (void *)0;
        proc->fd_table[i].pipe_write = 0;
        proc->fd_table[i].pty = (void *)0;
        proc->fd_table[i].pty_is_master = 0;
        proc->fd_table[i].unix_sock = (void *)0;
        proc->fd_table[i].eventfd = (void *)0;
        proc->fd_table[i].epoll = (void *)0;
        proc->fd_table[i].uring = (void *)0;
        proc->fd_table[i].open_flags = 0;
        proc->fd_table[i].fd_flags = 0;
    }

    /* Initialize mmap table */
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        proc->mmap_table[i].used = 0;
        proc->mmap_table[i].shm_id = -1;
        proc->mmap_table[i].demand = 0;
    }
    proc->mmap_next_addr = MMAP_REGION_BASE;

    /* Initialize working directory */
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';

    /* Initialize signals */
    proc->pending_signals = 0;
    for (int i = 0; i < MAX_SIGNALS; i++)
        proc->sig_handlers[i].sa_handler = SIG_DFL;
    proc->signal_frame_addr = 0;

    /* Initialize argv */
    proc->argc = 0;
    proc->argv_buf_len = 0;

    /* Initialize env */
    proc->env_count = 0;
    proc->env_buf_len = 0;

    /* Initialize TCP connection tracking */
    for (int i = 0; i < 8; i++)
        proc->tcp_conns[i] = 0;

    /* Map user stack pages (grows down from USER_STACK_TOP) */
    uint64_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t i = 0; i < stack_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            serial_puts("[proc] ERROR: out of memory for stack\n");
            kfree(proc);
            return NULL;
        }

        /* Zero the stack page */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(phys);
        for (uint64_t j = 0; j < PAGE_SIZE; j++)
            dst[j] = 0;

        uint64_t virt = stack_bottom + i * PAGE_SIZE;
        if (vmm_map_page_in(proc->cr3, virt, phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            serial_puts("[proc] ERROR: failed to map stack page\n");
            kfree(proc);
            return NULL;
        }
    }

    /* Create a kernel thread that will transition to user mode */
    thread_t *t = thread_create(process_enter_usermode, 0);
    if (!t) {
        serial_puts("[proc] ERROR: failed to create thread\n");
        kfree(proc);
        return NULL;
    }

    t->process = proc;
    proc->main_thread = t;
    process_register(proc);
    /* NOTE: caller must call sched_add(proc->main_thread) after setup */

    serial_printf("[proc] Created process %lu (cr3=%lx, entry=%lx)\n",
        proc->pid, proc->cr3, proc->user_entry);

    return proc;
}

void process_reap(process_t *proc) {
    if (!proc) return;
    while (!proc->exited)
        sched_yield();
    process_unregister(proc->pid);
    kfree(proc);
}

/* --- fork --- */

static void fork_child_entry(void) {
    thread_t *t = thread_get_current();
    process_t *proc = t->process;

    syscall_set_kernel_stack(t->stack_base + t->stack_size);
    __asm__ volatile ("mov %0, %%cr3" : : "r"(proc->cr3) : "memory");

    /* Fix GS state: move percpu from GS.base → KERNEL_GS_BASE for SYSRETQ */
    {
        uint32_t gs_lo, gs_hi;
        __asm__ volatile ("rdmsr" : "=a"(gs_lo), "=d"(gs_hi) : "c"((uint32_t)0xC0000101));
        if (gs_lo || gs_hi) {
            __asm__ volatile ("wrmsr" : : "c"((uint32_t)0xC0000102), "a"(gs_lo), "d"(gs_hi));
            __asm__ volatile ("wrmsr" : : "c"((uint32_t)0xC0000101), "a"((uint32_t)0), "d"((uint32_t)0));
        }
    }

    /* Load per-child fork context address into rax, then restore all regs.
     * Order matters: rsp must be loaded last (before sysretq). */
    __asm__ volatile (
        "mov %0, %%rax\n"
        "mov 24(%%rax), %%rbp\n"     /* fork_ctx.rbp (offset 24) */
        "mov 32(%%rax), %%rbx\n"     /* fork_ctx.rbx (offset 32) */
        "mov 40(%%rax), %%r12\n"     /* fork_ctx.r12 (offset 40) */
        "mov 48(%%rax), %%r13\n"     /* fork_ctx.r13 (offset 48) */
        "mov 56(%%rax), %%r14\n"     /* fork_ctx.r14 (offset 56) */
        "mov 64(%%rax), %%r15\n"     /* fork_ctx.r15 (offset 64) */
        "mov 0(%%rax), %%rcx\n"      /* fork_ctx.rip (offset 0) → RCX for SYSRETQ */
        "mov 16(%%rax), %%r11\n"     /* fork_ctx.rflags (offset 16) → R11 */
        "mov 8(%%rax), %%rsp\n"      /* fork_ctx.rsp (offset 8) */
        "xor %%rax, %%rax\n"         /* return 0 */
        "sysretq"
        :
        : "r"(&proc->fork_ctx)
        : "memory"
    );
}

process_t *process_fork(process_t *parent, const fork_context_t *ctx) {
    process_t *child = (process_t *)kmalloc(sizeof(process_t));
    if (!child) return NULL;

    child->pid = next_pid++;
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->capabilities = parent->capabilities;
    child->rlimit_mem_pages = parent->rlimit_mem_pages;
    child->rlimit_cpu_ticks = parent->rlimit_cpu_ticks;
    child->rlimit_nfds = parent->rlimit_nfds;
    child->used_mem_pages = parent->used_mem_pages;
    child->seccomp_mask = parent->seccomp_mask;
    child->seccomp_strict = parent->seccomp_strict;
    child->audit_flags = parent->audit_flags;
    child->exit_status = 0;
    child->exited = 0;
    child->user_entry = parent->user_entry;
    child->user_stack_top = parent->user_stack_top;
    child->pending_signals = 0;
    child->signal_frame_addr = 0;
    child->fork_ctx = *ctx;  /* per-child saved user context */

    /* Clone address space with COW */
    child->cr3 = vmm_clone_cow(parent->cr3);
    if (child->cr3 == 0) { kfree(child); return NULL; }

    /* Copy fd_table (pipe ref increments done by caller in syscall.c) */
    for (int i = 0; i < MAX_FDS; i++)
        child->fd_table[i] = parent->fd_table[i];

    /* Copy mmap_table (shm ref increments done by caller in syscall.c) */
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++)
        child->mmap_table[i] = parent->mmap_table[i];
    child->mmap_next_addr = parent->mmap_next_addr;

    /* Copy cwd */
    for (int i = 0; i < MAX_PATH; i++)
        child->cwd[i] = parent->cwd[i];

    /* Copy argv */
    child->argc = parent->argc;
    child->argv_buf_len = parent->argv_buf_len;
    for (int i = 0; i < parent->argv_buf_len; i++)
        child->argv_buf[i] = parent->argv_buf[i];

    /* Copy env */
    child->env_count = parent->env_count;
    child->env_buf_len = parent->env_buf_len;
    for (int i = 0; i < parent->env_buf_len; i++)
        child->env_buf[i] = parent->env_buf[i];

    /* Copy signal handlers */
    for (int i = 0; i < MAX_SIGNALS; i++)
        child->sig_handlers[i] = parent->sig_handlers[i];

    /* TCP connections are NOT inherited by forked children.
     * They are per-process kernel resources (like Linux). If inherited,
     * the child's sys_exit would close the parent's connections. */
    for (int i = 0; i < 8; i++)
        child->tcp_conns[i] = 0;

    /* fork_saved was filled by process_fork_set_context() before this call */

    /* Create kernel thread for child */
    thread_t *ct = thread_create(fork_child_entry, 0);
    if (!ct) { kfree(child); return NULL; }
    ct->process = child;
    child->main_thread = ct;

    process_register(child);
    /* NOTE: caller must call sched_add(child->main_thread) after
     * incrementing pipe/pty/unix_sock ref counts. */
    return child;
}

/* --- Signal delivery --- */

int process_deliver_signal(process_t *proc, int signum) {
    if (!proc || !proc->main_thread)
        return -1;

    switch (signum) {
    case SIGKILL:
        proc->exit_status = -(int64_t)signum;
        /* Try to remove from ready queue. Only set exited=1 if we
         * actually found and removed the thread — this proves it's not
         * currently running on any CPU. If sched_remove returns 0, the
         * thread was already dequeued by another CPU's scheduler and is
         * either running or about to run; schedule() will set exited=1
         * when it sees THREAD_DEAD. Setting exited=1 prematurely would
         * let waitpid kfree the process while schedule() still uses it. */
        if (proc->main_thread->state == THREAD_READY) {
            if (sched_remove(proc->main_thread))
                proc->exited = 1;
        }
        proc->main_thread->state = THREAD_DEAD;
        serial_printf("[proc] Process %lu killed (SIGKILL)\n", proc->pid);
        return 0;
    case SIGTERM:
    case SIGINT:
    case SIGCHLD:
        proc->pending_signals |= (1U << (uint32_t)signum);
        return 0;
    case SIGSTOP:
        if (proc->main_thread->state == THREAD_READY) {
            sched_remove(proc->main_thread);
        }
        proc->main_thread->state = THREAD_STOPPED;
        serial_printf("[proc] Process %lu stopped (SIGSTOP)\n", proc->pid);
        return 0;
    case SIGCONT:
        if (proc->main_thread->state == THREAD_STOPPED) {
            proc->main_thread->state = THREAD_READY;
            sched_add(proc->main_thread);
            serial_printf("[proc] Process %lu continued (SIGCONT)\n", proc->pid);
        }
        return 0;
    default:
        return -1;
    }
}

int process_kill_group(uint64_t pgid, int signum) {
    int count = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i] && proc_table[i]->pgid == pgid) {
            if (process_deliver_signal(proc_table[i], signum) == 0)
                count++;
        }
    }
    return count > 0 ? 0 : -1;
}
