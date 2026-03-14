/*
 * ARM64 SMP support — PSCI-based secondary core bringup + GIC SGI IPI
 *
 * Brings up secondary CPUs via PSCI CPU_ON (HVC) on QEMU virt.
 * Each AP initializes its own GIC CPU interface and timer, then
 * enters the scheduler idle loop.
 */

#define pr_fmt(fmt) "[arm64-smp] " fmt
#include "klog.h"

#include "arch/smp_hal.h"
#include "arch/percpu.h"
#include "arch/arm64/gic.h"
#include "arch/cpu.h"
#include "arch/serial.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "sched/sched.h"
#include "sched/thread.h"

/* Global per-CPU data (same as x86_64) */
percpu_t percpu_array[MAX_CPUS] __attribute__((aligned(64)));
uint32_t cpu_count = 1;
uint32_t bsp_cpu_id = 0;

/* SGI IDs for IPI */
#define SGI_TLB_SHOOTDOWN  1
#define SGI_RESCHEDULE     2

/* PSCI function IDs (SMCCC/HVC calling convention) */
#define PSCI_CPU_ON_64     0xC4000003ULL
#define PSCI_SUCCESS       0
#define PSCI_ALREADY_ON    (-4)

/* AP stack size: 64KB per AP */
#define AP_STACK_SIZE      65536
#define AP_STACK_PAGES     (AP_STACK_SIZE / PAGE_SIZE)

/* Communication variables defined in ap_boot.S */
extern uint64_t ap_boot_ttbr0;
extern uint64_t ap_boot_stack_top;

/* AP entry point defined in ap_boot.S */
extern void arm64_ap_entry(void);

/* GIC CPU interface init for secondary core */
extern void gic_cpu_interface_init(void);

/* Timeout for AP startup: ~100ms worth of spins */
#define AP_START_TIMEOUT   10000000UL

/*
 * Invoke PSCI CPU_ON via HVC.
 * x0 = function ID (PSCI_CPU_ON_64)
 * x1 = target CPU MPIDR
 * x2 = entry point (physical address)
 * x3 = context ID (passed to AP as x0)
 * Returns PSCI status in x0.
 */
static int64_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry_phys,
                            uint64_t context_id) {
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON_64;
    register uint64_t x1 __asm__("x1") = target_mpidr;
    register uint64_t x2 __asm__("x2") = entry_phys;
    register uint64_t x3 __asm__("x3") = context_id;

    __asm__ volatile (
        "hvc    #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory"
    );

    return (int64_t)x0;
}

/*
 * AP idle loop — entered after AP init completes.
 * The AP's idle thread runs here, yielding to the scheduler.
 */
static void ap_idle_loop(void) {
    for (;;) {
        arch_irq_enable();
        arch_halt();
    }
}

/*
 * arm64_ap_main — called from ap_boot.S after MMU/VBAR/stack are set up.
 * This runs on the AP with interrupts disabled.
 */
void arm64_ap_main(uint64_t cpu_id) {
    /* Validate cpu_id */
    if (cpu_id == 0 || cpu_id >= MAX_CPUS) {
        /* Invalid — halt this AP */
        for (;;)
            arch_halt();
    }

    /* Set TPIDR_EL1 to this AP's percpu data */
    percpu_t *pc = &percpu_array[cpu_id];
    arch_set_percpu_base((uint64_t)pc);

    /* Enable NEON/FP on this AP (CPACR_EL1) — needed for context_switch fxsave/fxrstor */
    arch_fpu_init();

    pr_info("AP%lu online, initializing\n", cpu_id);

    /* Initialize GIC CPU interface for this core.
     * The distributor was already initialized by the BSP.
     * Each core needs its own CPU interface enabled. */
    gic_cpu_interface_init();

    /* Enable the timer PPI (IRQ 30) for this core.
     * PPIs are per-CPU, so each core must enable its own. */
    gic_enable_irq(GIC_TIMER_NS_EL1);
    gic_set_priority(GIC_TIMER_NS_EL1, 0x80);

    /* Enable and arm the per-CPU EL1 physical timer (10ms interval) */
    {
        uint64_t freq;
        __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
        uint64_t tval = freq / 100;  /* 10ms */
        __asm__ volatile ("msr cntp_tval_el0, %0" : : "r"(tval));
        __asm__ volatile ("msr cntp_ctl_el0, %0" : : "r"((uint64_t)1));
        __asm__ volatile ("isb");
    }

    /* Create this AP's idle thread */
    thread_t *idle = thread_create(ap_idle_loop, 0);
    if (!idle) {
        pr_err("AP%lu: failed to create idle thread\n", cpu_id);
        for (;;)
            arch_halt();
    }
    pc->idle_thread = idle;
    pc->current_thread = idle;
    idle->state = THREAD_RUNNING;
    idle->last_cpu = (uint32_t)cpu_id;

    /* Memory barrier: ensure all AP init writes are visible to BSP */
    __asm__ volatile ("dmb sy" ::: "memory");

    /* Signal BSP that this AP is ready */
    pc->started = 1;
    __asm__ volatile ("dmb sy" ::: "memory");

    pr_info("AP%lu: ready, entering idle\n", cpu_id);

    /* Enable interrupts and enter idle loop.
     * The timer interrupt will drive sched_tick() on this core. */
    arch_irq_enable();
    for (;;)
        arch_halt();
}

