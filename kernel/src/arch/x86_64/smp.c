#define pr_fmt(fmt) "[smp]  " fmt
#include "klog.h"

#include "arch/percpu.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/idt.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "limine.h"
#include "arch/cpu.h"

/* MSR definitions */
#define MSR_EFER           0xC0000080
#define MSR_STAR           0xC0000081
#define MSR_LSTAR          0xC0000082
#define MSR_SFMASK         0xC0000084

/* Per-CPU data array */
percpu_t percpu_array[MAX_CPUS] __attribute__((aligned(64)));
uint32_t cpu_count = 1;
uint32_t bsp_cpu_id = 0;

/* Limine SMP request */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0
};

/* IDT pointer — shared across all CPUs */
static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) shared_idtp;

/* SYSCALL entry point */
extern void syscall_entry(void);

/* Per-CPU GDT setup */

static void gdt_set_entry_raw(struct gdt_entry *gdt, int i, uint32_t base,
                                uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

/* Initialize per-CPU GDT with standard segments + TSS for this CPU */
static void percpu_gdt_init(percpu_t *pc) {
    struct gdt_entry *gdt = pc->gdt;

    /* Null */
    gdt_set_entry_raw(gdt, 0, 0, 0, 0, 0);
    /* Kernel code */
    gdt_set_entry_raw(gdt, 1, 0, 0xFFFFF, 0x9A, 0xA0);
    /* Kernel data */
    gdt_set_entry_raw(gdt, 2, 0, 0xFFFFF, 0x92, 0xC0);
    /* User data (before user code for SYSRET) */
    gdt_set_entry_raw(gdt, 3, 0, 0xFFFFF, 0xF2, 0xC0);
    /* User code */
    gdt_set_entry_raw(gdt, 4, 0, 0xFFFFF, 0xFA, 0xA0);
    /* TSS (16-byte descriptor in slots 5-6) */
    gdt_set_entry_raw(gdt, 5, 0, 0, 0, 0);
    gdt_set_entry_raw(gdt, 6, 0, 0, 0, 0);

    /* Install TSS descriptor */
    uint64_t tss_base = (uint64_t)&pc->tss;
    uint32_t tss_limit = sizeof(tss_t) - 1;

    gdt[5].limit_low   = tss_limit & 0xFFFF;
    gdt[5].base_low    = tss_base & 0xFFFF;
    gdt[5].base_mid    = (tss_base >> 16) & 0xFF;
    gdt[5].access      = 0x89;  /* present, DPL=0, 64-bit TSS available */
    gdt[5].granularity = ((tss_limit >> 16) & 0x0F);
    gdt[5].base_high   = (tss_base >> 24) & 0xFF;

    /* High 8 bytes of TSS descriptor */
    uint32_t *slot6 = (uint32_t *)&gdt[6];
    slot6[0] = (uint32_t)(tss_base >> 32);
    slot6[1] = 0;

    /* Zero TSS */
    uint8_t *tp = (uint8_t *)&pc->tss;
    for (uint64_t i = 0; i < sizeof(tss_t); i++)
        tp[i] = 0;
    pc->tss.iopb_offset = sizeof(tss_t);

    /* Set up GDT pointer */
    pc->gdtp.limit = sizeof(pc->gdt) - 1;
    pc->gdtp.base  = (uint64_t)&pc->gdt;
}

/* Load per-CPU GDT and reload segments */
static void percpu_gdt_load(percpu_t *pc) {
    __asm__ volatile ("lgdt %0" : : "m"(pc->gdtp));

    /* Reload data segments */
    __asm__ volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        /* FS and GS set to 0 — we use MSR_KERNEL_GS_BASE for per-CPU */
        "xor %%ax, %%ax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        ::: "ax", "memory"
    );

    /* Far jump to reload CS — use push/retfq trick */
    __asm__ volatile (
        "leaq 1f(%%rip), %%rax\n"
        "pushq $0x08\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        ::: "rax", "memory"
    );

    /* Load TSS */
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)0x28));
}

