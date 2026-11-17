# NexusOS userland crt0 (Phase 57). Entry ABI (see kernel/usys_abi.h):
#   [esp] = argc, [esp+4] = argv (char**), [esp+8] = envp (char**)
    .text
    .globl _start
    .type _start, @function
_start:
    xorl %ebp, %ebp
    movl (%esp), %eax          # argc
    movl 4(%esp), %ecx         # argv
    movl 8(%esp), %edx         # envp
    andl $-16, %esp            # 16-byte align the stack for main
    subl $4, %esp
    pushl %edx
    pushl %ecx
    pushl %eax
    call __libc_start_main     # never returns
1:  hlt
    jmp 1b
    .size _start, .-_start
