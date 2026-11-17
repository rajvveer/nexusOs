; ============================================================================
; hello.exe — minimal PE32 i386 for NexusOS (Phase 54). ImageBase in user
; space, imports OutputDebugStringA + ExitProcess from KERNEL32. Compact 0x200
; alignment (our loader does not require 0x1000 alignment). nasm -f bin.
; ============================================================================
BITS 32
org 0x08000000
IMAGEBASE equ 0x08000000
filebase:
; ---- IMAGE_DOS_HEADER (64 bytes) ----
    dw 0x5A4D                       ; e_magic "MZ"
    times 0x3A db 0                 ; pad to e_lfanew at offset 0x3C
    dd ntheaders - filebase         ; e_lfanew = 0x40
; ---- IMAGE_NT_HEADERS32 ----
ntheaders:
    dd 0x00004550                   ; Signature "PE\0\0"
; IMAGE_FILE_HEADER (20 bytes)
    dw 0x014C                       ; Machine = i386
    dw 1                            ; NumberOfSections
    dd 0                            ; TimeDateStamp
    dd 0                            ; PointerToSymbolTable
    dd 0                            ; NumberOfSymbols
    dw opt_end - opt_start          ; SizeOfOptionalHeader (= 224)
    dw 0x0102                       ; Characteristics = EXECUTABLE_IMAGE | 32BIT
opt_start:
    dw 0x010B                       ; Magic = PE32
    db 0                            ; MajorLinkerVersion
    db 0                            ; MinorLinkerVersion
    dd 0x200                        ; SizeOfCode
    dd 0                            ; SizeOfInitializedData
    dd 0                            ; SizeOfUninitializedData
    dd _start - filebase            ; AddressOfEntryPoint (RVA == file off here)
    dd 0x200                        ; BaseOfCode
    dd 0x200                        ; BaseOfData
    dd IMAGEBASE                    ; ImageBase
    dd 0x200                        ; SectionAlignment (compact)
    dd 0x200                        ; FileAlignment
    dw 4                            ; MajorOperatingSystemVersion
    dw 0
    dw 0                            ; MajorImageVersion
    dw 0
    dw 4                            ; MajorSubsystemVersion
    dw 0
    dd 0                            ; Win32VersionValue
    dd SizeOfImage                  ; SizeOfImage
    dd 0x200                        ; SizeOfHeaders
    dd 0                            ; CheckSum
    dw 3                            ; Subsystem = CUI
    dw 0                            ; DllCharacteristics
    dd 0x100000                     ; SizeOfStackReserve
    dd 0x1000                       ; SizeOfStackCommit
    dd 0x100000                     ; SizeOfHeapReserve
    dd 0x1000                       ; SizeOfHeapCommit
    dd 0                            ; LoaderFlags
    dd 16                           ; NumberOfRvaAndSizes
    dd 0, 0                         ; [0] Export
    dd import_dir - filebase, import_dir_size   ; [1] Import
    times 14 dd 0, 0                ; [2..15]
opt_end:
; ---- Section header (one .text) ----
    db ".text", 0, 0, 0             ; Name[8]
    dd SizeOfImage - 0x200          ; Misc.VirtualSize
    dd 0x200                        ; VirtualAddress
    dd rawsize                      ; SizeOfRawData
    dd 0x200                        ; PointerToRawData
    dd 0, 0                         ; PointerToRelocations / Linenumbers
    dw 0, 0                         ; NumberOf...
    dd 0x60000020                   ; Characteristics = CODE|EXECUTE|READ
; ---- pad header area to 0x200 (section start) ----
    times 0x200 - ($ - filebase) db 0
; ============================================================================
; Section content (RVA 0x200 == file offset 0x200)
; ============================================================================
section_start:
iat:
    dd hint_ods  - filebase         ; -> OutputDebugStringA  (loader overwrites)
    dd hint_exit - filebase         ; -> ExitProcess
    dd 0
_start:
    push dword msg
    call [iat]                      ; OutputDebugStringA(msg)   (stdcall)
    push dword 0
    call [iat + 4]                  ; ExitProcess(0)            (never returns)
.hang:
    jmp .hang
msg:    db "Hello from PE32 (ring 3, Win32 shims)!", 0x0A, 0
import_dir:
    dd ilt - filebase               ; OriginalFirstThunk (ILT)
    dd 0                            ; TimeDateStamp
    dd 0                            ; ForwarderChain
    dd dllname - filebase           ; Name
    dd iat - filebase               ; FirstThunk (IAT)
    dd 0, 0, 0, 0, 0                ; null terminator descriptor
import_dir_size equ $ - import_dir
ilt:
    dd hint_ods  - filebase
    dd hint_exit - filebase
    dd 0
dllname:    db "KERNEL32.dll", 0
hint_ods:   dw 0
            db "OutputDebugStringA", 0
hint_exit:  dw 0
            db "ExitProcess", 0
    align 4
section_end:
rawsize     equ section_end - section_start
SizeOfImage equ 0x200 + ((rawsize + 0x1FF) & ~0x1FF)
