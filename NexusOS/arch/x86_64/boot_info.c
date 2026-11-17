/* The kernel's copy of the boot hand-off (boot_info.h). This file is the only reader of the BIOS
 * stage-2 blocks: VBE mode data at 0x9000 and the E820 map at 0x9100 ("NX64" signature at
 * 0x9104, 24-byte entries from 0x9108). */
#include "native.h"
#include "boot_info.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

struct nexus_boot_info boot_info;
extern uint32_t boot_info_physical;   /* entry.asm: set by start64 (UEFI), 0 on BIOS boots */

static void add_range(uint64_t base, uint64_t size) {
    struct nexus_boot_info* info = &boot_info;
    if (!size || base + size < base) return;
    uint64_t end = base + size;
    for (unsigned i = 0; i < info->memory_count; i++) {
        struct nexus_boot_range* r = &info->memory[i];
        if (end < r->base || base > r->base + r->size) continue;
        uint64_t low = r->base < base ? r->base : base, high = r->base + r->size > end ? r->base + r->size : end;
        for (unsigned j = i + 1; j < info->memory_count; j++) info->memory[j - 1] = info->memory[j];
        info->memory_count--;
        add_range(low, high - low); return;
    }
    if (info->memory_count == NEXUS_BOOT_MEMORY_MAX) return;
    unsigned at = 0;
    while (at < info->memory_count && info->memory[at].base < base) at++;
    for (unsigned j = info->memory_count; j > at; j--) info->memory[j] = info->memory[j - 1];
    info->memory[at] = (struct nexus_boot_range){base, size};
    info->memory_count++;
}
static void bios_boot_info(void) {
    boot_info.magic = NEXUS_BOOT_MAGIC; boot_info.version = NEXUS_BOOT_VERSION; boot_info.size = sizeof(boot_info);
    boot_info.source = NEXUS_BOOT_BIOS;
    struct e820 { uint64_t base, size; uint32_t type, attributes; } __attribute__((packed));
    const struct e820* map = phys_to_virt(0x9108); unsigned count = *(volatile uint16_t*)phys_to_virt(0x9100);
    if (*(volatile uint32_t*)phys_to_virt(0x9104) == 0x3436584E && count && count <= 32)
        for (unsigned i = 0; i < count; i++) {
            if (boot_info.region_count < NEXUS_BOOT_REGIONS_MAX)   /* raw E820 for diagnostics (diag.c) */
                boot_info.regions[boot_info.region_count++] = (struct nexus_boot_region){map[i].base, map[i].size, map[i].type, map[i].attributes};
            if (map[i].type != 1 || !(map[i].attributes & 1) || map[i].size > ~0ULL - map[i].base) continue;
            boot_info.memory_total += map[i].size;
            add_range(map[i].base, map[i].size);
        }
    /* Disk boots (build.py usb-image) carry the command line in stage 2 at 0x7E00 + 0x700 (boot/boot2.asm). */
    const volatile char* line = phys_to_virt(0x8500);
    static const char magic[] = "NXCMDLINE=";
    bool tagged = true;
    for (unsigned i = 0; i < sizeof(magic) - 1; i++) if (line[i] != magic[i]) tagged = false;
    if (tagged) {
        unsigned n = 0;
        for (const volatile char* c = line + sizeof(magic) - 1; *c && n + 1 < NEXUS_BOOT_CMDLINE_MAX && c < line + 0xF0; c++)
            boot_info.cmdline[n++] = *c >= ' ' && *c < 127 ? *c : ' ';
        boot_info.cmdline[n] = 0;
    }
    volatile uint8_t* vbe = phys_to_virt(0x9000);
    struct nexus_boot_framebuffer* fb = &boot_info.framebuffer;
    if (!vbe[10] || vbe[11] != 32 || vbe[20] != 6) return;    /* no LFB, not 32 bpp, not direct colour */
    fb->base = *(volatile uint32_t*)vbe; fb->pitch = *(volatile uint16_t*)(vbe + 4);
    fb->width = *(volatile uint16_t*)(vbe + 6); fb->height = *(volatile uint16_t*)(vbe + 8);
    fb->bpp = 32; fb->size = (uint64_t)fb->pitch * fb->height;
    fb->format = vbe[12] == 8 && vbe[13] == 16 && vbe[14] == 8 && vbe[15] == 8 && vbe[16] == 8 && vbe[17] == 0 ? NEXUS_FB_XRGB8888 :
                 vbe[12] == 8 && vbe[13] == 0 && vbe[14] == 8 && vbe[15] == 8 && vbe[16] == 8 && vbe[17] == 16 ? NEXUS_FB_XBGR8888 : NEXUS_FB_OTHER;
    /* boot2.asm (x86-64): the monitor's EDID from VBE/DDC at 0x9800, signature 'EDID' at 0x97F8. */
    struct nexus_boot_display* d = &boot_info.display;
    if (*(volatile uint32_t*)phys_to_virt(0x97F8) == 0x44494445) {
        const volatile uint8_t* e = phys_to_virt(0x9800);
        d->edid_size = 128; d->edid_source = NEXUS_EDID_DDC;
        for (unsigned i = 0; i < 128; i++) d->edid[i] = e[i];
        uint16_t vendor = (uint16_t)(e[8] << 8 | e[9]);
        d->edid_virtual = vendor == 0x4914 || vendor == 0x59B7;            /* QEMU RHT, VMware VMW: not a panel */
        if (e[54] | e[55]) { d->native_width = e[56] | (uint32_t)(e[58] & 0xF0) << 4; d->native_height = e[59] | (uint32_t)(e[61] & 0xF0) << 4; }
    }
    d->reason = d->native_width && fb->width == d->native_width && fb->height == d->native_height ? NEXUS_MODE_NATIVE :
                fb->width == 1920 && fb->height == 1080 ? NEXUS_MODE_DEFAULT : NEXUS_MODE_KEPT;
}

