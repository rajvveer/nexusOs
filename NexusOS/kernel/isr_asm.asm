; ============================================================================
; NexusOS — ISR Stubs (Assembly)
; ============================================================================
; CPU Exceptions (0-31). Some push an error code, others don't.
; We normalize them all to push a dummy error code if needed.
; ============================================================================

[bits 32]
section .text

extern isr_handler

; Macro for ISR with NO error code (we push a dummy 0)
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push dword 0               ; Dummy error code
    push dword %1              ; Interrupt number
    jmp isr_common_stub
%endmacro

; Macro for ISR WITH error code (CPU already pushed it)
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push dword %1              ; Interrupt number
    jmp isr_common_stub
%endmacro

; --- CPU Exceptions ---
ISR_NOERRCODE 0                ; Division By Zero
ISR_NOERRCODE 1                ; Debug
ISR_NOERRCODE 2                ; Non Maskable Interrupt
ISR_NOERRCODE 3                ; Breakpoint
ISR_NOERRCODE 4                ; Overflow
ISR_NOERRCODE 5                ; Bound Range Exceeded
ISR_NOERRCODE 6                ; Invalid Opcode
ISR_NOERRCODE 7                ; Device Not Available
ISR_ERRCODE   8                ; Double Fault
ISR_NOERRCODE 9                ; Coprocessor Segment Overrun
ISR_ERRCODE   10               ; Invalid TSS
ISR_ERRCODE   11               ; Segment Not Present
ISR_ERRCODE   12               ; Stack-Segment Fault
ISR_ERRCODE   13               ; General Protection Fault
ISR_ERRCODE   14               ; Page Fault
ISR_NOERRCODE 15               ; Reserved
ISR_NOERRCODE 16               ; x87 Floating-Point Exception
ISR_ERRCODE   17               ; Alignment Check
ISR_NOERRCODE 18               ; Machine Check
ISR_NOERRCODE 19               ; SIMD Floating-Point Exception
ISR_NOERRCODE 20               ; Virtualization Exception
ISR_ERRCODE   21               ; Control Protection Exception
ISR_NOERRCODE 22               ; Reserved
ISR_NOERRCODE 23               ; Reserved
ISR_NOERRCODE 24               ; Reserved
ISR_NOERRCODE 25               ; Reserved
ISR_NOERRCODE 26               ; Reserved
ISR_NOERRCODE 27               ; Reserved
ISR_NOERRCODE 28               ; Reserved
ISR_NOERRCODE 29               ; Reserved
ISR_NOERRCODE 30               ; Reserved
ISR_NOERRCODE 31               ; Reserved

; ============================================================================
; Common ISR stub — saves registers, calls C handler, restores, returns
; ============================================================================
isr_common_stub:
    pusha                       ; Push all general-purpose registers

    mov ax, ds
    push eax                   ; Save data segment

    mov ax, 0x10               ; Load kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                   ; Push pointer to registers struct
    call isr_handler           ; Call C handler
    add esp, 4                 ; Clean up pushed pointer

    pop eax                    ; Restore data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa                       ; Restore general-purpose registers
    add esp, 8                 ; Remove error code and ISR number
    iret                       ; Return from interrupt
