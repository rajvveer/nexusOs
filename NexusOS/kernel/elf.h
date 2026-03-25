/* ============================================================================
 * NexusOS — ELF32 Binary Loader (Header) — Phase 20
 * ============================================================================
 * Defines ELF32 structures and loading API.
 * ============================================================================ */

#ifndef ELF_H
#define ELF_H

#include "types.h"

/* ELF magic */
#define ELF_MAGIC 0x464C457F  /* "\x7FELF" */

/* ELF types */
#define ET_EXEC   2   /* Executable */
#define ET_DYN    3   /* Shared object */
#define EM_386    3   /* Intel 80386 */
#define EV_CURRENT 1

/* ELF class */
#define ELFCLASS32 1

/* Program header types */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2   /* Dynamic linking info */
#define PT_INTERP  3   /* Program interpreter */

/* Program header flags */
#define PF_X 0x1   /* Execute */
#define PF_W 0x2   /* Write */
#define PF_R 0x4   /* Read */

/* ELF32 Header */
typedef struct __attribute__((packed)) {
    uint32_t e_ident_magic;     /* Magic: 0x7F 'E' 'L' 'F' */
    uint8_t  e_ident_class;     /* 1=32-bit, 2=64-bit */
    uint8_t  e_ident_data;      /* 1=LE, 2=BE */
    uint8_t  e_ident_version;
    uint8_t  e_ident_osabi;
    uint8_t  e_ident_pad[8];
    uint16_t e_type;            /* ET_EXEC=2 */
    uint16_t e_machine;         /* EM_386=3 */
    uint32_t e_version;
    uint32_t e_entry;           /* Entry point virtual address */
    uint32_t e_phoff;           /* Program header table offset */
    uint32_t e_shoff;           /* Section header table offset */
    uint32_t e_flags;
    uint16_t e_ehsize;          /* ELF header size */
    uint16_t e_phentsize;       /* Program header entry size */
    uint16_t e_phnum;           /* Number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

/* ELF32 Program Header */
typedef struct __attribute__((packed)) {
    uint32_t p_type;    /* Segment type (PT_LOAD) */
    uint32_t p_offset;  /* Offset in file */
    uint32_t p_vaddr;   /* Virtual address in memory */
    uint32_t p_paddr;   /* Physical address (unused) */
    uint32_t p_filesz;  /* Size in file */
    uint32_t p_memsz;   /* Size in memory (may include BSS) */
    uint32_t p_flags;   /* PF_R, PF_W, PF_X */
    uint32_t p_align;   /* Alignment */
} elf32_phdr_t;

/* Validate an ELF32 binary in memory.
 * Returns true if it's a valid i386 ELF executable. */
bool elf_validate(const uint8_t* data, uint32_t size);

/* Load and execute an ELF32 binary.
 * data: pointer to ELF file contents in memory.
 * size: size of the ELF file.
 * name: process name.
 * Returns 0 on success, -1 on error. */
int elf_exec(const uint8_t* data, uint32_t size, const char* name);

/* Initialize ELF subsystem */
void elf_init(void);

#endif /* ELF_H */
