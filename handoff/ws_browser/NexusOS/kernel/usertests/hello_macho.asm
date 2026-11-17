; ============================================================================
; hello.macho — minimal i386 Mach-O MH_EXECUTE for NexusOS (Phase 54).
; One __TEXT segment + LC_UNIXTHREAD entry; syscalls via int 0x80. nasm -f bin.
; ============================================================================
BITS 32
org 0x08048000
__base:
mach_header:
    dd 0xFEEDFACE                   ; magic (MH_MAGIC, 32-bit host order)
    dd 7                            ; cputype = CPU_TYPE_X86
    dd 3                            ; cpusubtype
    dd 2                            ; filetype = MH_EXECUTE
    dd 2                            ; ncmds
    dd cmds_end - cmds              ; sizeofcmds
    dd 0                            ; flags
cmds:
seg_text:
    dd 1                            ; cmd = LC_SEGMENT
    dd seg_text_end - seg_text      ; cmdsize (= 56)
    db "__TEXT", 0,0,0,0,0,0,0,0,0,0 ; segname[16]
    dd __base                       ; vmaddr
    dd filesz                       ; vmsize
    dd 0                            ; fileoff
    dd filesz                       ; filesize
    dd 7                            ; maxprot = rwx
    dd 5                            ; initprot = r-x
    dd 0                            ; nsects
    dd 0                            ; flags
seg_text_end:
thread:
    dd 5                            ; cmd = LC_UNIXTHREAD
    dd thread_end - thread          ; cmdsize (= 80)
    dd 1                            ; flavor = x86_THREAD_STATE32
    dd 16                           ; count
    dd 0,0,0,0,0,0,0,0,0,0          ; eax..eflags (regs 0-9)
    dd _start                       ; eip (reg 10)
    dd 0,0,0,0,0                    ; cs,ds,es,fs,gs (regs 11-15)
thread_end:
cmds_end:
_start:
    mov eax, 0                      ; SYS_WRITE
    mov ebx, msg
    mov ecx, msglen
    int 0x80
    mov eax, 5                      ; SYS_EXIT
    xor ebx, ebx
    int 0x80
.hang:
    jmp .hang
msg:    db "Hello from Mach-O (ring 3, i386)!", 0x0A
msglen  equ $ - msg
filesz  equ $ - __base
