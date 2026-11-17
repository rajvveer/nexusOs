/* BOOTX64.EFI: the NexusOS UEFI loader (PE32+ EFI application, Boot Services only).
 *
 *  1. Read \EFI\NEXUS\KERNEL.BIN (and the optional \EFI\NEXUS\CMDLINE.TXT) from the ESP the loader
 *     itself was started from (Loaded Image -> Simple File System).
 *  2. Locate the kernel header (struct nexus_kernel_header), reserve the kernel's physical range
 *     [0x100000, __bss_end) at its link address and the AP trampoline page 0x7000.
 *  3. Pick a GOP mode (`video=WxH` on the command line, else the monitor's native mode from its EDID
 *     (EDID Active/Discovered protocol), else 1920x1080, else the current mode, else the largest 32-bit
 *     mode) and record the linear framebuffer, the mode list and the EDID for the kernel.
 *  4. Record the ACPI RSDP from the configuration table (ACPI 2.0 GUID first, then 1.0).
 *     Diagnostics (`diag` on the command line, or D held while the loader starts): a firmware-only
 *     \NEXUSDIAG.TXT is written to this volume now (GOP modes, EDID, ACPI tables, memory map, PCI, CPU);
 *     the kernel replaces it with the full report when it can reach the volume (kernel diag.c).
 *  5. Get the final memory map, convert it to merged usable-RAM ranges, ExitBootServices (retrying
 *     with a fresh map key when the map changed), then jump to start64 with RDI = boot info.
 *
 * The kernel rebuilds GDT/IDT/page tables itself (start64 drops to 32-bit protected mode and joins
 * the BIOS start32 path), so nothing the firmware set up survives except the memory contents. */
#include "efi.h"
#include "../boot_info.h"
#include "sb_verify.h"
#include "sb_pubkey.h"

/* Minimal EFI runtime-services view: GetVariable is the 7th function pointer after the 24-byte table header. */
typedef EFI_STATUS (*EFI_GET_VARIABLE)(const CHAR16* name, const EFI_GUID* guid, uint32_t* attributes, UINTN* size, void* data);
struct efi_runtime_services { uint8_t header[24]; void* before[6]; EFI_GET_VARIABLE get_variable; };
static const EFI_GUID global_variable_guid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};

#define KERNEL_LIMIT 0x600000ULL      /* the kernel image + BSS never extend past 6 MiB (linker.ld) */
#define LOW_LIMIT 0xFFFFFFULL         /* loader buffers: below 16 MiB, under the kernel's page pool */

static EFI_SYSTEM_TABLE* st;
static EFI_BOOT_SERVICES* bs;
static EFI_HANDLE self;
static uint64_t avoid_begin = 0x100000, avoid_end = KERNEL_LIMIT, trampoline = 0x7000;

