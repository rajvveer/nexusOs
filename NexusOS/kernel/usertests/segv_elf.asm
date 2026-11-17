; ============================================================================
; segv.elf — reads a kernel address from ring 3; the fault must kill ONLY this
; process while the OS keeps running (Phase 54). nasm -f bin.
; ============================================================================
BITS 32
org 0x08048000
__base:
    db 0x7F, "ELF", 1, 1, 1, 0
    times 8 db 0
    dw 2
    dw 3
    dd 1
    dd _start
    dd phdr - __base
    dd 0
    dd 0
    dw 52
    dw 32
    dw 1
    dw 0
    dw 0
    dw 0
phdr:
    dd 1
    dd 0
    dd __base
    dd __base
    dd filesz
    dd filesz
    dd 5
    dd 0x1000
_start:
    mov eax, 0                      ; SYS_WRITE
    mov ebx, msg
    mov ecx, msglen
    int 0x80
    mov eax, [0x00100000]           ; read kernel memory -> #PF (supervisor page)
    mov eax, 5                      ; (never reached) SYS_EXIT
    xor ebx, ebx
    int 0x80
.hang:
    jmp .hang
msg:    db "segv: reading kernel address 0x00100000 from ring 3...", 0x0A
msglen  equ $ - msg
filesz  equ $ - __base
