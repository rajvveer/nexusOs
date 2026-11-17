/* A/B boot slots for NexusOS core updates (included by bootx64.c, which provides print, read_file,
 * allocate_outside, append_option). See arch/x86_64/services/nexus-update for the other half.
 *
 * The slots live on the NexusOS root file system (FAT32 through the firmware's file system driver, or
 * ext4 through the read-only reader below), so the running system installs updates with ordinary files:
 *
 *   /boot/nexus/bootctl          text written by nexus-update:  default=a|b  trial=a|b|none  tries=N  nonce=HEX
 *   /boot/nexus/bootcount        fixed-size counter ("NXBC1 nonce=HEX count=N", padded): the only thing this
 *                                loader writes, in place (same length, no metadata change; on ext4 the file's
 *                                data block is rewritten through the disk, which is safe before the kernel runs)
 *   /boot/nexus/slot-X/kernel.bin  the slot's kernel (absent: the ESP's \EFI\NEXUS\KERNEL.BIN, the factory kernel)
 *   /boot/nexus/slot-X/BOOTX64.EFI the slot's loader (optional): started from here with "nexus.chained=1"
 *   /boot/nexus/slot-X/init      the slot's Aurora (the kernel runs it as init for nexus.slot=X)
 *
 * Decision: a pending trial slot (trial != default, nonce set) boots while bootcount for that nonce is below
 * tries; each attempt is counted before the kernel is started. When the count is used up the default slot
 * boots with nexus.rollback=<trial>. The kernel command line gets nexus.slot=X and, for a trial,
 * nexus.trial=<nonce> nexus.try=<n>; the system's boot check (nexus-update boot-check) then makes the trial the
 * default or records the rollback. A slot loader that the firmware refuses (e.g. Secure Boot) or that returns
 * is ignored and this loader continues with the same slot. */

