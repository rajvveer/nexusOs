/* Host test of the UEFI loader's display selection (arch/x86_64/uefi/bootx64.c: setup_framebuffer, find_edid,
   edid_preferred) against simulated firmware: GOP instances with mode lists, the console handle (a physical GOP or a
   ConSplitter-style virtual one), EDID Active/Discovered protocols on GOP child handles, failing SetMode, RGB/BGR and
   bit-mask pixel formats, padded scan lines, Blt-only modes, virtual-machine EDIDs and the video= options.
   Built for x86_64-windows-gnu, whose default calling convention is the UEFI (Microsoft x64) one. */
#define NEXUS_HOSTED 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arch/x86_64/uefi/bootx64.c"

/* ---- console ---- */
static char console[16384]; static unsigned console_used;
static EFI_STATUS output_string(EFI_SIMPLE_TEXT_OUTPUT* self, const CHAR16* text) {
    (void)self;
    for (; *text; text++) if (console_used + 1 < sizeof(console)) { console[console_used++] = (char)*text; console[console_used] = 0; }
    return EFI_SUCCESS;
}
static bool logged(const char* text) { return strstr(console, text) != NULL; }

/* ---- GOP instances ---- */
struct fake_gop {
    EFI_GRAPHICS_OUTPUT gop; EFI_GRAPHICS_MODE mode; EFI_GRAPHICS_MODE_INFO info[64]; unsigned count, sets; bool fail_set;
};
static EFI_STATUS query_mode(EFI_GRAPHICS_OUTPUT* gop, uint32_t m, UINTN* size, EFI_GRAPHICS_MODE_INFO** info) {
    struct fake_gop* f = (struct fake_gop*)gop;
    if (m >= f->count) return EFI_INVALID_PARAMETER;
    *size = sizeof(EFI_GRAPHICS_MODE_INFO); *info = &f->info[m]; return EFI_SUCCESS;
}
static EFI_STATUS set_mode(EFI_GRAPHICS_OUTPUT* gop, uint32_t m) {
    struct fake_gop* f = (struct fake_gop*)gop;
    if (m >= f->count) return EFI_INVALID_PARAMETER;
    f->sets++;
    if (f->fail_set) return EFI_ERROR_BIT | 7;                            /* EFI_DEVICE_ERROR */
    f->mode.mode = m; f->mode.info = &f->info[m];
    f->mode.frame_buffer_size = (UINTN)f->info[m].pixels_per_scan_line * 4 * f->info[m].height;
    return EFI_SUCCESS;
}
static void gop_init(struct fake_gop* f, const uint32_t (*modes)[3], unsigned count, unsigned current, uint32_t format) {
    memset(f, 0, sizeof(*f));
    f->gop.query_mode = query_mode; f->gop.set_mode = set_mode; f->gop.mode = &f->mode;
    for (unsigned i = 0; i < count; i++)
        f->info[i] = (EFI_GRAPHICS_MODE_INFO){0, modes[i][0], modes[i][1], format, 0, 0, 0, 0, modes[i][2] ? modes[i][2] : modes[i][0]};
    f->count = count; f->mode.max_mode = count; f->mode.mode = current; f->mode.info = &f->info[current];
    f->mode.frame_buffer_base = 0xC0000000ULL; f->mode.frame_buffer_size = (UINTN)f->info[current].pixels_per_scan_line * 4 * f->info[current].height;
}

