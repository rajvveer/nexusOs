; ============================================================================
; NexusOS Kernel Entry Point
; ============================================================================
; This is the first code that runs in the kernel.
; It zeros BSS, sets up the stack, and calls kernel_main() in C.
; ============================================================================

[bits 32]
[extern kernel_main]            ; Defined in kernel.c
[extern _bss_start]             ; Defined in linker.ld
[extern _bss_end]               ; Defined in linker.ld

section .text
global _start

_start:
    ; Set up kernel stack
    mov esp, 0x90000            ; Stack pointer at 576KB
    mov ebp, esp

    ; Zero the BSS section
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi                ; ECX = BSS size in bytes
    shr ecx, 2                  ; ECX = number of dwords
    xor eax, eax                ; Zero
    rep stosd                   ; Zero fill BSS

    ; Call the C kernel
    call kernel_main

    ; If kernel_main returns, halt the CPU
.hang:
    cli
    hlt
    jmp .hang
