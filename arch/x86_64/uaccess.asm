; User-memory copy with an SMAP window and a fault fixup (uaccess.h, uaccess.c).
;
; uint64_t uaccess_copy(void* to, const void* from, uint64_t n)   SysV: rdi, rsi, rdx -> rax = bytes NOT copied
; One direction of it is always a user address the caller validated with accessible(). STAC opens the SMAP window
; (only when CR4.SMAP is on: STAC/CLAC are #UD without SMAP), REP MOVSB copies, CLAC closes it. If the copy faults
; (the user page went away after validation), native_dispatch finds uaccess_copy_insn in uaccess_ex_table and resumes
; at uaccess_copy_resume with AC cleared: REP MOVSB is restartable, so RCX is exactly the count left.
bits 64
default rel
extern smap_enabled
global uaccess_copy, uaccess_copy_insn, uaccess_copy_resume, uaccess_ex_table, uaccess_ex_table_end
section .text
uaccess_copy:
    mov rcx, rdx
    cmp byte [rel smap_enabled], 0
    je uaccess_copy_insn
    stac
uaccess_copy_insn:
    rep movsb
uaccess_copy_resume:
    cmp byte [rel smap_enabled], 0
    je .done
    clac
.done:
    mov rax, rcx
    ret

; uint64_t uaccess_clear(void* to, uint64_t n): zero user memory (rdi, rsi) -> rax = bytes NOT cleared
global uaccess_clear
uaccess_clear:
    mov rcx, rsi
    xor eax, eax
    cmp byte [rel smap_enabled], 0
    je uaccess_clear_insn
    stac
uaccess_clear_insn:
    rep stosb
uaccess_clear_resume:
    cmp byte [rel smap_enabled], 0
    je .done
    clac
.done:
    mov rax, rcx
    ret

section .rodata
align 8
uaccess_ex_table:                       ; (faulting instruction, resume address) pairs
    dq uaccess_copy_insn, uaccess_copy_resume
    dq uaccess_clear_insn, uaccess_clear_resume
uaccess_ex_table_end:
