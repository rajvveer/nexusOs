/* Filesystem identification on block devices, the kernel-side part of what libblkid/udev provide when no udev
 * runs: type, UUID and label of LUKS1/2, ext2/3/4, exFAT, NTFS, FAT12/16/32, ISO9660 and swap, plus the
 * partition's PARTUUID (GPT unique GUID, MBR disk-signature-NN) and PARTLABEL. Used for /dev/disk/by-uuid,
 * by-label and by-partuuid links, fstab UUID=/LABEL=/PARTUUID=/PARTLABEL= sources, the ID_FS_* variables of block
 * uevents and the desktop's automount. Results are cached per partition and dropped when the partition list
 * changes or a writer closes the node (as udev's watch re-probes). UUID strings follow blkid: lower-case RFC
 * 4122 for 16-byte UUIDs, "XXXX-XXXX" volume serials for FAT/exFAT, 16 hex digits for NTFS, the creation
 * time "YYYY-MM-DD-HH-MM-SS-CC" for ISO9660. */
#include "native.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

struct fs_identity { char type[16], uuid[48], label[64], usage[12], partuuid[40], partlabel[40]; bool probed; };
struct probe_entry { const struct block_partition* part; unsigned generation; struct fs_identity id; bool used; };
#define PROBE_CACHE 64
static struct probe_entry cache[PROBE_CACHE];
static unsigned cache_epoch;
bool fatfs_identity(const uint8_t* bs, char* uuid, char* label, char* type);
__attribute__((weak)) bool exfat_identity(struct block_partition* part, const uint8_t* bs, char* uuid, char* label) { (void)part; (void)bs; (void)uuid; (void)label; return false; }

static void uuid_text(const uint8_t* u, char* out) {   /* bytes in on-disk (big-endian field) order */
    out[0] = 0;
    for (unsigned i = 0; i < 16; i++) { if (i == 4 || i == 6 || i == 8 || i == 10) strcat(out, "-"); append_hex(out, u[i], 2); }
}
static void guid_text(const uint8_t* g, char* out) {   /* GPT GUID (mixed-endian) */
    uint8_t u[16] = {g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]};
    uuid_text(u, out);
}
static void trimmed(char* out, const uint8_t* in, unsigned length, unsigned capacity) {
    unsigned n = 0;
    for (unsigned i = 0; i < length && n + 1 < capacity; i++) { if (!in[i]) break; out[n++] = in[i] >= 32 && in[i] < 127 ? (char)in[i] : '_'; }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}