static const EFI_GUID disk_io_guid = {0xCE345171, 0xBA0B, 0x11D2, {0x8E, 0x4F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const EFI_GUID block_io_guid = {0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
typedef struct {
    uint32_t media_id; uint8_t removable, present, logical_partition, read_only, write_caching;   /* BOOLEAN: 1 byte (not the kernel's bool) */
    uint32_t block_size, io_align; uint64_t last_block;
} AB_BLOCK_MEDIA;
typedef struct AB_BLOCK_IO {
    uint64_t revision; AB_BLOCK_MEDIA* media; void *reset, *read_blocks, *write_blocks;
    EFI_STATUS (*flush_blocks)(struct AB_BLOCK_IO*);
} AB_BLOCK_IO;
typedef struct AB_DISK_IO {
    uint64_t revision;
    EFI_STATUS (*read_disk)(struct AB_DISK_IO*, uint32_t media, uint64_t offset, UINTN size, void* buffer);
    EFI_STATUS (*write_disk)(struct AB_DISK_IO*, uint32_t media, uint64_t offset, UINTN size, const void* buffer);
} AB_DISK_IO;
typedef EFI_STATUS (*ab_locate_buffer_fn)(uint32_t type, const EFI_GUID* protocol, void* key, UINTN* count, EFI_HANDLE** handles);
typedef EFI_STATUS (*ab_file_write_fn)(EFI_FILE*, UINTN* size, const void* buffer);
typedef EFI_STATUS (*ab_load_image_fn)(bool policy, EFI_HANDLE parent, void* path, void* buffer, UINTN size, EFI_HANDLE* image);
typedef EFI_STATUS (*ab_start_image_fn)(EFI_HANDLE image, UINTN* exit_size, CHAR16** exit_data);
#define AB_FILE_MODE_RW 3ULL
#define AB_BOOTCOUNT_BYTES 64

struct ab_volume {
    int kind;                         /* 1: FAT through the firmware, 2: ext4 here */
    EFI_FILE* fat;
    AB_DISK_IO* disk; AB_BLOCK_IO* block; uint32_t media;
    uint64_t block_bytes, inodes_per_group, inode_bytes, desc_bytes, groups, first_data_block;
};

static void ab_wide(const char* path, CHAR16* out, unsigned max) {
    unsigned i = 0;
    for (; *path && i + 1 < max; path++) out[i++] = *path == '/' ? '\\' : (CHAR16)(uint8_t)*path;
    out[i] = 0;
}
static uint32_t ab_u32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t ab_u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static bool ab_disk(struct ab_volume* v, uint64_t offset, void* buffer, UINTN size) {
    return !EFI_ERROR(v->disk->read_disk(v->disk, v->media, offset, size, buffer));
}

/* ---- ext4, read only (4 KiB-class blocks, extents, 32/64-bit descriptors; htree folders are read linearly) */
static bool ab_ext4_inode(struct ab_volume* v, uint32_t ino, uint8_t* inode /* 256 bytes */) {
    if (!ino) return false;
    uint64_t group = (ino - 1) / v->inodes_per_group, index = (ino - 1) % v->inodes_per_group;
    if (group >= v->groups) return false;
    uint8_t desc[64];
    if (!ab_disk(v, (v->first_data_block + 1) * v->block_bytes + group * v->desc_bytes, desc, v->desc_bytes)) return false;
    uint64_t table = ab_u32(desc + 8) | (v->desc_bytes >= 64 ? (uint64_t)ab_u32(desc + 0x28) << 32 : 0);
    UINTN size = v->inode_bytes < 256 ? v->inode_bytes : 256;
    memset(inode, 0, 256);
    return ab_disk(v, table * v->block_bytes + index * v->inode_bytes, inode, size);
}
/* Physical block of logical block `logical` (0: hole). Extent trees up to depth 4. */
static uint64_t ab_ext4_map(struct ab_volume* v, const uint8_t* inode, uint64_t logical) {
    uint8_t node[4096]; const uint8_t* at = inode + 0x28;
    if (!(ab_u32(inode + 0x20) & 0x80000) || v->block_bytes > sizeof(node)) return 0;   /* EXTENTS_FL */
    for (unsigned level = 0; level < 5; level++) {
        if (ab_u16(at) != 0xF30A) return 0;
        unsigned entries = ab_u16(at + 2), depth = ab_u16(at + 6);
        if (!depth) {
            for (unsigned i = 0; i < entries; i++) {
                const uint8_t* e = at + 12 + 12 * i;
                uint64_t first = ab_u32(e), length = ab_u16(e + 4);
                if (length > 32768) return 0;   /* unwritten extent: reads as zeros */
                if (logical >= first && logical < first + length) return ((uint64_t)ab_u16(e + 6) << 32 | ab_u32(e + 8)) + (logical - first);
            }
            return 0;
        }
        const uint8_t* chosen = NULL;
        for (unsigned i = 0; i < entries; i++) { const uint8_t* e = at + 12 + 12 * i; if (ab_u32(e) <= logical) chosen = e; }
        if (!chosen || !ab_disk(v, ((uint64_t)ab_u16(chosen + 8) << 32 | ab_u32(chosen + 4)) * v->block_bytes, node, v->block_bytes)) return 0;
        at = node;
    }
    return 0;
}
static uint32_t ab_ext4_lookup(struct ab_volume* v, const char* path, uint8_t* inode) {
    uint32_t ino = 2; uint8_t block[4096];
    if (!ab_ext4_inode(v, ino, inode)) return 0;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char* name = path; unsigned length = 0; while (path[length] && path[length] != '/') length++;
        path += length;
        if ((ab_u16(inode) & 0xF000) != 0x4000) return 0;
        uint64_t size = ab_u32(inode + 4) | (uint64_t)ab_u32(inode + 0x6C) << 32, found = 0;
        for (uint64_t logical = 0; logical * v->block_bytes < size && !found; logical++) {
            uint64_t physical = ab_ext4_map(v, inode, logical);
            if (!physical || !ab_disk(v, physical * v->block_bytes, block, v->block_bytes)) continue;
            for (uint64_t off = 0; off + 8 <= v->block_bytes; ) {
                uint32_t child = ab_u32(block + off); unsigned record = ab_u16(block + off + 4), nl = block[off + 6];
                if (record < 8 || off + record > v->block_bytes) break;
                if (child && nl == length && off + 8 + nl <= v->block_bytes) {
                    bool same = true; for (unsigned i = 0; i < nl; i++) if (block[off + 8 + i] != (uint8_t)name[i]) { same = false; break; }
                    if (same) { found = child; break; }
                }
                off += record;
            }
        }
        if (!found || !ab_ext4_inode(v, (uint32_t)found, inode)) return 0;
        ino = (uint32_t)found;
    }
    return ino;
}
static bool ab_ext4_open(struct ab_volume* v) {
    uint8_t sb[1024];
    if (!ab_disk(v, 1024, sb, sizeof(sb)) || ab_u16(sb + 0x38) != 0xEF53) return false;
    uint32_t log = ab_u32(sb + 0x18), incompat = ab_u32(sb + 0x60);
    if (log > 2) return false;   /* 1..4 KiB blocks */
    v->block_bytes = 1024ULL << log; v->inodes_per_group = ab_u32(sb + 0x28); v->first_data_block = ab_u32(sb + 0x14);
    v->inode_bytes = ab_u32(sb + 0x4C) ? ab_u16(sb + 0x58) : 128;
    v->desc_bytes = (incompat & 0x80) && ab_u16(sb + 0xFE) >= 32 ? ab_u16(sb + 0xFE) : 32;
    if (v->desc_bytes > 64) v->desc_bytes = 64;
    uint64_t blocks = ab_u32(sb + 4) | ((incompat & 0x80) ? (uint64_t)ab_u32(sb + 0x150) << 32 : 0), per_group = ab_u32(sb + 0x20);
    if (!v->inodes_per_group || !per_group || !v->inode_bytes) return false;
    v->groups = (blocks - v->first_data_block + per_group - 1) / per_group;
    return true;
}

/* ---- the root volume: the first file system (other than the ESP) holding /boot/nexus/bootctl */
static EFI_STATUS ab_read(struct ab_volume* v, const char* path, uint8_t** data, uint64_t* size, uint64_t limit) {
    if (v->kind == 1) { CHAR16 wide[128]; ab_wide(path, wide, 128); return read_file(v->fat, wide, data, size, limit); }
    uint8_t inode[256];
    if (!ab_ext4_lookup(v, path, inode) || (ab_u16(inode) & 0xF000) != 0x8000) return EFI_NOT_FOUND;
    uint64_t length = ab_u32(inode + 4) | (uint64_t)ab_u32(inode + 0x6C) << 32;
    if (!length || length > limit) return EFI_BUFFER_TOO_SMALL;
    uint8_t* buffer = (uint8_t*)allocate_outside(length + v->block_bytes + 1);
    if (!buffer) return EFI_BUFFER_TOO_SMALL;
    for (uint64_t logical = 0; logical * v->block_bytes < length; logical++) {
        uint64_t physical = ab_ext4_map(v, inode, logical);
        if (!physical) memset(buffer + logical * v->block_bytes, 0, v->block_bytes);
        else if (!ab_disk(v, physical * v->block_bytes, buffer + logical * v->block_bytes, v->block_bytes)) return EFI_NOT_FOUND;
    }
    buffer[length] = 0; *data = buffer; *size = length; return EFI_SUCCESS;
}
/* Overwrite the first `size` bytes of an existing file of exactly that length, in place. */
static bool ab_rewrite(struct ab_volume* v, const char* path, const uint8_t* data, uint64_t size) {
    if (v->kind == 1) {
        CHAR16 wide[128]; ab_wide(path, wide, 128); EFI_FILE* file; uint64_t end = 0;
        if (EFI_ERROR(v->fat->open(v->fat, &file, wide, AB_FILE_MODE_RW, 0))) return false;
        bool ok = !EFI_ERROR(file->set_position(file, ~0ULL)) && !EFI_ERROR(file->get_position(file, &end)) && end == size &&
                  !EFI_ERROR(file->set_position(file, 0));
        UINTN n = size;
        if (ok) ok = !EFI_ERROR(((ab_file_write_fn)file->write)(file, &n, data)) && n == size;
        file->close(file); return ok;
    }
    uint8_t inode[256];
    if (!ab_ext4_lookup(v, path, inode) || (ab_u16(inode) & 0xF000) != 0x8000 || ab_u32(inode + 4) != size || size > v->block_bytes) return false;
    uint64_t physical = ab_ext4_map(v, inode, 0);
    if (!physical || EFI_ERROR(v->disk->write_disk(v->disk, v->media, physical * v->block_bytes, size, data))) return false;
    if (v->block && v->block->flush_blocks) v->block->flush_blocks(v->block);
    return true;
}
static bool ab_scan(EFI_HANDLE esp_device, struct ab_volume* v, UINTN* seen_fs, UINTN* seen_ext4) {
    UINTN count = 0; EFI_HANDLE* handles = NULL; uint8_t* probe; uint64_t probe_size;
    ab_locate_buffer_fn locate = (ab_locate_buffer_fn)bs->locate_handle_buffer;
    if (!EFI_ERROR(locate(2, &file_system_guid, NULL, &count, &handles)))
        for (UINTN i = 0; i < count; i++) {
            EFI_SIMPLE_FILE_SYSTEM* fs; EFI_FILE* root;
            if (handles[i] == esp_device || EFI_ERROR(bs->handle_protocol(handles[i], &file_system_guid, (void**)&fs)) ||
                EFI_ERROR(fs->open_volume(fs, &root))) continue;
            memset(v, 0, sizeof(*v)); v->kind = 1; v->fat = root; (*seen_fs)++;
            if (!EFI_ERROR(ab_read(v, "/boot/nexus/bootctl", &probe, &probe_size, 4096))) {
                /* A FAT32 root has no file permissions: anyone could plant a slot kernel there. Slots need ext4. */
                print("A/B: /boot/nexus on a FAT volume is ignored: core update slots need an ext4 root\n");
                serial_print("[uefi] A/B refused: slots on a FAT root are not used (an ext4 root is needed)\n");
            }
            root->close(root);
        }
    count = 0; handles = NULL;
    if (!EFI_ERROR(locate(2, &disk_io_guid, NULL, &count, &handles)))
        for (UINTN i = 0; i < count; i++) {
            AB_DISK_IO* disk; AB_BLOCK_IO* block;
            if (EFI_ERROR(bs->handle_protocol(handles[i], &disk_io_guid, (void**)&disk)) ||
                EFI_ERROR(bs->handle_protocol(handles[i], &block_io_guid, (void**)&block)) || !block->media || !block->media->present) continue;
            memset(v, 0, sizeof(*v)); v->kind = 2; v->disk = disk; v->block = block; v->media = block->media->media_id;
            if (!ab_ext4_open(v)) continue;
            (*seen_ext4)++;
            if (!EFI_ERROR(ab_read(v, "/boot/nexus/bootctl", &probe, &probe_size, 4096))) return true;
        }
    return false;
}
/* The root file system with /boot/nexus/bootctl. Firmware may not have connected the root disk's controller (it is not
 * the boot device): if nothing is found, connect every handle (as a boot manager's "connect all") and look again. */
typedef EFI_STATUS (*ab_connect_fn)(EFI_HANDLE controller, EFI_HANDLE* drivers, void* remaining, bool recursive);
static bool ab_find_root(EFI_HANDLE esp_device, struct ab_volume* v) {
    UINTN fs = 0, ext4 = 0, count = 0; EFI_HANDLE* handles = NULL;
    if (ab_scan(esp_device, v, &fs, &ext4)) return true;
    if (!EFI_ERROR(((ab_locate_buffer_fn)bs->locate_handle_buffer)(0, NULL, NULL, &count, &handles)))
        for (UINTN i = 0; i < count; i++) ((ab_connect_fn)bs->connect_controller)(handles[i], NULL, NULL, true);
    fs = ext4 = 0;
    bool found = ab_scan(esp_device, v, &fs, &ext4);
    if (!found && ext4 + fs) {
        char n[24]; serial_print("[uefi] no A/B slots (/boot/nexus/bootctl) on "); decimal_text(fs, n); serial_print(n); serial_print(" FAT and ");
        decimal_text(ext4, n); serial_print(n); serial_print(" ext4 volumes\n");
    }
    return found;
}

/* ---- the decision */
static bool ab_key(const char* text, const char* key, char* out, unsigned max) {
    unsigned k = 0; while (key[k]) k++;
    for (const char* p = text; *p; ) {
        const char* line = p; while (*p && *p != '\n') p++;
        const char* end = p; if (*p) p++;
        while (line < end && (*line == ' ' || *line == '\t')) line++;
        bool match = end - line > k && line[k] == '=';
        for (unsigned i = 0; match && i < k; i++) if (line[i] != key[i]) match = false;
        if (!match) continue;
        unsigned n = 0; for (const char* q = line + k + 1; q < end && *q != '\r' && *q != ' ' && n + 1 < max; q++) out[n++] = *q;
        out[n] = 0; return true;
    }
    return false;
}
static bool ab_slot_name(const char* s) { return (s[0] == 'a' || s[0] == 'b') && !s[1]; }
static bool ab_hex(const char* s) {
    if (!*s) return false;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
    return true;
}
static void ab_option(struct nexus_boot_info* info, const char* key, const char* value) {
    char text[96]; unsigned n = 0;
    for (const char* q = key; *q && n + 1 < sizeof(text); q++) text[n++] = *q;
    for (const char* q = value; *q && n + 1 < sizeof(text); q++) text[n++] = *q;
    text[n] = 0; append_option(info, text, n);
}
static void ab_count_text(char* out, const char* nonce, uint64_t count) {   /* AB_BOOTCOUNT_BYTES, newline padded */
    const char* head = "NXBC1 nonce="; unsigned n = 0;
    for (const char* q = head; *q; q++) out[n++] = *q;
    for (const char* q = nonce; *q && n < 48; q++) out[n++] = *q;
    char digits[24]; decimal_text(count, digits);
    for (const char* q = " count="; *q; q++) out[n++] = *q;
    for (const char* q = digits; *q && n + 1 < AB_BOOTCOUNT_BYTES; q++) out[n++] = *q;
    while (n < AB_BOOTCOUNT_BYTES) out[n++] = '\n';
}
static const char* ab_cmdline_value(const char* cmdline, const char* key, char* out, unsigned max) {
    unsigned k = 0; while (key[k]) k++;
    for (const char* p = cmdline; *p; p++) {
        if (p != cmdline && p[-1] != ' ') continue;
        bool match = true; for (unsigned i = 0; i < k; i++) if (p[i] != key[i]) { match = false; break; }
        if (!match) continue;
        unsigned n = 0; for (const char* q = p + k; *q && *q != ' ' && n + 1 < max; q++) out[n++] = *q;
        out[n] = 0; return out;
    }
    return NULL;
}
static bool ab_kernel_header(const uint8_t* kernel, uint64_t size) {
    for (uint64_t at = 0; at + sizeof(struct nexus_kernel_header) <= size && at < 0x10000; at += 8) {
        const struct nexus_kernel_header* h = (const void*)(kernel + at);
        if (h->magic == NEXUS_KERNEL_MAGIC) return h->version == 1 && h->load_address == 0x100000 && h->entry64 >= h->load_address &&
                                                   h->entry64 < h->load_address + size;
    }
    return false;
}
/* Pick the slot, count a trial, maybe hand over to the slot's own loader, and read the slot's kernel.
 * EFI_NOT_FOUND: no slot kernel (no slots at all, or the factory slot): use \EFI\NEXUS\KERNEL.BIN. */
/* Secure Boot (sec): a slot kernel is trusted like the ESP's KERNEL.BIN, against its own signature next to it
   (/boot/nexus/slot-X/kernel.sig, 256 bytes RSA-2048/SHA-256 from uefi/secureboot.py): a present signature must verify;
   without one the kernel is accepted only while the firmware's Secure Boot is off. */
static bool ab_slot_kernel, ab_slot_verified;
static bool ab_kernel_trusted(struct ab_volume* v, const char* kernel_path, const uint8_t* kernel, uint64_t kernel_size) {
    ab_slot_verified = false;
#ifdef SB_KERNEL_KEY_PRESENT
    char sig_path[64]; unsigned n = 0;
    while (kernel_path[n] && n + 1 < sizeof(sig_path)) { sig_path[n] = kernel_path[n]; n++; }
    sig_path[n] = 0;
    if (n < 4 || sig_path[n - 4] != '.') return false;
    sig_path[n - 3] = 's'; sig_path[n - 2] = 'i'; sig_path[n - 1] = 'g';   /* kernel.bin -> kernel.sig */
    uint8_t* sig; uint64_t sig_size;
    if (!EFI_ERROR(ab_read(v, sig_path, &sig, &sig_size, 4096))) {
        if (sig_size != 256 || !rsa_sha256_verify(kernel, kernel_size, sig, sb_kernel_modulus)) {
            print("A/B: "); print(kernel_path); print(" signature verification FAILED\n");
            serial_print("[uefi] A/B slot kernel signature FAILED\n"); return false;
        }
        ab_slot_verified = true; return true;
    }
    if (firmware_secure_boot()) { print("A/B: "); print(kernel_path); print(" is unsigned while Secure Boot is active\n");
        serial_print("[uefi] A/B slot kernel unsigned while Secure Boot is active\n"); return false; }
#else
    (void)v; (void)kernel_path; (void)kernel; (void)kernel_size;
#endif
    return true;
}
static EFI_STATUS ab_kernel(EFI_LOADED_IMAGE* loaded, struct nexus_boot_info* info, uint8_t** kernel, uint64_t* kernel_size, uint64_t limit) {
    struct ab_volume v; char slot[4] = "", text_trial[4] = "", nonce[40] = "", tries_text[8] = "", path[64];
    char chained[4];
    if (!ab_find_root(loaded->device, &v)) return EFI_NOT_FOUND;
    if (ab_cmdline_value(info->cmdline, "nexus.chained=", chained, sizeof(chained))) {
        /* started by the first-stage loader, which already chose the slot and counted the attempt */
        if (!ab_cmdline_value(info->cmdline, "nexus.slot=", slot, sizeof(slot)) || !ab_slot_name(slot)) return EFI_NOT_FOUND;
        print("A/B: chained loader for slot "); print(slot); print("\n");
        serial_print("[uefi] A/B chained loader slot="); serial_print(slot); serial_print("\n");
    } else {
        uint8_t* control; uint64_t control_size;
        if (EFI_ERROR(ab_read(&v, "/boot/nexus/bootctl", &control, &control_size, 4096))) return EFI_NOT_FOUND;
        if (!ab_key((const char*)control, "default", slot, sizeof(slot)) || !ab_slot_name(slot)) { print("A/B: bootctl has no valid default slot\n"); return EFI_NOT_FOUND; }
        bool trial = ab_key((const char*)control, "trial", text_trial, sizeof(text_trial)) && ab_slot_name(text_trial) &&
                     text_trial[0] != slot[0] && ab_key((const char*)control, "nonce", nonce, sizeof(nonce)) && ab_hex(nonce);
        uint64_t tries = 3;
        if (ab_key((const char*)control, "tries", tries_text, sizeof(tries_text)) && tries_text[0] >= '1' && tries_text[0] <= '9' && !tries_text[1]) tries = (uint64_t)(tries_text[0] - '0');
        if (trial) {
            uint8_t* counter; uint64_t counter_size; char seen[40] = "", count_text[8] = ""; uint64_t count = 0;
            if (!EFI_ERROR(ab_read(&v, "/boot/nexus/bootcount", &counter, &counter_size, 4096)) && counter_size == AB_BOOTCOUNT_BYTES) {
                for (uint64_t i = 0; i < counter_size; i++) if (counter[i] == ' ') counter[i] = '\n';
                if (ab_key((const char*)counter, "nonce", seen, sizeof(seen)) && ab_key((const char*)counter, "count", count_text, sizeof(count_text)))
                    for (const char* q = count_text; *q >= '0' && *q <= '9'; q++) count = count * 10 + (uint64_t)(*q - '0');
                bool same = true; for (unsigned i = 0; same && (seen[i] || nonce[i]); i++) if (seen[i] != nonce[i]) same = false;
                if (!same) count = 0;
                if (count < tries) {
                    char text[AB_BOOTCOUNT_BYTES]; ab_count_text(text, nonce, count + 1);
                    if (ab_rewrite(&v, "/boot/nexus/bootcount", (const uint8_t*)text, AB_BOOTCOUNT_BYTES)) {
                        char n[8]; decimal_text(count + 1, n);
                        print("A/B: trial of slot "); print(text_trial); print(", attempt "); print(n); print("\n");
                        serial_print("[uefi] A/B trial slot="); serial_print(text_trial); serial_print(" attempt="); serial_print(n); serial_print("\n");
                        slot[0] = text_trial[0]; ab_option(info, "nexus.trial=", nonce); ab_option(info, "nexus.try=", n);
                    } else { print("A/B: cannot record the attempt: the trial does not boot\n"); ab_option(info, "nexus.rollback=", text_trial); }
                } else {
                    print("A/B: slot "); print(text_trial); print(" failed its trial boots; rolling back to slot "); print(slot); print("\n");
                    serial_print("[uefi] A/B rollback from slot="); serial_print(text_trial); serial_print(" to slot="); serial_print(slot); serial_print("\n");
                    ab_option(info, "nexus.rollback=", text_trial);
                }
            } else { print("A/B: no usable /boot/nexus/bootcount: the trial does not boot\n"); ab_option(info, "nexus.rollback=", text_trial); }
        }
        ab_option(info, "nexus.slot=", slot);
        /* a slot loader of its own: start it (it reads the slot from the command line and counts nothing) */
        uint8_t* image; uint64_t image_size;
        memcpy(path, "/boot/nexus/slot-x/BOOTX64.EFI", 31); path[17] = slot[0];
        if (!EFI_ERROR(ab_read(&v, path, &image, &image_size, 8 << 20))) {
            EFI_HANDLE child = NULL; EFI_LOADED_IMAGE* child_image;
            EFI_STATUS status = ((ab_load_image_fn)bs->load_image)(false, self, NULL, image, image_size, &child);
            bool refused = EFI_ERROR(status);   /* e.g. Secure Boot: an image the db does not allow */
            if (!EFI_ERROR(status) && !EFI_ERROR(bs->handle_protocol(child, &loaded_image_guid, (void**)&child_image))) {
                static CHAR16 options[NEXUS_BOOT_CMDLINE_MAX + 32]; unsigned n = 0;
                for (const char* q = info->cmdline; *q && n + 20 < NEXUS_BOOT_CMDLINE_MAX; q++) options[n++] = (CHAR16)(uint8_t)*q;
                for (const char* q = " nexus.chained=1"; *q; q++) options[n++] = (CHAR16)*q;
                options[n] = 0;
                child_image->load_options = options; child_image->load_options_size = (uint32_t)((n + 1) * 2); child_image->device = loaded->device;
                print("A/B: starting the slot's loader\n"); serial_print("[uefi] A/B starting the slot loader\n");
                status = ((ab_start_image_fn)bs->start_image)(child, NULL, NULL);
            }
            print("A/B: the slot's loader did not take over (status "); print_hex(status); print("); continuing\n");
            serial_print(refused ? "[uefi] A/B the firmware refused the slot loader; continuing\n" : "[uefi] A/B the slot loader returned; continuing\n");
        }
    }
    memcpy(path, "/boot/nexus/slot-x/kernel.bin", 30); path[17] = slot[0];
    EFI_STATUS status = ab_read(&v, path, kernel, kernel_size, limit);
    if (!EFI_ERROR(status) && (!ab_kernel_header(*kernel, *kernel_size) || !ab_kernel_trusted(&v, path, *kernel, *kernel_size))) status = EFI_NOT_FOUND;
    if (EFI_ERROR(status) && text_trial[0] == slot[0] && nonce[0]) {
        /* the trial slot's kernel is missing or not a NexusOS kernel: roll back now instead of failing the boot */
        print("A/B: slot "); print(slot); print("'s kernel is unusable; rolling back\n");
        serial_print("[uefi] A/B unusable kernel in slot="); serial_print(slot); serial_print("\n");
        ab_option(info, "nexus.rollback=", text_trial);
        uint8_t* control; uint64_t control_size; char fallback[4] = "";
        if (EFI_ERROR(ab_read(&v, "/boot/nexus/bootctl", &control, &control_size, 4096)) ||
            !ab_key((const char*)control, "default", fallback, sizeof(fallback)) || !ab_slot_name(fallback)) return EFI_NOT_FOUND;
        ab_option(info, "nexus.slot=", fallback);   /* the kernel takes the last nexus.slot= */
        path[17] = fallback[0]; slot[0] = fallback[0];
        status = ab_read(&v, path, kernel, kernel_size, limit);
        if (!EFI_ERROR(status) && (!ab_kernel_header(*kernel, *kernel_size) || !ab_kernel_trusted(&v, path, *kernel, *kernel_size))) status = EFI_NOT_FOUND;
    }
    if (EFI_ERROR(status)) { print("A/B: slot "); print(slot); print(" has no kernel of its own: \\EFI\\NEXUS\\KERNEL.BIN\n"); return EFI_NOT_FOUND; }
    ab_slot_kernel = true;
    print("A/B: kernel from slot "); print(slot); print("\n");
    serial_print("[uefi] A/B kernel from slot="); serial_print(slot); serial_print("\n");
    return EFI_SUCCESS;
}
