; Limnx — ISR/IRQ stubs and GDT flush (x86_64 NASM)

section .text
bits 64

; ============================================================
; gdt_flush — load GDT and reload segment registers
;   rdi = pointer to gdt_ptr struct
; ============================================================
global gdt_flush
gdt_flush:
    lgdt [rdi]

    ; Reload data segments
    mov ax, 0x10        ; GDT_KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far return to reload CS with GDT_KERNEL_CODE (0x08)
    pop rdi             ; save return address
    push 0x08           ; new CS
    push rdi            ; return address
    retfq

; ============================================================
; ISR / IRQ stub macros
; ============================================================

; Exception without error code — push dummy 0
%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push 0              ; dummy error code
    push %1             ; interrupt number
    jmp isr_common_stub
%endmacro

; Exception with error code (CPU pushes it)
%macro ISR_ERR 1
global isr_%1
isr_%1:
    push %1             ; interrupt number (error code already on stack)
    jmp isr_common_stub
%endmacro

; IRQ stub — no error code, vector = irq + 32
%macro IRQ_STUB 2
global isr_%2
isr_%2:
    push 0              ; dummy error code
    push %2             ; vector number
    jmp isr_common_stub
%endmacro

; ============================================================
; Generate 32 exception stubs (ISR 0-31)
; ============================================================
ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8       ; Double Fault
ISR_NOERR  9
ISR_ERR   10       ; Invalid TSS
ISR_ERR   11       ; Segment Not Present
ISR_ERR   12       ; Stack-Segment Fault
ISR_ERR   13       ; General Protection Fault
ISR_ERR   14       ; Page Fault
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17       ; Alignment Check
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21       ; Control Protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30       ; Security Exception
ISR_NOERR 31

; ============================================================
; Generate 16 IRQ stubs (IRQ 0-15 → ISR 32-47)
; ============================================================
IRQ_STUB  0, 32
IRQ_STUB  1, 33
IRQ_STUB  2, 34
IRQ_STUB  3, 35
IRQ_STUB  4, 36
IRQ_STUB  5, 37
IRQ_STUB  6, 38
IRQ_STUB  7, 39
IRQ_STUB  8, 40
IRQ_STUB  9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

; ============================================================
; ISR 48 — LAPIC timer
; ISR 255 — LAPIC spurious
; ============================================================
global isr_48
isr_48:
    push 0              ; dummy error code
    push 48             ; vector number
    jmp isr_common_stub

global isr_49
isr_49:
    push 0              ; dummy error code
    push 49             ; vector number (TLB shootdown)
    jmp isr_common_stub

global isr_255
isr_255:
    push 0
    push 255
    jmp isr_common_stub

; ============================================================
; isr_common_stub — save all GP regs, call C handler, restore
;
; Conditional SWAPGS: If we came from user-mode (CS != 0x08),
; do SWAPGS to get kernel GS. On return, SWAPGS back.
; ============================================================
extern interrupt_dispatch

isr_common_stub:
    ; Check if we came from user-mode by inspecting CS on the stack.
    ; Stack layout at this point:
    ;   [rsp+0]  = int_no (pushed by stub)
    ;   [rsp+8]  = err_code (pushed by stub or dummy)  -- wait, reversed
    ; Actually the stub pushes: error_code first (or CPU does), then int_no.
    ; No — look at the macro: ISR_NOERR pushes 0 (dummy err), then int_no.
    ; ISR_ERR pushes int_no (err already on stack from CPU).
    ; So stack is: [rsp] = int_no, [rsp+8] = err_code
    ; Then CPU pushed: RIP, CS, RFLAGS, RSP, SS
    ; So CS is at [rsp + 8 + 8 + 8] = [rsp + 24]
    ; Wait: CPU pushes SS, RSP, RFLAGS, CS, RIP (in that order, so RIP at lowest).
    ; Then stub pushes err_code, then int_no.
    ; So: [rsp+0]=int_no, [rsp+8]=err_code, [rsp+16]=RIP, [rsp+24]=CS
    cmp qword [rsp+24], 0x08
    je .no_swapgs_entry
    swapgs
.no_swapgs_entry:

    ; Save all 15 general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass pointer to interrupt_frame_t (stack pointer) as arg
    mov rdi, rsp
    call interrupt_dispatch

    ; Restore all GP registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove int_no and err_code from stack
    ; Now [rsp] = RIP, [rsp+8] = CS ...
    ; Check CS to decide if we need SWAPGS on return
    cmp qword [rsp+8+8], 0x08  ; CS is at [rsp + 16] after removing int_no/err_code
    ; But we haven't removed them yet. After add rsp,16: CS is at [rsp+8]
    ; Let's do the check BEFORE removing, so CS is at [rsp + 16 + 8] = no...
    ; After the pops, we're back to: [rsp]=int_no, [rsp+8]=err_code, [rsp+16]=RIP, [rsp+24]=CS
    ; We want to check CS before iretq. Let's check at [rsp+24].

    cmp qword [rsp+24], 0x08
    je .no_swapgs_exit

    add rsp, 16         ; remove int_no and err_code
    swapgs
    iretq

.no_swapgs_exit:
    add rsp, 16         ; remove int_no and err_code
    iretq

; ============================================================
; ISR stub table — array of 50 function pointers
; (0-47 original + 48 LAPIC timer + 255 spurious mapped to slot 49)
; ============================================================
section .data

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr_%+i
%assign i i+1
%endrep
    dq isr_48           ; slot 48 = LAPIC timer
    dq isr_49           ; slot 49 = TLB shootdown IPI