/* QEMU only: fw_cfg file opt/nexus/cmdline appends kernel options on either boot path (tests). */
static void fw_cfg_cmdline(void) {
    port_word_out(0x510, 0);
    if (port_byte_in(0x511) != 'Q' || port_byte_in(0x511) != 'E' || port_byte_in(0x511) != 'M' || port_byte_in(0x511) != 'U') return;
    port_word_out(0x510, 0x19);
    uint32_t count = 0;
    for (unsigned i = 0; i < 4; i++) count = (count << 8) | port_byte_in(0x511);
    static const char name[] = "opt/nexus/cmdline";
    uint16_t selector = 0; uint32_t size = 0;
    for (uint32_t i = 0; i < count && i < 0x1000; i++) {
        uint8_t entry[64];
        for (unsigned j = 0; j < sizeof(entry); j++) entry[j] = port_byte_in(0x511);
        if (selector || memcmp(entry + 8, name, sizeof(name))) continue;
        size = ((uint32_t)entry[0] << 24) | ((uint32_t)entry[1] << 16) | ((uint32_t)entry[2] << 8) | entry[3];
        selector = (uint16_t)((entry[4] << 8) | entry[5]);
    }
    if (!selector || !size) return;
    unsigned used = (unsigned)strlen(boot_info.cmdline);
    if (used && used + 1 < NEXUS_BOOT_CMDLINE_MAX) boot_info.cmdline[used++] = ' ';
    port_word_out(0x510, selector);
    for (uint32_t i = 0; i < size; i++) {
        char c = (char)port_byte_in(0x511);
        if (!c) break;
        if (used + 1 < NEXUS_BOOT_CMDLINE_MAX) boot_info.cmdline[used++] = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
    }
    boot_info.cmdline[used] = 0;
}