static int read_bytes(struct block_partition* p, uint64_t offset, uint8_t* buffer, unsigned length) {
    if (offset % 512 || length % 512) return -22;
    if ((offset + length) / 512 > block_part_sectors(p)) return -5;
    return block_part_read(p, offset / 512, length / 512, buffer) < 0 ? -5 : 0;
}
static void probe(struct block_partition* p, struct fs_identity* id) {
    memset(id, 0, sizeof(*id)); id->probed = true;
    if (p->scheme == BLOCK_SCHEME_GPT && p->number) { guid_text(p->unique_guid, id->partuuid); strncpy(id->partlabel, p->name, sizeof(id->partlabel) - 1); }
    else if (p->scheme == BLOCK_SCHEME_MBR && p->number) {
        uint8_t* mbr = (void*)arena_alloc(4096);
        if (mbr && block_read(p->dev, 0, 1, mbr) == 0) {
            uint32_t signature = get_le32(mbr + 440);
            append_hex(id->partuuid, signature, 8); strcat(id->partuuid, "-"); append_hex(id->partuuid, p->number, 2);
        }
        if (mbr) arena_free((uint64_t)mbr, 4096);
    }
    if (p->dev->removed || block_part_sectors(p) < 8) return;
    uint8_t* b = (void*)arena_alloc(8192); if (!b) return;
    if (read_bytes(p, 0, b, 4096)) goto done;
    /* LUKS (both versions) */
    if (!memcmp(b, "LUKS\xBA\xBE", 6)) {
        strcpy(id->type, "crypto_LUKS"); strcpy(id->usage, "crypto");
        trimmed(id->uuid, b + 168, 40, sizeof(id->uuid));
        if (get_be16(b + 6) == 2) trimmed(id->label, b + 24, 48, sizeof(id->label));
        goto done;
    }
    /* ext2/3/4: superblock at byte 1024 */
    if (get_le16(b + 1024 + 56) == 0xEF53) {
        const uint8_t* sb = b + 1024; uint32_t compat = get_le32(sb + 0x5C), incompat = get_le32(sb + 0x60);
        strcpy(id->type, (incompat & (0x40 | 0x80 | 0x200 | 0x2000)) || (get_le32(sb + 0x64) & 0x400) ? "ext4" : (compat & 4) ? "ext3" : "ext2");
        strcpy(id->usage, "filesystem"); uuid_text(sb + 0x68, id->uuid); trimmed(id->label, sb + 0x78, 16, sizeof(id->label));
        goto done;
    }
    if (!memcmp(b + 3, "EXFAT   ", 8)) {
        strcpy(id->type, "exfat"); strcpy(id->usage, "filesystem");
        if (!exfat_identity(p, b, id->uuid, id->label)) {
            uint32_t serial = get_le32(b + 100); id->uuid[0] = 0;
            append_hex(id->uuid, serial >> 16, 4); strcat(id->uuid, "-"); append_hex(id->uuid, serial & 0xFFFF, 4);
        }
        for (char* c = id->uuid; *c; c++) if (*c >= 'a' && *c <= 'f') *c = (char)(*c - 32);
        goto done;
    }
    if (!memcmp(b + 3, "NTFS    ", 8)) {
        strcpy(id->type, "ntfs"); strcpy(id->usage, "filesystem");
        uint64_t serial = get_le64(b + 72); append_hex(id->uuid, serial, 16);
        for (char* c = id->uuid; *c; c++) if (*c >= 'a' && *c <= 'f') *c = (char)(*c - 32);
        goto done;
    }
    { char type[16]; if (fatfs_identity(b, id->uuid, id->label, type)) { strcpy(id->type, type); strcpy(id->usage, "filesystem"); goto done; } }
    /* swap: signature at the end of the first page */
    if (!memcmp(b + 4096 - 10, "SWAPSPACE2", 10) || !memcmp(b + 4096 - 10, "SWAP-SPACE", 10)) {
        strcpy(id->type, "swap"); strcpy(id->usage, "other");
        uuid_text(b + 1024 + 12, id->uuid); trimmed(id->label, b + 1024 + 28, 16, sizeof(id->label));
        goto done;
    }
    /* ISO9660 primary volume descriptor at 32 KiB */
    if (block_part_sectors(p) > 72 && !read_bytes(p, 32768, b, 4096) && b[0] == 1 && !memcmp(b + 1, "CD001", 5)) {
        strcpy(id->type, "iso9660"); strcpy(id->usage, "filesystem");
        trimmed(id->label, b + 40, 32, sizeof(id->label));
        const uint8_t* t = b + 813;   /* volume creation date-time "YYYYMMDDHHMMSSCC" */
        bool digits = true; for (unsigned i = 0; i < 16; i++) if (t[i] < '0' || t[i] > '9') digits = false;
        if (digits) {
            unsigned n = 0; static const unsigned group[7] = {4, 2, 2, 2, 2, 2, 2};
            for (unsigned g = 0, at = 0; g < 7; g++) { for (unsigned k = 0; k < group[g]; k++) id->uuid[n++] = (char)t[at++]; if (g < 6) id->uuid[n++] = '-'; }
            id->uuid[n] = 0;   /* YYYY-MM-DD-HH-MM-SS-CC */
        }
        goto done;
    }
done:
    arena_free((uint64_t)b, 8192);
}
void fsprobe_invalidate(const struct block_partition* part) {
    for (unsigned i = 0; i < PROBE_CACHE; i++) if (cache[i].used && (!part || cache[i].part == part || cache[i].part->dev == part->dev)) cache[i].used = false;
    cache_epoch++;
}
const struct fs_identity* fsprobe_get(struct block_partition* p) {
    for (unsigned i = 0; i < PROBE_CACHE; i++) if (cache[i].used && cache[i].part == p && cache[i].generation == p->dev->generation) return &cache[i].id;
    unsigned slot = 0;
    for (unsigned i = 0; i < PROBE_CACHE; i++) if (!cache[i].used) { slot = i; break; } else if (i == PROBE_CACHE - 1) slot = cache_epoch % PROBE_CACHE;
    cache[slot].used = true; cache[slot].part = p; cache[slot].generation = p->dev->generation;
    probe(p, &cache[slot].id);
    return &cache[slot].id;
}
int fsprobe_type(struct block_partition* part, char* type, unsigned capacity) {
    const struct fs_identity* id = fsprobe_get(part);
    if (!id->type[0]) return -22;
    strncpy(type, id->type, capacity - 1); type[capacity - 1] = 0; return 0;
}
/* every listed partition and whole disk (number 0 views) */
static unsigned candidates(struct block_partition** out, unsigned capacity) {
    unsigned n = 0;
    for (unsigned i = 0; i < block_device_count() && n < capacity; i++) {
        struct block_device* d = block_device_get(i);
        if (d && !d->removed) { struct block_partition* w = block_whole(d); if (w) out[n++] = w; }
    }
    for (unsigned i = 0; i < block_partition_count() && n < capacity; i++) {
        struct block_partition* p = (struct block_partition*)block_partition_get(i);
        if (p && p->number && !p->dev->removed) out[n++] = p;
    }
    return n;
}
/* "UUID=..", "LABEL=..", "PARTUUID=..", "PARTLABEL=.." -> "/dev/<name>" */
int fsprobe_resolve(const char* spec, char* device, unsigned capacity) {
    const char* value; unsigned kind;
    if (!strncmp(spec, "UUID=", 5)) { kind = 0; value = spec + 5; }
    else if (!strncmp(spec, "LABEL=", 6)) { kind = 1; value = spec + 6; }
    else if (!strncmp(spec, "PARTUUID=", 9)) { kind = 2; value = spec + 9; }
    else if (!strncmp(spec, "PARTLABEL=", 10)) { kind = 3; value = spec + 10; }
    else return -22;
    char want[72]; unsigned n = 0;
    if (*value == '"') value++;
    while (*value && *value != '"' && n + 1 < sizeof(want)) want[n++] = *value++;
    want[n] = 0;
    struct block_partition* list[96]; unsigned count = candidates(list, 96);
    for (unsigned i = 0; i < count; i++) {
        const struct fs_identity* id = fsprobe_get(list[i]);
        const char* have = kind == 0 ? id->uuid : kind == 1 ? id->label : kind == 2 ? id->partuuid : id->partlabel;
        bool match = have[0] && (kind == 1 || kind == 3 ? !strcmp(have, want) : true);
        if (match && (kind == 0 || kind == 2)) {   /* UUIDs compare case-insensitively */
            unsigned k = 0;
            for (; have[k] && want[k]; k++) { char a = have[k], b = want[k]; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) break; }
            match = !have[k] && !want[k];
        }
        if (!match) continue;
        if (strlen(list[i]->linux_name) + 6 > capacity) return -36;
        strcpy(device, "/dev/"); strcat(device, list[i]->linux_name);
        return 0;
    }
    return -2;
}
/* root= on the kernel command line, in the Linux forms (/dev/sdb3, UUID=, LABEL=, PARTUUID=, PARTLABEL=):
 * 1 when it names p, 0 when it names something else, -1 without root=. blockdev.c select_root and
 * ext4_dev.c use it, so a USB-booted system and an installed one never take each other's root. */
