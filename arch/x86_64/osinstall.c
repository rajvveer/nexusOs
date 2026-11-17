/* In-kernel installer: copies a NexusOS system booted from USB (build.py usb-image layout) to another disk,
 * typically the internal NVMe drive, so the machine boots without the stick (BIOS and UEFI).
 *
 * Request: `nexus-os-install DISK` (osinstall_user.c) checks the target and writes REQUEST on the live system,
 * then the machine restarts; or the kernel option nexus.install=DISK. The copy runs at the next boot right after
 * the root filesystem is mounted and before any process starts, so the live root is quiescent (nothing is written
 * while it is copied) and the copy is consistent. The request is removed first (the copy never re-installs).
 * Conditions: the root is on a hot-plugged USB disk with the usb-image layout (GPT: BIOS boot partition carrying
 * the "NXBOOTD1" descriptor in its last sector, an EFI System Partition, the root partition); the target is
 * another disk with 512-byte sectors, large enough, with nothing mounted or open; on the console the user types
 * YES (60 s; the option nexus.install.yes skips this, for automated tests).
 * Copy: protective MBR with the boot code, a new GPT (new disk and partition GUIDs) at the same partition
 * offsets, the BIOS boot partition, the ESP, and the root (a FAT32 root: reserved area, FATs and the allocated
 * clusters only; other filesystems: every sector). The target's kernel command line (stage 2 copy and
 * EFI/NEXUS/CMDLINE.TXT, located through the descriptor) becomes root=PARTUUID=<new root GUID> plus the
 * options of this boot other than root=, rootwait and nexus.install*. Everything is read back and checked.
 * Then: nexus.install.poweroff powers off (tests); otherwise Enter restarts, Escape continues the live session.
 * Encrypt disk (`nexus-os-install DISK --encrypt`, request line encrypt=1, or the option nexus.install.encrypt): after
 * YES the passphrase is chosen on the console (twice, 8+ characters); the target's root partition is 16 MiB larger and
 * typed "Linux LUKS", becomes a LUKS2 volume (luks.c: argon2id with an eighth of the RAM up to 1 GiB, aes-xts-plain64
 * with a random 512-bit volume key, cryptsetup-compatible), and the root is copied through a temporary dm-crypt
 * mapping; the command line is cryptroot=PARTUUID=<root> root=/dev/mapper/cryptroot (cryptroot.c asks at boot). */
#include "native.h"
#include "boot_info.h"
#include "vfs.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define REQUEST "/home/nexus/.nexus-os-install"
#define CHUNK_SECTORS 2048u
#define LUKS_EXTRA 32768u   /* --encrypt: the target root partition is 16 MiB larger (LUKS2 header + keyslots) */
int console_prompt(const char* prompt, char* out, unsigned capacity, bool secret, uint64_t timeout_ns);
int block_rescan(struct block_device* dev);
#include "luks.h"
#include "kcrypto.h"
#include "keymaps.h"
#include "cryptroot.h"
int dm_remove_name(const char* name);
uint64_t memory_total_pages(void);

static const uint8_t bios_guid[16] = {0x48,0x61,0x68,0x21,0x49,0x64,0x6F,0x6E,0x74,0x4E,0x65,0x65,0x64,0x45,0x46,0x49};
static const uint8_t esp_guid[16] = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};

