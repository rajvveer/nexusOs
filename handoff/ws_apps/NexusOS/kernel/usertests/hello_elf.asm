; ============================================================================
; hello.elf — minimal static ET_EXEC i386 ELF for NexusOS (Phase 54)
; Prints via SYS_WRITE(0) and exits via SYS_EXIT(5). nasm -f bin.
; ============================================================================
BITS 32
org 0x08048000
__base:
ehdr:
    db 0x7F, "ELF", 1, 1, 1, 0      ; magic, ELFCLASS32, little-endian, ver, osabi
    times 8 db 0                    ; e_ident padding
    dw 2                            ; e_type = ET_EXEC
    dw 3                            ; e_machine = EM_386
    dd 1                            ; e_version
    dd _start                       ; e_entry (absolute, org set)
    dd phdr - __base                ; e_phoff
    dd 0                            ; e_shoff
    dd 0                            ; e_flags
    dw 52                           ; e_ehsize
    dw 32                           ; e_phentsize
    dw 1                            ; e_phnum
    dw 0                            ; e_shentsize
    dw 0                            ; e_shnum
    dw 0                            ; e_shstrndx
phdr:
    dd 1                            ; p_type = PT_LOAD
    dd 0                            ; p_offset
    dd __base                       ; p_vaddr
    dd __base                       ; p_paddr
    dd filesz                       ; p_filesz
    dd filesz                       ; p_memsz
    dd 5                            ; p_flags = R | X
    dd 0x1000                       ; p_align
_start:
    mov eax, 0                      ; SYS_WRITE
    mov ebx, msg
    mov ecx, msglen
    int 0x80
    mov eax, 5                      ; SYS_EXIT
    xor ebx, ebx                    ; code 0
    int 0x80
.hang:
    jmp .hang
msg:    db "Hello from ELF (ring 3, isolated address space)!", 0x0A
msglen  equ $ - msg
filesz  equ $ - __base
