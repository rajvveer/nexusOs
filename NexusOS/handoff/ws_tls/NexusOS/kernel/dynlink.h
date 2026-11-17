/* ============================================================================
 * NexusOS — ELF Dynamic Linker (Header) — Phase 32
 * ============================================================================
 * Shared library loading, symbol resolution, and dlopen/dlsym/dlclose API.
 * ============================================================================ */

#ifndef DYNLINK_H
#define DYNLINK_H

#include "types.h"

/* ---- Limits ---- */
#define DYNLINK_MAX_LIBS       16    /* Max loaded shared libraries */
#define DYNLINK_MAX_SYMBOLS    256   /* Max built-in symbols */
#define DYNLINK_LIB_NAME_MAX   32    /* Max library name length */
#define DYNLINK_SO_BASE        0x10000000  /* Base address for .so loading (256MB) */
#define DYNLINK_SO_STRIDE      0x00100000  /* 1MB spacing between libraries */

/* ---- ELF32 Section Header ---- */
typedef struct __attribute__((packed)) {
    uint32_t sh_name;       /* Section name (index into string table) */
    uint32_t sh_type;       /* Section type */
    uint32_t sh_flags;      /* Section flags */
    uint32_t sh_addr;       /* Virtual address */
    uint32_t sh_offset;     /* Offset in file */
    uint32_t sh_size;       /* Size in bytes */
    uint32_t sh_link;       /* Link to another section */
    uint32_t sh_info;       /* Additional info */
    uint32_t sh_addralign;  /* Alignment */
    uint32_t sh_entsize;    /* Entry size (for symbol/reloc tables) */
} elf32_shdr_t;

/* Section header types */
#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_RELA       4
#define SHT_HASH       5
#define SHT_DYNAMIC    6
#define SHT_NOTE       7
#define SHT_NOBITS     8
#define SHT_REL        9
#define SHT_DYNSYM     11

/* ---- ELF32 Dynamic Entry ---- */
typedef struct __attribute__((packed)) {
    int32_t  d_tag;     /* Dynamic entry tag */
    uint32_t d_val;     /* Value (or pointer) */
} elf32_dyn_t;

/* Dynamic tags */
#define DT_NULL        0    /* End of dynamic section */
#define DT_NEEDED      1    /* Name of needed library (strtab offset) */
#define DT_PLTRELSZ    2    /* Size of PLT relocs */
#define DT_PLTGOT      3    /* Address of PLT/GOT */
#define DT_HASH        4    /* Symbol hash table */
#define DT_STRTAB      5    /* String table address */
#define DT_SYMTAB      6    /* Symbol table address */
#define DT_RELA        7    /* Rela relocs */
#define DT_RELASZ      8    /* Size of rela relocs */
#define DT_RELAENT     9    /* Size of one rela entry */
#define DT_STRSZ       10   /* String table size */
#define DT_SYMENT      11   /* Size of one symbol entry */
#define DT_REL         17   /* Rel relocs */
#define DT_RELSZ       18   /* Size of rel relocs */
#define DT_RELENT      19   /* Size of one rel entry */
#define DT_PLTREL      20   /* PLT reloc type */
#define DT_JMPREL      23   /* PLT relocs address */

/* ---- ELF32 Symbol Table Entry ---- */
typedef struct __attribute__((packed)) {
    uint32_t st_name;   /* Symbol name (index into strtab) */
    uint32_t st_value;  /* Symbol value (address) */
    uint32_t st_size;   /* Symbol size */
    uint8_t  st_info;   /* Bind (upper 4) + Type (lower 4) */
    uint8_t  st_other;  /* Visibility */
    uint16_t st_shndx;  /* Section index */
} elf32_sym_t;

/* Symbol binding */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2

/* Symbol type */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2

/* Macros */
#define ELF32_ST_BIND(i)  ((i) >> 4)
#define ELF32_ST_TYPE(i)  ((i) & 0xF)
#define ELF32_ST_INFO(b,t) (((b)<<4)+((t)&0xF))

/* ---- ELF32 Relocation Entry ---- */
typedef struct __attribute__((packed)) {
    uint32_t r_offset;  /* Address to apply relocation */
    uint32_t r_info;    /* Symbol index + type */
} elf32_rel_t;

/* Relocation macros */
#define ELF32_R_SYM(i)   ((i) >> 8)
#define ELF32_R_TYPE(i)   ((uint8_t)(i))
#define ELF32_R_INFO(s,t) (((s)<<8)+(uint8_t)(t))

/* i386 relocation types */
#define R_386_NONE       0   /* No relocation */
#define R_386_32         1   /* S + A */
#define R_386_PC32       2   /* S + A - P */
#define R_386_GLOB_DAT   6   /* S */
#define R_386_JMP_SLOT   7   /* S */
#define R_386_RELATIVE   8   /* B + A */

/* ---- Loaded Library Descriptor ---- */
typedef struct {
    char     name[DYNLINK_LIB_NAME_MAX];
    bool     active;
    uint32_t load_base;         /* Virtual address where library is loaded */
    uint32_t load_size;         /* Total mapped size */

    /* Parsed from .dynamic */
    elf32_sym_t* symtab;        /* Symbol table pointer (in mapped memory) */
    const char*  strtab;        /* String table pointer (in mapped memory) */
    uint32_t     strtab_size;
    uint32_t     sym_count;     /* Number of symbols */

    /* Relocation info */
    elf32_rel_t* rel;           /* REL relocations */
    uint32_t     relsz;         /* Size of REL section */
    elf32_rel_t* pltrel;        /* PLT/JMPREL relocations */
    uint32_t     pltrelsz;      /* Size of PLT section */

    uint32_t     ref_count;     /* Reference count */
} loaded_lib_t;

/* ---- Built-in Symbol Entry ---- */
typedef struct {
    const char* name;
    uint32_t    addr;
} builtin_sym_t;

/* ---- API ---- */

/* Initialize the dynamic linker subsystem */
void dynlink_init(void);

/* Load a shared library by name.
 * data: ELF file contents.  size: file size.  name: library name.
 * Returns library handle (index+1) or 0 on failure. */
uint32_t dynlink_load_library(const uint8_t* data, uint32_t size, const char* name);

/* Resolve a symbol name across all loaded libraries and built-ins.
 * Returns symbol address or 0 if not found. */
uint32_t dynlink_resolve_symbol(const char* name);

/* Find symbol in a specific library */
uint32_t dynlink_lib_symbol(uint32_t handle, const char* name);

/* Register a built-in (kernel-provided) symbol */
void dynlink_register_builtin(const char* name, uint32_t addr);

/* Unload a shared library by handle */
int dynlink_unload_library(uint32_t handle);

/* Get loaded library count */
int dynlink_get_lib_count(void);

/* Get loaded library table (for ldd/inspection) */
const loaded_lib_t* dynlink_get_libs(void);

/* Get built-in symbol table info */
int dynlink_get_builtin_count(void);
const builtin_sym_t* dynlink_get_builtins(void);

/* --- POSIX dlopen/dlsym/dlclose API --- */

/* Open a shared library. Returns handle or NULL (0). 
 * name: library filename in VFS. flags: unused (reserved). */
uint32_t dl_open(const char* name, int flags);

/* Look up a symbol in a loaded library.
 * handle: from dl_open.  name: symbol name.
 * Returns symbol address or 0 if not found. */
uint32_t dl_sym(uint32_t handle, const char* name);

/* Close a shared library. Returns 0 on success. */
int dl_close(uint32_t handle);

#endif /* DYNLINK_H */
