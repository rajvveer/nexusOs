/* ============================================================================
 * NexusOS — v5.0 Grand Finale (Header) — Phase 50 (Era 7 finale)
 * ============================================================================
 * The capstone phase. Four pillars, each scoped honestly against what the
 * existing subsystems actually support:
 *
 *   1. Universal binary launcher — auto-detect PE / ELF32 / Mach-O by magic
 *      bytes and dispatch to the existing loader (pe_exec / elf_exec /
 *      macho_exec, Phases 34 / 20 / 37). One `urun <file>` for any format.
 *
 *   2. Footprint report — measure & assert the v5.0 goals: kernel < 1 MB
 *      (from the linker symbols _kernel_start/_kernel_end) and RAM usage
 *      < 16 MB (from the physical memory manager). Real numbers, not claims.
 *
 *   3. Disk installer — write a real MBR partition table + a copy of the
 *      live system to the primary-master IDE disk via the ATA PIO driver
 *      (ata_write_sectors), then verify by reading it back. Contained to the
 *      QEMU disk image; never touches the host.
 *
 *   4. ISO generation — produced build-side (build.bat); `finale` documents it.
 * ============================================================================ */

#ifndef FINALE_H
#define FINALE_H

#include "types.h"

/* v5.0 goal thresholds (the roadmap targets). */
#define FINALE_KERNEL_MAX   (1u * 1024u * 1024u)    /* < 1 MB kernel  */
#define FINALE_RAM_MAX      (16u * 1024u * 1024u)   /* < 16 MB RAM    */

/* MBR install layout (LBA, primary master). Partition 1 holds the system. */
#define FINALE_PART_START_LBA   2048u               /* 1 MiB aligned  */
#define FINALE_BOOT_SIGNATURE   0xAA55

void finale_init(void);

/* --- Universal binary launcher --------------------------------------------- */
/* Detect format by magic and run via the matching loader. Returns the loader's
 * result (0 = ok) or -1 if the file is missing / format unrecognized. */
int  finale_urun(const char* filename);
/* Name the detected format ("PE32" / "ELF32" / "Mach-O" / "unknown"). */
const char* finale_detect_format(const uint8_t* data, uint32_t size);

/* --- Footprint report ------------------------------------------------------ */
uint32_t finale_kernel_size(void);      /* bytes, from linker symbols          */
uint32_t finale_ram_used(void);         /* bytes, from the PMM                  */
void     finale_show_stats(void);       /* the v5.0 report card                 */

/* --- Disk installer -------------------------------------------------------- */
/* Write MBR + system to the IDE disk and verify. confirm must be true (the
 * shell requires `install disk yes`) or it only prints the plan. */
int  finale_install_disk(bool confirm);
void finale_install_plan(void);         /* show the layout without writing      */

/* --- The finale banner ----------------------------------------------------- */
void finale_show(void);

#endif /* FINALE_H */
