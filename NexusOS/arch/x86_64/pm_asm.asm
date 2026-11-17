; System sleep CPU context (pm.c): the boot CPU's switch into ACPI sleep and back, and the real-mode wake
; trampoline the firmware jumps to through the FACS waking vector (ACPI 6.5 section 16.3).
;
; int pm_suspend_cpu(int (*enter)(void* context), void* context)
;   Saves the callee-saved registers, RFLAGS and RSP, then calls enter(context) (which writes SLP_EN). When the
;   platform loses power, the firmware's resume path enters pm_wake_trampoline (copied to WAKE, below 1 MiB) in
;   real mode; it optionally restores the VBE mode through the video BIOS, enters long mode on the kernel's page
;   tables (with the identity map pm.c put back for this) and jumps to pm_wake_long, which reloads the saved RSP,
;   lets pm_resume_cpu restore the rest of the CPU state and returns 1 through the saved frame. When enter()
;   returns instead (no power loss), its result is returned.
KERNEL_CS equ 0x10
KERNEL_DS equ 0x18
CODE32_SELECTOR equ 0x08
WAKE equ 0x7800                         ; second half of the AP trampoline page (0x7000, smp.c)
bits 64
section .text
global pm_suspend_cpu, pm_wake_long
extern pm_resume_cpu
pm_suspend_cpu:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    pushfq
    mov [rel pm_cpu_rsp], rsp
    mov rax, rdi
    mov rdi, rsi
    call rax
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
pm_wake_long:                           ; long mode, higher half, CR3 = kernel tables, interrupts off
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, [rel pm_cpu_rsp]
    cld
    call pm_resume_cpu                  ; CR0/CR4/EFER/MTRR/PAT, GDT/TSS/IDT, MSRs, CR3, FPU (pm.c)
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    mov eax, 1
    ret

; void hibernate_restore_jump(uint64_t list, uint64_t temp_cr3, uint64_t image_cr3, uint64_t entry)
;   Hibernation restore (hibernate.c), last step of the boot kernel: on temporary tables (kernel image at its
;   higher-half address, direct map of all RAM) copy every spare frame to its image frame - list pages hold
;   [next list page, pair count, (destination, source) x 255], physical addresses - then switch to the image's
;   kernel tables, flush global translations and enter pm_wake_long. Registers only: this kernel's stack, data
;   and page tables are overwritten (its text with identical bytes).
DIRECT_MAP_BASE equ 0xFFFF800000000000
global hibernate_restore_jump
hibernate_restore_jump:
    cli
    cld
    mov r8, rdi
    mov r9, rdx
    mov r10, rcx
    mov cr3, rsi
    mov r11, DIRECT_MAP_BASE
.page:
    test r8, r8
    jz .done
    lea rbx, [r8 + r11]
    mov r12, [rbx + 8]
    lea r13, [rbx + 16]
.pair:
    test r12, r12
    jz .next
    mov rdi, [r13]
    add rdi, r11
    mov rsi, [r13 + 8]
    add rsi, r11
    mov rcx, 512
    rep movsq
    add r13, 16
    dec r12
    jmp .pair
.next:
    mov r8, [rbx]
    jmp .page
.done:
    mov cr3, r9
    mov rax, cr4
    mov rcx, rax
    and rcx, ~0x80                      ; CR4.PGE off and on: global entries of the boot kernel are gone
    mov cr4, rcx
    mov cr4, rax
    jmp r10

section .data
global pm_cpu_rsp
align 8
pm_cpu_rsp: dq 0

section .rodata
global pm_wake_trampoline, pm_wake_trampoline_end, pm_wake_gdtr, pm_wake_cr3, pm_wake_entry, pm_wake_vbe_mode
align 16
pm_wake_trampoline:
bits 16
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, WAKE + 0x7F0                ; top of the trampoline page: the video BIOS call needs a stack
    mov bx, [WAKE + (pm_wake_vbe_mode - pm_wake_trampoline)]
    test bx, bx
    jz .no_video
    mov ax, 0x4F02                      ; VBE set mode (BX = mode | LFB | keep memory), as boot2.asm left it
    int 0x10
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
.no_video:
    o32 lgdt [WAKE + (pm_wake_gdtr - pm_wake_trampoline)]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE32_SELECTOR:(WAKE + (pm_wake_protected - pm_wake_trampoline))
bits 32
pm_wake_protected:
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)  ; PAE, OSFXSR, OSXMMEXCPT
    mov cr4, eax
    mov eax, [WAKE + (pm_wake_cr3 - pm_wake_trampoline)]
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11) | 1         ; EFER.LME, NXE, SCE
    wrmsr
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3) | (1 << 29) | (1 << 30))   ; EM, TS, NW, CD off
    or eax, (1 << 31) | (1 << 16) | (1 << 1)                 ; PG, WP, MP
    mov cr0, eax
    jmp KERNEL_CS:(WAKE + (pm_wake_long64 - pm_wake_trampoline))
bits 64
pm_wake_long64:
    mov rax, [WAKE + (pm_wake_entry - pm_wake_trampoline)]
    jmp rax
align 8
pm_wake_gdtr: dw 0
    dd 0
align 8
pm_wake_cr3: dd 0
align 8
pm_wake_entry: dq 0
pm_wake_vbe_mode: dw 0
pm_wake_trampoline_end:
