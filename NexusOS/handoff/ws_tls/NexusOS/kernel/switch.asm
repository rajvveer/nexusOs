; ============================================================================
; NexusOS — Context Switch
; ============================================================================
; void context_switch(uint32_t* old_esp, uint32_t new_esp)
;
; Saves callee-saved registers on old stack, saves ESP,
; loads new ESP, restores registers, and returns to new EIP.
; ============================================================================

[bits 32]

section .text
global context_switch

; Arguments:
;   [esp+4] = pointer to old process ESP (where to save current ESP)
;   [esp+8] = new process ESP (to load)
context_switch:
    ; Save callee-saved registers on current stack
    push ebp
    push ebx
    push esi
    push edi

    ; Save current ESP into *old_esp
    mov eax, [esp + 20]         ; old_esp pointer (arg1, +16 for pushes +4 for ret addr)
    mov [eax], esp              ; *old_esp = current ESP

    ; Load new ESP
    mov esp, [esp + 24]         ; new_esp (arg2)

    ; Restore callee-saved registers from new stack
    pop edi
    pop esi
    pop ebx
    pop ebp

    ; Return to new process (EIP on top of new stack)
    ret
