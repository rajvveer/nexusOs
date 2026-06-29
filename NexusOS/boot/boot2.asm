; ============================================================================
; NexusOS Stage 2 Bootloader — Phase 14 (high-load rework: Phase 42)
; ============================================================================
; Loaded at 0x7E00 by Stage 1
; Sets VESA 1024x768x32 mode, enables A20, then loads the kernel HIGH:
; BIOS-reads 32KB chunks into a bounce buffer at 0x10000 and copies each
; chunk to 0x100000+ during a brief protected-mode hop. This lifts the old
; 576KB kernel ceiling (real-mode load at 0x10000 ran into the VGA hole at
; 0xA0000 once the kernel outgrew it — Phase 42 did).
; ============================================================================

[bits 16]
[org 0x7E00]

KERNEL_OFFSET  equ 0x100000     ; kernel's final home (1MB; heap starts at 2MB)
KERNEL_SECTORS equ 1440         ; up to 720KB of kernel
CHUNK_SECTORS  equ 64           ; 32KB bounce buffer per protected-mode copy
BOUNCE_SEG     equ 0x1000       ; bounce buffer at linear 0x10000

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

    ; Enable A20 first — the kernel is copied above 1MB during loading
    call enable_a20
    mov si, MSG_A20
    call print_string_16

    ; Load kernel from disk (chunked high load)
    call load_kernel
    mov si, MSG_KERNEL_LOADED
    call print_string_16

    ; Switch to Protected Mode
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_mode_entry

; ============================================================================
; setup_vesa: Enumerate VBE modes, prefer 1920x1080x32 (Phase 53), fall back to
;   1024x768x32 (mode 0x118), then to a blind 0x4118 set. Stores FB info at
;   physical 0x9000 for the kernel:
;     0x9000 dd PhysBasePtr   0x9004 dw pitch   0x9006 dw width
;     0x9008 dw height        0x900A db active  0x900B db bpp
; ============================================================================
WANT_W   equ 1920
WANT_H   equ 1080

setup_vesa:
    pusha
    push es

    ; Clear the info area at 0x9000
    mov di, 0x9000
    xor ax, ax
    mov cx, 16
    rep stosw

    ; --- Step 0: VBE 0x4F00 get controller info (has the mode-list far ptr) ---
    ; ModeInfoBlock/VbeInfoBlock scratch at 0x0500 (free low mem; NOT 0x8000 —
    ; stage2 extends past 0x8000 and would corrupt the GDT).
    xor ax, ax
    mov es, ax
    mov di, 0x0600              ; VbeInfoBlock scratch at 0x600
    mov dword [es:di], 'VBE2'   ; request VBE 2.0 info
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .try_alt_mode

    ; VideoModePtr is a real-mode far ptr (off:seg) at VbeInfoBlock offset 14.
    mov ax, [es:0x0600 + 14]    ; offset
    mov [.mode_off], ax
    mov ax, [es:0x0600 + 16]    ; segment
    mov [.mode_seg], ax

.scan_loop:
    ; Load es:si = current mode-list pointer
    mov ax, [.mode_seg]
    mov es, ax
    mov si, [.mode_off]
    mov cx, [es:si]            ; mode number
    cmp cx, 0xFFFF             ; end-of-list terminator
    je  .try_118               ; no 1080p found -> fall back to 1024x768
    add word [.mode_off], 2    ; advance list pointer for next iteration

    ; Query this mode (0x4F01) into ModeInfoBlock at 0x0500
    push cx
    mov ax, 0x4F01
    mov di, 0x0500
    xor bx, bx
    mov es, bx                 ; es:di = 0000:0500
    int 0x10
    pop cx
    cmp ax, 0x004F
    jne .scan_loop

    ; Need a linear-framebuffer-capable mode (ModeAttributes bit 7 @ off 0)
    test byte [0x0500 + 0], 0x80
    jz  .scan_loop
    ; Match 1920x1080x32
    cmp word [0x0500 + 18], WANT_W
    jne .scan_loop
    cmp word [0x0500 + 20], WANT_H
    jne .scan_loop
    cmp byte [0x0500 + 25], 32
    jne .scan_loop

    ; Found it — record info and set the mode (cx still = mode number)
    mov eax, [0x0500 + 40]     ; PhysBasePtr
    mov [0x9000], eax
    mov ax, [0x0500 + 16]      ; pitch
    mov [0x9004], ax
    mov word [0x9006], WANT_W
    mov word [0x9008], WANT_H
    mov al, [0x0500 + 25]      ; bpp
    mov [0x900B], al

    mov ax, 0x4F02
    mov bx, cx
    or  bx, 0x4000             ; LFB bit
    int 0x10
    cmp ax, 0x004F
    jne .try_118               ; set failed -> fall back

    mov byte [0x900A], 1
    pop es
    popa
    ret

