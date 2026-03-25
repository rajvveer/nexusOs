; ============================================================================
; NexusOS — IRQ Stubs (Assembly)
; ============================================================================
; Hardware interrupts (IRQ 0-15) mapped to ISR 32-47
; ============================================================================

[bits 32]
section .text

extern irq_handler

; Macro for IRQ stub
%macro IRQ 2
global irq%1
irq%1:
    push dword 0               ; Dummy error code
    push dword %2              ; Interrupt number (32+IRQ)
    jmp irq_common_stub
%endmacro

; --- Hardware IRQs ---
IRQ 0,  32                     ; Timer
IRQ 1,  33                     ; Keyboard
IRQ 2,  34                     ; Cascade (PIC2)
IRQ 3,  35                     ; COM2
IRQ 4,  36                     ; COM1
IRQ 5,  37                     ; LPT2
IRQ 6,  38                     ; Floppy
IRQ 7,  39                     ; LPT1 / Spurious
IRQ 8,  40                     ; RTC
IRQ 9,  41                     ; Free
IRQ 10, 42                     ; Free
IRQ 11, 43                     ; Free
IRQ 12, 44                     ; PS/2 Mouse
IRQ 13, 45                     ; FPU
IRQ 14, 46                     ; Primary ATA
IRQ 15, 47                     ; Secondary ATA

; ============================================================================
; Common IRQ stub
; ============================================================================
irq_common_stub:
    pusha

    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8
    iret
