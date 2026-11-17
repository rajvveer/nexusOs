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

KERNEL_OFFSET  equ 0x100000     ; kernel's final home (1MB; Phase 57: heap is at 4MB)
%ifdef NEXUS_X86_64
KERNEL_SECTORS equ 5755         ; x86-64: 2.88MB floppy (5760 sectors, 80 x 2 x 36): LBA 5..5759, up to
                                ; 2946560 bytes landing at 0x100000-0x3D0000 (linker.ld keeps image + BSS
                                ; under 4MB); build.py writes the kernel's real count into kernel_sectors.
%else
KERNEL_SECTORS equ 2816         ; up to 1408KB (1441792 bytes) of kernel: LBA 5..2820
                                ; of a 2880-sector floppy (boot + stage 2 use LBA 0..4),
                                ; lands at 0x100000-0x260000, below the 3MB stack guard.
                                ; Phase 57 raised it from 1984 (1015808 bytes) once the
                                ; heap left 2MB. cur_lba/sectors_left are 16-bit (fine:
                                ; max LBA 2820 -> cylinder 78 of 80, head 0/1, spt 18).
%endif
CHUNK_SECTORS  equ 64           ; 32KB bounce buffer per protected-mode copy
BOUNCE_SEG     equ 0x1000       ; bounce buffer at linear 0x10000

; ============================================================================
; Entry Point
; ============================================================================
stage2_start:
    ; Stage 1 jumps here with the BIOS boot drive still in DL (0 for a floppy
    ; or El Torito floppy emulation). Keep it for every kernel read.
    mov [boot_drive], dl
    test dl, 0x80
    jz .geom_done               ; floppy: keep the 1.44MB 18x2 defaults
%ifdef NEXUS_X86_64
    mov ah, 0x41                ; hard disk / USB stick: INT 13h extensions (LBA reads)?
    mov bx, 0x55AA
    int 0x13
    jc .chs
    cmp bx, 0xAA55
    jne .chs
    test cl, 1
    jz .chs
    mov byte [use_lba], 1
.chs:
    mov dl, [boot_drive]
%endif
    mov ah, 0x08                ; hard disk: ask the BIOS for its CHS geometry
    int 0x13
    jc .geom_done
    and cx, 0x3F                ; CL[5:0] = sectors per track
    mov [spt], cx
    movzx ax, dh                ; DH = last head index
    inc ax
    mov [heads], ax
.geom_done:
    mov si, MSG_STAGE2
    call print_string_16
%ifdef NEXUS_X86_64
    call check_diag_key
