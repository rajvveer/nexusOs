; NexusOS native x86-64 entry, entered by our existing BIOS stage 2 in ring 0.
MAX_CPUS equ 32                         ; keep in sync with native.h
TSS_SELECTOR equ 0x80                   ; CPU n uses TSS_SELECTOR + 16 * n (native.h)
CODE32_SELECTOR equ 0x08                ; flat 32-bit kernel code (Linux __KERNEL32_CS)
KERNEL_CS equ 0x10
KERNEL_DS equ 0x18
USER_DS equ 0x2B
USER_CS equ 0x33
TRAMPOLINE equ 0x7000                   ; AP real-mode entry page (SIPI vector 0x07)
KERNEL_VMA equ 0xFFFFFFFF80000000       ; linked at KERNEL_VMA + physical (linker.ld, native.h)
%define PA(x) ((x) - KERNEL_VMA)        ; physical address of a kernel symbol (32-bit boot code)
bits 32
section .entry
global start32
extern __bss_start, __bss_end, kernel64_main, trap64, __stack_chk_guard
global start64, nexus_kernel_header, boot_info_physical
start32:
    cli
    cld
    mov edi, PA(__bss_start)
    mov ecx, PA(__bss_end)
    sub ecx, edi
    xor eax, eax
    shr ecx, 2
    rep stosd
    mov esp, PA(kernel_stack_top)
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 1
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 3
    out dx, al

    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    xor eax, ecx
    test eax, 1 << 21
    jz unsupported
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb unsupported
    mov eax, 0x80000001
    cpuid
    and edx, (1 << 29) | (1 << 20) | (1 << 11)
    cmp edx, (1 << 29) | (1 << 20) | (1 << 11)
    jne unsupported

    ; boot_pdpt maps the low 4 GiB (2 MiB pages) twice: identity (PML4 0, dropped by protect_kernel)
    ; and the direct map at DIRECT_MAP_BASE (PML4 256); boot_kpdpt maps KERNEL_VMA onto the first GiB.
    mov eax, PA(boot_pdpt)
    or eax, 3
    mov [PA(boot_pml4)], eax
    mov [PA(boot_pml4) + 256 * 8], eax
    mov eax, PA(boot_kpdpt)
    or eax, 3
    mov [PA(boot_pml4) + 511 * 8], eax
    mov edi, PA(boot_pdpt)
    mov eax, PA(boot_pd)
    or eax, 3
    mov [PA(boot_kpdpt) + 510 * 8], eax
    mov ecx, 4
.pdpt:
    mov [edi], eax
    add edi, 8
    add eax, 4096
    loop .pdpt
    mov edi, PA(boot_pd)
    mov eax, 0x83
    mov ecx, 2048
.identity:
    mov [edi], eax
    add edi, 8
    add eax, 0x200000
    loop .identity
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax
    mov eax, PA(boot_pml4)
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11) | 1
    wrmsr
    lgdt [PA(gdt_pointer)]
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or eax, (1 << 31) | (1 << 16) | (1 << 1)
    mov cr0, eax
    jmp KERNEL_CS:PA(long_entry)

unsupported:
    mov esi, PA(unsupported_message)
.print:
    lodsb
    test al, al
    jz .stop
    mov ah, al
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    jmp .print
.stop:
    mov dx, 0xF4
    mov eax, 0x11
    out dx, eax
    hlt
    jmp .stop

bits 64
long_entry:                             ; still at its physical address: continue in the higher half
    mov rax, .high
    jmp rax
.high:
    lgdt [rel gdt_pointer64]
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, kernel_stack_top
    xor ebp, ebp
    ; Seed the stack-protector canary before the first C function runs. Always start from RDTSC mixed with the
    ; stack pointer (both valid on every CPU model, e.g. qemu64 which lacks RDRAND), then fold in RDRAND when
    ; CPUID says it exists. Low byte zeroed like glibc so a string overflow cannot forge it past a NUL.
    rdtsc
    shl rdx, 32
    or rax, rdx
    xor rax, rsp
    mov r8, rax                         ; r8 = base canary
    mov eax, 1
    cpuid                               ; ECX bit 30 = RDRAND
    test ecx, (1 << 30)
    jz .canary_have
    mov ecx, 10
.canary_try:
    rdrand rax
    jc .canary_mix
    dec ecx
    jnz .canary_try
    jmp .canary_have
.canary_mix:
    xor r8, rax
.canary_have:
    mov rbx, 0xFFFFFFFFFFFFFF00
    and r8, rbx
    mov [rel __stack_chk_guard], r8
    call kernel64_main
.halt:
    cli
    hlt
    jmp .halt

; UEFI entry. BOOTX64.EFI (uefi/bootx64.c) finds this header in kernel.bin, places the image at
; 0x100000 and jumps to start64 after ExitBootServices: long mode on the firmware's identity page
; tables, interrupts off, RDI = boot information (physical, below 4 GiB). start64 keeps nothing
; from the firmware: it loads our GDT, drops through compatibility mode to 32-bit protected mode
; with paging off, and joins start32, which builds the same page tables and long-mode state as a
; BIOS boot. (Leaving long mode also clears firmware CR4 features such as LA57/PCIDE/CET.)
align 8
nexus_kernel_header:                    ; struct nexus_kernel_header (boot_info.h)
    dq 0x34364C4E524B584E               ; "NXKRNL64"
    dd 1, 48                            ; version, header size
    dq 0x100000                         ; load address
    dq PA(start64)                      ; 64-bit entry (physical)
    dq PA(__bss_end)                    ; image + BSS end (physical)
    dq TRAMPOLINE                       ; AP start page
bits 64
start64:
    cli
    cld
    mov [rel boot_info_physical], edi
    lgdt [rel gdt_pointer]
    lea rax, [rel .compat]
    push qword CODE32_SELECTOR
    push rax
    retfq
bits 32
.compat:
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and eax, ~((1 << 31) | (1 << 16))   ; paging off (long mode becomes inactive), WP off
    mov cr0, eax
    mov ecx, 0xC0000080
    rdmsr
    and eax, ~(1 << 8)                  ; EFER.LME off; start32 enables it again
    wrmsr
    xor eax, eax
    mov cr4, eax
    jmp start32
bits 64

global syscall_entries, gdt64, isr_table, cpu_kstack_top
global switch_context, resume_user, user_iretq
global boot_pml4, boot_pdpt, boot_pd, kernel_stack_guard
global syscall_stack_top, ist_stack_top, user_result
global ap_trampoline, ap_trampoline_end, ap_gdtr, ap_cr3, ap_entry, ap_start64, ap_stacks
extern ap_main
global reset_user_fpu, user_read32, user_read32_fault, user_read32_fixup
extern xsave_mask, smap_enabled
; int user_read32(uint64_t address, uint32_t* out): a user load that may fault. trap64 turns a #PF
; at user_read32_fault into a return through user_read32_fixup (-EFAULT), so callers need no BKL.
user_read32:
    cmp byte [rel smap_enabled], 0      ; SMAP window (uaccess.h); STAC/CLAC are #UD without SMAP
    je user_read32_fault
    stac
user_read32_fault:
    mov eax, [rdi]
    mov [rsi], eax
    xor eax, eax
    jmp user_read32_close
user_read32_fixup:
    mov eax, -14
user_read32_close:
    cmp byte [rel smap_enabled], 0
    je .open
    clac
.open:
    ret
reset_user_fpu:
    fninit
%rep 8
    fldz
%endrep
    fninit
    ldmxcsr [rel initial_mxcsr]
%assign xr 0
%rep 16
    pxor xmm%+xr, xmm%+xr
%assign xr xr + 1
%endrep
    test byte [rel xsave_mask], 4
    jz .no_avx
    vzeroall
.no_avx:
    ret
; SYSCALL leaves RSP untouched: each CPU's LSTAR points at its own stub, which knows where that
; CPU's kernel stack (the running task's) and user-RSP scratch slot live. FMASK clears IF.
%assign c 0
%rep MAX_CPUS
syscall_entry_%+c:
    mov [rel cpu_user_rsp + c * 8], rsp
    mov rsp, [rel cpu_kstack_top + c * 8]
    push qword USER_DS
    push qword [rel cpu_user_rsp + c * 8]
    push r11
    push qword USER_CS
    push rcx
    push qword 0
    push qword 128
    jmp common_trap
%assign c c + 1
%endrep

switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

resume_user:
    cli
    cld
    mov rsi, rdi
    str ax                              ; this CPU's index from its TSS selector
    movzx eax, ax
    sub eax, TSS_SELECTOR
    shr eax, 4
    lea rdx, [rel cpu_kstack_top]
    mov rdi, [rdx + rax * 8]
    sub rdi, 176
    mov rsp, rdi
    mov ecx, 22
    rep movsq
    jmp restore_registers

%assign vector 0
%rep 256
isr%+vector:
%if vector != 8 && vector != 10 && vector != 11 && vector != 12 && vector != 13 && vector != 14 && vector != 17 && vector != 21 && vector != 29 && vector != 30
    push qword 0
%endif
%if vector == 0x80
    push qword 0x180                    ; int $0x80 (IA32_SYSCALL_VECTOR); SYSCALL pushes 128
%else
    push qword vector
%endif
    jmp common_trap
%assign vector vector + 1
%endrep

common_trap:
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
    cld
    mov rdi, rsp
    and rsp, -16
    call trap64
    mov rsp, rax
