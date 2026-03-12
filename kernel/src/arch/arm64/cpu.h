#ifndef LIMNX_ARM64_CPU_H
#define LIMNX_ARM64_CPU_H

#include <stdint.h>

/* --- CPU control --- */

static inline void arch_halt(void) {
    __asm__ volatile ("wfi");
}

static inline void arch_pause(void) {
    __asm__ volatile ("yield");
}

static inline void arch_irq_enable(void) {
    __asm__ volatile ("msr daifclr, #0xf");
}

static inline void arch_irq_disable(void) {
    __asm__ volatile ("msr daifset, #0xf");
}

static inline uint64_t arch_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("mrs %0, daif" : "=r"(flags));
    __asm__ volatile ("msr daifset, #0xf" ::: "memory");
    return flags;
}

static inline void arch_irq_restore(uint64_t flags) {
    __asm__ volatile ("msr daif, %0" : : "r"(flags) : "memory");
}

/* --- FPU/SIMD (NEON) --- */

static inline void arch_fpu_init(void) {
    /* Enable FP/SIMD: clear CPACR_EL1.FPEN trap bits (bits 20-21 = 0b11) */
    uint64_t cpacr;
    __asm__ volatile ("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ULL << 20);
    __asm__ volatile ("msr cpacr_el1, %0" : : "r"(cpacr));
    __asm__ volatile ("isb");
}

static inline void arch_fpu_save(void *buf) {
    /* Save 32 SIMD registers (Q0-Q31) + FPCR + FPSR */
    uint64_t *p = (uint64_t *)buf;
    __asm__ volatile (
        "stp q0,  q1,  [%0, #0]\n"
        "stp q2,  q3,  [%0, #32]\n"
        "stp q4,  q5,  [%0, #64]\n"
        "stp q6,  q7,  [%0, #96]\n"
        "stp q8,  q9,  [%0, #128]\n"
        "stp q10, q11, [%0, #160]\n"
        "stp q12, q13, [%0, #192]\n"
        "stp q14, q15, [%0, #224]\n"
        "stp q16, q17, [%0, #256]\n"
        "stp q18, q19, [%0, #288]\n"
        "stp q20, q21, [%0, #320]\n"
        "stp q22, q23, [%0, #352]\n"
        "stp q24, q25, [%0, #384]\n"
        "stp q26, q27, [%0, #416]\n"
        "stp q28, q29, [%0, #448]\n"
        "stp q30, q31, [%0, #480]\n"
        : : "r"(p) : "memory"
    );
}

static inline void arch_fpu_restore(const void *buf) {
    const uint64_t *p = (const uint64_t *)buf;
    __asm__ volatile (
        "ldp q0,  q1,  [%0, #0]\n"
        "ldp q2,  q3,  [%0, #32]\n"
        "ldp q4,  q5,  [%0, #64]\n"
        "ldp q6,  q7,  [%0, #96]\n"
        "ldp q8,  q9,  [%0, #128]\n"
        "ldp q10, q11, [%0, #160]\n"
        "ldp q12, q13, [%0, #192]\n"
        "ldp q14, q15, [%0, #224]\n"
        "ldp q16, q17, [%0, #256]\n"
        "ldp q18, q19, [%0, #288]\n"
        "ldp q20, q21, [%0, #320]\n"
        "ldp q22, q23, [%0, #352]\n"
        "ldp q24, q25, [%0, #384]\n"
        "ldp q26, q27, [%0, #416]\n"
        "ldp q28, q29, [%0, #448]\n"
        "ldp q30, q31, [%0, #480]\n"
        : : "r"(p) : "memory"
    );
}

/* --- TLS (Thread-Local Storage) --- */

static inline void arch_set_tls_base(uint64_t base) {
    __asm__ volatile ("msr tpidr_el0, %0" : : "r"(base));
}

static inline uint64_t arch_get_tls_base(void) {
    uint64_t base;
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(base));
    return base;
}

/* --- Per-CPU data (TPIDR_EL1) --- */

static inline void arch_set_percpu_base(uint64_t base) {
    __asm__ volatile ("msr tpidr_el1, %0" : : "r"(base));
}

static inline uint64_t arch_get_percpu_base(void) {
    uint64_t base;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r"(base));
    return base;
}

/* --- Memory barrier --- */

static inline void arch_memory_barrier(void) {
    __asm__ volatile ("dmb sy" ::: "memory");
}

#endif /* LIMNX_ARM64_CPU_H */
