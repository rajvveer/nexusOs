bits 64
section .text.start
global _start
extern user_main
_start:
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    and rsp, -16
    call user_main
    mov edi, eax
    mov eax, 60
    syscall
    ud2