restore_registers:
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
    add rsp, 16
user_iretq:                             ; trap64 turns a fault here (stale user CS/SS) into a user fault
    iretq

ap_start64:
    lgdt [rel gdt_pointer64]            ; the trampoline loaded the GDT by its physical address
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov eax, 1
    lock xadd [rel ap_next_index], eax  ; 1, 2, ... in arrival order
    cmp eax, MAX_CPUS
    jae .park
    lea rdx, [rel ap_stacks]
    mov rsp, [rdx + rax * 8]
    test rsp, rsp
    jz .park
    xor ebp, ebp
    mov edi, eax
    call ap_main
.park:
    cli
    hlt
    jmp .park

section .rodata
; Application-processor trampoline, copied to TRAMPOLINE (below 1 MiB) before INIT-SIPI-SIPI.
; Real mode -> 32-bit protected mode -> long mode with the kernel's page tables, then ap_start64.
; The BSP fills ap_gdtr / ap_cr3 / ap_entry in the copy.
align 16
ap_trampoline:
bits 16
    cli
    cld
    xor ax, ax
    mov ds, ax
    o32 lgdt [TRAMPOLINE + (ap_gdtr - ap_trampoline)]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE32_SELECTOR:(TRAMPOLINE + (ap_protected - ap_trampoline))
bits 32
ap_protected:
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax
    mov eax, [TRAMPOLINE + (ap_cr3 - ap_trampoline)]
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11) | 1
    wrmsr
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or eax, (1 << 31) | (1 << 16) | (1 << 1)
    mov cr0, eax
    jmp KERNEL_CS:(TRAMPOLINE + (ap_long - ap_trampoline))
bits 64
ap_long:
    mov rax, [TRAMPOLINE + (ap_entry - ap_trampoline)]
    jmp rax
align 8
ap_gdtr: dw 0
    dd 0
align 8
ap_cr3: dd 0
align 8
ap_entry: dq 0
ap_trampoline_end:

align 4
initial_mxcsr: dd 0x1F80
unsupported_message: db '[64] UNSUPPORTED CPU: long mode, NX and SYSCALL required', 10, 0
align 8
isr_table:
%assign vector 0
%rep 256
    dq isr%+vector
%assign vector vector + 1
%endrep
global test_elf, test_elf_end
test_elf: incbin "user.elf"
test_elf_end:
global vdso_image, vdso_image_end
align 16
vdso_image: incbin "vdso.so"
vdso_image_end:

align 8
syscall_entries:
%assign c 0
%rep MAX_CPUS
    dq syscall_entry_%+c
%assign c c + 1
%endrep

section .data
align 8
cpu_kstack_top: dq syscall_stack_top   ; per CPU: kernel stack of the running task (TSS RSP0 twin)
    times MAX_CPUS - 1 dq 0
ap_stacks: times MAX_CPUS dq 0         ; per AP: initial (scheduler) stack, filled by the BSP
global ap_next_index                    ; smp_restart (after a system sleep) starts the count again
ap_next_index: dd 1
boot_info_physical: dd 0                ; set by start64 (UEFI); 0 = BIOS boot (0x9000/0x9100 blocks)
align 8
gdt64:                                  ; Linux x86-64 layout (native.h); each CPU runs on a copy
    dq 0, 0x00CF9B000000FFFF            ; 0x08 CODE32_SELECTOR: flat 32-bit kernel code (AP trampoline, UEFI entry)
    dq 0x00AF9B000000FFFF, 0x00CF93000000FFFF ; 0x10 kernel code64, 0x18 kernel data
    dq 0x00CFFB000000FFFF, 0x00CFF3000000FFFF ; 0x23 user code32, 0x2b user data
    dq 0x00AFFB000000FFFF               ; 0x33 user code64
    times 9 dq 0                        ; 0x38, 0x40, 0x50 LDT (16 bytes), 0x60-0x77 TLS, 0x78
    times MAX_CPUS * 2 dq 0             ; one 16-byte TSS descriptor per CPU from TSS_SELECTOR
gdt_end:
gdt_pointer:                            ; physical base: 32-bit boot code and start64 (paging off)
    dw gdt_end - gdt64 - 1
    dq PA(gdt64)
gdt_pointer64:
    dw gdt_end - gdt64 - 1
    dq gdt64

section .bss
align 4096
boot_pml4: resb 4096
boot_pdpt: resb 4096
boot_kpdpt: resb 4096
boot_pd: resb 4 * 4096
kernel_stack_guard: resb 4096
kernel_stack: resb 64 * 1024
kernel_stack_top:
syscall_stack: resb 64 * 1024
syscall_stack_top:
ist_stack: resb 16 * 1024
ist_stack_top:
cpu_user_rsp: resq MAX_CPUS
user_result: resd 1