struct layout {
    struct block_device* source; const struct block_partition *bios, *esp, *root;
    uint32_t cmdline_sector;   /* ESP-relative sector of CMDLINE.TXT's data (descriptor) */
};
static void say_number(const char* text, uint64_t value) { char d[24] = ""; vga_print(text); append_u64(d, value); vga_print(d); }
static void guid_text(const uint8_t* g, char* out) {
    static const uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    out[0] = 0;
    for (unsigned i = 0; i < 16; i++) { if (i == 4 || i == 6 || i == 8 || i == 10) strcat(out, "-"); append_hex(out, g[order[i]], 2); }
}
static const struct block_partition* root_partition(void) {
    const struct block_partition* p = block_root();
    if (p) return p;
    for (unsigned i = 0; i < block_partition_count(); i++) {   /* ext4 root: the listed partition the driver holds */
        const struct block_partition* q = block_partition_get(i);
        if (q && q->holders && q->number) return q;
    }
    return NULL;
}
static bool find_layout(struct layout* l, uint8_t* sector) {
    memset(l, 0, sizeof(*l));
    l->root = root_partition();
    if (!l->root || l->root->scheme != BLOCK_SCHEME_GPT) return false;
    l->source = l->root->dev;
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        if (!p || p->dev != l->source || p->scheme != BLOCK_SCHEME_GPT) continue;
        if (!memcmp(p->type_guid, bios_guid, 16)) l->bios = p;
        else if (!memcmp(p->type_guid, esp_guid, 16)) l->esp = p;
    }
    if (!l->bios || !l->esp || l->source->block_size != 512 || block_part_sectors(l->bios) < 2048) return false;
    if (block_part_read(l->bios, block_part_sectors(l->bios) - 1, 1, sector) || memcmp(sector, "NXBOOTD1", 8)) return false;
    l->cmdline_sector = get_le32(sector + 12);
    return l->cmdline_sector < block_part_sectors(l->esp);
}
/* "target=nvme0n1\nserial=...\n" from the request file, or the nexus.install= option */
static bool request(char* target, unsigned capacity, char* serial, unsigned serial_capacity, bool* from_file, bool* encrypt) {
    serial[0] = 0; *from_file = false; *encrypt = boot_option("nexus.install.encrypt");
    if (boot_value("nexus.install", target, capacity) && target[0]) return true;
    fat32_file_t f;
    if (!disk_lookup(REQUEST, &f) || f.is_dir || !f.size || f.size > 1024) return false;
    char text[1025]; int32_t n = disk_read_file(&f, 0, f.size, text);
    if (n <= 0) return false;
    text[n] = 0; *from_file = true; target[0] = 0;
    for (char* line = text; *line; ) {
        char* end = line; while (*end && *end != '\n') end++;
        char saved = *end; *end = 0;
        if (!strncmp(line, "target=", 7)) { strncpy(target, line + 7, capacity - 1); target[capacity - 1] = 0; }
        if (!strncmp(line, "serial=", 7)) { strncpy(serial, line + 7, serial_capacity - 1); serial[serial_capacity - 1] = 0; }
        if (!strcmp(line, "encrypt=1")) *encrypt = true;
        line = saved ? end + 1 : end;
    }
    if (!strncmp(target, "/dev/", 5)) for (unsigned i = 0; (target[i] = target[i + 5]) != 0; i++) {}
    return target[0] != 0;
}
static bool busy(struct block_device* d) {
    if (d->holders) return true;
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        if (p && p->dev == d && (p->holders || p->opens)) return true;
    }
    return false;
}
/* the options of this boot without root=, rootwait and nexus.install*, after root=PARTUUID=<guid> */
static void target_cmdline(const char* guid, bool encrypted, char* out, unsigned capacity) {
    if (encrypted) { strcpy(out, "cryptroot=PARTUUID="); strcat(out, guid); strcat(out, " root=/dev/mapper/cryptroot"); }   /* cryptroot.c */
    else { strcpy(out, "root=PARTUUID="); strcat(out, guid); }
    for (const char* p = boot_info.cmdline; *p; ) {
        while (*p == ' ') p++;
        const char* start = p; while (*p && *p != ' ') p++;
        unsigned n = (unsigned)(p - start);
        if (!n || (n >= 5 && !memcmp(start, "root=", 5)) || (n == 8 && !memcmp(start, "rootwait", 8)) || (n >= 13 && !memcmp(start, "nexus.install", 13)) ||
            (n >= 9 && !memcmp(start, "cryptroot", 9))) continue;
        if (strlen(out) + n + 2 >= capacity) break;
        strcat(out, " "); unsigned at = strlen(out); memcpy(out + at, start, n); out[at + n] = 0;
    }
    /* an encrypted root's prompt cannot read the root's keyboard configuration: the command line carries it */
    if (encrypted && !strstr(out, "vconsole.keymap=") && strcmp(keymap_name(), "us") && strlen(out) + strlen(keymap_name()) + 18 < capacity) {
        strcat(out, " vconsole.keymap="); strcat(out, keymap_name());
    }
}
static int copy_range(const struct block_partition* from, struct block_device* to, uint64_t to_lba, uint64_t first, uint64_t count, uint8_t* buffer) {
    while (count) {
        uint32_t n = count > CHUNK_SECTORS ? CHUNK_SECTORS : (uint32_t)count;
        if (block_part_read(from, first, n, buffer)) return -5;
        if (block_write(to, to_lba + first, n, buffer)) return -5;
        first += n; count -= n;
    }
    return 0;
}
/* A FAT32 root: reserved sectors and FATs, then only the allocated clusters (runs). Returns sectors copied or -errno. */
static int64_t copy_fat32(const struct block_partition* root, struct block_device* to, uint64_t to_lba, uint8_t* buffer, bool* fat) {
    uint8_t bs[512]; *fat = false;
    if (block_part_read(root, 0, 1, bs)) return -5;
    uint16_t bytes = get_le16(bs + 11), reserved = get_le16(bs + 14); uint8_t per = bs[13], fats = bs[16];
    uint32_t total = get_le32(bs + 32), fat_size = get_le32(bs + 36);
    if (bs[510] != 0x55 || bs[511] != 0xAA || bytes != 512 || !per || (per & (per - 1)) || !reserved || !fats || fats > 2 || !fat_size ||
        get_le16(bs + 22) || !total || total > block_part_sectors(root)) return 0;   /* not FAT32: caller copies everything */
    *fat = true;
    uint16_t fs_info = get_le16(bs + 48);
    uint64_t data = reserved + (uint64_t)fats * fat_size, clusters = (total - data) / per, copied = data, free_count = 0;
    if (copy_range(root, to, to_lba, 0, data, buffer)) return -5;
    uint64_t run_start = 0, run_length = 0, last_percent = 0;
    for (uint64_t c = 2; c < clusters + 2; ) {
        uint64_t fat_sector = reserved + c * 4 / 512;
        if (block_part_read(root, fat_sector, 1, bs)) return -5;
        for (unsigned k = (unsigned)(c * 4 % 512) / 4; k < 128 && c < clusters + 2; k++, c++) {
            bool used = (get_le32(bs + k * 4) & 0x0FFFFFFF) != 0;
            if (!used) free_count++;
            if (used && run_length && run_start + run_length == c && run_length < CHUNK_SECTORS / per) { run_length++; continue; }
            if (run_length) {
                if (copy_range(root, to, to_lba, data + (run_start - 2) * per, run_length * per, buffer)) return -5;
                copied += run_length * per; run_length = 0;
            }
            if (used) { run_start = c; run_length = 1; }
        }
        uint64_t percent = c * 100 / (clusters + 2);
        if (percent >= last_percent + 10) { last_percent = percent; say_number("[64] INSTALL root ", percent); vga_print("%\n"); }
    }
    if (run_length) { if (copy_range(root, to, to_lba, data + (run_start - 2) * per, run_length * per, buffer)) return -5; copied += run_length * per; }
    /* a consistent copy is a clean volume (FAT[1] bit 27 in every FAT copy) */
    for (unsigned k = 0; k < fats; k++) {
        uint64_t lba = to_lba + reserved + (uint64_t)k * fat_size;   /* to_lba is where the root starts on the target */
        if (block_read(to, lba, 1, bs)) return -5;
        put_le32(bs + 4, get_le32(bs + 4) | 0x08000000);
        if (block_write(to, lba, 1, bs)) return -5;
    }
    /* FSInfo of the copy: the free count of the FAT just copied (the live root's on-disk hint may be older) */
    if (fs_info && fs_info < reserved && !block_read(to, to_lba + fs_info, 1, bs) && get_le32(bs) == 0x41615252 &&
        get_le32(bs + 484) == 0x61417272 && get_le32(bs + 508) == 0xAA550000) {
        put_le32(bs + 488, (uint32_t)free_count); put_le32(bs + 492, 0xFFFFFFFFU);
        if (block_write(to, to_lba + fs_info, 1, bs)) return -5;
    }
    return (int64_t)copied;
}
static bool write_gpt(struct layout* l, struct block_device* to, uint64_t total, uint8_t* buffer, uint8_t root_guid[16], bool encrypt) {
    static const uint8_t luks_type[16] = {0xCB, 0x7C, 0x7D, 0xCA, 0xED, 0x63, 0x53, 0x4C, 0x86, 0x1C, 0x17, 0x42, 0x53, 0x60, 0x59, 0xCC};
    uint8_t* header = buffer; uint8_t* entries = buffer + 512;
    if (block_read(l->source, 1, 1, header) || memcmp(header, "EFI PART", 8)) return false;
    uint32_t count = get_le32(header + 80), size = get_le32(header + 84);
    uint64_t entries_lba = get_le64(header + 72);
    if (size != 128 || !count || count > 128 || entries_lba != 2) return false;
    if (block_read(l->source, 2, 32, entries)) return false;
    uint64_t last = total - 34; bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* e = entries + i * 128;
        static const uint8_t zero[16];
        if (!memcmp(e, zero, 16)) continue;
        if (!native_random(e + 16, 16)) return false;
        e[16 + 7] = (uint8_t)((e[16 + 7] & 0x0F) | 0x40); e[16 + 8] = (uint8_t)((e[16 + 8] & 0x3F) | 0x80);   /* RFC 4122 v4 */
        if (get_le64(e + 32) == l->root->start) {
            memcpy(root_guid, e + 16, 16); found = true;
            if (encrypt) { memcpy(e, luks_type, 16); put_le64(e + 40, get_le64(e + 40) + LUKS_EXTRA); }   /* "Linux LUKS", 16 MiB more */
        }
        if (get_le64(e + 40) > last) return false;
    }
    if (!found) return false;
    uint8_t disk_guid[16]; if (!native_random(disk_guid, 16)) return false;
    disk_guid[7] = (uint8_t)((disk_guid[7] & 0x0F) | 0x40); disk_guid[8] = (uint8_t)((disk_guid[8] & 0x3F) | 0x80);
    uint32_t entries_crc = block_crc32(entries, 128 * 128);
    for (unsigned copy = 0; copy < 2; copy++) {
        uint8_t h[512]; memset(h, 0, sizeof(h));
        memcpy(h, "EFI PART", 8); put_le32(h + 8, 0x00010000); put_le32(h + 12, 92);
        put_le64(h + 24, copy ? total - 1 : 1); put_le64(h + 32, copy ? 1 : total - 1);
        put_le64(h + 40, 34); put_le64(h + 48, last); memcpy(h + 56, disk_guid, 16);
        put_le64(h + 72, copy ? total - 33 : 2); put_le32(h + 80, 128); put_le32(h + 84, 128); put_le32(h + 88, entries_crc);
        put_le32(h + 16, block_crc32(h, 92));
        if (block_write(to, copy ? total - 33 : 2, 32, entries) || block_write(to, copy ? total - 1 : 1, 1, h)) return false;
    }
    uint8_t mbr[512];   /* protective MBR: this disk's boot code, a new signature */
    if (block_read(l->source, 0, 1, mbr)) return false;
    memset(mbr + 440, 0, 72);
    if (!native_random(mbr + 440, 4)) return false;
    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00; mbr[446 + 4] = 0xEE; mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put_le32(mbr + 446 + 8, 1); put_le32(mbr + 446 + 12, total - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)(total - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    return block_write(to, 0, 1, mbr) == 0;
}
static void finish(bool poweroff) {
    memory_file_writeback(true, 0, ~0ULL); block_flush_all();
    if (poweroff) { disk_shutdown(); block_report(); platform_shutdown(0x10); }
    char answer[4];
    int r = console_prompt("Remove the USB drive, then press Enter to restart (Escape: keep using the live system): ", answer, sizeof(answer), false, 0);
    if (r >= 0) { disk_shutdown(); acpi_reset(); }
}
/* --encrypt: the passphrase twice on the console (8 characters at least, 3 tries); it never leaves this function's
 * buffers (wiped by the caller after formatting). */
