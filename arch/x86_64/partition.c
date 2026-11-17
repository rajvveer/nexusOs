/* Partition tables: MBR (primary and logical partitions) and GPT (header and entry-array CRC32
 * checked, backup header used when the primary is damaged). See blockdev.h for the root order. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

/* Reflected CRC-32 (polynomial 0xEDB88320, as zlib/UEFI), four bits per step. */
uint32_t block_crc32(const void* data, uint64_t length) {
    static const uint32_t table[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    const uint8_t* p = data; uint32_t crc = 0xFFFFFFFFu;
    while (length--) { crc ^= *p++; crc = (crc >> 4) ^ table[crc & 15]; crc = (crc >> 4) ^ table[crc & 15]; }
    return ~crc;
}
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void warn(const struct block_device* dev, const char* text) { vga_print("[64] PART "); vga_print(dev->name); vga_print(": "); vga_print(text); vga_print("\n"); }

struct scan { struct block_device* dev; struct block_partition* out; unsigned capacity, count; };
static struct block_partition* add(struct scan* s, unsigned number, uint8_t scheme, uint64_t start, uint64_t blocks) {
    if (s->count == s->capacity) { warn(s->dev, "partition list full"); return NULL; }
    if (!blocks || start > s->dev->blocks || blocks > s->dev->blocks - start) { warn(s->dev, "partition outside the disk ignored"); return NULL; }
    struct block_partition* p = &s->out[s->count++];
    memset(p, 0, sizeof(*p));
    p->dev = s->dev; p->number = number; p->scheme = scheme; p->start = start; p->blocks = blocks;
    strcpy(p->label, p->dev->name);
    if (number) block_name_number(p->label + strlen(p->label), "p", number, NULL);
    return p;
}
/* A FAT boot sector at LBA 0 means a partitionless ("superfloppy") volume, not an MBR. */
static bool fat_boot_sector(const uint8_t* s) {
    uint16_t bytes = (uint16_t)(s[11] | s[12] << 8), reserved = (uint16_t)(s[14] | s[15] << 8);
    uint32_t fat16 = (uint32_t)(s[22] | s[23] << 8), fat32 = le32(s + 36);
    return (s[0] == 0xEB || s[0] == 0xE9) && (bytes == 512 || bytes == 1024 || bytes == 2048 || bytes == 4096) &&
           s[13] && !(s[13] & (s[13] - 1)) && reserved && (s[16] == 1 || s[16] == 2) && s[21] >= 0xF0 && (fat16 || fat32) &&
           s[510] == 0x55 && s[511] == 0xAA;
}

static bool gpt_header(struct scan* s, uint8_t* buffer, uint64_t buffer_bytes, uint64_t lba, uint8_t** entries_out, uint32_t* count_out, uint32_t* size_out, uint64_t* first_out, uint64_t* last_out) {
    struct block_device* dev = s->dev; uint32_t bs = dev->block_size;
    if (lba >= dev->blocks || block_read(dev, lba, 1, buffer) < 0) return false;
    uint32_t header_size = le32(buffer + 12), crc = le32(buffer + 16);
    if (memcmp(buffer, "EFI PART", 8) || header_size < 92 || header_size > bs || le64(buffer + 24) != lba) return false;
    buffer[16] = buffer[17] = buffer[18] = buffer[19] = 0;
    if (block_crc32(buffer, header_size) != crc) return false;
    uint64_t first = le64(buffer + 40), last = le64(buffer + 48), table = le64(buffer + 72);
    uint32_t count = le32(buffer + 80), size = le32(buffer + 84), table_crc = le32(buffer + 88);
    if (first > last || last >= dev->blocks || !count || size < 128 || (size & 7) || count > 4096) return false;
    uint64_t bytes = (uint64_t)count * size, blocks = (bytes + bs - 1) / bs;
    if (bytes > buffer_bytes - bs) { warn(dev, "GPT entry array too large"); return false; }
    if (table < 2 || table >= dev->blocks || blocks > dev->blocks - table || block_read(dev, table, (uint32_t)blocks, buffer + bs) < 0) return false;
    if (block_crc32(buffer + bs, bytes) != table_crc) return false;
    *entries_out = buffer + bs; *count_out = count; *size_out = size; *first_out = first; *last_out = last;
    return true;
}
static void gpt_scan(struct scan* s, uint8_t* buffer, uint64_t buffer_bytes) {
    static const uint8_t zero[16];
    uint8_t* entries; uint32_t count, size; uint64_t first, last;
    if (!gpt_header(s, buffer, buffer_bytes, 1, &entries, &count, &size, &first, &last)) {
        if (!gpt_header(s, buffer, buffer_bytes, s->dev->blocks - 1, &entries, &count, &size, &first, &last)) { warn(s->dev, "protective MBR without a valid GPT"); return; }
        warn(s->dev, "primary GPT damaged; using the backup header");
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = entries + (uint64_t)i * size;
        if (!memcmp(e, zero, 16)) continue;
        uint64_t begin = le64(e + 32), end = le64(e + 40);
        if (begin < first || end > last || begin > end) { warn(s->dev, "GPT entry outside the usable area ignored"); continue; }
        struct block_partition* p = add(s, i + 1, BLOCK_SCHEME_GPT, begin, end - begin + 1);
        if (!p) continue;
        memcpy(p->type_guid, e, 16); memcpy(p->unique_guid, e + 16, 16); p->attributes = le64(e + 48);
        unsigned n = 0;
        for (unsigned c = 0; c < 36 && n < sizeof(p->name) - 1; c++) {
            uint16_t unit = (uint16_t)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
            if (!unit) break;
            p->name[n++] = unit >= 32 && unit < 127 ? (char)unit : '?';
        }
        p->name[n] = 0;
    }
}
static void mbr_scan(struct scan* s, const uint8_t* mbr, uint8_t* buffer) {
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t* e = mbr + 446 + 16 * i;
        uint8_t type = e[4]; uint32_t start = le32(e + 8), count = le32(e + 12);
        if (!type || !count) continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            /* Extended partition: a chain of EBRs, each with one logical partition and a link. */
            uint64_t next = start; unsigned number = 5;
            for (unsigned guard = 0; guard < 64 && next; guard++) {
                if (next >= s->dev->blocks || block_read(s->dev, next, 1, buffer) < 0 || buffer[510] != 0x55 || buffer[511] != 0xAA) { warn(s->dev, "bad extended boot record"); break; }
                const uint8_t* logical = buffer + 446; const uint8_t* link = buffer + 462;
                if (logical[4] && le32(logical + 12)) {
                    struct block_partition* p = add(s, number++, BLOCK_SCHEME_MBR, next + le32(logical + 8), le32(logical + 12));
                    if (p) p->mbr_type = logical[4];
                }
                uint64_t following = (link[4] == 0x05 || link[4] == 0x0F || link[4] == 0x85) && le32(link + 8) ? start + le32(link + 8) : 0;
                if (following && following <= next) { warn(s->dev, "extended partition loop"); break; }
                next = following;
            }
            continue;
        }
        struct block_partition* p = add(s, i + 1, BLOCK_SCHEME_MBR, start, count);
        if (p) p->mbr_type = type;
    }
}
unsigned partition_scan(struct block_device* dev, struct block_partition* out, unsigned capacity, uint8_t* scratch, uint64_t scratch_bytes) {
    struct scan s = {dev, out, capacity, 0};
    uint32_t bs = dev->block_size;
    if (!capacity || scratch_bytes < 3ULL * bs || block_read(dev, 0, 1, scratch) < 0) { warn(dev, "cannot read sector 0"); return 0; }
    uint8_t* mbr = scratch + scratch_bytes - bs; memcpy(mbr, scratch, bs);
    bool valid = mbr[510] == 0x55 && mbr[511] == 0xAA, protective = false;
    for (unsigned i = 0; valid && i < 4; i++) {
        const uint8_t* e = mbr + 446 + 16 * i;
        if (e[0] && e[0] != 0x80) valid = false;
        if (e[4] == 0xEE) protective = true;
    }
    if (protective) gpt_scan(&s, scratch, scratch_bytes - bs);
    else if (fat_boot_sector(mbr)) add(&s, 0, BLOCK_SCHEME_WHOLE, 0, dev->blocks);
    else if (valid) mbr_scan(&s, mbr, scratch);
    return s.count;
}
