; ============================================================================
; NexusOS — kguard assembly helpers — Phase 55
; ============================================================================
; kguard_try / kguard_longjmp are a setjmp/longjmp pair for ring-0 recovery
; contexts, and kguard_df_entry is the body of the double-fault task.
; ============================================================================

[bits 32]
section .text

global kguard_try
global kguard_longjmp
global kguard_df_entry
extern kguard_push
extern kguard_df_handler

; kguard_ctx_t layout (kguard.h): ebx esi edi ebp esp eip  (6 x uint32_t)

; int kguard_try(kguard_ctx_t* ctx, const char* what)
; Saves the callee-saved registers, the caller's ESP (as it will be right after
; we return) and our return address, registers the context with kguard_push,
; and returns 0. kguard_longjmp later "returns" from here a second time with
; EAX = fault code.
kguard_try:
    mov eax, [esp + 4]          ; ctx
    mov [eax + 0], ebx
    mov [eax + 4], esi
    mov [eax + 8], edi
    mov [eax + 12], ebp
    lea ecx, [esp + 4]          ; caller's ESP once 'ret' has popped our return address
    mov [eax + 16], ecx
    mov ecx, [esp]              ; return address = resume point
    mov [eax + 20], ecx
    push dword [esp + 8]        ; what
    push dword [esp + 8]        ; ctx (shifted by the push above)
    call kguard_push
    add esp, 8
    xor eax, eax
    ret

; void kguard_longjmp(const kguard_ctx_t* ctx, int code)   -- never returns
; Restores the snapshot and resumes after the matching kguard_try call with
; EAX = code. Interrupts are re-enabled because the faulting exception frame
; (whose iret would have restored IF) is being discarded.
kguard_longjmp:
    mov edx, [esp + 4]          ; ctx
    mov eax, [esp + 8]          ; code
    mov ebx, [edx + 0]
    mov esi, [edx + 4]
    mov edi, [edx + 8]
    mov ebp, [edx + 12]
    mov esp, [edx + 16]
    mov ecx, [edx + 20]
    sti
    jmp ecx

; Double-fault task entry. The CPU reached this through the task gate in IDT
; entry 8, so we run on the #DF task's own stack with the error code (always 0)
; already pushed. kguard_df_handler() returns only when it has patched the
; interrupted task's TSS to resume inside a recovery context; the iret with
; NT=1 then switches back to that task. Looping to the entry keeps the #DF TSS
; re-entrant: the next task switch into it resumes at the jmp.
kguard_df_entry:
    call kguard_df_handler
    add esp, 4                  ; drop the error code
    iret
    jmp kguard_df_entry