.try_118:
    ; --- Fallback: classic 1024x768x32 via mode 0x118 ---
    mov ax, 0x4F01
    mov cx, 0x0118
    mov di, 0x0500
    xor bx, bx
    mov es, bx
    int 0x10
    cmp ax, 0x004F
    jne .try_alt_mode

    cmp word [0x0500 + 18], 1024
    jne .try_alt_mode
    cmp word [0x0500 + 20], 768
    jne .try_alt_mode

    mov eax, [0x0500 + 40]
    mov [0x9000], eax
    mov ax, [0x0500 + 16]
    mov [0x9004], ax
    mov word [0x9006], 1024
    mov word [0x9008], 768
    mov al, [0x0500 + 25]
    mov [0x900B], al

    mov ax, 0x4F02
    mov bx, 0x4118
    int 0x10
    cmp ax, 0x004F
    jne .try_alt_mode

    mov byte [0x900A], 1
    pop es
    popa
    ret

.mode_off: dw 0
.mode_seg: dw 0

.try_alt_mode:
    pop es
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
; load_kernel — chunked high load
; Reads CHUNK_SECTORS at a time into the bounce buffer (real mode, BIOS),
; then hops into protected mode to rep-movsd the chunk up to [dest_phys].
; ============================================================================
load_kernel:
    mov word [cur_lba], 5
    mov word [sectors_left], KERNEL_SECTORS
    mov dword [dest_phys], KERNEL_OFFSET

.chunk_loop:
    cmp word [sectors_left], 0
    je .done

    ; chunk = min(CHUNK_SECTORS, sectors_left)
    mov ax, [sectors_left]
    cmp ax, CHUNK_SECTORS
    jbe .have_count
    mov ax, CHUNK_SECTORS
.have_count:
    mov [chunk_count], ax
    sub [sectors_left], ax
    mov word [dest_offset], 0x0000

.read_loop:
    cmp word [chunk_count], 0
    je .chunk_read_done
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
    mov ax, BOUNCE_SEG
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
    inc word [cur_lba]
    dec word [chunk_count]
    jmp .read_loop

.chunk_read_done:
    call pm_copy_chunk
    jmp .chunk_loop

.done:
    xor ax, ax
    mov es, ax
    ret
.disk_err:
    mov si, MSG_DISK_ERR
    call print_string_16
    jmp $

; ----------------------------------------------------------------------------
; pm_copy_chunk: copy [dest_offset] bytes from the bounce buffer (0x10000)
; to [dest_phys], advancing it. Brief PM hop; SS:SP untouched throughout.
; ----------------------------------------------------------------------------
pm_copy_chunk:
    cli
    push ds
    push es
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:.pm32

[bits 32]
.pm32:
    mov ax, 0x10                 ; flat 32-bit data
    mov ds, ax
    mov es, ax
    mov esi, BOUNCE_SEG * 16
    mov edi, [dest_phys]
    movzx ecx, word [dest_offset]
    add [dest_phys], ecx
    shr ecx, 2
    cld
    rep movsd
    jmp 0x18:.pm16               ; 16-bit protected code segment

[bits 16]
.pm16:
    mov ax, 0x20                 ; 16-bit data: restore 64KB-limit caches
    mov ds, ax
    mov es, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:.real
.real:
    xor ax, ax
    mov ds, ax
    pop es
    pop ds
    sti
    ret

cur_lba:       dw 0
sectors_left:  dw 0
chunk_count:   dw 0
dest_offset:   dw 0
dest_phys:     dd 0

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
gdt_null:   dq 0
gdt_code:   dw 0xFFFF, 0x0000              ; 0x08: 32-bit flat code
            db 0x00, 10011010b, 11001111b, 0x00
gdt_data:   dw 0xFFFF, 0x0000              ; 0x10: 32-bit flat data
            db 0x00, 10010010b, 11001111b, 0x00
gdt_code16: dw 0xFFFF, 0x0000              ; 0x18: 16-bit code (PM->real hop)
            db 0x00, 10011010b, 00000000b, 0x00
gdt_data16: dw 0xFFFF, 0x0000              ; 0x20: 16-bit data (PM->real hop)
            db 0x00, 10010010b, 00000000b, 0x00
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