/* ---- handles: up to 4 protocols each ---- */
struct fake_handle { const EFI_GUID* guid[4]; void* interface[4]; };
static struct fake_handle handles[6]; static unsigned handle_count;
static unsigned locates, frees;
static struct fake_handle* new_handle(void) { struct fake_handle* h = &handles[handle_count++]; memset(h, 0, sizeof(*h)); return h; }
static void attach(struct fake_handle* h, const EFI_GUID* guid, void* interface) {
    for (unsigned i = 0; i < 4; i++) if (!h->guid[i]) { h->guid[i] = guid; h->interface[i] = interface; return; }
    abort();
}
static EFI_STATUS handle_protocol(EFI_HANDLE handle, const EFI_GUID* guid, void** interface) {
    struct fake_handle* h = handle;
    if (!h) return EFI_INVALID_PARAMETER;
    for (unsigned i = 0; i < 4; i++) if (h->guid[i] && guid_equal(h->guid[i], guid)) { *interface = h->interface[i]; return EFI_SUCCESS; }
    return EFI_ERROR_BIT | 3;                                             /* EFI_UNSUPPORTED */
}
static EFI_STATUS locate_protocol(const EFI_GUID* guid, void* registration, void** interface) {
    (void)registration;
    for (unsigned k = 0; k < handle_count; k++) if (!EFI_ERROR(handle_protocol(&handles[k], guid, interface))) return EFI_SUCCESS;
    return EFI_NOT_FOUND;
}
static EFI_STATUS locate_handle_buffer(uint32_t type, const EFI_GUID* guid, void* key, UINTN* count, EFI_HANDLE** buffer) {
    assert(type == ByProtocol && !key); locates++;
    EFI_HANDLE* list = malloc(sizeof(EFI_HANDLE) * 8); unsigned n = 0; void* interface;
    for (unsigned k = 0; k < handle_count; k++) if (!EFI_ERROR(handle_protocol(&handles[k], guid, &interface))) list[n++] = &handles[k];
    if (!n) { free(list); return EFI_NOT_FOUND; }
    *count = n; *buffer = list; return EFI_SUCCESS;
}
static EFI_STATUS free_pool(void* buffer) { frees++; free(buffer); return EFI_SUCCESS; }

/* ---- EDIDs ---- */
struct edid_block { EFI_EDID_PROTOCOL protocol; uint8_t bytes[256]; };
static void make_edid(struct edid_block* e, const char vendor[3], unsigned width, unsigned height, bool corrupt, unsigned size) {
    memset(e, 0, sizeof(*e));
    static const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    memcpy(e->bytes, header, 8);
    unsigned id = (unsigned)(vendor[0] - '@') << 10 | (unsigned)(vendor[1] - '@') << 5 | (unsigned)(vendor[2] - '@');
    e->bytes[8] = (uint8_t)(id >> 8); e->bytes[9] = (uint8_t)id; e->bytes[18] = 1; e->bytes[19] = 4;
    uint8_t* d = e->bytes + 54;                                           /* detailed timing 1: 148.5 MHz-ish, active size */
    d[0] = 0x02; d[1] = 0x3A; d[2] = (uint8_t)width; d[4] = (uint8_t)((width >> 8) << 4); d[5] = (uint8_t)height; d[7] = (uint8_t)((height >> 8) << 4);
    uint8_t sum = 0; for (unsigned i = 0; i < 127; i++) sum = (uint8_t)(sum + e->bytes[i]);
    e->bytes[127] = (uint8_t)(0x100 - sum + (corrupt ? 1 : 0));
    e->protocol.size = size; e->protocol.edid = e->bytes;
}

/* ---- one run ---- */
static EFI_SIMPLE_TEXT_OUTPUT text_out; static EFI_BOOT_SERVICES services; static EFI_SYSTEM_TABLE firmware;
static struct nexus_boot_info info;
static void reset(void) {
    handle_count = 0; console_used = 0; console[0] = 0; locates = frees = 0;
    memset(&info, 0, sizeof(info)); memset(&services, 0, sizeof(services)); memset(&firmware, 0, sizeof(firmware));
    text_out.output_string = output_string;
    services.handle_protocol = handle_protocol; services.locate_protocol = locate_protocol;
    services.locate_handle_buffer = locate_handle_buffer; services.free_pool = free_pool;
    firmware.con_out = &text_out; firmware.boot_services = &services;
    st = &firmware; bs = &services; trust_vm_edid = false;
}
static void run(struct fake_handle* console_handle, const char* cmdline) {
    firmware.console_out_handle = console_handle;
    strcpy(info.cmdline, cmdline);
    setup_framebuffer(&info);
    assert(locates == frees);                                             /* every handle buffer returned */
}

static const uint32_t pc_modes[][3] = {{800, 600, 0}, {1024, 768, 0}, {1280, 1024, 0}, {1920, 1080, 0}, {2560, 1440, 0}, {3840, 2160, 0}};
static struct fake_gop gpu, virtual_gop, other_gpu;
static struct edid_block edid, edid2;