/* Set up SYSCALL MSRs for this CPU */
static void setup_syscall_msrs(void) {
    /* Enable SCE in EFER */
    uint64_t efer = arch_rdmsr(MSR_EFER);
    efer |= 1;  /* SCE bit */
    arch_wrmsr(MSR_EFER, efer);

    arch_wrmsr(MSR_STAR, ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32));

    /* LSTAR: SYSCALL entry point */
    arch_wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* SFMASK: clear IF on syscall entry */
    arch_wrmsr(MSR_SFMASK, (1 << 9));  /* bit 9 = IF */
}

/* AP idle loop — runs on each AP after initialization */
static void ap_idle_loop(void) {
    for (;;) {
        arch_irq_enable();
        arch_halt();
    }
}

/* Enable FPU/SSE on this CPU */
static void ap_fpu_init(void) {
    arch_fpu_init();
}

/* AP entry point — called by Limine when AP is woken up */
static void ap_entry(struct limine_smp_info *info) {
    percpu_t *pc = (percpu_t *)info->extra_argument;

    /* Load per-CPU GDT + TSS */
    percpu_gdt_load(pc);

    /* Load IDT (shared) */
    __asm__ volatile ("lidt %0" : : "m"(shared_idtp));

    /* Enable FPU/SSE on this AP */
    ap_fpu_init();

    /* Set up SYSCALL MSRs */
    setup_syscall_msrs();

    /* Set GS.base directly for kernel mode (percpu_get reads gs:16).
     * KERNEL_GS_BASE stays 0; process_enter_usermode sets it before SYSRETQ. */
    arch_wrmsr(0xC0000101, (uint64_t)pc);  /* MSR_GS_BASE */

    /* Initialize LAPIC */
    lapic_init();
    lapic_timer_calibrate();
    lapic_timer_start(10);  /* 10ms scheduling tick */

    /* Create idle thread for this CPU (not added to queue by thread_create) */
    thread_t *idle = thread_create(ap_idle_loop, 0);
    if (idle) {
        idle->state = THREAD_RUNNING;
        pc->idle_thread = idle;
        pc->current_thread = idle;
    }

    /* Signal BSP that we're ready */
    arch_memory_barrier();
    pc->started = 1;

    pr_info("AP %u ready (LAPIC ID %u)\n", pc->cpu_id, pc->lapic_id);

    /* Switch to the idle thread's allocated stack (32KB) before entering
     * the idle loop. ap_entry runs on Limine's small goto stack which is
     * too small for timer ISR + schedule frames. */
    if (idle) {
        uint64_t stack_top = idle->stack_base + idle->stack_size;
        pc->kernel_rsp = stack_top;
        __asm__ volatile (
            "mov %0, %%rsp\n"
            "sti\n"
            "1: hlt\n"
            "jmp 1b\n"
            :
            : "r"(stack_top)
            : "memory"
        );
    }

    /* Fallback (shouldn't reach here) */
    arch_irq_enable();
    for (;;)
        arch_halt();
}