static void print_number(uint64_t value) { char text[12]; uint_to_str((uint32_t)value, text); vga_print(text); }
void boot_info_init(void) {
    const struct nexus_boot_info* uefi = boot_info_physical ? phys_to_virt(boot_info_physical) : NULL;
    if (uefi && uefi->magic == NEXUS_BOOT_MAGIC && uefi->version == NEXUS_BOOT_VERSION &&
        (uefi->size == sizeof(boot_info) || uefi->size == NEXUS_BOOT_INFO_V1_SIZE) && uefi->memory_count <= NEXUS_BOOT_MEMORY_MAX) {
        memset(&boot_info, 0, sizeof(boot_info));
        memcpy(&boot_info, uefi, uefi->size);                 /* an older loader has no display block */
        boot_info.cmdline[NEXUS_BOOT_CMDLINE_MAX - 1] = 0;
        struct nexus_boot_display* d = &boot_info.display;
        if (d->edid_size > sizeof(d->edid)) d->edid_size = sizeof(d->edid);
        if (d->mode_count > NEXUS_BOOT_MODES_MAX) d->mode_count = NEXUS_BOOT_MODES_MAX;
        if (d->reason > NEXUS_MODE_LARGEST) d->reason = NEXUS_MODE_KEPT;
        if (boot_info.region_count > NEXUS_BOOT_REGIONS_MAX) boot_info.region_count = NEXUS_BOOT_REGIONS_MAX;
    } else {
        memset(&boot_info, 0, sizeof(boot_info));
        bios_boot_info();
        /* boot2.asm (x86-64): D held while stage 2 started -> diagnostics (diag.c) */
        if (*(volatile uint32_t*)phys_to_virt(0x97F0) == 0x47414944) {
            unsigned used = (unsigned)strlen(boot_info.cmdline);
            if (used + 6 < NEXUS_BOOT_CMDLINE_MAX) memcpy(boot_info.cmdline + used, used ? " diag" : "diag", used ? 6 : 5);
        }
    }
    fw_cfg_cmdline();
}
/* Called once the console is up (kernel64_main after screen_init). */
void boot_info_print(void) {
    vga_print("[64] BOOT source="); vga_print(boot_info.source == NEXUS_BOOT_UEFI ? "UEFI" : "BIOS");
    vga_print(" ram_ranges="); print_number(boot_info.memory_count);
    vga_print(" usable_mib="); print_number(boot_info.memory_total >> 20);
    if (boot_info.framebuffer.format) {
        vga_print(" fb="); print_number(boot_info.framebuffer.width); vga_print("x"); print_number(boot_info.framebuffer.height);
    }
    if (boot_info.flags & NEXUS_BOOT_KERNEL_COPIED) vga_print(" kernel-copied");
    if (boot_info.cmdline[0]) { vga_print(" cmdline=\""); vga_print(boot_info.cmdline); vga_print("\""); }
    vga_print("\n");
    vga_print("[64] SECUREBOOT firmware="); vga_print(boot_info.flags & NEXUS_BOOT_SECURE_BOOT ? "on" : "off");
    vga_print(" kernel_verified="); vga_print(boot_info.flags & NEXUS_BOOT_KERNEL_VERIFIED ? "yes" : "no"); vga_print("\n");
}
bool boot_secure_boot(void) { return (boot_info.flags & NEXUS_BOOT_SECURE_BOOT) != 0; }
bool boot_kernel_verified(void) { return (boot_info.flags & NEXUS_BOOT_KERNEL_VERIFIED) != 0; }
/* key=value option: copies the value (truncated to size - 1), false if absent. */
bool boot_value(const char* key, char* value, unsigned size) {
    unsigned length = (unsigned)strlen(key);
    for (const char* p = boot_info.cmdline; *p; ) {
        while (*p == ' ') p++;
        const char* start = p;
        while (*p && *p != ' ') p++;
        if ((unsigned)(p - start) > length && !memcmp(start, key, length) && start[length] == '=') {
            unsigned n = 0;
            for (const char* v = start + length + 1; v < p && n + 1 < size; v++) value[n++] = *v;
            if (size) value[n] = 0;
            return true;
        }
    }
    return false;
}
bool boot_option(const char* word) {
    unsigned length = (unsigned)strlen(word);
    for (const char* p = boot_info.cmdline; *p; ) {
        while (*p == ' ') p++;
        const char* start = p;
        while (*p && *p != ' ') p++;
        if ((unsigned)(p - start) == length && !memcmp(start, word, length)) return true;
    }
    return false;
}
uint32_t boot_option_number(const char* prefix) {
    unsigned length = (unsigned)strlen(prefix);
    for (const char* p = boot_info.cmdline; *p; ) {
        while (*p == ' ') p++;
        const char* start = p;
        while (*p && *p != ' ') p++;
        if ((unsigned)(p - start) <= length || memcmp(start, prefix, length)) continue;
        uint32_t value = 0;
        for (const char* q = start + length; q < p; q++) {
            if (*q < '0' || *q > '9' || value > 100000000U) return 0;
            value = value * 10 + (uint32_t)(*q - '0');
        }
        return value;
    }
    return 0;
}
