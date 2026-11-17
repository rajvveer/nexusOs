; ============================================================================
; NexusOS — IDT Load (Assembly)
; ============================================================================

[bits 32]
section .text
global idt_load

idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ; No sti here: the 8259s are still at the BIOS vectors until pic_init
    ; remaps them, so kernel_main enables interrupts only after that.
    ret
