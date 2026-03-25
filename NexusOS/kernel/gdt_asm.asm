; ============================================================================
; NexusOS — GDT Flush (Assembly)
; ============================================================================
; Called from C: gdt_flush(uint32_t gdt_ptr_address)
; Loads the new GDT and reloads all segment registers.
; ============================================================================

[bits 32]
section .text
global gdt_flush

gdt_flush:
    mov eax, [esp + 4]         ; Get pointer to GDT descriptor
    lgdt [eax]                 ; Load the GDT

    ; Reload data segment registers with kernel data selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to reload CS with kernel code selector (0x08)
    jmp 0x08:.flush
.flush:
    ret
