#include "syscall/syscall_internal.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/thread.h"
#include "smp/percpu.h"

extern int64_t syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

int64_t sys_sigaction(uint64_t signum, uint64_t handler,
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

int64_t sys_sigreturn(uint64_t a1, uint64_t a2,
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

int64_t sys_sigprocmask(uint64_t how, uint64_t new_mask,
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