static bool choose_passphrase(char* out, unsigned capacity, int* length) {
    char again[256];
    keymap_configure(true);   /* the layout the boot prompt will use too (target_cmdline) */
    vga_print("  Keyboard layout: "); vga_print(keymap_title()); vga_print(" ("); vga_print(keymap_name()); vga_print(")\n");
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        int n = console_prompt("  Choose the disk passphrase (8 characters or more): ", out, capacity, true, 300000000000ULL);
        if (n < 0) break;
        if (n < 8) { vga_print("  [64] INSTALL passphrase too short\n"); continue; }
        int m = console_prompt("  Type the passphrase again: ", again, sizeof(again), true, 300000000000ULL);
        bool same = m == n && !memcmp(out, again, (unsigned)n);
        crypto_wipe(again, sizeof(again));
        if (same) { *length = n; return true; }
        vga_print("  [64] INSTALL the two passphrases differ\n");
    }
    crypto_wipe(out, capacity); return false;
}
/* The root, encrypted: the target's new partition table is read, its root partition becomes LUKS2 (luks.c), and the
 * root filesystem is copied through a temporary dm-crypt mapping. Returns sectors copied or -errno. */
static int64_t copy_encrypted(struct layout* l, struct block_device* target, const char* passphrase, int length, uint8_t* buffer, bool* fat) {
    if (block_rescan(target) < 0) return -5;
    const struct block_partition* root = NULL;
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        if (p && p->dev == target && p->number && p->start == l->root->start) root = p;
    }
    if (!root || block_part_sectors(root) < block_part_sectors(l->root) + LUKS_EXTRA) return -5;
    uint64_t kib = memory_total_pages() * 4 / 8;   /* Argon2id memory: an eighth of the RAM, 64 MiB .. 1 GiB */
    if (kib < 65536) kib = 65536;
    if (kib > 1048576) kib = 1048576;
    struct luks_format_options o = {4, (uint32_t)kib, 1, 0};
    struct luks_volume v; struct block_device* mapped = NULL;
    vga_print("[64] INSTALL encrypting the root: LUKS2, aes-xts-plain64, argon2id"); say_number(" memory_kib=", kib); vga_print("\n");
    int e = luks2_format(root, passphrase, (unsigned)length, &o, &v);
    if (!e) e = luks_activate(root, &v, "nexus-install", &mapped);
    crypto_wipe(&v, sizeof(v));
    if (e) { if (mapped) dm_remove_name("nexus-install"); return e < 0 ? e : -5; }
    int64_t copied = copy_fat32(l->root, mapped, 0, buffer, fat);
    if (copied >= 0 && !*fat) copied = copy_range(l->root, mapped, 0, 0, block_part_sectors(l->root), buffer) ? -5 : (int64_t)block_part_sectors(l->root);
    uint8_t a[512], b[512];
    if (copied >= 0 && (block_part_read(l->root, 0, 1, a) || block_read(mapped, 0, 1, b) || memcmp(a, b, 512))) copied = -5;   /* through the key */
    if (copied >= 0 && block_flush(mapped)) copied = -5;
    if (dm_remove_name("nexus-install")) vga_print("[64] INSTALL warning: the temporary mapping stays\n");
    return copied;
}
/* At an orderly shutdown (vfs.c disk_shutdown) of an encrypted root installed with this layout: the boot prompt takes
 * its keyboard layout from the boot command line (the root is still locked then), so when the system's configured
 * layout (keymaps.c keymap_from_files: /etc/vconsole.conf, Aurora's keyboard.conf, /etc/default/keyboard) differs,
 * the command line on that disk (stage 2 copy and EFI/NEXUS/CMDLINE.TXT, through the NXBOOTD1 descriptor) gets
 * vconsole.keymap=<layout> -- what update-initramfs does on Linux after a keyboard change. Read back and checked. */