int main(void) {
    /* 1. The console is the GPU's own GOP handle with an Active EDID (2560x1440 monitor): native mode. */
    reset(); gop_init(&gpu, pc_modes, 6, 0, PixelBlueGreenRedReserved8BitPerColor);
    struct fake_handle* h = new_handle(); attach(h, &graphics_guid, &gpu.gop);
    make_edid(&edid, "GSM", 2560, 1440, false, 128); attach(h, &edid_active_guid, &edid.protocol);
    run(h, "");
    assert(gpu.mode.mode == 4 && info.framebuffer.width == 2560 && info.framebuffer.height == 1440 && info.framebuffer.pitch == 10240);
    assert(info.framebuffer.format == NEXUS_FB_XRGB8888 && info.framebuffer.base == 0xC0000000ULL && info.framebuffer.size == 10240ULL * 1440);
    assert(info.display.reason == NEXUS_MODE_NATIVE && info.display.edid_source == NEXUS_EDID_ACTIVE && info.display.edid_size == 128);
    assert(info.display.native_width == 2560 && info.display.native_height == 1440 && !info.display.edid_virtual && !memcmp(info.display.edid, edid.bytes, 128));
    assert(info.display.mode_count == 6 && info.display.mode_total == 6 && info.display.modes[4].flags == 1 && info.display.modes[3].flags == 0);
    assert(logged("GOP: 2560x1440 pitch=10240") && logged("EDID(active) native 2560x1440 [native]"));
    assert(locates == 0);                                                 /* found on the console handle: no search */
    puts("native mode from the console GOP's EDID passed");

    /* 2. ConSplitter-style console (a virtual GOP without EDID); GPU A has no monitor, GPU B a 4K monitor whose EDID
          is only Discovered: B's EDID decides, the mode is set through the console GOP. */
    reset(); gop_init(&virtual_gop, pc_modes, 6, 1, PixelBlueGreenRedReserved8BitPerColor);
    gop_init(&gpu, pc_modes, 6, 1, PixelBlueGreenRedReserved8BitPerColor); gop_init(&other_gpu, pc_modes, 6, 1, PixelBlueGreenRedReserved8BitPerColor);
    struct fake_handle* splitter = new_handle(); attach(splitter, &graphics_guid, &virtual_gop.gop);
    struct fake_handle* a = new_handle(); attach(a, &graphics_guid, &other_gpu.gop);
    struct fake_handle* b = new_handle(); attach(b, &graphics_guid, &gpu.gop);
    make_edid(&edid, "DEL", 3840, 2160, false, 256); attach(b, &edid_discovered_guid, &edid.protocol);
    run(splitter, "");
    assert(virtual_gop.mode.mode == 5 && virtual_gop.sets == 1 && gpu.sets == 0 && info.framebuffer.width == 3840 && info.framebuffer.pitch == 15360);
    assert(info.display.edid_source == NEXUS_EDID_DISCOVERED && info.display.edid_size == 256 && info.display.reason == NEXUS_MODE_NATIVE);
    assert(locates == 1 && logged("EDID(discovered) native 3840x2160 [native]"));
    /* the console's own GOP wins over an Active EDID elsewhere */
    reset(); gop_init(&gpu, pc_modes, 6, 1, PixelBlueGreenRedReserved8BitPerColor); gop_init(&other_gpu, pc_modes, 6, 1, PixelBlueGreenRedReserved8BitPerColor);
    a = new_handle(); attach(a, &graphics_guid, &other_gpu.gop); make_edid(&edid2, "SAM", 1280, 1024, false, 128); attach(a, &edid_active_guid, &edid2.protocol);
    b = new_handle(); attach(b, &graphics_guid, &gpu.gop); make_edid(&edid, "DEL", 2560, 1440, false, 128); attach(b, &edid_discovered_guid, &edid.protocol);
    splitter = new_handle(); attach(splitter, &graphics_guid, &gpu.gop);   /* console handle shares GPU B's GOP */
    run(splitter, "");
    assert(info.display.native_width == 2560 && gpu.mode.mode == 4 && info.display.edid_source == NEXUS_EDID_DISCOVERED);
    puts("EDID on a GOP child handle behind a ConSplitter console passed");

    /* 3. The monitor's native mode (3440x1440) is not offered: 1920x1080 default. */
    reset(); gop_init(&gpu, pc_modes, 6, 0, PixelBlueGreenRedReserved8BitPerColor);
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); make_edid(&edid, "AUS", 3440, 1440, false, 128); attach(h, &edid_active_guid, &edid.protocol);
    run(h, "");
    assert(gpu.mode.mode == 3 && info.display.reason == NEXUS_MODE_DEFAULT && info.display.native_width == 3440 && logged("[1920x1080 default]"));
    /* 4. video=WxH wins; a video= mode the firmware lacks falls back to the native one with a message. */
    reset(); gop_init(&gpu, pc_modes, 6, 0, PixelBlueGreenRedReserved8BitPerColor);
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); make_edid(&edid, "GSM", 2560, 1440, false, 128); attach(h, &edid_active_guid, &edid.protocol);
    run(h, "quiet video=1280x1024");
    assert(gpu.mode.mode == 2 && info.display.reason == NEXUS_MODE_OPTION && logged("[video= option]"));
    reset(); gop_init(&gpu, pc_modes, 6, 0, PixelBlueGreenRedReserved8BitPerColor);
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); attach(h, &edid_active_guid, &edid.protocol);
    run(h, "video=1366x768");
    assert(gpu.mode.mode == 4 && info.display.reason == NEXUS_MODE_NATIVE && logged("the video= mode is not offered"));
    puts("default, video= and missing-mode fallbacks passed");

    /* 5. A virtual machine's EDID (QEMU "RHT", VMware "VMW") is recorded but not used, unless video=native. */
    const char* vms[] = {"RHT", "VMW", "VBX", "MSH"};
    for (unsigned v = 0; v < 4; v++) {
        reset(); gop_init(&gpu, pc_modes, 6, 0, PixelBlueGreenRedReserved8BitPerColor);
        h = new_handle(); attach(h, &graphics_guid, &gpu.gop); make_edid(&edid, vms[v], 1280, 1024, false, 128); attach(h, &edid_active_guid, &edid.protocol);
        run(h, "");
        assert(gpu.mode.mode == 3 && info.display.edid_virtual && !info.display.native_width && info.display.edid_size == 128 && logged("EDID(virtual display)"));
    }
    reset(); gop_init(&gpu, pc_modes, 6, 0, PixelBlueGreenRedReserved8BitPerColor);
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); make_edid(&edid, "RHT", 1280, 1024, false, 128); attach(h, &edid_active_guid, &edid.protocol);
    run(h, "video=native");
    assert(gpu.mode.mode == 2 && info.display.edid_virtual && info.display.native_width == 1280 && info.display.reason == NEXUS_MODE_NATIVE);
    puts("virtual-machine EDIDs passed");

    /* 6. Broken EDIDs are ignored: bad checksum, short buffer, no detailed timing, absurd size. */
    for (unsigned k = 0; k < 4; k++) {
        reset(); gop_init(&gpu, pc_modes, 6, 0, PixelBlueGreenRedReserved8BitPerColor);
        h = new_handle(); attach(h, &graphics_guid, &gpu.gop);
        make_edid(&edid, "GSM", 2560, 1440, k == 0, k == 1 ? 64 : 128);
        if (k == 2) { memset(edid.bytes + 54, 0, 18); uint8_t s = 0; for (unsigned i = 0; i < 127; i++) s = (uint8_t)(s + edid.bytes[i]); edid.bytes[127] = (uint8_t)(0x100 - s); }
        if (k == 3) { edid.bytes[56] = 0x10; edid.bytes[58] = 0; uint8_t s = 0; for (unsigned i = 0; i < 127; i++) s = (uint8_t)(s + edid.bytes[i]); edid.bytes[127] = (uint8_t)(0x100 - s); }   /* 16 px wide */
        attach(h, &edid_active_guid, &edid.protocol);
        run(h, "");
        assert(gpu.mode.mode == 3 && !info.display.edid_size && !info.display.native_width && logged("no EDID"));
    }
    puts("broken EDIDs passed");

    /* 7. RGB (RGBX in memory) and bit-mask layouts; padded scan lines. */
    reset(); gop_init(&gpu, pc_modes, 6, 0, PixelRedGreenBlueReserved8BitPerColor);
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(gpu.mode.mode == 3 && info.framebuffer.format == NEXUS_FB_XBGR8888 && logged(" RGBX"));
    static const uint32_t padded[][3] = {{1024, 768, 1024}, {1920, 1080, 2048}};
    reset(); gop_init(&gpu, padded, 2, 0, PixelBitMask);
    gpu.info[0].red_mask = gpu.info[1].red_mask = 0xFF0000; gpu.info[0].green_mask = gpu.info[1].green_mask = 0xFF00; gpu.info[0].blue_mask = gpu.info[1].blue_mask = 0xFF;
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(gpu.mode.mode == 1 && info.framebuffer.pitch == 8192 && info.framebuffer.width == 1920 && info.framebuffer.format == NEXUS_FB_XRGB8888);
    assert(info.framebuffer.size == 8192ULL * 1080 && info.display.modes[1].pitch_pixels == 2048);
    reset(); gop_init(&gpu, padded, 2, 1, PixelBitMask);                  /* 10-bit masks: no usable mode */
    gpu.info[0].red_mask = gpu.info[1].red_mask = 0x3FF00000; gpu.info[0].green_mask = gpu.info[1].green_mask = 0xFFC00; gpu.info[0].blue_mask = gpu.info[1].blue_mask = 0x3FF;
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(gpu.sets == 0 && info.framebuffer.format == NEXUS_FB_OTHER);   /* the kernel refuses it: serial console */
    reset(); gop_init(&gpu, padded, 2, 1, PixelBltOnly); gpu.mode.frame_buffer_base = 0;
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(!info.framebuffer.format && !info.framebuffer.base && logged("no linear framebuffer"));
    puts("pixel layouts and padded scan lines passed");

    /* 8. SetMode fails: the current mode is kept and recorded; the firmware's mode is used without 1080p/EDID. */
    reset(); gop_init(&gpu, pc_modes, 6, 1, PixelBlueGreenRedReserved8BitPerColor); gpu.fail_set = true;
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(gpu.sets == 1 && info.framebuffer.width == 1024 && info.display.reason == NEXUS_MODE_KEPT && info.display.modes[1].flags == 1 && logged("mode switch failed"));
    static const uint32_t small[][3] = {{1024, 768, 0}, {1280, 800, 0}, {640, 480, 0}};
    reset(); gop_init(&gpu, small, 3, 1, PixelBlueGreenRedReserved8BitPerColor);
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(gpu.sets == 0 && info.framebuffer.width == 1280 && info.display.reason == NEXUS_MODE_FIRMWARE);
    reset(); gop_init(&gpu, small, 3, 0, PixelBlueGreenRedReserved8BitPerColor);
    gpu.info[0].pixel_format = PixelBltOnly;                              /* current mode unusable: the largest one */
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(gpu.mode.mode == 1 && info.display.reason == NEXUS_MODE_LARGEST);
    puts("SetMode failure and firmware/largest fallbacks passed");

    /* 9. More modes than the list holds; no GOP at all. */
    static uint32_t many[64][3];
    for (unsigned i = 0; i < 64; i++) { many[i][0] = 640 + 16 * i; many[i][1] = 480 + 8 * i; many[i][2] = 0; }
    many[60][0] = 1920; many[60][1] = 1080;
    reset(); gop_init(&gpu, (const uint32_t (*)[3])many, 64, 0, PixelBlueGreenRedReserved8BitPerColor);
    h = new_handle(); attach(h, &graphics_guid, &gpu.gop); run(h, "");
    assert(info.display.mode_count == NEXUS_BOOT_MODES_MAX && info.display.mode_total == 64 && gpu.mode.mode == 60 && info.framebuffer.width == 1920);
    reset(); h = new_handle(); run(h, "");
    assert(!info.framebuffer.format && logged("GOP: none"));
    puts("mode-list limits and no-GOP passed");
    puts("UEFI GOP TESTS PASSED");
    return 0;
}
