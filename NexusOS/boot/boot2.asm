; ============================================================================
; NexusOS Stage 2 Bootloader — Phase 14
; ============================================================================
; Loaded at 0x7E00 by Stage 1
; Sets VESA 1024x768x32 mode, then loads kernel, enables A20, switches to PM
; ============================================================================

[bits 16]
[org 0x7E00]

KERNEL_OFFSET equ 0x10000
KERNEL_SECTORS equ 1200

; Floppy geometry
SECTORS_PER_TRACK equ 18
HEADS equ 2

; ============================================================================
; Entry Point
; ============================================================================
stage2_start:
    mov si, MSG_STAGE2
    call print_string_16

    ; --- SET VESA MODE BEFORE ANYTHING ELSE ---
    call setup_vesa

    ; Load kernel from disk
    call load_kernel
    mov si, MSG_KERNEL_LOADED
    call print_string_16

    ; Enable A20
    call enable_a20
    mov si, MSG_A20
    call print_string_16

    ; Switch to Protected Mode
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_mode_entry

; ============================================================================
; setup_vesa: Set 1024x768x32 using VBE BIOS calls
; Stores FB info at physical address 0x9000 for the kernel
; ============================================================================
setup_vesa:
    pusha

    ; Clear the info area at 0x9000
    mov di, 0x9000
    xor ax, ax
    mov cx, 16
    rep stosw

    ; Step 1: Get mode info for mode 0x118 (1024x768x32bpp)
    ; VBE function 0x4F01: Return VBE Mode Information
    ; ES:DI = pointer to 256-byte ModeInfoBlock
    mov ax, 0x4F01
    mov cx, 0x0118          ; Mode 0x118 = 1024x768x32
    mov di, 0x8000          ; Store mode info at 0x8000 (temporary)
    int 0x10
    cmp ax, 0x004F
    jne .try_alt_mode

    ; Verify resolution and bpp from mode info
    cmp word [0x8000 + 18], 1024    ; XResolution at offset 18
    jne .try_alt_mode
    cmp word [0x8000 + 20], 768     ; YResolution at offset 20
    jne .try_alt_mode

    ; Get framebuffer physical address from offset 40
    mov eax, [0x8000 + 40]         ; PhysBasePtr
    mov [0x9000], eax              ; Store FB address for kernel

    ; Store pitch (bytes per scanline) from offset 16
    mov ax, [0x8000 + 16]
    mov [0x9004], ax

    ; Store resolution
    mov word [0x9006], 1024
    mov word [0x9008], 768

    ; Store BPP from offset 25
    mov al, [0x8000 + 25]
    mov [0x900B], al

    ; Step 2: Set the mode
    ; VBE function 0x4F02: Set VBE Mode
    ; BX = mode number | 0x4000 (bit 14 = use LFB)
    mov ax, 0x4F02
    mov bx, 0x4118          ; Mode 0x118 + LFB bit
    int 0x10
    cmp ax, 0x004F
    jne .try_alt_mode

    ; Success! Set flag
    mov byte [0x900A], 1           ; VESA active flag
    popa
    ret

.try_alt_mode:
    ; Try alternative: mode 0x0105 might map differently
    ; Or just try setting 0x4118 directly without checking
    mov ax, 0x4F02
    mov bx, 0x4118
    int 0x10
    cmp ax, 0x004F
    jne .vesa_failed

    ; Set defaults even without proper mode query
    mov dword [0x9000], 0xFD000000  ; Default QEMU FB address
    mov word [0x9004], 4096         ; Default pitch
    mov word [0x9006], 1024
    mov word [0x9008], 768
    mov byte [0x900A], 1
    popa
    ret

.vesa_failed:
    mov si, MSG_VESA_FAIL
    call print_string_16
    mov byte [0x900A], 0           ; VESA not active
    popa
    ret

; ============================================================================
; load_kernel
; ============================================================================
load_kernel:
    mov word [cur_lba], 5
    mov word [sectors_left], KERNEL_SECTORS
    mov word [dest_offset], 0x0000
    mov word [dest_segment], 0x1000

.read_loop:
    cmp word [sectors_left], 0
    je .done
    mov ax, [cur_lba]
    xor dx, dx
    mov bx, SECTORS_PER_TRACK
    div bx
    inc dl
    mov cl, dl
    xor dx, dx
    mov bx, HEADS
    div bx
    mov ch, al
    mov dh, dl
    mov dl, 0x00
    mov ax, [dest_segment]
    mov es, ax
    mov bx, [dest_offset]
    mov ah, 0x02
    mov al, 1
    mov di, 3
.retry:
    pusha
    int 0x13
    jnc .read_ok
    popa
    dec di
    jz .disk_err
    xor ah, ah
    int 0x13
    jmp .retry
.read_ok:
    popa
    add word [dest_offset], 512
    cmp word [dest_offset], 0
    jne .no_seg_overflow
    mov ax, [dest_segment]
    add ax, 0x1000
    mov [dest_segment], ax
.no_seg_overflow:
    inc word [cur_lba]
    dec word [sectors_left]
    jmp .read_loop
.done:
    xor ax, ax
    mov es, ax
    ret
.disk_err:
    mov si, MSG_DISK_ERR
    call print_string_16
    jmp $

cur_lba:       dw 0
sectors_left:  dw 0
dest_offset:   dw 0
dest_segment:  dw 0

; ============================================================================
; enable_a20
; ============================================================================
enable_a20:
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE
    out 0x92, al
.done:
    ret

; ============================================================================
; print_string_16
; ============================================================================
print_string_16:
    pusha
.loop:
    lodsb
    cmp al, 0
    je .done
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .loop
.done:
    popa
    ret

; ============================================================================
; GDT
; ============================================================================
gdt_start:
gdt_null:  dq 0
gdt_code:  dw 0xFFFF, 0x0000
           db 0x00, 10011010b, 11001111b, 0x00
gdt_data:  dw 0xFFFF, 0x0000
           db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ============================================================================
; 32-bit Protected Mode Entry
; ============================================================================
[bits 32]
protected_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp KERNEL_OFFSET

; ============================================================================
; Messages
; ============================================================================
MSG_STAGE2:         db '[NexusOS] Stage 2 loading...', 13, 10, 0
MSG_KERNEL_LOADED:  db '[NexusOS] Kernel loaded!', 13, 10, 0
MSG_A20:            db '[NexusOS] A20 enabled!', 13, 10, 0
MSG_DISK_ERR:       db '[NexusOS] Disk error!', 13, 10, 0
MSG_VESA_FAIL:      db '[NexusOS] VESA fallback to text mode', 13, 10, 0

; Pad to 2048 bytes
times 2048 - ($ - $$) db 0
