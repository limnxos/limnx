/*
 * ARM64 kernel entry point for QEMU virt machine.
 * Loaded by QEMU -kernel at the address specified in the ELF header.
 * Runs at EL1 (QEMU -kernel mode drops to EL1 before entry).
 *
 * On entry: x0 = DTB physical address (from QEMU)
 */

.section .text.boot
.global _start
_start:
    /* Disable interrupts */
    msr     daifset, #0xf

    /* Disable alignment checking early — QEMU Cortex-A57 resets with A=1.
     * GCC may generate unaligned accesses (e.g. STR X to 4-byte-aligned addr). */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 1)          /* A  = alignment check off */
    bic     x0, x0, #(1 << 3)          /* SA = stack alignment off */
    msr     sctlr_el1, x0
    isb

    /* Read CPU ID from MPIDR_EL1 — only boot on CPU 0 */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbnz    x1, .Lhalt

    /* Save DTB address (x0) for later use by kernel */
    adrp    x1, arm64_dtb_base
    add     x1, x1, :lo12:arm64_dtb_base
    str     x0, [x1]

    /* Set up the stack (grows down from _stack_top) */
    adrp    x0, _stack_top
    add     x0, x0, :lo12:_stack_top
    mov     sp, x0

    /* Zero BSS */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
.Lbss_loop:
    cmp     x0, x1
    b.ge    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_loop
.Lbss_done:

    /* === Enable MMU with identity mapping === */

    /*
     * Set up MAIR_EL1 (Memory Attribute Indirection Register):
     *   Attr0 = 0xFF: Normal memory, Write-Back, Read/Write Allocate
     *   Attr1 = 0x00: Device-nGnRnE (strongly ordered, for MMIO)
     */
    ldr     x0, =0x00000000000000FF
    msr     mair_el1, x0

    /*
     * Set up TCR_EL1 (Translation Control Register):
     *   T0SZ  = 16  → 48-bit VA for TTBR0 (bits 0-5)
     *   IRGN0 = 0b01 → Write-Back, Write-Allocate (bits 8-9)
     *   ORGN0 = 0b01 → Write-Back, Write-Allocate (bits 10-11)
     *   SH0   = 0b11 → Inner Shareable (bits 12-13)
     *   TG0   = 0b00 → 4KB granule (bits 14-15)
     *   T1SZ  = 16  → 48-bit VA for TTBR1 (bits 16-21)
     *   TG1   = 0b10 → 4KB granule (bits 30-31)
     *   IPS   = 0b010 → 40-bit PA (bits 32-34) = 1TB
     *
     * We only use TTBR0 (low half). TTBR1 is unused (no high-half kernel).
     * Set T1SZ=16 anyway for completeness.
     */
    ldr     x0, =(16 | (1 << 8) | (1 << 10) | (3 << 12) | (0 << 14) | \
                   (16 << 16) | (1 << 24) | (1 << 26) | (3 << 28) | (2 << 30) | \
                   (2UL << 32))
    msr     tcr_el1, x0
    isb

    /*
     * Build identity-map page tables in the boot_pagetables area.
     * Use L1 1GB block descriptors for simplicity:
     *   L0[0] → boot_l1_table
     *   L1[0] = 0x00000000, 1GB block, Device memory (UART, GIC)
     *   L1[1] = 0x40000000, 1GB block, Normal memory (RAM)
     */
    adrp    x0, boot_l0_table
    add     x0, x0, :lo12:boot_l0_table

    /* Zero L0 table (512 entries) */
    mov     x1, x0
    mov     x2, #512
.Lzero_l0:
    str     xzr, [x1], #8
    subs    x2, x2, #1
    b.ne    .Lzero_l0

    /* Zero L1 table */
    adrp    x1, boot_l1_table
    add     x1, x1, :lo12:boot_l1_table
    mov     x3, x1
    mov     x2, #512
.Lzero_l1:
    str     xzr, [x3], #8
    subs    x2, x2, #1
    b.ne    .Lzero_l1

    /* L0[0] = address of L1 table | TABLE descriptor (bits 1:0 = 0b11) */
    orr     x2, x1, #0x3       /* PTE_PRESENT | PTE_TABLE */
    str     x2, [x0, #0]       /* L0 entry 0 */

    /*
     * L1[0] = 0x00000000 | block descriptor | Device-nGnRnE
     * Block descriptor: bits[1:0] = 0b01 (block, not table)
     * AttrIndx = 1 (Device memory), bits[4:2] = 0b001
     * AF bit 10 = 1
     * AP[2:1] = 00 (RW from EL1, no EL0 access)
     */
    ldr     x2, =(0x00000000 | 1 | (1 << 2) | (1 << 10))  /* block | AttrIndx=1 | AF */
    str     x2, [x1, #0]       /* L1 entry 0: 0x00000000-0x3FFFFFFF Device */

    /*
     * L1[1] = 0x40000000 | block descriptor | Normal WB
     * AttrIndx = 0 (Normal memory), bits[4:2] = 0b000
     * AF bit 10 = 1
     * SH = Inner Shareable (bits 9:8 = 0b11)
     * AP[2:1] = 00 (RW from EL1, no EL0 access)
     */
    ldr     x2, =(0x40000000 | 1 | (0 << 2) | (3 << 8) | (1 << 10))  /* block | Normal | ISH | AF */
    str     x2, [x1, #8]       /* L1 entry 1: 0x40000000-0x7FFFFFFF RAM */

    /* Load TTBR0_EL1 with L0 table address */
    msr     ttbr0_el1, x0

    /* TTBR1 unused but set to zero to avoid stale data */
    msr     ttbr1_el1, xzr

    /* Ensure all page table writes are visible before enabling MMU */
    dsb     sy
    isb

    /* Enable MMU: set SCTLR_EL1.M=1, C=1 (data cache), I=1 (insn cache)
     * Clear A (alignment check) and SA (stack alignment check) */
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)          /* M  = MMU enable */
    orr     x0, x0, #(1 << 2)          /* C  = Data cache enable */
    orr     x0, x0, #(1 << 12)         /* I  = Instruction cache enable */
    bic     x0, x0, #(1 << 1)          /* A  = Alignment check disable */
    bic     x0, x0, #(1 << 3)          /* SA = Stack alignment disable */
    msr     sctlr_el1, x0
    isb

    /* MMU is now on — we're still running at our physical address
     * because the identity mapping maps VA=PA for the kernel region */

    /* Call kmain */
    bl      kmain

    /* If kmain returns, halt */
.Lhalt:
    wfi
    b       .Lhalt

/* DTB base address — saved from x0 at entry */
.section .data
.align 3
.global arm64_dtb_base
arm64_dtb_base:
    .quad   0

/* Boot page tables — must be 4KB aligned */
.section .bss
.align 12
.global boot_l0_table
boot_l0_table:
    .space 4096
.global boot_l1_table
boot_l1_table:
    .space 4096

/* Kernel stack — 64KB */
.align 16
_stack_bottom:
    .space 65536
.global _stack_top
_stack_top:
