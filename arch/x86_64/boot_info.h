/* Versioned boot hand-off shared by both boot paths.
 *
 * - BIOS: boot/boot2.asm leaves VBE data at 0x9000 and the E820 map at 0x9100; boot_info.c converts
 *   them into this structure (the only code that still reads those blocks).
 * - UEFI: uefi/bootx64.c fills it from GOP, the final UEFI memory map and the ACPI configuration
 *   table, then jumps to start64 (entry.asm) with RDI = its physical address (below 4 GiB).
 *
 * The kernel copies it (through its direct map) into `boot_info` before anything else runs; generic code
 * (screen, physical memory, ACPI) reads only that copy. All addresses are physical. */
#ifndef NEXUS_BOOT_INFO_H
#define NEXUS_BOOT_INFO_H
#include "../../kernel/types.h"

#define NEXUS_BOOT_MAGIC 0x4F464E4942584EULL     /* "NXBINFO" */
#define NEXUS_BOOT_VERSION 1
#define NEXUS_BOOT_MEMORY_MAX 64
#define NEXUS_BOOT_CMDLINE_MAX 256

enum nexus_boot_source { NEXUS_BOOT_BIOS = 1, NEXUS_BOOT_UEFI = 2 };
/* Pixel layouts. XRGB8888 = bytes B,G,R,X in memory (VBE direct colour 8:16/8:8/8:0, UEFI
 * PixelBlueGreenRedReserved8BitPerColor): the only layout the console and /dev/fb0 accept. */
enum nexus_fb_format { NEXUS_FB_NONE = 0, NEXUS_FB_XRGB8888 = 1, NEXUS_FB_XBGR8888 = 2, NEXUS_FB_OTHER = 3 };
/* flags */
#define NEXUS_BOOT_NO_TRAMPOLINE 1u   /* the AP start page 0x7000 is not free RAM: run on the BSP only */
#define NEXUS_BOOT_KERNEL_COPIED 2u   /* UEFI: kernel range was boot-services memory, copied after exit */
#define NEXUS_BOOT_SECURE_BOOT 4u     /* UEFI firmware reported SecureBoot=1 (loader read the EFI variable) */
#define NEXUS_BOOT_KERNEL_VERIFIED 8u /* the loader verified KERNEL.BIN's RSA-2048/SHA-256 signature before jumping */

struct nexus_boot_range { uint64_t base, size; };
/* Raw firmware memory map: BIOS E820 (type 1-7) or UEFI (EFI memory type, attributes' low 32 bits); adjacent
 * entries with equal type and attributes are merged; at most NEXUS_BOOT_REGIONS_MAX are kept. */
#define NEXUS_BOOT_REGIONS_MAX 160
struct nexus_boot_region { uint64_t base, size; uint32_t type, attributes; };
/* Display discovery for diagnostics (diag.c) and the console: the firmware's mode list, why the mode
 * was chosen, and the monitor's EDID (UEFI EDID Active/Discovered protocol or VBE/DDC). */
#define NEXUS_BOOT_MODES_MAX 48
enum { NEXUS_EDID_NONE = 0, NEXUS_EDID_ACTIVE = 1, NEXUS_EDID_DISCOVERED = 2, NEXUS_EDID_DDC = 3 };
enum { NEXUS_MODE_KEPT = 0, NEXUS_MODE_OPTION = 1, NEXUS_MODE_NATIVE = 2, NEXUS_MODE_DEFAULT = 3, NEXUS_MODE_FIRMWARE = 4,
       NEXUS_MODE_LARGEST = 5 };
struct nexus_boot_mode { uint16_t width, height; uint8_t format, flags; uint16_t pitch_pixels; };   /* flags 1 = chosen */
struct nexus_boot_display {
    uint32_t edid_size;                                   /* bytes kept in edid[] (0 = none) */
    uint8_t edid_source, mode_count, reason, edid_virtual;   /* NEXUS_EDID_*, modes listed, NEXUS_MODE_*, 1 = VM display */
    uint32_t native_width, native_height;                 /* EDID preferred timing (0 = unknown or a VM display) */
    uint32_t mode_total;                                  /* modes the firmware reported (may exceed the list) */
    struct nexus_boot_mode modes[NEXUS_BOOT_MODES_MAX];
    uint8_t edid[256];
};
struct nexus_boot_framebuffer {
    uint64_t base, size;          /* size: pitch * height bytes */
    uint32_t pitch, width, height, bpp, format, reserved;
};
struct nexus_boot_info {
    uint64_t magic;
    uint32_t version, size;       /* size = sizeof(struct nexus_boot_info) of the writer */
    uint32_t source, flags;
    uint32_t memory_count, reserved;
    uint64_t rsdp;                /* ACPI RSDP, 0 = not provided (the kernel scans EBDA/0xE0000) */
    uint64_t memory_total;        /* bytes of usable RAM, including ranges the kernel ignores */
    struct nexus_boot_framebuffer framebuffer;
    struct nexus_boot_range memory[NEXUS_BOOT_MEMORY_MAX]; /* usable RAM, sorted and merged */
    char cmdline[NEXUS_BOOT_CMDLINE_MAX];                  /* NUL-terminated ASCII */
    struct nexus_boot_display display;                     /* added after version 1's first layout (size tells) */
    uint32_t region_count, region_flags;                   /* firmware memory map for diagnostics (diag.c) */
    struct nexus_boot_region regions[NEXUS_BOOT_REGIONS_MAX];
};
#define NEXUS_BOOT_INFO_V1_SIZE __builtin_offsetof(struct nexus_boot_info, display)

/* Header embedded in kernel.bin (entry.asm, 8-byte aligned within the first 64 KiB) that the UEFI
 * loader locates by its magic. The kernel is linked in the higher half (KERNEL_VMA + physical); every
 * address here is physical, and start64 runs at its physical address on the firmware's identity map. */
#define NEXUS_KERNEL_MAGIC 0x34364C4E524B584EULL  /* "NXKRNL64" */
struct nexus_kernel_header {
    uint64_t magic;
    uint32_t version, header_size;
    uint64_t load_address;        /* 0x100000: where kernel.bin must be placed */
    uint64_t entry64;             /* start64 (physical): 64-bit entry, RDI = boot info */
    uint64_t memory_end;          /* __bss_end (physical): the image plus BSS must fit [load_address, memory_end) */
    uint64_t trampoline;          /* AP real-mode start page the kernel will use */
};

#ifdef NEXUS_NATIVE64
extern struct nexus_boot_info boot_info;
void boot_info_init(void);
void boot_info_print(void);
/* Kernel command line (UEFI: CMDLINE.TXT on the ESP / shell arguments; QEMU: fw_cfg
 * opt/nexus/cmdline on either path). boot_option("noioapic") matches a whole word. */
bool boot_option(const char* word);
bool boot_secure_boot(void);       /* UEFI firmware reported SecureBoot=1 */
bool boot_kernel_verified(void);   /* the loader verified KERNEL.BIN's signature */
uint32_t boot_option_number(const char* prefix);   /* "name=123" given as prefix "name=": 123, absent/invalid: 0 */
bool boot_value(const char* key, char* value, unsigned size);   /* "root=PARTUUID=..." -> value of key=, false if absent */
#endif
#endif
