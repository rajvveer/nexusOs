/* ============================================================================
 * NexusOS — Mach-O Binary Loader (Header) — Phase 37
 * ============================================================================
 * Parses 32-bit (i386) Mach-O executables: validates the header, walks the
 * load commands, maps the segments into memory, locates the entry point
 * (LC_UNIXTHREAD or LC_MAIN), and can transition to ring-3 execution — the
 * macOS counterpart of the Phase 34 PE32 loader.
 * ============================================================================ */

#ifndef MACHO_H
#define MACHO_H

#include "types.h"

/* Mach-O magic numbers */
#define MH_MAGIC      0xFEEDFACE   /* 32-bit, host byte order      */
#define MH_CIGAM      0xCEFAEDFE   /* 32-bit, byte-swapped         */
#define MH_MAGIC_64   0xFEEDFACF   /* 64-bit (rejected: we are i386) */

/* CPU types */
#define CPU_TYPE_X86  7            /* == CPU_TYPE_I386             */

/* File types */
#define MH_OBJECT     0x1
#define MH_EXECUTE    0x2
#define MH_DYLIB      0x6
#define MH_BUNDLE     0x8

/* Load command types */
#define LC_SEGMENT      0x1
#define LC_SYMTAB       0x2
#define LC_UNIXTHREAD   0x5
#define LC_LOAD_DYLIB   0xC
#define LC_ID_DYLIB     0xD
#define LC_MAIN         0x80000028

/* Mach-O 32-bit header */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
} mach_header_t;

/* Generic load command prefix */
typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
} load_command_t;

/* LC_SEGMENT (32-bit) */
typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} segment_command_t;

/* LC_LOAD_DYLIB / LC_ID_DYLIB */
typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t name_offset;   /* offset of the path string within the command */
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compat_version;
} dylib_command_t;

/* LC_MAIN */
typedef struct __attribute__((packed)) {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;      /* file offset of main() within __TEXT */
    uint64_t stacksize;
} entry_point_command_t;

/* Parsed summary returned by macho_get_info() */
typedef struct {
    bool     valid;
    uint32_t cputype;
    uint32_t filetype;
    uint32_t ncmds;
    int      nsegments;
    int      ndylibs;
    uint32_t entry;            /* entry virtual address (0 if none found) */
    char     first_dylib[64];  /* name of the first LC_LOAD_DYLIB, if any */
} macho_info_t;

/* Validate a 32-bit i386 Mach-O executable in memory. */
bool macho_validate(const uint8_t* data, uint32_t size);

/* Parse header + load commands into `out`. Returns true on success. */
bool macho_get_info(const uint8_t* data, uint32_t size, macho_info_t* out);

/* Load segments and execute (ring 3). Returns 0 on a successful launch. */
int  macho_exec(const uint8_t* data, uint32_t size, const char* name);

/* Initialize the Mach-O subsystem. */
void macho_init(void);

/* One-line status string for `machoinfo`. */
const char* macho_get_status(void);

#endif /* MACHO_H */