static const EFI_GUID loaded_image_guid = {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const EFI_GUID file_system_guid = {0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const EFI_GUID graphics_guid = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
static const EFI_GUID acpi20_guid = {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static const EFI_GUID acpi10_guid = {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};
static const EFI_GUID edid_active_guid = {0xBD8C1056, 0x9F36, 0x44EC, {0x92, 0xA8, 0xA6, 0x33, 0x7F, 0x81, 0x79, 0x86}};
static const EFI_GUID edid_discovered_guid = {0x1C0C34F6, 0xD380, 0x41FA, {0xA0, 0x49, 0x8A, 0xD0, 0x6C, 0x1A, 0x66, 0xAA}};

/* The compiler may emit calls to these; volatile accesses keep it from turning them into calls to themselves. */
void* memset(void* target, int value, UINTN count) {
    volatile uint8_t* p = target; while (count--) *p++ = (uint8_t)value; return target;
}
void* memcpy(void* target, const void* source, UINTN count) {
    volatile uint8_t* d = target; const volatile uint8_t* s = source; while (count--) *d++ = *s++; return target;
}
static bool guid_equal(const EFI_GUID* a, const EFI_GUID* b) {
    const uint8_t *x = (const void*)a, *y = (const void*)b;
    for (unsigned i = 0; i < sizeof(EFI_GUID); i++) if (x[i] != y[i]) return false;
    return true;
}

static void print(const char* text) {
    CHAR16 buffer[130]; unsigned used = 0;
    for (;; text++) {
        if (!*text || used >= 126) {
            buffer[used] = 0; if (used) st->con_out->output_string(st->con_out, buffer);
            used = 0; if (!*text) return;
        }
        if (*text == '\n') buffer[used++] = '\r';
        buffer[used++] = (CHAR16)(uint8_t)*text;
    }
}
static void hex_text(uint64_t value, char* text) {
    text[0] = '0'; text[1] = 'x';
    for (int i = 0; i < 16; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (60 - 4 * i)) & 15];
    text[18] = 0;
}
static void print_hex(uint64_t value) { char text[19]; hex_text(value, text); print(text); }
static void decimal_text(uint64_t value, char* text) {
    char digits[21]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *text++ = digits[--n];
    *text = 0;
}
/* After ExitBootServices the console protocols are gone: talk to COM1 directly. */
static void serial_print(const char* text) {
    for (; *text; text++) {
        for (unsigned spins = 0; spins < 100000; spins++) {
            uint8_t status; __asm__ volatile("inb %1,%0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
            if (status & 0x20) break;
        }
        __asm__ volatile("outb %0,%1" :: "a"((uint8_t)*text), "Nd"((uint16_t)0x3F8));
    }
}
static EFI_STATUS fail(const char* why, EFI_STATUS status) {
    print("NexusOS UEFI loader: "); print(why); print(" (status "); print_hex(status); print(")\n");
    bs->stall(5000000);
    return status ? status : EFI_NOT_FOUND;
}

static bool overlaps(uint64_t a, uint64_t a_end, uint64_t b, uint64_t b_end) { return a < b_end && b < a_end; }
/* Loader-owned buffers must never sit where the kernel or its AP trampoline will be placed. */
static uint64_t allocate_outside(uint64_t bytes) {
    UINTN pages = (bytes + 4095) / 4096; uint64_t clash[32]; unsigned clashes = 0, found = 0; uint64_t result = 0;
    for (unsigned attempt = 0; attempt < 32 && !found; attempt++) {
        uint64_t address = attempt < 16 ? LOW_LIMIT : 0xFFFFFFFFULL;
        if (EFI_ERROR(bs->allocate_pages(AllocateMaxAddress, EfiLoaderData, pages, &address))) {
            if (attempt < 16) { attempt = 15; continue; }
            break;
        }
        if (!overlaps(address, address + pages * 4096, avoid_begin, avoid_end) &&
            !overlaps(address, address + pages * 4096, trampoline, trampoline + 4096)) { result = address; found = 1; break; }
        clash[clashes++] = address;   /* held while searching so the next attempt lands elsewhere */
    }
    /* then released: kept, they would be loader data inside the kernel's range and the kernel could not be placed */
    for (unsigned i = 0; i < clashes; i++) bs->free_pages(clash[i], pages);
    return result;
}

static EFI_STATUS read_file(EFI_FILE* root, const CHAR16* path, uint8_t** data, uint64_t* size, uint64_t limit) {
    EFI_FILE* file;
    EFI_STATUS status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;
    uint64_t end = 0;
    status = file->set_position(file, ~0ULL);
    if (!EFI_ERROR(status)) status = file->get_position(file, &end);
    if (!EFI_ERROR(status)) status = file->set_position(file, 0);
    if (!EFI_ERROR(status) && (!end || end > limit)) status = EFI_BUFFER_TOO_SMALL;
    uint8_t* buffer = NULL;
    if (!EFI_ERROR(status)) { buffer = (uint8_t*)allocate_outside(end + 1); if (!buffer) status = EFI_BUFFER_TOO_SMALL; }
    for (uint64_t done = 0; !EFI_ERROR(status) && done < end; ) {
        UINTN chunk = end - done;
        status = file->read(file, &chunk, buffer + done);
        if (!EFI_ERROR(status) && !chunk) status = EFI_NOT_FOUND;
        done += chunk;
    }
    file->close(file);
    if (EFI_ERROR(status)) return status;
    buffer[end] = 0; *data = buffer; *size = end; return EFI_SUCCESS;
}

static void append_option(struct nexus_boot_info* info, const char* text, uint64_t length) {
    unsigned used = 0; while (info->cmdline[used]) used++;
    while (length && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) { text++; length--; }
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' || text[length - 1] == '\n')) length--;
    if (!length) return;
    if (used && used + 1 < NEXUS_BOOT_CMDLINE_MAX) info->cmdline[used++] = ' ';
    for (uint64_t i = 0; i < length && used + 1 < NEXUS_BOOT_CMDLINE_MAX; i++)
        info->cmdline[used++] = text[i] == '\r' || text[i] == '\n' || text[i] == '\t' ? ' ' : text[i];
    info->cmdline[used] = 0;
}
/* Shell arguments arrive as UCS-2 LoadOptions ("bootx64.efi noioapic"). Boot-manager entries often
 * carry binary OptionalData instead, so anything that is not printable ASCII text is ignored. */
static void options_from_image(struct nexus_boot_info* info, const EFI_LOADED_IMAGE* loaded) {
    if (!loaded->load_options || loaded->load_options_size < 2 || (loaded->load_options_size & 1)) return;
    const CHAR16* wide = loaded->load_options; unsigned count = loaded->load_options_size / 2;
    char text[NEXUS_BOOT_CMDLINE_MAX]; unsigned length = 0;
    for (unsigned i = 0; i < count && wide[i]; i++) {
        if (wide[i] < 0x20 || wide[i] > 0x7E || length + 1 >= sizeof(text)) return;
        text[length++] = (char)wide[i];
    }
    text[length] = 0;
    unsigned start = 0, end = 0;
    while (end < length && text[end] != ' ') end++;
    if (end >= 4 && (text[end - 4] == '.' && (text[end - 3] | 32) == 'e' && (text[end - 2] | 32) == 'f' && (text[end - 1] | 32) == 'i'))
        start = end;
    append_option(info, text + start, length - start);
}
static bool cmdline_video(const char* cmdline, uint32_t* width, uint32_t* height) {
    for (const char* p = cmdline; *p; p++) {
        if ((p != cmdline && p[-1] != ' ') || p[0] != 'v' || p[1] != 'i' || p[2] != 'd' || p[3] != 'e' || p[4] != 'o' || p[5] != '=') continue;
        p += 6; uint32_t w = 0, h = 0;
        while (*p >= '0' && *p <= '9') w = w * 10 + (uint32_t)(*p++ - '0');
        if (*p++ != 'x') return false;
        while (*p >= '0' && *p <= '9') h = h * 10 + (uint32_t)(*p++ - '0');
        if (w < 640 || h < 480 || w > 16384 || h > 16384) return false;
        *width = w; *height = h; return true;
    }
    return false;
}

static uint32_t pixel_layout(const EFI_GRAPHICS_MODE_INFO* mode) {
    if (mode->pixel_format == PixelBlueGreenRedReserved8BitPerColor) return NEXUS_FB_XRGB8888;
    if (mode->pixel_format == PixelRedGreenBlueReserved8BitPerColor) return NEXUS_FB_XBGR8888;
    if (mode->pixel_format == PixelBitMask) {
        if (mode->red_mask == 0xFF0000 && mode->green_mask == 0xFF00 && mode->blue_mask == 0xFF) return NEXUS_FB_XRGB8888;
        if (mode->red_mask == 0xFF && mode->green_mask == 0xFF00 && mode->blue_mask == 0xFF0000) return NEXUS_FB_XBGR8888;
        return NEXUS_FB_OTHER;
    }
    return NEXUS_FB_NONE;
}
/* EDID block 0 with a valid header and checksum; the preferred mode is its first detailed timing. */
static bool edid_preferred(const uint8_t* e, uint32_t size, uint32_t* width, uint32_t* height) {
    static const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (size < 128) return false;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 128; i++) { sum = (uint8_t)(sum + e[i]); if (i < 8 && e[i] != header[i]) return false; }
    const uint8_t* d = e + 54;
    if (sum || (!d[0] && !d[1])) return false;                             /* bad checksum / not a timing descriptor */
    *width = d[2] | (uint32_t)(d[4] & 0xF0) << 4; *height = d[5] | (uint32_t)(d[7] & 0xF0) << 4;
    return *width >= 640 && *height >= 480;
}
static bool trust_vm_edid;   /* video=native: use a virtual display's preferred timing too (tests, VM users) */
static bool cmdline_word(const char* cmdline, const char* word) {
    for (const char* p = cmdline; *p; p++) {
        if (p != cmdline && p[-1] != ' ') continue;
        unsigned i = 0; while (word[i] && p[i] == word[i]) i++;
        if (!word[i] && (!p[i] || p[i] == ' ')) return true;
    }
    return false;
}
static bool edid_from(EFI_HANDLE handle, const EFI_GUID* guid, uint8_t source, struct nexus_boot_display* display) {
    EFI_EDID_PROTOCOL* edid = NULL; uint32_t width, height;
    if (!handle || EFI_ERROR(bs->handle_protocol(handle, guid, (void**)&edid)) || !edid || !edid->edid ||
        !edid_preferred(edid->edid, edid->size, &width, &height)) return false;
    display->edid_size = edid->size < sizeof(display->edid) ? edid->size : sizeof(display->edid);
    memcpy(display->edid, edid->edid, display->edid_size);
    display->edid_source = source; display->native_width = width; display->native_height = height;
    /* A virtual machine's display (QEMU RHT, VMware VMW, VirtualBox VBX, Hyper-V MSH) has no native panel:
     * its preferred timing is just a window size, so the 1920x1080 default stays in charge. */
    uint16_t vendor = (uint16_t)(edid->edid[8] << 8 | edid->edid[9]);
    if (vendor == 0x4914 || vendor == 0x59B7 || vendor == 0x5858 || vendor == 0x3668) {
        display->edid_virtual = 1; if (!trust_vm_edid) display->native_width = display->native_height = 0;
    }
    return true;
}
/* The monitor behind the console's GOP: its own handle first (Active, then Discovered), else the GOP
 * device handles, preferring the one whose GOP is the console's and an Active EDID. */
static void find_edid(EFI_GRAPHICS_OUTPUT* gop, struct nexus_boot_display* display) {
    if (edid_from(st->console_out_handle, &edid_active_guid, NEXUS_EDID_ACTIVE, display) ||
        edid_from(st->console_out_handle, &edid_discovered_guid, NEXUS_EDID_DISCOVERED, display)) return;
    UINTN count = 0; EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(bs->locate_handle_buffer(ByProtocol, &graphics_guid, NULL, &count, &handles)) || !handles) return;
    for (unsigned pass = 0; pass < 4 && !display->edid_size; pass++)
        for (UINTN i = 0; i < count && !display->edid_size; i++) {
            EFI_GRAPHICS_OUTPUT* other = NULL;
            if (EFI_ERROR(bs->handle_protocol(handles[i], &graphics_guid, (void**)&other)) || (pass < 2 && other != gop)) continue;
            edid_from(handles[i], pass & 1 ? &edid_discovered_guid : &edid_active_guid, pass & 1 ? NEXUS_EDID_DISCOVERED : NEXUS_EDID_ACTIVE, display);
        }
    bs->free_pool(handles);
}
static void setup_framebuffer(struct nexus_boot_info* info) {
    EFI_GRAPHICS_OUTPUT* gop = NULL;
    if (EFI_ERROR(bs->handle_protocol(st->console_out_handle, &graphics_guid, (void**)&gop)) || !gop)
        if (EFI_ERROR(bs->locate_protocol(&graphics_guid, NULL, (void**)&gop)) || !gop) { print("GOP: none (serial console only)\n"); return; }
    struct nexus_boot_display* display = &info->display;
    trust_vm_edid = cmdline_word(info->cmdline, "video=native");
    find_edid(gop, display);
    uint32_t want_width = 0, want_height = 0;
    bool explicit_mode = cmdline_video(info->cmdline, &want_width, &want_height);
    int option = -1, native = -1, standard = -1, largest = -1, chosen = -1; uint64_t largest_area = 0;
    uint32_t current = gop->mode->mode;
    display->mode_total = gop->mode->max_mode;
    for (uint32_t m = 0; m < gop->mode->max_mode; m++) {
        EFI_GRAPHICS_MODE_INFO* mode; UINTN size;
        if (EFI_ERROR(gop->query_mode(gop, m, &size, &mode))) continue;
        uint32_t layout = pixel_layout(mode);
        if (display->mode_count < NEXUS_BOOT_MODES_MAX)
            display->modes[display->mode_count++] = (struct nexus_boot_mode){(uint16_t)mode->width, (uint16_t)mode->height, (uint8_t)layout,
                                                                             0, (uint16_t)mode->pixels_per_scan_line};
        if (layout != NEXUS_FB_XRGB8888 && layout != NEXUS_FB_XBGR8888) continue;
        if (explicit_mode && option < 0 && mode->width == want_width && mode->height == want_height) option = (int)m;
        if (native < 0 && display->native_width && mode->width == display->native_width && mode->height == display->native_height) native = (int)m;
        if (standard < 0 && mode->width == 1920 && mode->height == 1080) standard = (int)m;
        if ((uint64_t)mode->width * mode->height > largest_area) { largest_area = (uint64_t)mode->width * mode->height; largest = (int)m; }
    }
    EFI_GRAPHICS_MODE_INFO* now; UINTN now_size;
    bool current_ok = !EFI_ERROR(gop->query_mode(gop, current, &now_size, &now)) &&
                      (pixel_layout(now) == NEXUS_FB_XRGB8888 || pixel_layout(now) == NEXUS_FB_XBGR8888);
    if (option >= 0) { chosen = option; display->reason = NEXUS_MODE_OPTION; }
    else if (native >= 0) { chosen = native; display->reason = NEXUS_MODE_NATIVE; }
    else if (standard >= 0) { chosen = standard; display->reason = NEXUS_MODE_DEFAULT; }
    else if (current_ok) { chosen = (int)current; display->reason = NEXUS_MODE_FIRMWARE; }
    else if (largest >= 0) { chosen = largest; display->reason = NEXUS_MODE_LARGEST; }
    if (explicit_mode && option < 0) print("GOP: the video= mode is not offered by the firmware\n");
    if (chosen >= 0 && (uint32_t)chosen != current && EFI_ERROR(gop->set_mode(gop, (uint32_t)chosen))) {
        print("GOP: mode switch failed; keeping the current mode\n"); display->reason = NEXUS_MODE_KEPT;
    }
    for (unsigned i = 0; i < display->mode_count; i++)
        if (display->modes[i].width == gop->mode->info->width && display->modes[i].height == gop->mode->info->height &&
            display->modes[i].format == pixel_layout(gop->mode->info)) { display->modes[i].flags |= 1; break; }
    const EFI_GRAPHICS_MODE_INFO* mode = gop->mode->info;
    uint32_t layout = pixel_layout(mode);
    if (layout == NEXUS_FB_NONE || !gop->mode->frame_buffer_base) { print("GOP: no linear framebuffer (Blt only)\n"); return; }
    struct nexus_boot_framebuffer* fb = &info->framebuffer;
    fb->base = gop->mode->frame_buffer_base; fb->width = mode->width; fb->height = mode->height;
    fb->pitch = mode->pixels_per_scan_line * 4; fb->bpp = 32; fb->format = layout;
    fb->size = (uint64_t)fb->pitch * fb->height;
    if (gop->mode->frame_buffer_size && fb->size > gop->mode->frame_buffer_size) fb->size = gop->mode->frame_buffer_size;
    char text[24];
    print("GOP: "); decimal_text(fb->width, text); print(text); print("x"); decimal_text(fb->height, text); print(text);
    print(" pitch="); decimal_text(fb->pitch, text); print(text); print(" base="); print_hex(fb->base);
    print(layout == NEXUS_FB_XRGB8888 ? " BGRX" : " RGBX");
    if (display->edid_virtual && !display->native_width) print(" EDID(virtual display)");
    else if (display->edid_size) {
        print(display->edid_source == NEXUS_EDID_ACTIVE ? " EDID(active) native " : " EDID(discovered) native ");
        decimal_text(display->native_width, text); print(text); print("x"); decimal_text(display->native_height, text); print(text);
    } else print(" no EDID");
    static const char* const reasons[] = {" [kept]", " [video= option]", " [native]", " [1920x1080 default]", " [firmware mode]", " [largest]"};
    print(reasons[display->reason]); print("\n");
}

/* Usable RAM: memory free once boot services are gone and that is ordinary write-back RAM.
 * Loader code/data (this loader, the kernel range, the boot info) stay excluded. */
static bool usable_type(uint32_t type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode || type == EfiBootServicesData;
}
static void add_range(struct nexus_boot_info* info, uint64_t base, uint64_t size) {
    if (!size) return;
    uint64_t end = base + size;
    for (unsigned i = 0; i < info->memory_count; i++) {
        struct nexus_boot_range* r = &info->memory[i];
        if (end < r->base || base > r->base + r->size) continue;
        /* overlap or adjacency: absorb r and re-insert the union */
        uint64_t merged_base = r->base < base ? r->base : base, merged_end = r->base + r->size > end ? r->base + r->size : end;
        for (unsigned j = i + 1; j < info->memory_count; j++) info->memory[j - 1] = info->memory[j];
        info->memory_count--;
        add_range(info, merged_base, merged_end - merged_base);
        return;
    }
    if (info->memory_count == NEXUS_BOOT_MEMORY_MAX) {
        unsigned smallest = 0;
        for (unsigned i = 1; i < info->memory_count; i++) if (info->memory[i].size < info->memory[smallest].size) smallest = i;
        if (info->memory[smallest].size >= size) return;
        for (unsigned j = smallest + 1; j < info->memory_count; j++) info->memory[j - 1] = info->memory[j];
        info->memory_count--;
    }
    unsigned at = 0;
    while (at < info->memory_count && info->memory[at].base < base) at++;
    for (unsigned j = info->memory_count; j > at; j--) info->memory[j] = info->memory[j - 1];
    info->memory[at] = (struct nexus_boot_range){base, size};
    info->memory_count++;
}
static void build_ranges(struct nexus_boot_info* info, const uint8_t* map, UINTN size, UINTN descriptor_size) {
    info->memory_count = 0; info->memory_total = 0; info->region_count = 0; info->region_flags = 0;
    for (UINTN offset = 0; offset + sizeof(EFI_MEMORY_DESCRIPTOR) <= size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR* d = (const void*)(map + offset);
        /* the raw map for diagnostics: adjacent entries of one type and attribute set merged */
        struct nexus_boot_region* last = info->region_count ? &info->regions[info->region_count - 1] : NULL;
        if (last && last->type == d->type && last->attributes == (uint32_t)d->attribute && last->base + last->size == d->physical_start) last->size += d->pages * 4096;
        else if (info->region_count < NEXUS_BOOT_REGIONS_MAX)
            info->regions[info->region_count++] = (struct nexus_boot_region){d->physical_start, d->pages * 4096, d->type, (uint32_t)d->attribute};
        else info->region_flags |= 1;                                               /* list full */
        if (!usable_type(d->type) || !(d->attribute & EFI_MEMORY_WB) || !d->pages) continue;
        info->memory_total += d->pages * 4096;
        add_range(info, d->physical_start, d->pages * 4096);
    }
}
/* Can [begin, end) be overwritten once boot services exit? Only if every byte is described as
 * conventional or boot-services memory (nothing runtime, ACPI, reserved, or loader-owned). */
static bool reclaimable(uint64_t begin, uint64_t end, const EFI_LOADED_IMAGE* loaded) {
    uint64_t rsp; __asm__ volatile("mov %%rsp,%0" : "=r"(rsp));
    if (overlaps(begin, end, rsp - 0x10000, rsp + 0x1000) ||
        overlaps(begin, end, (uint64_t)loaded->image_base, (uint64_t)loaded->image_base + loaded->image_size)) return false;
    UINTN size = 0, key, descriptor_size; uint32_t version;
    if (bs->get_memory_map(&size, NULL, &key, &descriptor_size, &version) != EFI_BUFFER_TOO_SMALL) return false;
    size += 16 * descriptor_size;
    uint8_t* map = (uint8_t*)allocate_outside(size);
    if (!map || EFI_ERROR(bs->get_memory_map(&size, (void*)map, &key, &descriptor_size, &version))) return false;
    for (uint64_t at = begin; at < end; ) {
        bool covered = false;
        for (UINTN offset = 0; offset + sizeof(EFI_MEMORY_DESCRIPTOR) <= size; offset += descriptor_size) {
            const EFI_MEMORY_DESCRIPTOR* d = (const void*)(map + offset);
            uint64_t d_end = d->physical_start + d->pages * 4096;
            if (at < d->physical_start || at >= d_end) continue;
            if (!usable_type(d->type)) return false;
            covered = true; at = d_end; break;
        }
        if (!covered) return false;
    }
    return true;
}

/* Firmware SecureBoot state: the global "SecureBoot" variable is 1 when Secure Boot is active. */
static bool firmware_secure_boot(void) {
    struct efi_runtime_services* rt = (void*)st->runtime_services;
    if (!rt || !rt->get_variable) return false;
    uint8_t value = 0; UINTN size = sizeof(value); uint32_t attrs = 0;
    static const CHAR16 name[] = {'S','e','c','u','r','e','B','o','o','t',0};
    if (EFI_ERROR(rt->get_variable(name, &global_variable_guid, &attrs, &size, &value))) return false;
    return value == 1;
}
/* ---- diagnostics (firmware part of NEXUSDIAG.TXT) ---- */
struct text { char* p; UINTN used, size; };
static void t_s(struct text* o, const char* s) { while (s && *s && o->used + 1 < o->size) o->p[o->used++] = *s++; }
static void t_c(struct text* o, char c) { if (o->used + 1 < o->size) o->p[o->used++] = c; }
static void t_d(struct text* o, uint64_t v) { char b[24]; decimal_text(v, b); t_s(o, b); }
static void t_x(struct text* o, uint64_t v, unsigned digits) { for (unsigned i = digits; i--; ) t_c(o, "0123456789abcdef"[(v >> (4 * i)) & 15]); }
static void t_h(struct text* o, uint64_t v) { t_s(o, "0x"); unsigned n = 1; while (n < 16 && (v >> (4 * n))) n++; t_x(o, v, n); }
static uint32_t pci_cfg(unsigned bus, unsigned device, unsigned function, unsigned offset) {
    uint32_t address = 0x80000000U | bus << 16 | device << 11 | function << 8 | (offset & 0xFC), value;
    __asm__ volatile("outl %0,%1" :: "a"(address), "Nd"((uint16_t)0xCF8));
    __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"((uint16_t)0xCFC));
    return value;
}
static bool diag_key(void) {
    if (!st->con_in) return false;
    for (unsigned round = 0; round < 30; round++) {                       /* ~300 ms: D held from power-on */
        EFI_INPUT_KEY key;
        while (!EFI_ERROR(st->con_in->read_key_stroke(st->con_in, &key))) if (key.unicode_char == 'd' || key.unicode_char == 'D') return true;
        bs->stall(10000);
    }
    return false;
}
static void diag_firmware_report(EFI_FILE* root, struct nexus_boot_info* info) {
    UINTN size = 512 * 1024; uint64_t buffer = allocate_outside(size);
    if (!buffer) return;
    struct text o = {(char*)buffer, 0, size}, *x = &o;
    t_s(x, "NexusOS hardware diagnostics - FIRMWARE PART (written by BOOTX64.EFI before the kernel started).\r\n");
    t_s(x, "If the kernel can reach this volume it replaces this file with the full report (drivers, kernel log).\r\n");
    t_s(x, "If you see this text after a full boot, the kernel could not write here (e.g. a USB stick: no USB storage driver).\r\n");
    t_s(x, "\r\n== Firmware ==\r\nvendor \""); for (const CHAR16* v = st->firmware_vendor; v && *v; v++) t_c(x, *v < 0x80 ? (char)*v : '?');
    t_s(x, "\" revision "); t_h(x, st->firmware_revision); t_s(x, " UEFI "); t_d(x, st->header.revision >> 16); t_c(x, '.'); t_d(x, (st->header.revision & 0xFFFF) / 10);
    t_s(x, "\r\ncommand line \""); t_s(x, info->cmdline); t_s(x, "\"\r\n");
    uint32_t r[4], b[13]; char brand[49];
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0), "c"(0));
    b[0] = r[1]; b[1] = r[3]; b[2] = r[2]; b[3] = 0; t_s(x, "CPU "); t_s(x, (const char*)b);
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000000U), "c"(0));
    if (r[0] >= 0x80000004U) {
        for (unsigned i = 0; i < 3; i++) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002U + i), "c"(0)); memcpy(brand + 16 * i, r, 16); }
        brand[48] = 0; t_s(x, " \""); t_s(x, brand); t_c(x, '"');
    }
    const struct nexus_boot_framebuffer* fb = &info->framebuffer; const struct nexus_boot_display* dsp = &info->display;
    t_s(x, "\r\n\r\n== Display (GOP) ==\r\nframebuffer "); t_d(x, fb->width); t_c(x, 'x'); t_d(x, fb->height); t_s(x, " pitch "); t_d(x, fb->pitch);
    t_s(x, " at "); t_h(x, fb->base); t_s(x, fb->format == NEXUS_FB_XRGB8888 ? " BGRX" : fb->format == NEXUS_FB_XBGR8888 ? " RGBX" : " other/none");
    t_s(x, "\r\nmodes ("); t_d(x, dsp->mode_total); t_s(x, "):");
    for (unsigned i = 0; i < dsp->mode_count; i++) { t_c(x, ' '); t_d(x, dsp->modes[i].width); t_c(x, 'x'); t_d(x, dsp->modes[i].height); if (dsp->modes[i].flags & 1) t_c(x, '*'); }
    t_s(x, "\r\nEDID "); t_d(x, dsp->edid_size); t_s(x, " bytes");
    for (unsigned i = 0; i < dsp->edid_size; i++) { if (!(i % 16)) t_s(x, "\r\n "); t_c(x, ' '); t_x(x, dsp->edid[i], 2); }
    t_s(x, "\r\n\r\n== ACPI ==\r\nRSDP "); t_h(x, info->rsdp); t_s(x, "\r\n");
    if (info->rsdp) {
        const uint8_t* rsdp = (const uint8_t*)info->rsdp; uint64_t root = 0; unsigned width = 4;
        if (rsdp[15] >= 2) { memcpy(&root, rsdp + 24, 8); width = 8; }
        if (!root) { memcpy(&root, rsdp + 16, 4); width = 4; }
        const uint8_t* list = (const uint8_t*)root; uint32_t length = 0; if (list) memcpy(&length, list + 4, 4);
        for (uint32_t at = 36; list && length < 0x10000 && at + width <= length; at += width) {
            uint64_t address = 0; memcpy(&address, list + at, width); const uint8_t* table = (const uint8_t*)address;
            if (!table) continue;
            uint32_t table_length; memcpy(&table_length, table + 4, 4);
            t_s(x, "  "); for (unsigned k = 0; k < 4; k++) t_c(x, (char)table[k]); t_s(x, " at "); t_h(x, address); t_s(x, " len "); t_d(x, table_length);
            t_s(x, " oem \""); for (unsigned k = 10; k < 16; k++) t_c(x, table[k] >= 0x20 && table[k] < 0x7F ? (char)table[k] : ' '); t_s(x, "\"\r\n");
        }
    }
    t_s(x, "\r\n== UEFI memory map (before ExitBootServices) ==\r\n");
    UINTN map_size = 0, key, descriptor_size; uint32_t version;
    if (bs->get_memory_map(&map_size, NULL, &key, &descriptor_size, &version) == EFI_BUFFER_TOO_SMALL && descriptor_size >= sizeof(EFI_MEMORY_DESCRIPTOR)) {
        map_size += 16 * descriptor_size; uint64_t map = allocate_outside(map_size);
        if (map && !EFI_ERROR(bs->get_memory_map(&map_size, (void*)map, &key, &descriptor_size, &version))) {
            static const char* const names[] = {"reserved", "loader code", "loader data", "boot code", "boot data", "runtime code", "runtime data",
                                                "conventional", "unusable", "ACPI reclaim", "ACPI NVS", "MMIO", "MMIO port", "PAL code", "persistent", "unaccepted"};
            for (UINTN at = 0; at + sizeof(EFI_MEMORY_DESCRIPTOR) <= map_size; at += descriptor_size) {
                const EFI_MEMORY_DESCRIPTOR* d = (const void*)(map + at);
                t_s(x, "  "); t_x(x, d->physical_start, 16); t_s(x, " pages "); t_d(x, d->pages); t_c(x, ' '); t_s(x, d->type < 16 ? names[d->type] : "other");
                t_s(x, " attr "); t_h(x, d->attribute); t_s(x, "\r\n");
            }
        }
        if (map) bs->free_pages(map, (map_size + 4095) / 4096);
    }
    t_s(x, "\r\n== PCI (config space) ==\r\n");
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned device = 0; device < 32; device++) {
            if ((pci_cfg(bus, device, 0, 0) & 0xFFFF) == 0xFFFF) continue;
            unsigned functions = (pci_cfg(bus, device, 0, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (unsigned function = 0; function < functions; function++) {
                uint32_t id = pci_cfg(bus, device, function, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint32_t class = pci_cfg(bus, device, function, 8), header = (pci_cfg(bus, device, function, 0x0C) >> 16) & 0x7F;
                t_x(x, bus, 2); t_c(x, ':'); t_x(x, device, 2); t_c(x, '.'); t_x(x, function, 1); t_c(x, ' ');
                t_x(x, id & 0xFFFF, 4); t_c(x, ':'); t_x(x, id >> 16, 4); t_s(x, " class "); t_x(x, class >> 8, 6); t_s(x, " rev "); t_x(x, class & 0xFF, 2);
                for (unsigned bar = 0; bar < (header == 0 ? 6u : header == 1 ? 2u : 0u); bar++) {
                    uint32_t value = pci_cfg(bus, device, function, 0x10 + 4 * bar);
                    if (value) { t_s(x, " bar"); t_d(x, bar); t_c(x, '='); t_h(x, value); }
                }
                t_s(x, " irq "); t_d(x, pci_cfg(bus, device, function, 0x3C) & 0xFF); t_s(x, "\r\n");
            }
        }
    EFI_FILE* file;
    if (!EFI_ERROR(root->open(root, &file, u"\\NEXUSDIAG.TXT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) file->remove(file);
    EFI_STATUS status = root->open(root, &file, u"\\NEXUSDIAG.TXT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR(status)) {
        UINTN bytes = o.used; status = file->write(file, &bytes, o.p);
        if (!EFI_ERROR(status)) status = file->flush(file);
        file->close(file);
    }
    print(EFI_ERROR(status) ? "Diagnostics: could not write \\NEXUSDIAG.TXT on the boot volume\n" : "Diagnostics: firmware part written to \\NEXUSDIAG.TXT\n");
    bs->free_pages(buffer, size / 4096);
}
#include "abslot.c"   /* A/B boot slots on the root file system (core updates, rollback) */

EFI_STATUS EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE* system) {
    self = image; st = system; bs = system->boot_services;
    bs->set_watchdog_timer(0, 0, 0, NULL);
    print("NexusOS UEFI loader (BOOTX64.EFI)\n");
    EFI_LOADED_IMAGE* loaded; EFI_SIMPLE_FILE_SYSTEM* volume; EFI_FILE* root;
    EFI_STATUS status = bs->handle_protocol(self, &loaded_image_guid, (void**)&loaded);
    if (EFI_ERROR(status)) return fail("no loaded-image protocol", status);
    status = bs->handle_protocol(loaded->device, &file_system_guid, (void**)&volume);
    if (EFI_ERROR(status)) return fail("boot device has no file system", status);
    status = volume->open_volume(volume, &root);
    if (EFI_ERROR(status)) return fail("cannot open the ESP volume", status);

    struct nexus_boot_info* info = (void*)allocate_outside(sizeof(*info));
    if (!info) return fail("no memory for boot information", 0);
    memset(info, 0, sizeof(*info));
    info->magic = NEXUS_BOOT_MAGIC; info->version = NEXUS_BOOT_VERSION; info->size = sizeof(*info);
    info->source = NEXUS_BOOT_UEFI;
    options_from_image(info, loaded);
    uint8_t* text; uint64_t text_size;
    if (!EFI_ERROR(read_file(root, u"\\EFI\\NEXUS\\CMDLINE.TXT", &text, &text_size, NEXUS_BOOT_CMDLINE_MAX * 4)))
        append_option(info, (const char*)text, text_size);
    bool diag = cmdline_word(info->cmdline, "diag");
    if (!diag && diag_key()) { append_option(info, "diag", 4); diag = true; print("D held: diagnostics boot\n"); }
    if (info->cmdline[0]) { print("Command line: "); print(info->cmdline); print("\n"); }

    uint8_t* kernel; uint64_t kernel_size;
    status = ab_kernel(loaded, info, &kernel, &kernel_size, KERNEL_LIMIT - 0x100000);
    if (status == EFI_NOT_FOUND) status = read_file(root, u"\\EFI\\NEXUS\\KERNEL.BIN", &kernel, &kernel_size, KERNEL_LIMIT - 0x100000);
    if (EFI_ERROR(status)) return fail("cannot read \\EFI\\NEXUS\\KERNEL.BIN", status);
    const struct nexus_kernel_header* header = NULL;
    for (uint64_t at = 0; at + sizeof(*header) <= kernel_size && at < 0x10000; at += 8)
        if (((const struct nexus_kernel_header*)(kernel + at))->magic == NEXUS_KERNEL_MAGIC) { header = (const void*)(kernel + at); break; }
    if (!header || header->version != 1 || header->header_size < sizeof(*header) || header->load_address != 0x100000 ||
        header->memory_end > KERNEL_LIMIT || header->memory_end < header->load_address + kernel_size ||
        header->entry64 < header->load_address || header->entry64 >= header->load_address + kernel_size ||
        (header->trampoline & 4095) || header->trampoline >= 0x100000)
        return fail("KERNEL.BIN has no valid NexusOS x86-64 header", 0);
    uint64_t load = header->load_address, end = (header->memory_end + 4095) & ~4095ULL, entry = header->entry64;

    /* Secure chain: verify KERNEL.BIN's RSA-2048/SHA-256 signature with the embedded public key before trusting it.
     * When the firmware reports Secure Boot active the signature is mandatory; otherwise a present signature is still
     * enforced (a tampered image is refused) but an absent one only skips verification. */
    bool secure = firmware_secure_boot();
    if (secure) { info->flags |= NEXUS_BOOT_SECURE_BOOT; if (!cmdline_word(info->cmdline, "nexus.secureboot=1")) append_option(info, "nexus.secureboot=1", 18); }   /* nexus-update's policy */
    print(secure ? "Firmware Secure Boot: active\n" : "Firmware Secure Boot: inactive\n");
#ifdef SB_KERNEL_KEY_PRESENT
    uint8_t* sig; uint64_t sig_size;
    EFI_STATUS sig_status = read_file(root, u"\\EFI\\NEXUS\\KERNEL.SIG", &sig, &sig_size, 4096);
    if (ab_slot_kernel) {   /* an A/B slot kernel: ab_kernel checked it against the slot's own kernel.sig (abslot.c) */
        if (ab_slot_verified) { info->flags |= NEXUS_BOOT_KERNEL_VERIFIED; print("KERNEL.BIN signature: verified (slot kernel.sig, RSA-2048/SHA-256)\n"); }
        else print("KERNEL.BIN signature: none present, verification skipped (Secure Boot inactive)\n");
    } else if (!EFI_ERROR(sig_status)) {
        if (sig_size != 256 || !rsa_sha256_verify(kernel, kernel_size, sig, sb_kernel_modulus))
            return fail("KERNEL.BIN signature verification FAILED (tampered or wrong key)", 0);
        info->flags |= NEXUS_BOOT_KERNEL_VERIFIED;
        print("KERNEL.BIN signature: verified (RSA-2048/SHA-256)\n");
    } else if (secure) {
        return fail("Secure Boot active but KERNEL.BIN is unsigned (no KERNEL.SIG)", 0);
    } else {
        print("KERNEL.BIN signature: none present, verification skipped (Secure Boot inactive)\n");
    }
#endif
    bool copy_late = false;
    uint64_t address = load;
    status = bs->allocate_pages(AllocateAddress, EfiLoaderCode, (end - load) / 4096, &address);
    if (EFI_ERROR(status)) {
        if (!reclaimable(load, end, loaded)) return fail("kernel range 0x100000.. is in use by the firmware", status);
        copy_late = true; info->flags |= NEXUS_BOOT_KERNEL_COPIED;
    }
    address = trampoline;
    if (EFI_ERROR(bs->allocate_pages(AllocateAddress, EfiLoaderData, 1, &address)) && !reclaimable(trampoline, trampoline + 4096, loaded)) {
        info->flags |= NEXUS_BOOT_NO_TRAMPOLINE;
        print("AP trampoline page 0x7000 is firmware memory: application processors stay offline\n");
    }

    setup_framebuffer(info);
    for (UINTN i = 0; i < st->table_count; i++) if (guid_equal(&st->tables[i].guid, &acpi20_guid)) info->rsdp = (uint64_t)st->tables[i].table;
    if (!info->rsdp)
        for (UINTN i = 0; i < st->table_count; i++) if (guid_equal(&st->tables[i].guid, &acpi10_guid)) info->rsdp = (uint64_t)st->tables[i].table;
    print("ACPI RSDP: "); print_hex(info->rsdp); print(copy_late ? "\nKernel range is boot-services memory: copying after exit\n" : "\n");
    if (diag) diag_firmware_report(root, info);
    if (!copy_late) memcpy((void*)load, kernel, kernel_size);

    UINTN map_size = 0, key = 0, descriptor_size = 0; uint32_t version;
    status = bs->get_memory_map(&map_size, NULL, &key, &descriptor_size, &version);
    if (status != EFI_BUFFER_TOO_SMALL || descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR)) return fail("GetMemoryMap size query failed", status);
    UINTN capacity = map_size + 64 * descriptor_size;
    uint8_t* map = (uint8_t*)allocate_outside(capacity);
    if (!map) return fail("no memory for the memory map", 0);
    print("Exiting boot services\n");
    bool exited = false;
    for (unsigned attempt = 0; attempt < 8 && !exited; attempt++) {
        map_size = capacity;
        status = bs->get_memory_map(&map_size, (void*)map, &key, &descriptor_size, &version);
        if (EFI_ERROR(status)) break;
        build_ranges(info, map, map_size, descriptor_size);
        /* A failed ExitBootServices means the map changed: only GetMemoryMap/ExitBootServices may follow. */
        status = bs->exit_boot_services(self, key);
        exited = !EFI_ERROR(status);
    }
    if (!exited) return fail("ExitBootServices failed", status);
    __asm__ volatile("cli" ::: "memory");
    if (copy_late) memcpy((void*)load, kernel, kernel_size);
    char number[24];
    serial_print("[uefi] boot services exited; usable ranges="); decimal_text(info->memory_count, number); serial_print(number);
    serial_print(" usable MiB="); decimal_text(info->memory_total >> 20, number); serial_print(number);
    serial_print("; entering kernel\n");
    __asm__ volatile("cld; jmp *%1" :: "D"(info), "r"(entry) : "memory");
    for (;;) __asm__ volatile("cli; hlt");
}
