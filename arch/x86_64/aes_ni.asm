; AES-NI kernels for dm-crypt (aes.c): XTS (IEEE 1619) and CBC, eight blocks in flight where the mode allows it
; (XTS both ways, CBC decryption), one block for CBC encryption and single-block ECB (tweaks, ESSIV IVs).
;
; The kernel is built without SSE: XMM registers hold the interrupted task's user state, which the scheduler saves only
; at task switches (XSAVE in task_yield). Kernel code is never preempted asynchronously and no interrupt handler uses
; SSE, so each routine saves the XMM registers it clobbers (movdqu to its own stack frame) on entry and restores them
; before it returns: the task's state is intact, on any CPU, whatever ran before or after. Only legacy-SSE encodings
; are used, which leave the upper halves of YMM/ZMM registers alone; MXCSR and x87 state are not touched by AES-NI or
; these integer-SSE instructions. Key material left in XMM registers is overwritten by the restore, the XTS tweaks
; kept on the stack frame are wiped.
;
; Round keys: `rounds`+1 16-byte keys; for decryption the equivalent-inverse schedule in use order (aes.c).
bits 64
default rel
section .rodata
align 16
gf128mul_mask: dq 0x87, 1

section .text
global aesni_ecb1
global aesni_xts8
global aesni_cbc_encrypt
global aesni_cbc_decrypt8

FRAME equ 128 + 256 + 8              ; scratch (8 tweaks), 16 saved registers, alignment

%macro SAVE_XMM 0
    sub rsp, FRAME
%assign i 0
%rep 16
    movdqu [rsp + 128 + 16*i], xmm %+ i
%assign i i+1
%endrep
%endmacro
%macro RESTORE_XMM 0
%assign i 0
%rep 16
    movdqu xmm %+ i, [rsp + 128 + 16*i]
%assign i i+1
%endrep
    add rsp, FRAME
%endmacro
%macro WIPE_SCRATCH 0
    pxor xmm0, xmm0
%assign i 0
%rep 8
    movdqu [rsp + 16*i], xmm0
%assign i i+1
%endrep
%endmacro

; T = T * alpha in GF(2^128), little-endian (xmm8 = T, xmm15 = mask, xmm10 scratch)
%macro GFDOUBLE 0
    movdqa xmm10, xmm8
    psrad xmm10, 31
    paddq xmm8, xmm8
    pshufd xmm10, xmm10, 0x13
    pand xmm10, xmm15
    pxor xmm8, xmm10
%endmacro

; eight-block rounds on xmm0..xmm7 with the schedule at rdi and `rounds` in esi; %1 = aesenc|aesdec, %2 = ..last
%macro ROUNDS8 2
    movdqu xmm9, [rdi]
%assign i 0
%rep 8
    pxor xmm %+ i, xmm9
%assign i i+1
%endrep
    lea r11, [rdi + 16]
    mov eax, esi
    dec eax
%%round:
    movdqu xmm9, [r11]
%assign i 0
%rep 8
    %1 xmm %+ i, xmm9
%assign i i+1
%endrep
    add r11, 16
    dec eax
    jnz %%round
    movdqu xmm9, [r11]
%assign i 0
%rep 8
    %2 xmm %+ i, xmm9
%assign i i+1
%endrep
%endmacro
; one-block rounds on xmm0
%macro ROUNDS1 2
    movdqu xmm9, [rdi]
    pxor xmm0, xmm9
    lea r11, [rdi + 16]
    mov eax, esi
    dec eax
%%round:
    movdqu xmm9, [r11]
    %1 xmm0, xmm9
    add r11, 16
    dec eax
    jnz %%round
    movdqu xmm9, [r11]
    %2 xmm0, xmm9
%endmacro

; void aesni_ecb1(const uint8_t* keys, unsigned rounds, uint8_t* block, int decrypt)
aesni_ecb1:
    SAVE_XMM
    movdqu xmm0, [rdx]
    test ecx, ecx
    jnz .decrypt
    ROUNDS1 aesenc, aesenclast
    jmp .out
.decrypt:
    ROUNDS1 aesdec, aesdeclast
