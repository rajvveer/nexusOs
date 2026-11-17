/* /etc/crypttab at boot (kernel.c, after the root is mounted and before /etc/fstab, swap and init: what
 * systemd-cryptsetup-generator and cryptsetup do on Linux). Lines: <name> <device> [<keyfile>|none|-] [<options>].
 *   device   UUID=<LUKS uuid>, PARTUUID=, LABEL=, PARTLABEL=, /dev/... (a LUKS1/LUKS2 volume, crypt_find_volume)
 *   keyfile  an absolute path on the root: its bytes are the key (cryptsetup --key-file); on a POSIX root (ext4) it
 *            must be a regular file owned by root with no group/other permission bits, else it is refused; FAT32
 *            has no ownership or permissions (a warning). none, - or absent: the passphrase prompt (cryptroot.c's
 *            dialog, the configured keyboard layout, tries= attempts; a failure leaves the volume closed).
 *   options  luks (the default), discard (dm-crypt allow_discards), nofail (a missing device is no error and is
 *            not waited for; without it the device gets 10 s to appear), noauto (skipped), readonly/read-only,
 *            tries=N (default 3), keyfile-size=N, keyfile-offset=N; plain, tcrypt, bitlk, swap, tmp, header=,
 *            keyscript= are not supported (the line is skipped); x-systemd.*, _netdev, initramfs, timeout= ignored.
 * The root's own line (the mapping cryptroot.c already opened, or its LUKS partition) is skipped. Serial markers:
 * "[64] CRYPTTAB <name> ..." (PROMPT, UNLOCKED, ready, FAILED, skipped). */
#include "native.h"
#include "vfs.h"
#include "luks.h"
#include "kcrypto.h"
#include "keymaps.h"
#include "cryptroot.h"
#include "storage_util.h"
#include "../../kernel/vga.h"

bool dm_name_exists(const char* name);
void usb_drive(void);
#define KEYFILE_MAX 65536u