void arch_smp_init(void) {
    /* BSP percpu is already set up (cpu_id=0, started=1, TPIDR_EL1 set).
     * This was done earlier in the single-core stub init. */
    percpu_array[0].cpu_id = 0;
    percpu_array[0].started = 1;
    percpu_array[0].current_thread = thread_get_current();

    /* Set TPIDR_EL1 for BSP */
    arch_set_percpu_base((uint64_t)&percpu_array[0]);

    pr_info("BSP (CPU 0) initialized\n");

    /* Read kernel page table base (TTBR0_EL1) for AP boot */
    uint64_t bsp_ttbr0;
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(bsp_ttbr0));

    /* Compute physical address of AP entry point.
     * On ARM64 with identity mapping (VA=PA), the symbol address IS physical. */
    uint64_t ap_entry_phys = (uint64_t)arm64_ap_entry;

    pr_info("kernel TTBR0=%lx, AP entry=%lx\n", bsp_ttbr0, ap_entry_phys);

    /* Bring up secondary CPUs (1 through MAX_CPUS-1) */
    for (uint32_t i = 1; i < MAX_CPUS; i++) {
        /* Initialize percpu data for this AP */
        percpu_array[i].cpu_id = i;
        percpu_array[i].started = 0;
        percpu_array[i].current_thread = NULL;
        percpu_array[i].idle_thread = NULL;
        percpu_array[i].rq_head = NULL;
        percpu_array[i].rq_tail = NULL;
        percpu_array[i].rq_lock = 0;

        /* Allocate AP kernel stack (64KB = 16 pages) */
        uint64_t stack_phys = pmm_alloc_contiguous(AP_STACK_PAGES);
        if (stack_phys == 0) {
            pr_err("failed to allocate stack for AP%u\n", i);
            break;
        }
        uint64_t stack_top = stack_phys + AP_STACK_SIZE;

        /* Set communication variables for ap_boot.S */
        ap_boot_ttbr0 = bsp_ttbr0;
        ap_boot_stack_top = stack_top;

        /* Memory barrier: ensure AP boot params are visible */
        __asm__ volatile ("dmb sy" ::: "memory");

        /* PSCI CPU_ON: target MPIDR = i (Aff0), entry = ap_entry_phys,
         * context_id = cpu_id */
        pr_info("starting AP%u via PSCI CPU_ON (MPIDR=%u)\n", i, i);
        int64_t ret = psci_cpu_on((uint64_t)i, ap_entry_phys, (uint64_t)i);

        if (ret == PSCI_SUCCESS) {
            /* Wait for AP to signal started */
            uint64_t timeout = AP_START_TIMEOUT;
            while (!percpu_array[i].started && timeout > 0) {
                __asm__ volatile ("yield");
                timeout--;
            }

            if (percpu_array[i].started) {
                cpu_count++;
                pr_info("AP%u started successfully (cpu_count=%u)\n",
                        i, cpu_count);
            } else {
                pr_err("AP%u timed out waiting for start\n", i);
                /* Free the stack since AP didn't start */
                for (uint32_t p = 0; p < AP_STACK_PAGES; p++)
                    pmm_free_page(stack_phys + p * PAGE_SIZE);
            }
        } else if (ret == PSCI_ALREADY_ON) {
            pr_warn("AP%u already on (PSCI returned ALREADY_ON)\n", i);
            /* Free unused stack */
            for (uint32_t p = 0; p < AP_STACK_PAGES; p++)
                pmm_free_page(stack_phys + p * PAGE_SIZE);
        } else {
            pr_info("AP%u not present (PSCI returned %ld)\n", i, ret);
            /* Free unused stack */
            for (uint32_t p = 0; p < AP_STACK_PAGES; p++)
                pmm_free_page(stack_phys + p * PAGE_SIZE);
            /* If first AP fails with invalid params, stop trying */
            break;
        }
    }

    if (cpu_count > 1) {
        pr_info("SMP active: %u CPUs online\n", cpu_count);
        sched_set_smp_active();
    } else {
        pr_info("single-core mode (no secondary CPUs found)\n");
    }
}

void arch_send_ipi(uint32_t target_cpu_id, uint32_t vector) {
    /* Map vector to SGI ID */
    uint32_t sgi_id = (vector == SGI_TLB_SHOOTDOWN) ? SGI_TLB_SHOOTDOWN : vector;
    gic_send_sgi(target_cpu_id, sgi_id);
}

void arch_tlb_shootdown(void) {
    /*
     * ARM64: TLBI broadcasts are handled by hardware on most implementations.
     * TLBI VMALLE1 invalidates all TLB entries for EL1 on all PEs in the
     * same inner-shareable domain.
     *
     * No IPI needed — use broadcast TLBI.
     */
    __asm__ volatile (
        "dsb ishst\n"       /* Ensure PTE writes are visible */
        "tlbi vmalle1is\n"  /* Invalidate all EL1 TLB, inner-shareable */
        "dsb ish\n"         /* Wait for TLBI to complete */
        "isb\n"             /* Synchronize context */
    );
}

void arch_syscall_init(void) {
    /* ARM64: SVC handler is installed via VBAR_EL1 in arch_interrupt_init.
     * No MSR setup needed (unlike x86_64 STAR/LSTAR/SFMASK). */
    pr_info("ARM64 syscall init (via VBAR_EL1 vector table)\n");
}

void arch_set_kernel_stack(uint64_t stack_top) {
    (void)stack_top;
}

/* Per-CPU saved exception frame pointer — set by arm64_sync_handler,
 * read by sys_fork to extract user context without kstack_top offsets. */
uint64_t *arm64_exception_frame[MAX_CPUS];
