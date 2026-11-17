/* Encrypted root: unlock a LUKS1/LUKS2 volume at boot, before the root filesystem is mounted (kernel64_main calls
 * cryptroot_boot() after the USB controllers are up and no plain root was found), then look for a hibernation image
 * on it (kernel.c: the Linux initramfs order unlock -> resume -> mount).
 *
 * Which volume: cryptroot=<spec> (/dev/nvme0n1p3, UUID=<LUKS uuid>, PARTUUID=, LABEL=, PARTLABEL=, /dev/disk/by-*),
 * or rd.luks.uuid=<uuid> (systemd's form; the mapping is then luks-<uuid>); without either, the only LUKS volume on
 * a partition typed GPT "Linux LUKS" (CA7D7CCB-...) or NexusOS root. cryptroot.name=<mapping> (else NAME of
 * root=/dev/mapper/NAME, else cryptroot), cryptroot.tries=<n> (default 3), rootwait (keep looking for a USB disk for
 * 15 s). The prompt: a dialog on the framebuffer when there is one (the console stays off it meanwhile), else the
 * text console; keys from PS/2 and USB HID keyboards in the configured layout (keymaps.c: rd.vconsole.keymap= /
 * vconsole.keymap=), Backspace, Ctrl+U clears, Escape starts again. A wrong passphrase says so and asks again; after
 * the last try the machine powers off. Unsupported or corrupt metadata, or too little memory for the keyslot's Argon2,
 * is reported and not retried. On success /dev/mapper/<name> (dm-crypt over the data segment, the volume key only in
 * the kernel) becomes the root candidate that blockdev.c (FAT32) and ext4_dev.c (ext4) look at first.
 * crypt_unlock() is shared with /etc/crypttab (crypttab.c). Serial markers: "[64] CRYPTROOT ...". */
#include "native.h"
#include "boot_info.h"
#include "blockdev.h"
#include "luks.h"
#include "kcrypto.h"
#include "keymaps.h"
#include "cryptroot.h"
#include "storage_util.h"
#include "../../kernel/vga.h"
#include "../../kernel/font8x8.h"

const struct block_partition* block_unlocked_root;
const struct block_partition* cryptroot_volume;   /* the LUKS partition below it (osinstall.c boot_keymap_sync) */
int block_spec_matches(struct block_partition* p, const char* spec);
int console_read_secret(char* out, unsigned capacity, void (*changed)(unsigned length, void* ctx), void* ctx);
int console_prompt(const char* prompt, char* out, unsigned capacity, bool secret, uint64_t timeout_ns);
void usb_drive(void);
volatile uint32_t* framebuffer_pixels(void);
static const uint8_t luks_type[16] = {0xCB, 0x7C, 0x7D, 0xCA, 0xED, 0x63, 0x53, 0x4C, 0x86, 0x1C, 0x17, 0x42, 0x53, 0x60, 0x59, 0xCC};
static const uint8_t nexus_type[16] = BLOCK_GUID_NEXUS_ROOT;

/* ---- the volume ---- */
static struct block_partition* candidate(unsigned index) {   /* partitions, then whole disks */
    unsigned parts = block_partition_count();
    if (index < parts) return (struct block_partition*)block_partition_get(index);
    struct block_device* d = block_device_get(index - parts);
    return d && !d->removed ? block_whole(d) : NULL;
}
struct block_partition* crypt_find_volume(const char* spec, unsigned* count) {
    struct block_partition* found = NULL; *count = 0;
    unsigned total = block_partition_count() + block_device_count();
    for (unsigned i = 0; i < total; i++) {
        struct block_partition* p = candidate(i);
        if (!p || p->dev->removed || !block_part_sectors(p)) continue;
        if (spec) { if (block_spec_matches(p, spec) == 1 && luks_probe(p)) { found = p; (*count)++; break; } continue; }
        bool typed = p->scheme == BLOCK_SCHEME_GPT && (!memcmp(p->type_guid, luks_type, 16) || !memcmp(p->type_guid, nexus_type, 16));
        if (typed && luks_probe(p)) { if (!found) found = p; (*count)++; }
    }
    return found;
}