void arch_smp_init(void) {
    if (!smp_request.response) {
        pr_info("No SMP response from bootloader\n");
        cpu_count = 1;
        return;
    }

    struct limine_smp_response *resp = smp_request.response;
    cpu_count = (uint32_t)resp->cpu_count;
    uint32_t bsp_lapic = resp->bsp_lapic_id;

    if (cpu_count > MAX_CPUS)
        cpu_count = MAX_CPUS;

    pr_info("%u CPUs detected, BSP LAPIC ID=%u\n",
                  cpu_count, bsp_lapic);

    /* Save IDT pointer for APs */
    __asm__ volatile ("sidt %0" : "=m"(shared_idtp));

    /* === Initialize BSP (cpu 0) === */
    percpu_t *bsp = &percpu_array[0];
    bsp->cpu_id = 0;
    bsp->lapic_id = bsp_lapic;
    bsp->self = (uint64_t)bsp;
    bsp->current_thread = thread_get_current();
    bsp->idle_thread = NULL;  /* BSP idle handled by sched */
    bsp->signal_deliver_pending = 0;
    bsp->signal_deliver_rdi = 0;
    bsp->rq_head = NULL;
    bsp->rq_tail = NULL;
    bsp->rq_lock = 0;
    bsp->started = 1;
    bsp_cpu_id = 0;

    /* Set up per-CPU GDT for BSP */
    percpu_gdt_init(bsp);
    percpu_gdt_load(bsp);

    /* Set GS.base for kernel mode */
    arch_wrmsr(0xC0000101, (uint64_t)bsp);  /* MSR_GS_BASE */

    /* Re-setup SYSCALL MSRs */
    setup_syscall_msrs();

    /* Set kernel_rsp from current thread */
    thread_t *cur = thread_get_current();
    if (cur && cur->stack_base && cur->stack_size)
        bsp->kernel_rsp = cur->stack_base + cur->stack_size;

    /* Initialize LAPIC on BSP */
    lapic_init();
    lapic_timer_calibrate();
    lapic_timer_start(10);  /* 10ms tick */

    /* LAPIC timer now drives scheduling — disable PIT scheduling */
    idt_set_lapic_timer_active();

    pr_info("BSP initialized with per-CPU GDT + LAPIC timer\n");

    /* === Start APs === */
    uint32_t ap_started = 0;

    for (uint64_t i = 0; i < resp->cpu_count && i < MAX_CPUS; i++) {
        struct limine_smp_info *info = resp->cpus[i];
        if (info->lapic_id == bsp_lapic)
            continue;  /* skip BSP */

        uint32_t ap_id = ap_started + 1;  /* CPU 0 = BSP, CPU 1+ = APs */
        percpu_t *pc = &percpu_array[ap_id];

        pc->cpu_id = ap_id;
        pc->lapic_id = info->lapic_id;
        pc->self = (uint64_t)pc;
        pc->current_thread = NULL;
        pc->idle_thread = NULL;
        pc->signal_deliver_pending = 0;
        pc->signal_deliver_rdi = 0;
        pc->rq_head = NULL;
        pc->rq_tail = NULL;
        pc->rq_lock = 0;
        pc->started = 0;

        /* Set up per-CPU GDT + TSS */
        percpu_gdt_init(pc);

        /* Pass percpu pointer via extra_argument */
        info->extra_argument = (uint64_t)pc;

        /* Wake up the AP */
        arch_memory_barrier();
        info->goto_address = ap_entry;

        ap_started++;
    }

    /* Wait for all APs to signal ready */
    for (uint32_t i = 1; i <= ap_started; i++) {
        volatile uint32_t *flag = &percpu_array[i].started;
        int timeout = 1000000;
        while (!*flag && --timeout > 0)
            arch_pause();

        if (!*flag)
            pr_warn("AP %u did not start\n", i);
    }

    pr_info("%u APs started\n", ap_started);
}

/* TLB shootdown: send IPI to all other CPUs */
void arch_tlb_shootdown(void) {
    uint32_t my_id = lapic_get_id();
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (percpu_array[i].lapic_id != my_id)
            lapic_send_ipi(percpu_array[i].lapic_id, LAPIC_TLB_VECTOR);
    }
}

/* Send IPI to a specific CPU by logical ID */
void arch_send_ipi(uint32_t target_cpu_id, uint32_t vector) {
    if (target_cpu_id >= cpu_count)
        return;
    lapic_send_ipi(percpu_array[target_cpu_id].lapic_id, vector);
}

/* SYSCALL init wrapper — called from syscall_init() on BSP */
#define MSR_GS_BASE 0xC0000101
void arch_syscall_init(void) {
    setup_syscall_msrs();
    /* Set GS.base for kernel mode — percpu_get() reads gs:16.
     * KERNEL_GS_BASE starts as 0; process_enter_usermode will set it
     * to percpu before SYSRETQ (so SWAPGS in syscall_entry works). */
    arch_wrmsr(MSR_GS_BASE, (uint64_t)&percpu_array[0]);
}

void arch_set_kernel_stack(uint64_t stack_top) {
    tss_set_rsp0(stack_top);
    /* Update per-CPU kernel_rsp (read by syscall_entry.asm via gs:0).
     * Read MSR_GS_BASE to get percpu pointer — during early boot before
     * syscall_init sets GS.base, fall back to BSP percpu directly. */
    uint64_t gs_base = arch_rdmsr(MSR_GS_BASE);
    if (gs_base) {
        ((percpu_t *)gs_base)->kernel_rsp = stack_top;
    } else {
        percpu_array[0].kernel_rsp = stack_top;
    }
}