bool boot_value(const char* key, char* value, unsigned size);
int block_spec_matches(struct block_partition* p, const char* spec);
int block_root_wanted(struct block_partition* p) {
    char spec[96];
    if (!boot_value("root", spec, sizeof(spec)) || !spec[0]) return -1;
    return block_spec_matches(p, spec);
}
/* A device in the Linux forms (/dev/sdb3, UUID=, LABEL=, PARTUUID=, PARTLABEL=, /dev/disk/by-*): 1 when it names p.
 * Also used for cryptroot= (cryptroot.c). */
int block_spec_matches(struct block_partition* p, const char* spec) {
    if (!strncmp(spec, "/dev/", 5) && strncmp(spec, "/dev/disk/", 10)) return !strcmp(spec + 5, p->linux_name);
    const char* links[4] = {"/dev/disk/by-uuid/", "/dev/disk/by-label/", "/dev/disk/by-partuuid/", "/dev/disk/by-partlabel/"};
    static const char* const forms[4] = {"UUID=", "LABEL=", "PARTUUID=", "PARTLABEL="};
    unsigned kind = 4; const char* value = NULL;
    for (unsigned k = 0; k < 4 && kind == 4; k++) {
        if (!strncmp(spec, forms[k], strlen(forms[k]))) { kind = k; value = spec + strlen(forms[k]); }
        else if (!strncmp(spec, links[k], strlen(links[k]))) { kind = k; value = spec + strlen(links[k]); }
    }
    if (kind == 4) return 0;
    char want[72]; unsigned n = 0;
    if (*value == '"') value++;
    while (*value && *value != '"' && n + 1 < sizeof(want)) want[n++] = *value++;
    want[n] = 0;
    if (!want[0]) return 0;
    const struct fs_identity* id = fsprobe_get(p);
    const char* have = kind == 0 ? id->uuid : kind == 1 ? id->label : kind == 2 ? id->partuuid : id->partlabel;
    if (kind == 1 || kind == 3) return have[0] && !strcmp(have, want);
    unsigned k = 0;
    for (; have[k] && want[k]; k++) { char a = have[k], b = want[k]; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) break; }
    return have[0] && !have[k] && !want[k];
}
/* /dev/disk/by-{uuid,label,partuuid} (blkdev_node.c); link names escape '/' and spaces as udev does (\x2f, \x20). */
struct storage_link { char name[72]; char target[40]; unsigned kind; };
static void link_name(const char* value, char* out, unsigned capacity) {
    unsigned n = 0;
    for (; *value && n + 5 < capacity; value++) {
        char c = *value;
        if (c == '/' || c == ' ' || c == '\\' || (unsigned char)c < 0x20) { out[n++] = '\\'; out[n++] = 'x'; out[n++] = "0123456789abcdef"[(unsigned char)c >> 4]; out[n++] = "0123456789abcdef"[c & 15]; }
        else out[n++] = c;
    }
    out[n] = 0;
}
int fsprobe_links(struct storage_link* out, unsigned capacity) {
    struct block_partition* list[96]; unsigned count = candidates(list, 96), n = 0;
    for (unsigned i = 0; i < count; i++) {
        const struct fs_identity* id = fsprobe_get(list[i]);
        const char* values[3] = {id->uuid, id->label, id->partuuid};
        for (unsigned k = 0; k < 3 && n < capacity; k++) {
            if (!values[k][0]) continue;
            bool duplicate = false;
            for (unsigned j = 0; j < n; j++) if (out[j].kind == k && !strcmp(out[j].name, values[k])) duplicate = true;
            if (duplicate) continue;
            link_name(values[k], out[n].name, sizeof(out[n].name)); strncpy(out[n].target, list[i]->linux_name, sizeof(out[n].target) - 1);
            out[n].target[sizeof(out[n].target) - 1] = 0; out[n].kind = k; n++;
        }
    }
    return (int)n;
}
/* ID_FS_* uevent variables (blkdev_node.c) for one partition: up to 6 strings into `storage`. */
unsigned fsprobe_uevent_pairs(struct block_partition* p, char storage[6][96]) {
    const struct fs_identity* id = fsprobe_get(p);
    unsigned n = 0;
    if (id->type[0]) {
        strcpy(storage[n], "ID_FS_TYPE="); strcat(storage[n++], id->type);
        strcpy(storage[n], "ID_FS_USAGE="); strcat(storage[n++], id->usage);
        if (id->uuid[0]) { strcpy(storage[n], "ID_FS_UUID="); s_strncat(storage[n++], id->uuid, 60); }
        if (id->label[0]) { strcpy(storage[n], "ID_FS_LABEL="); s_strncat(storage[n++], id->label, 60); }
    }
    if (id->partuuid[0]) { strcpy(storage[n], "ID_PART_ENTRY_UUID="); s_strncat(storage[n++], id->partuuid, 60); }
    if (id->partlabel[0] && n < 6) { strcpy(storage[n], "ID_PART_ENTRY_NAME="); s_strncat(storage[n++], id->partlabel, 60); }
    return n;
}
/* The sysfs uevent file of a block device (blkdev_node.c): sd-device (libudev) takes each KEY=VALUE line of it as a
 * property and the rest from the udev database, which does not exist here (no udev runs). So the file also carries
 * what udev's blkid builtin would have stored: util-linux lsblk reads FSTYPE/LABEL/UUID/PARTUUID/PARTLABEL/PARTTYPE/
 * PTTYPE from libudev as soon as libudev finds the device (O_PATH on /sys). *_ENC and ID_PART_ENTRY_NAME use udev's
 * encoding (\xNN for anything but [0-9A-Za-z#+-.:=@_]). Appends "\nKEY=VALUE" lines while they fit in capacity. */