/* ---- the dialog ---- */
struct ui {
    volatile uint32_t* fb; uint32_t w, h, stride; bool bgr, on;
    const char* heading; char line[160]; const char* prefix; unsigned tries_left, length;
    char status[160]; uint32_t status_color;
};
static uint32_t px(const struct ui* u, uint32_t rgb) { return u->bgr ? (rgb & 0xFF00FF00U) | (rgb >> 16 & 0xFF) | (rgb & 0xFF) << 16 : rgb; }
static void fill(struct ui* u, int x, int y, int w, int h, uint32_t rgb) {
    uint32_t c = px(u, rgb);
    for (int j = y < 0 ? 0 : y; j < y + h && j < (int)u->h; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < (int)u->w; i++) u->fb[(uint64_t)j * u->stride + (uint32_t)i] = c;
}
static void text(struct ui* u, int x, int y, unsigned sx, unsigned sy, uint32_t rgb, const char* s) {   /* 8x8 glyphs scaled sx by sy */
    uint32_t c = px(u, rgb);
    for (; *s; s++, x += 8 * (int)sx)
        for (unsigned gy = 0; gy < 8 * sy; gy++) for (unsigned gx = 0; gx < 8 * sx; gx++)
            if (font8x8_data[(uint8_t)*s & 0x7F][gy / sy] & (0x80 >> (gx / sx))) {
                int X = x + (int)gx, Y = y + (int)gy;
                if (X >= 0 && Y >= 0 && X < (int)u->w && Y < (int)u->h) u->fb[(uint64_t)Y * u->stride + (uint32_t)X] = c;
            }
}
static void draw(struct ui* u) {
    if (!u->on) return;
    int k = u->w >= 2400 ? 3 : u->w >= 1600 ? 2 : 1;   /* HiDPI: everything k times larger */
    int pw = 640 * k, ph = 300 * k; if (pw > (int)u->w - 16) pw = (int)u->w - 16;
    int x = ((int)u->w - pw) / 2, y = ((int)u->h - ph) / 2, pad = 28 * k;
    fill(u, 0, 0, (int)u->w, (int)u->h, 0x0E1220);
    fill(u, x - 1, y - 1, pw + 2, ph + 2, 0x3A4660); fill(u, x, y, pw, ph, 0x1B2130);
    fill(u, x, y, pw, 5 * k, 0x5B8CFF);
    text(u, x + pad, y + 28 * k, (unsigned)(3 * k), (unsigned)(3 * k), 0xF2F4F8, u->heading);
    text(u, x + pad, y + 72 * k, (unsigned)k, (unsigned)(2 * k), 0xB8C2D6, u->line);
    text(u, x + pad, y + 94 * k, (unsigned)k, (unsigned)(2 * k), 0xB8C2D6, "Enter its passphrase and press Enter.");
    int fx = x + pad, fy = y + 128 * k, fw = pw - 2 * pad, fh = 40 * k;
    fill(u, fx - 2 * k, fy - 2 * k, fw + 4 * k, fh + 4 * k, 0x5B8CFF); fill(u, fx, fy, fw, fh, 0x0D1018);
    unsigned dots = u->length, max = (unsigned)((fw - 24 * k) / (16 * k));
    for (unsigned i = 0; i < dots && i < max; i++) fill(u, fx + 12 * k + 16 * k * (int)i, fy + fh / 2 - 5 * k, 10 * k, 10 * k, 0xE6ECF5);
    if (!dots) text(u, fx + 12 * k, fy + fh / 2 - 8 * k, (unsigned)k, (unsigned)(2 * k), 0x5E6880, "passphrase");
    text(u, x + pad, y + 196 * k, (unsigned)k, (unsigned)(2 * k), u->status_color ? u->status_color : 0x8FA0BF, u->status);
    char foot[120] = "Keyboard: "; strcat(foot, keymap_title()); strcat(foot, ".   Attempts left: "); append_u64(foot, u->tries_left);
    text(u, x + pad, y + ph - 36 * k, (unsigned)k, (unsigned)(2 * k), 0x6F7C96, foot);
}
static void changed(unsigned length, void* ctx) { struct ui* u = ctx; u->length = length; draw(u); }
static void status(struct ui* u, const char* message, uint32_t color) {
    strncpy(u->status, message, sizeof(u->status) - 1); u->status[sizeof(u->status) - 1] = 0; u->status_color = color;
    vga_print("[64] "); vga_print(u->prefix); vga_print(" "); vga_print(message); vga_print("\n");
    draw(u);
}
static const char* failure(int error) {
    return error == -12 ? "Not enough memory for this keyslot's key derivation (Argon2)." :
           error == -95 ? "This LUKS volume uses a cipher, hash or feature NexusOS cannot open." :
           error == -5 ? "The disk could not be read." : "The LUKS metadata is damaged.";
}

