; ============================================================================
; NexusOS Stage 1 Bootloader
; ============================================================================
; Loaded by BIOS at 0x7C00 (16-bit Real Mode)
; Job: Load Stage 2 bootloader from disk, then jump to it
; ============================================================================

[bits 16]
[org 0x7C00]

STAGE2_OFFSET equ 0x7E00       ; Where we load stage 2 (right after boot sector)
STAGE2_SECTORS equ 4           ; Number of sectors to read for stage 2

; ----------------------------------------------------------------------------
; Entry Point
; ----------------------------------------------------------------------------
start:
    ; Set up segments and stack
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; Stack grows downward from 0x7C00

    ; Save boot drive number (BIOS passes it in DL)
    mov [BOOT_DRIVE], dl

    ; Print boot message
    mov si, MSG_BOOT
    call print_string

    ; Load Stage 2 from disk
    call load_stage2

    ; Print success message
    mov si, MSG_LOADED
    call print_string

    ; Jump to Stage 2
    jmp STAGE2_OFFSET

; ----------------------------------------------------------------------------
; load_stage2: Read sectors from disk using BIOS int 0x13
; ----------------------------------------------------------------------------
load_stage2:
    mov bx, STAGE2_OFFSET       ; ES:BX = destination buffer
    mov al, STAGE2_SECTORS      ; Number of sectors to read
    mov ch, 0                   ; Cylinder 0
    mov cl, 2                   ; Start from sector 2 (sector 1 is boot sector)
    mov dh, 0                   ; Head 0
    mov dl, [BOOT_DRIVE]        ; Drive number

    mov ah, 0x02                ; BIOS read sectors function
    int 0x13                    ; Call BIOS disk interrupt

    jc disk_error               ; Jump if carry flag set (error)

    cmp al, STAGE2_SECTORS      ; Check all sectors were read
    jne disk_error

    ret

; ----------------------------------------------------------------------------
; disk_error: Print error and halt
; ----------------------------------------------------------------------------
disk_error:
    mov si, MSG_DISK_ERR
    call print_string
    jmp halt

; ----------------------------------------------------------------------------
; print_string: Print null-terminated string pointed to by SI
; ----------------------------------------------------------------------------
print_string:
    pusha
.loop:
    lodsb                       ; Load byte from SI into AL
    cmp al, 0
    je .done
    mov ah, 0x0E                ; BIOS teletype output
    mov bh, 0
    int 0x10                    ; Call BIOS video interrupt
    jmp .loop
.done:
    popa
    ret

; ----------------------------------------------------------------------------
; halt: Infinite loop
; ----------------------------------------------------------------------------
halt:
    cli
    hlt
    jmp halt

; ----------------------------------------------------------------------------
; Data
; ----------------------------------------------------------------------------
BOOT_DRIVE:     db 0
MSG_BOOT:       db '[NexusOS] Stage 1: Booting...', 13, 10, 0
MSG_LOADED:     db '[NexusOS] Stage 2 loaded!', 13, 10, 0
MSG_DISK_ERR:   db '[NexusOS] Disk read error!', 13, 10, 0

; ----------------------------------------------------------------------------
; Boot Sector Padding and Magic Number
; ----------------------------------------------------------------------------
times 510 - ($ - $$) db 0      ; Pad to 510 bytes
dw 0xAA55                       ; Boot signature