static bool udev_plain(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '#' || c == '+' || c == '-' ||
           c == '.' || c == ':' || c == '=' || c == '@' || c == '_';
}
static void udev_append(char* text, unsigned capacity, const char* key, const char* value, bool encode) {
    unsigned at = strlen(text), need = strlen(key) + 2;
    for (const char* c = value; *c; c++) need += encode && !udev_plain(*c) ? 4 : 1;
    if (at + need >= capacity) return;
    text[at++] = '\n'; for (const char* c = key; *c; c++) text[at++] = *c; text[at++] = '=';
    for (const char* c = value; *c; c++) {
        if (!encode || udev_plain(*c)) { text[at++] = *c; continue; }
        text[at++] = '\\'; text[at++] = 'x'; text[at++] = "0123456789abcdef"[(uint8_t)*c >> 4]; text[at++] = "0123456789abcdef"[(uint8_t)*c & 15];
    }
    text[at] = 0;
}
void fsprobe_udev_properties(struct block_partition* p, char* text, unsigned capacity) {
    const struct fs_identity* id = fsprobe_get(p);
    if (id->type[0]) {
        udev_append(text, capacity, "ID_FS_TYPE", id->type, false); udev_append(text, capacity, "ID_FS_USAGE", id->usage, false);
        if (id->uuid[0]) { udev_append(text, capacity, "ID_FS_UUID", id->uuid, false); udev_append(text, capacity, "ID_FS_UUID_ENC", id->uuid, true); }
        if (id->label[0]) { udev_append(text, capacity, "ID_FS_LABEL", id->label, false); udev_append(text, capacity, "ID_FS_LABEL_ENC", id->label, true); }
    }
    if (p->number && p->scheme != BLOCK_SCHEME_WHOLE) {
        char value[48], digits[16];
        udev_append(text, capacity, "ID_PART_ENTRY_SCHEME", p->scheme == BLOCK_SCHEME_GPT ? "gpt" : "dos", false);
        if (id->partuuid[0]) udev_append(text, capacity, "ID_PART_ENTRY_UUID", id->partuuid, false);
        if (id->partlabel[0]) udev_append(text, capacity, "ID_PART_ENTRY_NAME", id->partlabel, true);
        if (p->scheme == BLOCK_SCHEME_GPT) guid_text(p->type_guid, value); else { strcpy(value, "0x"); append_hex(value, p->mbr_type, 2); }
        udev_append(text, capacity, "ID_PART_ENTRY_TYPE", value, false);
        uint_to_str(p->number, digits); udev_append(text, capacity, "ID_PART_ENTRY_NUMBER", digits, false);
    } else if (!p->number) {   /* a whole disk: the scheme of its partition table */
        for (unsigned i = 0; i < block_partition_count(); i++) {
            const struct block_partition* q = block_partition_get(i);
            if (q && q->dev == p->dev && q->number && q->scheme != BLOCK_SCHEME_WHOLE) {
                udev_append(text, capacity, "ID_PART_TABLE_TYPE", q->scheme == BLOCK_SCHEME_GPT ? "gpt" : "dos", false); break;
            }
        }
    }
}