void boot_keymap_sync(void) {
    if (!cryptroot_volume || cryptroot_volume->scheme != BLOCK_SCHEME_GPT) return;
    char wanted[32];
    if (!keymap_from_files(wanted, sizeof(wanted))) return;
    struct block_device* disk = cryptroot_volume->dev;
    const struct block_partition *bios = NULL, *esp = NULL;
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        if (!p || p->dev != disk || p->scheme != BLOCK_SCHEME_GPT) continue;
        if (!memcmp(p->type_guid, bios_guid, 16)) bios = p; else if (!memcmp(p->type_guid, esp_guid, 16)) esp = p;
    }
    if (!bios || !esp || disk->block_size != 512 || disk->read_only || block_part_sectors(bios) < 2048) return;
    uint8_t* sector = (void*)arena_alloc(1024); if (!sector) return;
    uint8_t* back = sector + 512;
    char line[NEXUS_BOOT_CMDLINE_MAX], current[40] = "us", updated[NEXUS_BOOT_CMDLINE_MAX] = "";
    uint32_t cmdline_sector = 0; int error = 0;
    if (block_part_read(bios, block_part_sectors(bios) - 1, 1, sector) || memcmp(sector, "NXBOOTD1", 8)) error = 1;
    else if ((cmdline_sector = get_le32(sector + 12)) >= block_part_sectors(esp) || block_read(disk, esp->start + cmdline_sector, 1, sector)) error = 1;
    if (!error) {   /* the line (space padded, '\n' at the end), without its keymap options */
        unsigned n = 0; while (n < 511 && n + 1 < sizeof(line) && sector[n] != '\n') { line[n] = (char)sector[n]; n++; }
        while (n && line[n - 1] == ' ') n--;
        line[n] = 0;
        for (const char* p = line; *p; ) {
            while (*p == ' ') p++;
            const char* start = p; while (*p && *p != ' ') p++;
            unsigned k = (unsigned)(p - start); if (!k) continue;
            const char* value = !strncmp(start, "vconsole.keymap=", 16) ? start + 16 : !strncmp(start, "rd.vconsole.keymap=", 19) ? start + 19 : NULL;
            if (value) { char name[40] = ""; unsigned m = (unsigned)(p - value) < sizeof(name) ? (unsigned)(p - value) : sizeof(name) - 1;
                memcpy(name, value, m); name[m] = 0; if (!keymap_canonical(name, current, sizeof(current))) strcpy(current, "?"); continue; }
            if (strlen(updated) + k + 2 >= sizeof(updated)) { error = 1; break; }
            if (updated[0]) strcat(updated, " ");
            unsigned at = strlen(updated); memcpy(updated + at, start, k); updated[at + k] = 0;
        }
    }
    if (!error && !strcmp(current, wanted)) { arena_free((uint64_t)sector, 1024); return; }
    if (!error && strcmp(wanted, "us")) { strcat(updated, " vconsole.keymap="); strcat(updated, wanted); }
    if (!error && strlen(updated) > 0xF0 - 11) error = 1;   /* the stage 2 copy's room */
    uint64_t stage = bios->start + 3;
    if (!error && (block_read(disk, stage, 1, sector) || memcmp(sector + 0x100, "NXCMDLINE=", 10))) error = 1;
    if (!error) {
        memset(sector + 0x100, 0, 0xF0); memcpy(sector + 0x100, "NXCMDLINE=", 10); memcpy(sector + 0x10A, updated, strlen(updated));
        if (block_write(disk, stage, 1, sector) || block_read(disk, stage, 1, back) || memcmp(sector, back, 512)) error = 2;
    }
    if (!error) {
        memset(sector, ' ', 512); sector[511] = '\n'; memcpy(sector, updated, strlen(updated));
        if (block_write(disk, esp->start + cmdline_sector, 1, sector) || block_flush(disk) ||
            block_read(disk, esp->start + cmdline_sector, 1, back) || memcmp(sector, back, 512)) error = 2;
    }
    vga_print("[64] KEYMAP boot prompt layout "); vga_print(current); vga_print(" -> "); vga_print(wanted);
    vga_print(error == 2 ? ": FAILED to write the boot command line\n" : error ? ": no NexusOS boot command line on this disk\n" : ": boot command line updated\n");
    arena_free((uint64_t)sector, 1024);
}
void osinstall_check(void) {
    char target_name[40], serial[24]; bool from_file, encrypt;
    if (!request(target_name, sizeof(target_name), serial, sizeof(serial), &from_file, &encrypt)) return;
    vga_print("[64] INSTALL requested: target "); vga_print(target_name); vga_print("\n");
    uint8_t* sector = (void*)arena_alloc(4096);
    uint8_t* buffer = (void*)arena_alloc(CHUNK_SECTORS * 512);
    struct layout l; const char* why = NULL; struct block_device* target = NULL;
    if (!sector || !buffer) why = "out of memory";
    else if (!find_layout(&l, sector)) why = "the running system was not started from a NexusOS USB image (build.py usb-image)";
    else if (!l.source->hotplug && !boot_option("nexus.install.any-source")) why = "the running system is not on a USB drive";
    if (!why) {
        for (unsigned i = 0; i < block_device_count(); i++) {
            struct block_device* d = block_device_get(i);
            if (d && !d->removed && !strcmp(d->linux_name, target_name)) target = d;
        }
        uint64_t need = l.root->start + block_part_sectors(l.root) + 34 + (encrypt ? LUKS_EXTRA : 0);
        if (!target) why = "no such disk";
        else if (target == l.source) why = "the target is the USB drive the system runs from";
        else if (target->block_size != 512) why = "the target disk does not use 512-byte sectors";
        else if (serial[0] && strcmp(serial, target->serial)) why = "the disk's serial number differs from the request";
        else if (target->read_only) why = "the target disk is read-only";
        else if (busy(target)) why = "a partition of the target disk is in use";
        else if (target->blocks < need) why = "the target disk is too small";
    }
    if (from_file) {   /* one attempt per request: never again, and never on the copy */
        if (disk_remove_path(REQUEST)) vga_print("[64] INSTALL warning: could not remove the request file\n");
        memory_file_writeback(true, 0, ~0ULL); block_flush_all();
    }
    if (why) {
        vga_print("[64] INSTALL refused: "); vga_print(why); vga_print("\n");
        if (sector) arena_free((uint64_t)sector, 4096);
        if (buffer) arena_free((uint64_t)buffer, CHUNK_SECTORS * 512);
        return;
    }
    vga_print("\n  Install NexusOS on "); vga_print(target->linux_name); vga_print(" ("); vga_print(target->vendor); vga_print(" ");
    vga_print(target->name); say_number(", ", target->blocks / 2048); vga_print(" MiB)\n  ALL DATA ON THIS DISK WILL BE ERASED.\n");
    if (!boot_option("nexus.install.yes")) {
        char answer[8];
        int r = console_prompt("  Type YES and press Enter to install, anything else cancels: ", answer, sizeof(answer), false, 60000000000ULL);
        if (r != 3 || strcmp(answer, "YES")) {
            vga_print("[64] INSTALL cancelled\n");
            arena_free((uint64_t)sector, 4096); arena_free((uint64_t)buffer, CHUNK_SECTORS * 512); return;
        }
    }
    char passphrase[256]; int passphrase_length = 0;
    if (encrypt) {
        vga_print("  The disk will be encrypted (LUKS2). The passphrase is asked at every start; without it the data is lost.\n");
        if (!choose_passphrase(passphrase, sizeof(passphrase), &passphrase_length)) {
            vga_print("[64] INSTALL cancelled (no passphrase)\n");
            arena_free((uint64_t)sector, 4096); arena_free((uint64_t)buffer, CHUNK_SECTORS * 512); return;
        }
    }
    uint8_t root_guid[16]; char guid[40], line[NEXUS_BOOT_CMDLINE_MAX];
    uint64_t start = native_clock_ns(); int64_t root_copied = 0; bool fat = false; int error = 0;
    vga_print("[64] INSTALL writing the partition table\n");
    if (!write_gpt(&l, target, target->blocks, buffer, root_guid, encrypt)) error = -5;
    if (!error) { vga_print("[64] INSTALL boot partitions\n");
        error = copy_range(l.bios, target, l.bios->start, 0, block_part_sectors(l.bios), buffer);
        if (!error) error = copy_range(l.esp, target, l.esp->start, 0, block_part_sectors(l.esp), buffer); }
    if (!error && encrypt) {
        root_copied = copy_encrypted(&l, target, passphrase, passphrase_length, buffer, &fat);
        if (root_copied < 0) error = (int)root_copied;
    } else if (!error) {
        vga_print("[64] INSTALL root filesystem\n");
        root_copied = copy_fat32(l.root, target, l.root->start, buffer, &fat);
        if (root_copied < 0) error = (int)root_copied;
        else if (!fat) { error = copy_range(l.root, target, l.root->start, 0, block_part_sectors(l.root), buffer); root_copied = (int64_t)block_part_sectors(l.root); }
    }
    crypto_wipe(passphrase, sizeof(passphrase));
    if (!error) {   /* command line: stage 2 copy (offset 0x700) and CMDLINE.TXT (descriptor) */
        guid_text(root_guid, guid); target_cmdline(guid, encrypt, line, sizeof(line));
        uint64_t stage = l.bios->start + 3;
        if (block_read(target, stage, 1, sector)) error = -5;
        else {
            memset(sector + 0x100, 0, 0xF0); memcpy(sector + 0x100, "NXCMDLINE=", 10);
            unsigned n = strlen(line); if (n > 0xF0 - 11) n = 0xF0 - 11;
            memcpy(sector + 0x10A, line, n);
            if (block_write(target, stage, 1, sector)) error = -5;
        }
        memset(sector, ' ', 512); sector[511] = '\n'; memcpy(sector, line, strlen(line));
        if (!error && block_write(target, l.esp->start + l.cmdline_sector, 1, sector)) error = -5;
    }
    if (!error && block_flush(target)) error = -5;
    if (!error) {   /* read back: GPT header CRC, the root boot sector, the command line */
        uint8_t h[512];
        if (block_read(target, 1, 1, h) || memcmp(h, "EFI PART", 8)) error = -5;
        else { uint32_t crc = get_le32(h + 16); put_le32(h + 16, 0); if (crc != block_crc32(h, 92)) error = -5; }
        uint8_t a[512], b[512];
        if (!error && !encrypt && (block_part_read(l.root, 0, 1, a) || block_read(target, l.root->start, 1, b) || memcmp(a, b, 512))) error = -5;
        if (!error && encrypt && (block_read(target, l.root->start, 1, b) || memcmp(b, "LUKS\xba\xbe", 6))) error = -5;
        if (!error && (block_read(target, l.esp->start + l.cmdline_sector, 1, b) || memcmp(b, line, strlen(line)))) error = -5;
    }
    arena_free((uint64_t)sector, 4096); arena_free((uint64_t)buffer, CHUNK_SECTORS * 512);
    if (error) { vga_print("[64] INSTALL FAILED: I/O error on the target disk\n"); return; }
    block_rescan(target);
    uint64_t ms = (native_clock_ns() - start) / 1000000;
    vga_print("[64] INSTALL COMPLETE target="); vga_print(target->linux_name); vga_print(encrypt ? " cryptroot=PARTUUID=" : " root=PARTUUID="); vga_print(guid);
    say_number(fat ? " fat32_sectors=" : " sectors=", (uint64_t)root_copied); say_number(" ms=", ms); vga_print("\n");
    vga_print("  NexusOS is installed. The computer starts it from that disk once the USB drive is removed.\n");
    finish(boot_option("nexus.install.poweroff"));
}