/* Unlock `volume` as /dev/mapper/<name>: with r->key (a keyfile's bytes) silently, else from the prompt (r->tries
 * passphrases). 0 opened (*dev), -1 wrong passphrase(s)/key, else -errno (reported). */
int crypt_unlock(const struct block_partition* volume, const char* name, const struct crypt_request* r, struct block_device** dev) {
    *dev = NULL;
    if (!sha256_selftest() || !argon2_selftest()) { vga_print("[64] "); vga_print(r->prefix); vga_print(" FAIL: crypto self-test failed\n"); return -5; }
    int version = luks_probe(volume);
    if (r->key) {
        struct luks_volume v; uint64_t t0 = native_clock_ns();
        int error = luks_unlock(volume, (const char*)r->key, r->key_bytes, &v);
        if (!error) error = luks_activate_flags(volume, &v, name, r->flags, dev);
        crypto_wipe(&v, sizeof(v));
        vga_print("[64] "); vga_print(r->prefix);
        if (!error) { vga_print(" UNLOCKED with its keyfile in "); char n[24] = ""; append_u64(n, (native_clock_ns() - t0) / 1000000); vga_print(n); vga_print(" ms\n"); }
        else { vga_print(error == -1 ? " the keyfile opens no keyslot\n" : " "); if (error != -1) { vga_print(failure(error)); vga_print("\n"); } }
        return error;
    }
    struct ui u; memset(&u, 0, sizeof(u)); u.prefix = r->prefix; u.heading = r->heading; u.tries_left = r->tries ? r->tries : 3;
    strcpy(u.line, r->intro); strcat(u.line, " (LUKS"); append_u64(u.line, (uint64_t)version); strcat(u.line, ", "); strcat(u.line, volume->linux_name); strcat(u.line, ").");
    const struct framebuffer_info* fb = framebuffer_get();
    if (fb && framebuffer_pixels() && fb->width >= 640 && fb->height >= 400 && (fb->format == NEXUS_FB_XRGB8888 || fb->format == NEXUS_FB_XBGR8888)) {
        u.on = true; u.fb = framebuffer_pixels(); u.w = fb->width; u.h = fb->height; u.stride = fb->stride / 4;
        u.bgr = fb->format == NEXUS_FB_XBGR8888; framebuffer_retain();   /* the console stays off the screen meanwhile */
    }
    vga_print("[64] "); vga_print(r->prefix); vga_print(" volume "); vga_print(volume->linux_name); vga_print(version == 2 ? " (LUKS2)" : " (LUKS1)");
    vga_print(" mapping /dev/mapper/"); vga_print(name); vga_print(u.on ? ", graphical prompt" : ", console prompt");
    vga_print(", keyboard "); vga_print(keymap_name()); vga_print("\n");
    status(&u, "Waiting for the passphrase.", 0);
    char passphrase[512]; int error = -1;
    while (u.tries_left) {
        int n;
        vga_print("[64] "); vga_print(r->prefix); vga_print(" PROMPT\n");
        if (u.on) { u.length = 0; draw(&u); n = console_read_secret(passphrase, sizeof(passphrase), changed, &u); }
        else n = console_prompt(r->console_prompt, passphrase, sizeof(passphrase), true, 0);
        if (n < 0) { status(&u, "Cancelled: type the passphrase again.", 0xF2C14E); continue; }
        u.length = 0; status(&u, "Unlocking... (the key derivation takes a few seconds)", 0x8FD3A8);
        struct luks_volume v; uint64_t t0 = native_clock_ns();
        error = luks_unlock(volume, passphrase, (unsigned)n, &v);
        crypto_wipe(passphrase, sizeof(passphrase));
        if (!error) {
            error = luks_activate_flags(volume, &v, name, r->flags, dev);
            crypto_wipe(&v, sizeof(v));
            if (!error) {
                char line[120] = "UNLOCKED keyslot in "; append_u64(line, (native_clock_ns() - t0) / 1000000); strcat(line, " ms: /dev/mapper/"); strcat(line, name);
                status(&u, line, 0x8FD3A8); break;
            }
            status(&u, "The volume opened, but its mapping could not be created.", 0xFF6B6B); break;
        }
        crypto_wipe(&v, sizeof(v));
        if (error == -1) { u.tries_left--; status(&u, u.tries_left ? "Wrong passphrase. Try again." : "Wrong passphrase.", 0xFF6B6B); continue; }
        status(&u, failure(error), 0xFF6B6B);
        break;
    }
    if (error && r->fatal) {
        status(&u, error == -1 ? "Too many failed attempts: powering off in 10 seconds." : "The encrypted disk cannot be opened: powering off in 10 seconds.", 0xFF6B6B);
        for (uint64_t until = native_clock_ns() + 10000000000ULL; native_clock_ns() < until; ) __asm__ volatile("pause");
        vga_print("[64] "); vga_print(r->prefix); vga_print(" POWER OFF\n");
        block_flush_all(); platform_shutdown(0x10);   /* ACPI S5 (isa-debug-exit under QEMU tests) */
    }
    if (u.on) { fill(&u, 0, 0, (int)u.w, (int)u.h, 0x101522); framebuffer_release(); }
    return error;
}