%endif

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

    ; Switch to Protected Mode. Mask both 8259s first: they still deliver at
    ; the BIOS vectors (IRQ0-7 -> INT 8-15), so a timer tick taken before the
    ; kernel's pic_init remap would arrive as a bogus #DF (vector 8).
    ; pic_init unmasks after remapping and kernel_main does the first sti.
    cli
    mov al, 0xFF
    out 0xA1, al
    out 0x21, al
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
; x86-64 build (NEXUS_X86_64): first the monitor's native mode from its EDID
;   (VBE/DDC 0x4F15, kept at 0x9800 with 'EDID' at 0x97F8; a virtual
;   machine's EDID - QEMU RHT, VMware VMW - is kept but not used), then
;   1920x1080; the pitch is LinBytesPerScanLine on VBE 3.0.
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
%ifdef NEXUS_X86_64
    mov ax, [.mode_off]
    mov [.list_off], ax
    call read_edid
%endif

.scan_loop:
    ; Load es:si = current mode-list pointer
    mov ax, [.mode_seg]
    mov es, ax
    mov si, [.mode_off]
    mov cx, [es:si]            ; mode number
    cmp cx, 0xFFFF             ; end-of-list terminator
%ifdef NEXUS_X86_64
    je  .list_end
%else
    je  .try_118               ; no 1080p found -> fall back to 1024x768
%endif
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
%ifdef NEXUS_X86_64
    mov ax, [.want_w]
    cmp word [0x0500 + 18], ax
    jne .scan_loop
    mov ax, [.want_h]
    cmp word [0x0500 + 20], ax
    jne .scan_loop
%else
    cmp word [0x0500 + 18], WANT_W
    jne .scan_loop
    cmp word [0x0500 + 20], WANT_H
    jne .scan_loop
%endif
    cmp byte [0x0500 + 25], 32
    jne .scan_loop

    ; Found it — record info and set the mode (cx still = mode number)
    mov eax, [0x0500 + 40]     ; PhysBasePtr
    mov [0x9000], eax
    mov ax, [0x0500 + 16]      ; pitch
%ifdef NEXUS_X86_64
    cmp word [0x0600 + 4], 0x0300
    jb  .pitch_done
    mov ax, [0x0500 + 50]      ; VBE 3.0 LinBytesPerScanLine (the LFB pitch)
.pitch_done:
    mov [0x9004], ax
    mov ax, [0x0500 + 18]
    mov [0x9006], ax
    mov ax, [0x0500 + 20]
    mov [0x9008], ax
%else
    mov [0x9004], ax
    mov word [0x9006], WANT_W
    mov word [0x9008], WANT_H
%endif
    mov al, [0x0500 + 25]      ; bpp
    mov [0x900B], al

    mov ax, 0x4F02
    mov bx, cx
    or  bx, 0x4000             ; LFB bit
    int 0x10
    cmp ax, 0x004F
    jne .try_118               ; set failed -> fall back

    mov byte [0x900A], 1
    call .save_format
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
    call .save_format
    pop es
    popa
    ret

.mode_off: dw 0
.mode_seg: dw 0
%ifdef NEXUS_X86_64
.list_off: dw 0
.want_w: dw WANT_W
.want_h: dw WANT_H

    ; End of the mode list: the EDID mode was not offered -> scan again for 1920x1080.
.list_end:
    cmp word [.want_w], WANT_W
    jne .retry_default
    cmp word [.want_h], WANT_H
    je  .try_118
.retry_default:
    mov word [.want_w], WANT_W
    mov word [.want_h], WANT_H
    mov ax, [.list_off]
    mov [.mode_off], ax
    jmp .scan_loop
%endif

.save_format:
    ; Preserve the queried RGB masks; native user mappings must not guess them.
    mov eax, [0x0500 + 31]
    mov [0x900C], eax
    mov eax, [0x0500 + 35]
    mov [0x9010], eax
    mov al, [0x0500 + 27]
    mov [0x9014], al
    ret

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

%ifdef NEXUS_X86_64
; check_diag_key: D (or d) waiting in the BIOS keyboard buffer -> 'DIAG' at 0x97F0 (boot_info.c adds the
;   `diag` option). The flag and the EDID signature are cleared first: RAM survives a warm reboot.
check_diag_key:
    pusha
    push es
    mov dword [0x97F0], 0
    mov dword [0x97F8], 0
    xor ax, ax
    mov es, ax
    mov bx, [es:0x046C]        ; BIOS tick count (18.2 Hz): look for ~0.5 s
.poll:
    mov ah, 0x01
    int 0x16
    jnz .key
    mov ax, [es:0x046C]
    sub ax, bx
    cmp ax, 9
    jb .poll
    jmp .no_key
.key:
    xor ah, ah                 ; consume it
    int 0x16
    or al, 0x20
    cmp al, 'd'
    jne .poll                  ; other keys are dropped while looking
    mov dword [0x97F0], 'DIAG'
    mov si, MSG_DIAG
    call print_string_16
.no_key:
    pop es
    popa
    ret
MSG_DIAG: db '[NexusOS] D held: diagnostics boot', 13, 10, 0

; read_edid: VBE/DDC EDID block 0 into 0x9800; a valid one that is not a VM's
;   sets .want_w/.want_h to its preferred (first detailed) timing.
read_edid:
    pusha
    push es
    xor ax, ax
    mov es, ax
    mov ax, 0x4F15
    xor bx, bx                 ; BL=0: DDC capabilities
    xor cx, cx
    xor di, di
    int 0x10
    cmp ax, 0x004F
    jne .edid_done
    mov ax, 0x4F15
    mov bx, 1                  ; BL=1: read EDID, CX=controller 0, DX=block 0
    xor cx, cx
    xor dx, dx
    mov di, 0x9800
    int 0x10
    cmp ax, 0x004F
    jne .edid_done
    cmp dword [0x9800], 0xFFFFFF00
    jne .edid_done
    cmp dword [0x9804], 0x00FFFFFF
    jne .edid_done
    mov si, 0x9800
    mov cx, 128
    xor al, al
.edid_sum:
    add al, [si]
    inc si
    loop .edid_sum
    test al, al
    jnz .edid_done
    mov dword [0x97F8], 'EDID'
    cmp byte [vm_edid_ok], 0   ; tests patch the byte after 'NXVE' to use a VM's EDID too
    jne .edid_timing
    cmp word [0x9808], 0x1449  ; "RHT" (QEMU)
    je  .edid_done
    cmp word [0x9808], 0xB759  ; "VMW" (VMware)
    je  .edid_done
.edid_timing:
    cmp word [0x9800 + 54], 0  ; first descriptor is a timing (pixel clock)
    je  .edid_done
    movzx ax, byte [0x9800 + 58]
    and al, 0xF0
    shl ax, 4
    or  al, [0x9800 + 56]
    cmp ax, 640
    jb  .edid_done
    mov bx, ax
    movzx ax, byte [0x9800 + 61]
    and al, 0xF0
    shl ax, 4
    or  al, [0x9800 + 59]
    cmp ax, 480
    jb  .edid_done
    mov [setup_vesa.want_h], ax
    mov [setup_vesa.want_w], bx
.edid_done:
    pop es
    popa
    ret
    db 'NXVE'
vm_edid_ok: db 0
%endif

; ============================================================================
; load_kernel — chunked high load
; Reads CHUNK_SECTORS at a time into the bounce buffer (real mode, BIOS),
; then hops into protected mode to rep-movsd the chunk up to [dest_phys].
; ============================================================================
load_kernel:
%ifdef NEXUS_X86_64
    mov eax, [kernel_lba]       ; 5 on the floppy; usb-image patches it
    mov [cur_lba], eax
    mov ax, [kernel_sectors]
    mov [sectors_left], ax
%else
    mov word [cur_lba], 5
    mov word [sectors_left], KERNEL_SECTORS
%endif
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
%ifdef NEXUS_X86_64
    cmp byte [use_lba], 0
    je .chs_read
    mov ax, [dest_offset]       ; INT 13h AH=42h: one sector to BOUNCE_SEG:dest_offset
    mov [dap_offset], ax
    mov eax, [cur_lba]
    mov [dap_lba], eax
    mov di, 3
.lba_retry:
    pusha
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jnc .read_ok
    popa
    dec di
    jz .disk_err
    pusha
    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    popa
    jmp .lba_retry
.chs_read:
%endif
    mov ax, [cur_lba]
    xor dx, dx
    div word [spt]
    inc dl
    mov cl, dl
    xor dx, dx
    div word [heads]
    mov ch, al
    mov dh, dl
    mov dl, [boot_drive]
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
%ifdef NEXUS_X86_64
    inc dword [cur_lba]
%else
    inc word [cur_lba]
%endif
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

boot_drive:    db 0
%ifdef NEXUS_X86_64
spt:           dw 36            ; CHS geometry (2.88MB floppy defaults)
%else
spt:           dw 18            ; CHS geometry (1.44MB floppy defaults)
%endif
heads:         dw 2
%ifdef NEXUS_X86_64
cur_lba:       dd 0
use_lba:       db 0
dap:           db 16, 0         ; INT 13h AH=42h disk address packet
               dw 1
dap_offset:    dw 0
               dw BOUNCE_SEG
dap_lba:       dd 0, 0
%else
cur_lba:       dw 0
%endif
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

%ifdef NEXUS_X86_64
; Fixed fields at the end of stage 2 (build.py patches kernel_sectors; usb-image patches its copy):
;   0x700 kernel command line: "NXCMDLINE=" + text + NUL (read by the 64-bit kernel, boot_info.c)
;   0x7F0 kernel_lba: first sector of kernel.bin on the boot disk
;   0x7F4 kernel_sectors: the kernel's size in sectors (build.py)
times 0x700 - ($ - $$) db 0
boot_cmdline:   db 0
times 0x7F0 - ($ - $$) db 0
kernel_lba:     dd 5
kernel_sectors: dw KERNEL_SECTORS
%endif
; Pad to 2048 bytes
times 2048 - ($ - $$) db 0
