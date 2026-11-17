; ============================================================================
; spin.elf — loops printing its pid with sleeps, then exits (Phase 54).
; Run two in the background to show pre-emptive interleaving. nasm -f bin.
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
    dd 7                            ; R | W | X (self-modifies the pid digits)
    dd 0x1000
_start:
    mov esi, 24                     ; iteration count (~7s at 300ms/tick)
.loop:
    mov eax, 6                      ; SYS_GETPID
    int 0x80                        ; eax = pid
    xor edx, edx
    mov ecx, 10
    div ecx                         ; eax = pid/10, edx = pid%10
    add al, '0'
    add dl, '0'
    mov [pidtens],  al
    mov [pidunits], dl
    mov eax, 0                      ; SYS_WRITE
    mov ebx, msg
    mov ecx, msglen
    int 0x80
    mov eax, 22                     ; SYS_SLEEP
    mov ebx, 300                    ; 300 ms
    int 0x80
    dec esi
    jnz .loop
    mov eax, 5                      ; SYS_EXIT
    xor ebx, ebx
    int 0x80
.hang:
    jmp .hang
msg:        db "spin pid="
pidtens:    db "0"
pidunits:   db "0"
            db " tick", 0x0A
msglen      equ $ - msg
filesz      equ $ - __base