.out:
    movdqu [rdx], xmm0
    RESTORE_XMM
    ret

; void aesni_xts8(const uint8_t* keys, unsigned rounds, uint8_t* data, uint64_t blocks, uint8_t* tweak, int decrypt)
; tweak: the encrypted IV on entry, T * alpha^blocks on return (IEEE 1619 chains it through a data unit).
aesni_xts8:
    SAVE_XMM
    movdqu xmm8, [r8]
    movdqa xmm15, [gf128mul_mask]
.eight:
    cmp rcx, 8
    jb .tail
%assign i 0
%rep 8
    movdqu [rsp + 16*i], xmm8
    movdqu xmm %+ i, [rdx + 16*i]
    pxor xmm %+ i, xmm8
    GFDOUBLE
%assign i i+1
%endrep
    test r9d, r9d
    jnz .dec8
    ROUNDS8 aesenc, aesenclast
    jmp .out8
.dec8:
    ROUNDS8 aesdec, aesdeclast
.out8:
%assign i 0
%rep 8
    movdqu xmm9, [rsp + 16*i]
    pxor xmm %+ i, xmm9
    movdqu [rdx + 16*i], xmm %+ i
%assign i i+1
%endrep
    add rdx, 128
    sub rcx, 8
    jmp .eight
.tail:
    test rcx, rcx
    jz .done
    movdqu xmm0, [rdx]
    pxor xmm0, xmm8
    test r9d, r9d
    jnz .dec1
    ROUNDS1 aesenc, aesenclast
    jmp .out1
.dec1:
    ROUNDS1 aesdec, aesdeclast
.out1:
    pxor xmm0, xmm8
    movdqu [rdx], xmm0
    GFDOUBLE
    add rdx, 16
    dec rcx
    jmp .tail
.done:
    movdqu [r8], xmm8
    WIPE_SCRATCH
    RESTORE_XMM
    ret

; void aesni_cbc_encrypt(const uint8_t* keys, unsigned rounds, uint8_t* data, uint64_t blocks, uint8_t* iv)
; iv: in, and the last ciphertext block (the chaining value) on return
aesni_cbc_encrypt:
    SAVE_XMM
    movdqu xmm1, [r8]
.block:
    test rcx, rcx
    jz .done
    movdqu xmm0, [rdx]
    pxor xmm0, xmm1
    ROUNDS1 aesenc, aesenclast
    movdqu [rdx], xmm0
    movdqa xmm1, xmm0
    add rdx, 16
    dec rcx
    jmp .block
.done:
    movdqu [r8], xmm1
    RESTORE_XMM
    ret

; void aesni_cbc_decrypt8(const uint8_t* dec_keys, unsigned rounds, uint8_t* data, uint64_t blocks, uint8_t* iv)
aesni_cbc_decrypt8:
    SAVE_XMM
    movdqu xmm8, [r8]                 ; the previous ciphertext block
.eight:
    cmp rcx, 8
    jb .tail
%assign i 0
%rep 8
    movdqu xmm %+ i, [rdx + 16*i]
%assign i i+1
%endrep
    ROUNDS8 aesdec, aesdeclast
    pxor xmm0, xmm8                   ; P[i] = D(C[i]) ^ C[i-1]: every C read before any P is stored
%assign i 1
%rep 7
    movdqu xmm9, [rdx + 16*(i-1)]
    pxor xmm %+ i, xmm9
%assign i i+1
%endrep
    movdqu xmm8, [rdx + 112]
%assign i 0
%rep 8
    movdqu [rdx + 16*i], xmm %+ i
%assign i i+1
%endrep
    add rdx, 128
    sub rcx, 8
    jmp .eight
.tail:
    test rcx, rcx
    jz .done
    movdqu xmm0, [rdx]
    movdqa xmm10, xmm0
    ROUNDS1 aesdec, aesdeclast
    pxor xmm0, xmm8
    movdqu [rdx], xmm0
    movdqa xmm8, xmm10
    add rdx, 16
    dec rcx
    jmp .tail
.done:
    movdqu [r8], xmm8
    RESTORE_XMM
    ret
