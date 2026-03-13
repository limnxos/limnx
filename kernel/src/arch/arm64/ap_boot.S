/*
 * ARM64 AP (Application Processor) boot stub
 *
 * Secondary cores are brought up via PSCI CPU_ON, which jumps to
 * arm64_ap_entry with:
 *   x0 = context_id (cpu_id, passed as 3rd arg to CPU_ON)
 *
 * This code runs with MMU off, at the physical entry address.
 * We must configure MAIR, TCR, TTBR0, enable MMU, set VBAR,
 * then call arm64_ap_main(cpu_id) in C.
 */

.section .text
.global arm64_ap_entry

arm64_ap_entry:
    /* x0 = cpu_id (context_id from PSCI CPU_ON) */
    mov     x19, x0                 /* save cpu_id in callee-saved reg */

    /* Disable all interrupts */
    msr     daifset, #0xf

    /* Disable alignment checking (same as BSP boot.S) */
    mrs     x1, sctlr_el1
    bic     x1, x1, #(1 << 1)      /* A  = alignment check off */
    bic     x1, x1, #(1 << 3)      /* SA = stack alignment off */
    msr     sctlr_el1, x1
    isb

    /*
     * Set up MAIR_EL1 (same values as BSP boot.S):
     *   Attr0 = 0xFF: Normal memory, Write-Back, Read/Write Allocate
     *   Attr1 = 0x00: Device-nGnRnE (for MMIO)
     */
    ldr     x1, =0x00000000000000FF
    msr     mair_el1, x1

    /*
     * Set up TCR_EL1 (same values as BSP boot.S):
     *   T0SZ=16, IRGN0=WB-WA, ORGN0=WB-WA, SH0=ISH, TG0=4KB
     *   T1SZ=16, TG1=4KB, IPS=40-bit
     */
    ldr     x1, =(16 | (1 << 8) | (1 << 10) | (3 << 12) | (0 << 14) | \
                   (16 << 16) | (1 << 24) | (1 << 26) | (3 << 28) | (2 << 30) | \
                   (2UL << 32))
    msr     tcr_el1, x1
    isb

    /*
     * Load TTBR0_EL1 with the BSP's kernel page table.
     * ap_boot_ttbr0 is set by arch_smp_init() before calling PSCI CPU_ON.
     */
    adrp    x1, ap_boot_ttbr0
    add     x1, x1, :lo12:ap_boot_ttbr0
    ldr     x1, [x1]
    msr     ttbr0_el1, x1

    /* TTBR1 unused */
    msr     ttbr1_el1, xzr

    /* Ensure page table writes are visible before enabling MMU */
    dsb     sy
    isb

    /* Enable MMU: M=1, C=1, I=1, A=0, SA=0 */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)      /* M  = MMU enable */
    orr     x1, x1, #(1 << 2)      /* C  = Data cache enable */
    orr     x1, x1, #(1 << 12)     /* I  = Instruction cache enable */
    bic     x1, x1, #(1 << 1)      /* A  = Alignment check disable */
    bic     x1, x1, #(1 << 3)      /* SA = Stack alignment disable */
    msr     sctlr_el1, x1
    isb

    /* MMU is now on — identity mapped, VA=PA */

    /* Set VBAR_EL1 to shared exception vector table */
    adrp    x1, arm64_vectors
    add     x1, x1, :lo12:arm64_vectors
    msr     vbar_el1, x1
    isb

    /* Enable NEON/FP (CPACR_EL1.FPEN = 0b11) */
    mrs     x1, cpacr_el1
    orr     x1, x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /*
     * Set up per-AP kernel stack.
     * ap_boot_stack_top is set by arch_smp_init() before calling CPU_ON.
     */
    adrp    x1, ap_boot_stack_top
    add     x1, x1, :lo12:ap_boot_stack_top
    ldr     x1, [x1]
    mov     sp, x1

    /* Call arm64_ap_main(cpu_id) — x19 holds saved cpu_id */
    mov     x0, x19
    bl      arm64_ap_main

    /* Should never return, but halt if it does */
.Lap_halt:
    wfi
    b       .Lap_halt

/*
 * Communication variables — set by BSP's arch_smp_init() before PSCI CPU_ON.
 * These are read by AP boot code above.
 */
.section .data
.align 3

.global ap_boot_ttbr0
ap_boot_ttbr0:
    .quad   0

.global ap_boot_stack_top
ap_boot_stack_top:
    .quad   0
