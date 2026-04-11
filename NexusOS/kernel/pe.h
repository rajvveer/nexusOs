/* ============================================================================
 * NexusOS — PE32 Binary Loader (Header) — Phase 34
 * ============================================================================
 * Defines PE32 (Portable Executable) structures and loading API.
 * ============================================================================ */

#ifndef PE_H
#define PE_H

#include "types.h"

/* DOS MZ Magic */
#define IMAGE_DOS_SIGNATURE 0x5A4D  /* "MZ" */

/* PE Signature */
#define IMAGE_NT_SIGNATURE  0x00004550  /* "PE\0\0" */

/* Machine Types */
#define IMAGE_FILE_MACHINE_I386 0x014C

/* Characteristics */
#define IMAGE_FILE_EXECUTABLE_IMAGE 0x0002
#define IMAGE_FILE_32BIT_MACHINE    0x0100

/* Section Characteristics */
#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000

/* Data Directory Indices */
#define IMAGE_DIRECTORY_ENTRY_IMPORT    1

/* Structs (Packed) */

typedef struct __attribute__((packed)) {
    uint16_t e_magic;      /* Magic number (MZ) */
    uint16_t e_cblp;       /* Bytes on last page of file */
    uint16_t e_cp;         /* Pages in file */
    uint16_t e_crlc;       /* Relocations */
    uint16_t e_cparhdr;    /* Size of header in paragraphs */
    uint16_t e_minalloc;   /* Minimum extra paragraphs needed */
    uint16_t e_maxalloc;   /* Maximum extra paragraphs needed */
    uint16_t e_ss;         /* Initial (relative) SS value */
    uint16_t e_sp;         /* Initial SP value */
    uint16_t e_csum;       /* Checksum */
    uint16_t e_ip;         /* Initial IP value */
    uint16_t e_cs;         /* Initial (relative) CS value */
    uint16_t e_lfarlc;     /* File address of relocation table */
    uint16_t e_ovno;       /* Overlay number */
    uint16_t e_res[4];     /* Reserved words */
    uint16_t e_oemid;      /* OEM identifier (for e_oeminfo) */
    uint16_t e_oeminfo;    /* OEM information; e_oemid specific */
    uint16_t e_res2[10];   /* Reserved words */
    uint32_t e_lfanew;     /* File address of new exe header (PE) */
} IMAGE_DOS_HEADER;

typedef struct __attribute__((packed)) {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} IMAGE_FILE_HEADER;

typedef struct __attribute__((packed)) {
    uint32_t VirtualAddress;
    uint32_t Size;
} IMAGE_DATA_DIRECTORY;

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

typedef struct __attribute__((packed)) {
    uint16_t Magic;         /* 0x10B for PE32 */
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32;

typedef struct __attribute__((packed)) {
    uint32_t Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32;

typedef struct __attribute__((packed)) {
    uint8_t  Name[8];
    union {
        uint32_t PhysicalAddress;
        uint32_t VirtualSize;
    } Misc;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct __attribute__((packed)) {
    union {
        uint32_t Characteristics;
        uint32_t OriginalFirstThunk;
    };
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR;

/* Validate a PE32 binary in memory. */
bool pe_validate(const uint8_t* data, uint32_t size);

/* Load and execute a PE32 binary. */
int pe_exec(const uint8_t* data, uint32_t size, const char* name);

/* Initialize PE subsystem */
void pe_init(void);

#endif /* PE_H */