static void say(const char* name, const char* a, const char* b) {
    vga_print("[64] CRYPTTAB "); vga_print(name); vga_print(" "); vga_print(a); if (b) vga_print(b); vga_print("\n");
}
static bool number(const char* text, uint64_t* out) { const char* end; return parse_u64(text, out, &end) && !*end; }
static bool valid_name(const char* n) {
    unsigned i = 0;
    for (; n[i]; i++) if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= 'A' && n[i] <= 'Z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '-' || n[i] == '_' || n[i] == '.')) return false;
    return i && i < 64;
}
/* the keyfile's bytes (offset/size applied) into an arena buffer; NULL with the reason reported */
static uint8_t* read_keyfile(const char* name, const char* path, uint64_t offset, uint64_t size, unsigned* bytes) {
    fat32_file_t f;
    if (path[0] != '/') { say(name, "FAILED: the keyfile must be an absolute path on the root: ", path); return NULL; }
    if (!disk_lookup(path, &f) || f.is_dir) { say(name, "FAILED: no keyfile ", path); return NULL; }
    if (f.ino) {   /* POSIX root: ownership and permission bits (S_IFMT 0170000, regular 0100000) */
        if ((f.mode & 0170000) != 0100000 || f.uid != 0 || (f.mode & 077)) {
            char m[5] = {(char)('0' + (f.mode >> 9 & 7)), (char)('0' + (f.mode >> 6 & 7)), (char)('0' + (f.mode >> 3 & 7)), (char)('0' + (f.mode & 7)), 0};
            vga_print("[64] CRYPTTAB "); vga_print(name); vga_print(" FAILED: keyfile "); vga_print(path);
            vga_print(f.uid ? " is not owned by root" : (f.mode & 0170000) != 0100000 ? " is not a regular file" : " is readable by group or others");
            vga_print(" (mode "); vga_print(m); vga_print("): refused\n");
            return NULL;
        }
    } else say(name, "warning: FAT32 has no file permissions, every local user can read the keyfile ", path);
    if (offset >= f.size && f.size) { say(name, "FAILED: keyfile-offset is beyond the keyfile ", path); return NULL; }
    uint64_t n = f.size - offset; if (size && size < n) n = size;
    if (!n || n > KEYFILE_MAX) { say(name, n ? "FAILED: the keyfile is larger than 64 KiB (use keyfile-size=) " : "FAILED: the keyfile is empty ", path); return NULL; }
    uint8_t* key = (void*)arena_alloc(n);
    if (!key) { say(name, "FAILED: out of memory", NULL); return NULL; }
    if (disk_read_file(&f, offset, n, key) != (int32_t)n) { crypto_wipe(key, n); arena_free((uint64_t)key, n); say(name, "FAILED: the keyfile cannot be read ", path); return NULL; }
    *bytes = (unsigned)n; return key;
}
static void open_line(char** fields, unsigned n, bool* layout_ready) {
    const char* name = fields[0], *device = fields[1], *keyfile = n > 2 ? fields[2] : "none";
    char options[256] = "luks"; if (n > 3) { options[0] = 0; s_strncat(options, fields[3], sizeof(options) - 1); }
    if (!valid_name(name)) { vga_print("[64] CRYPTTAB invalid mapping name '"); vga_print(name); vga_print("': line skipped\n"); return; }
    bool nofail = false, noauto = false; unsigned tries = 3, flags = 0; uint64_t key_offset = 0, key_size = 0;
    for (char* cursor = options; *cursor; ) {
        char* option = cursor; while (*cursor && *cursor != ',') cursor++;
        if (*cursor) *cursor++ = 0;
        char* value = s_strchr(option, '='); if (value) *value++ = 0;
        uint64_t v;
        if (!strcmp(option, "luks") || !option[0]) continue;
        if (!strcmp(option, "discard")) flags |= LUKS_DISCARD;
        else if (!strcmp(option, "nofail")) nofail = true;
        else if (!strcmp(option, "noauto")) noauto = true;
        else if (!strcmp(option, "readonly") || !strcmp(option, "read-only")) flags |= LUKS_READONLY;
        else if (!strcmp(option, "tries") && value && number(value, &v) && v <= 100) tries = v ? (unsigned)v : 100;
        else if (!strcmp(option, "keyfile-size") && value && number(value, &v) && v && v <= KEYFILE_MAX) key_size = v;
        else if (!strcmp(option, "keyfile-offset") && value && number(value, &v)) key_offset = v;
        else if (!strcmp(option, "plain") || !strcmp(option, "tcrypt") || !strcmp(option, "bitlk") || !strcmp(option, "swap") ||
                 !strcmp(option, "tmp") || !strcmp(option, "header") || !strcmp(option, "keyscript") || !strcmp(option, "luks2-token")) {
            say(name, "skipped: option not supported: ", option); return;
        } else if (strncmp(option, "x-systemd.", 10) && strcmp(option, "_netdev") && strcmp(option, "initramfs") && strcmp(option, "timeout") &&
                   strcmp(option, "luks1") && strcmp(option, "luks2") && strcmp(option, "noearly") && strcmp(option, "headless")) say(name, "option ignored: ", option);
    }
    if (noauto) { say(name, "skipped (noauto)", NULL); return; }
    if (dm_name_exists(name)) { say(name, "skipped: /dev/mapper/", name); return; }
    unsigned count;
    struct block_partition* volume = crypt_find_volume(device, &count);
    for (uint64_t start = native_clock_ns(); !volume && !nofail && native_clock_ns() - start < 10000000000ULL; ) {   /* a USB disk may still come */
        for (uint64_t until = native_clock_ns() + 50000000ULL; native_clock_ns() < until; ) { usb_drive(); __asm__ volatile("pause"); }
        volume = crypt_find_volume(device, &count);
    }
    if (!volume) { say(name, nofail ? "not present (nofail): " : "FAILED: no LUKS volume ", device); return; }
    if (volume == cryptroot_volume) { say(name, "skipped: the root's volume, already open", NULL); return; }
    struct crypt_request r = {.prefix = NULL, .heading = "Unlock volume", .intro = "The encrypted volume in /etc/crypttab",
                              .console_prompt = "Passphrase for an encrypted volume: ", .tries = tries, .flags = flags};
    char prefix[80] = "CRYPTTAB "; strcat(prefix, name); r.prefix = prefix;
    char intro[120] = "/etc/crypttab: the encrypted volume "; strcat(intro, name); r.intro = intro;
    uint8_t* key = NULL; unsigned key_bytes = 0;
    if (strcmp(keyfile, "none") && strcmp(keyfile, "-")) {
        key = read_keyfile(name, keyfile, key_offset, key_size, &key_bytes);
        if (!key) return;
        r.key = key; r.key_bytes = key_bytes;
    } else if (!*layout_ready) { keymap_configure(true); *layout_ready = true; }
    struct block_device* dev = NULL;
    int e = crypt_unlock(volume, name, &r, &dev);
    if (key) { crypto_wipe(key, key_bytes); arena_free((uint64_t)key, key_bytes); }
    if (e) { say(name, "FAILED: not opened", NULL); return; }
    vga_print("[64] CRYPTTAB "); vga_print(name); vga_print(" ready: /dev/mapper/"); vga_print(name);
    if (flags & LUKS_DISCARD) vga_print(" (discard)");
    if (flags & LUKS_READONLY) vga_print(" (read-only)");
    vga_print("\n");
}
void crypttab_boot(void) {
    fat32_file_t f;
    if (!disk_lookup("/etc/crypttab", &f) || f.is_dir || !f.size) return;
    if (f.size > 65536) { vga_print("[64] CRYPTTAB /etc/crypttab is larger than 64 KiB: ignored\n"); return; }
    char* text = (void*)arena_alloc(f.size + 1); if (!text) return;
    if (disk_read_file(&f, 0, f.size, text) != (int32_t)f.size) { arena_free((uint64_t)text, f.size + 1); return; }
    text[f.size] = 0;
    bool layout_ready = false;
    for (char* line = text; line && *line; ) {
        char* end = s_strchr(line, '\n'); if (end) *end = 0;
        char* fields[4] = {0}; unsigned n = 0; char* p = line;
        while (*p && n < 4) {
            while (*p == ' ' || *p == '\t' || *p == '\r') p++;
            if (!*p || *p == '#') break;
            fields[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
            if (*p) *p++ = 0;
        }
        line = end ? end + 1 : NULL;
        if (n >= 2) open_line(fields, n, &layout_ready);
        else if (n == 1) { vga_print("[64] CRYPTTAB line without a device: "); vga_print(fields[0]); vga_print("\n"); }
    }
    crypto_wipe(text, f.size + 1); arena_free((uint64_t)text, f.size + 1);
}