static bool lvm_style(const char* name) {   /* <vg>-<lv> (a single '-'): root= names an LV on the volume, not the mapping */
    for (const char* p = name; *p; p++) { if (p[0] == '-' && p[1] == '-') { p++; continue; } if (*p == '-') return true; }
    return false;
}
bool cryptroot_boot(void) {
    char spec[96], name[64] = "cryptroot", value[80]; bool given = false;
    if (boot_value("cryptroot", spec, sizeof(spec)) && spec[0]) given = true;
    else if (boot_value("rd.luks.uuid", value, sizeof(value)) && value[0]) {
        const char* uuid = !strncmp(value, "luks-", 5) ? value + 5 : value;
        strcpy(spec, "UUID="); s_strncat(spec, uuid, sizeof(spec) - 6); strcpy(name, "luks-"); s_strncat(name, uuid, sizeof(name) - 6); given = true;
    }
    if (boot_value("cryptroot.name", value, sizeof(value)) && value[0] && strlen(value) < sizeof(name)) strcpy(name, value);
    else if (boot_value("root", value, sizeof(value)) && !strncmp(value, "/dev/mapper/", 12) && value[12] &&
             strlen(value + 12) < sizeof(name) && !s_strchr(value + 12, '/') && !lvm_style(value + 12)) strcpy(name, value + 12);   /* root=/dev/mapper/NAME */
    /* root= naming a plain device (a USB stick's own root, waited for later) wins over guessing a LUKS volume;
     * root=/dev/<vg>/<lv> (LVM, maybe on LUKS) does not */
    if (!given && boot_value("root", value, sizeof(value)) && value[0] && strncmp(value, "/dev/mapper/", 12)) {
        const char* slash = !strncmp(value, "/dev/", 5) && strncmp(value, "/dev/disk/", 10) ? s_strchr(value + 5, '/') : NULL;
        if (!slash || s_strchr(slash + 1, '/')) return false;
    }
    unsigned count = 0;
    struct block_partition* volume = crypt_find_volume(given ? spec : NULL, &count);
    for (uint64_t start = native_clock_ns(); !volume && given && boot_option("rootwait") && native_clock_ns() - start < 15000000000ULL; ) {
        for (uint64_t until = native_clock_ns() + 50000000ULL; native_clock_ns() < until; ) { usb_drive(); __asm__ volatile("pause"); }
        volume = crypt_find_volume(spec, &count);
    }
    if (!volume) { if (given) { vga_print("[64] CRYPTROOT no LUKS volume matches cryptroot="); vga_print(spec); vga_print("\n"); } return false; }
    if (!given && count > 1) { vga_print("[64] CRYPTROOT several LUKS volumes and no cryptroot=: not guessing\n"); return false; }
    keymap_configure(false);   /* the root's files are not readable yet: the boot command line says */
    unsigned tries = boot_option_number("cryptroot.tries="); if (!tries || tries > 100) tries = 3;
    struct crypt_request r = {.prefix = "CRYPTROOT", .heading = "Unlock disk", .intro = "NexusOS is on an encrypted disk",
                              .console_prompt = "Passphrase for the encrypted disk: ", .tries = tries, .fatal = true};
    struct block_device* dev = NULL;
    if (crypt_unlock(volume, name, &r, &dev)) return false;
    block_unlocked_root = block_whole(dev); cryptroot_volume = volume;
    return true;
}
