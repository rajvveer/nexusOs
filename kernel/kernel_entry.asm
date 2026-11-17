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
    ; Set up kernel stack at 4MB, growing down over the 3-4MB region (a 16KB
    ; guard band at 3MB is unmapped by paging_protect_kernel). Must match
    ; BOOT_STACK_TOP in memory.h (pmm_init keeps the PMM off 0-8MB: kernel,
    ; headroom, stack, and since Phase 57 the 4MB heap at 4-8MB). BSS lives at
    ; 8MB+ via the linker script, so no overlap.
    ; IF stays clear (boot2's cli) until kernel_main's sti after pic_init.
    mov esp, 0x400000
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
