#ifndef LIMNX_X86_64_CPU_H
#define LIMNX_X86_64_CPU_H

#include <stdint.h>

/* --- CPU control --- */

static inline void arch_halt(void) {
    __asm__ volatile ("hlt");
}

static inline void arch_pause(void) {
    __asm__ volatile ("pause");
}

static inline void arch_irq_enable(void) {
    __asm__ volatile ("sti");
}

static inline void arch_irq_disable(void) {
    __asm__ volatile ("cli");
}

static inline uint64_t arch_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void arch_irq_restore(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory");
}

/* --- FPU/SSE --- */

static inline void arch_fpu_init(void) {
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);  /* clear CR0.EM */
    cr0 |=  (1ULL << 1);  /* set CR0.MP */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);   /* CR4.OSFXSR */
    cr4 |= (1ULL << 10);  /* CR4.OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile ("fninit");
}

static inline void arch_fpu_save(void *buf) {
    __asm__ volatile ("fxsave %0" : "=m"(*(uint8_t *)buf));
}

static inline void arch_fpu_restore(const void *buf) {
    __asm__ volatile ("fxrstor %0" : : "m"(*(const uint8_t *)buf));
}

/* --- TLS (Thread-Local Storage) --- */

#define MSR_FS_BASE 0xC0000100

static inline void arch_set_tls_base(uint64_t base) {
    uint32_t lo = (uint32_t)base;
    uint32_t hi = (uint32_t)(base >> 32);
    __asm__ volatile ("wrmsr" : : "c"((uint32_t)MSR_FS_BASE), "a"(lo), "d"(hi));
}

static inline uint64_t arch_get_tls_base(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"((uint32_t)MSR_FS_BASE));
    return ((uint64_t)hi << 32) | lo;
}

/* --- MSR access (x86-specific, used by arch code) --- */

static inline void arch_wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t arch_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* --- GS base (x86-specific, for per-CPU data) --- */

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static inline void arch_set_gs_base(uint64_t base) {
    arch_wrmsr(MSR_GS_BASE, base);
}

static inline uint64_t arch_get_gs_base(void) {
    return arch_rdmsr(MSR_GS_BASE);
}

static inline void arch_prepare_usermode_return(void) {
    /* For SYSRETQ: move percpu pointer from GS.base → KERNEL_GS_BASE,
     * clear GS.base so user sees GS=0. The next SWAPGS in syscall_entry
     * will restore the kernel's percpu pointer. */
    uint64_t gs = arch_get_gs_base();
    if (gs) {
        arch_wrmsr(MSR_KERNEL_GS_BASE, gs);
        arch_set_gs_base(0);
    }
}

/* --- Memory barrier --- */

static inline void arch_memory_barrier(void) {
    __asm__ volatile ("" ::: "memory");
}

#endif /* LIMNX_X86_64_CPU_H */
