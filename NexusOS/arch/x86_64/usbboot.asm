; ============================================================================
; NexusOS disk boot sector (GPT protective MBR code), for USB sticks and hard disks
; ============================================================================
; build.py usb-image writes this into LBA 0 of a GPT disk (the partition table and
; the 0x55AA signature are filled in by the image builder; only bytes 0..439 are code).
; The BIOS loads it at 0x7C00 with the boot drive in DL. It stores the E820 map for the
; 64-bit kernel (as boot/boot.asm does), then reads stage 2 (boot/boot2.asm, 4 sectors)
; from the BIOS boot partition with INT 13h extensions (LBA, no CHS limits) and jumps to
; it at 0x7E00 with DL unchanged. stage2_lba (in the disk address packet below) is
; patched by the builder: the first LBA of the BIOS boot partition.
; ============================================================================
[bits 16]
[org 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive], dl
    call memory_map
    mov ah, 0x41                ; INT 13h extensions present?
    mov bx, 0x55AA
    mov dl, [drive]
    int 0x13
    jc fail
    cmp bx, 0xAA55
    jne fail
    test cl, 1                  ; packet (AH=42h) interface
    jz fail
    mov di, 3
.read:
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jnc .loaded
    xor ah, ah                  ; reset and retry
    mov dl, [drive]
    int 0x13
    dec di
    jnz .read
    jmp fail
.loaded:
    mov si, msg_ok
    call print
    mov dl, [drive]
    jmp 0x0000:0x7E00

fail:
    mov si, msg_fail
    call print
.halt:
    cli
    hlt
    jmp .halt

print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    jmp print
.done:
    ret

; E820 map for the 64-bit kernel: count at 0x9100, 'NX64' at 0x9104, 24-byte entries from 0x9108.
memory_map:
    pusha
    mov word [0x9100], 0
    mov dword [0x9104], 'NX64'
    mov di, 0x9108
    xor ebx, ebx
.next:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [es:di + 20], 1
    push di
    int 0x15
    pop di
    jc .done
    cmp eax, 0x534D4150
    jne .invalid
    cmp ecx, 20
    jb .invalid
    inc word [0x9100]
    add di, 24
    test ebx, ebx
    jz .done
    cmp word [0x9100], 32
    jb .next
.invalid:
    mov word [0x9100], 0
.done:
    popa
    ret

drive:    db 0x80
msg_ok:   db 'NexusOS: stage 2', 13, 10, 0
msg_fail: db 'NexusOS: disk read error (no INT 13h extensions?)', 13, 10, 0

times 0x1A0 - ($ - $$) db 0
dap:                            ; 0x1A0: INT 13h AH=42h disk address packet
    db 16, 0
    dw 4                        ; sectors: stage 2
    dw 0x7E00, 0x0000           ; buffer 0000:7E00
stage2_lba:
    dq 0                        ; 0x1A8: patched by the image builder

times 440 - ($ - $$) db 0       ; disk signature, partition table and 0x55AA come from the builder
