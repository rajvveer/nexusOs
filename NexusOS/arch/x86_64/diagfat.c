/* NEXUSDIAG.TXT on the NexusOS boot medium: a small, self-contained FAT16/FAT32 writer for one file in the
 * root directory of a volume that is not the mounted root (the ESP holding EFI\NEXUS\KERNEL.BIN; the root
 * file system never has the ESP partition type, so fat32.c never caches it). The file gets a long name entry
 * plus the short alias NEXUSD~1.TXT; an older NEXUSDIAG.TXT is deleted first (its clusters freed). Order:
 * data clusters, FAT chain (every FAT copy), directory entries, FSInfo hint invalidated, device flush.
 * Only volumes carrying EFI\NEXUS\KERNEL.BIN are written, so no other operating system's volume is touched. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/rtc.h"

#define DIR_SECTORS_MAX 128                      /* 64 KiB of directory: 2048 entries */
struct vol {
    const struct block_partition* part;
    uint32_t spc, reserved, fats, fat_size, root_sectors, first_data, clusters, root_cluster, fsinfo; int type;
    uint8_t* sector;                             /* 512-byte scratch */
    uint8_t* dir; uint64_t dir_lba[DIR_SECTORS_MAX]; unsigned dir_sectors; uint32_t dir_last_cluster;
    uint64_t fat_cached; uint8_t* fat;           /* one cached FAT sector (first copy) */
};
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static bool vol_open(const struct block_partition* part, struct vol* v) {
    uint8_t* b = v->sector;
    if (block_part_sectors(part) < 64 || block_part_read(part, 0, 1, b) < 0 || b[510] != 0x55 || b[511] != 0xAA) return false;
    uint32_t spc = b[13], reserved = le16(b + 14), fats = b[16], entries = le16(b + 17);
    uint32_t total = le16(b + 19) ? le16(b + 19) : le32(b + 32), fat_size = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if (le16(b + 11) != 512 || !spc || (spc & (spc - 1)) || !reserved || !fats || fats > 2 || !fat_size || !total || total > block_part_sectors(part)) return false;
    v->part = part; v->spc = spc; v->reserved = reserved; v->fats = fats; v->fat_size = fat_size;
    v->root_sectors = (entries * 32 + 511) / 512; v->first_data = reserved + fats * fat_size + v->root_sectors;
    if (v->first_data >= total) return false;
    v->clusters = (total - v->first_data) / spc;
    v->type = v->clusters < 4085 ? 12 : v->clusters < 65525 ? 16 : 32;
    if (v->type == 12) return false;                                              /* floppy-sized volumes: not supported */
    v->root_cluster = v->type == 32 ? le32(b + 44) : 0; v->fsinfo = v->type == 32 ? le16(b + 48) : 0;
    if (v->type == 32 && (v->root_cluster < 2 || v->root_cluster >= v->clusters + 2)) return false;
    v->fat_cached = ~0ULL;
    return true;
}
static uint64_t cluster_lba(const struct vol* v, uint32_t c) { return v->first_data + (uint64_t)(c - 2) * v->spc; }
static uint32_t eoc(const struct vol* v) { return v->type == 32 ? 0x0FFFFFFFU : 0xFFFFU; }
static bool is_eoc(const struct vol* v, uint32_t value) { return v->type == 32 ? value >= 0x0FFFFFF8U : value >= 0xFFF8U; }
static bool fat_get(struct vol* v, uint32_t c, uint32_t* value) {
    uint32_t offset = v->type == 32 ? c * 4 : c * 2; uint64_t lba = v->reserved + offset / 512;
    if (lba != v->fat_cached) { if (block_part_read(v->part, lba, 1, v->fat) < 0) return false; v->fat_cached = lba; }
    *value = v->type == 32 ? le32(v->fat + offset % 512) & 0x0FFFFFFFU : le16(v->fat + offset % 512);
    return true;
}
static bool fat_set(struct vol* v, uint32_t c, uint32_t value) {
    uint32_t offset = v->type == 32 ? c * 4 : c * 2;
    for (uint32_t f = 0; f < v->fats; f++) {
        uint64_t lba = v->reserved + (uint64_t)f * v->fat_size + offset / 512;
        if (block_part_read(v->part, lba, 1, v->sector) < 0) return false;
        if (v->type == 32) put32(v->sector + offset % 512, (le32(v->sector + offset % 512) & 0xF0000000U) | (value & 0x0FFFFFFFU));
        else put16(v->sector + offset % 512, (uint16_t)value);
        if (block_part_write(v->part, lba, 1, v->sector) < 0) return false;
        if (!f && lba == v->fat_cached) memcpy(v->fat, v->sector, 512);
    }
    return true;
}
/* Load a directory (fixed FAT16 root when cluster == 0) into v->dir. */
static bool dir_load(struct vol* v, uint32_t cluster) {
    v->dir_sectors = 0; v->dir_last_cluster = 0;
    if (!cluster) {
        if (v->root_sectors > DIR_SECTORS_MAX) return false;
        for (uint32_t i = 0; i < v->root_sectors; i++) v->dir_lba[v->dir_sectors++] = v->reserved + (uint64_t)v->fats * v->fat_size + i;
    } else {
        for (unsigned guard = 0; cluster >= 2 && cluster < v->clusters + 2 && guard < 4096; guard++) {
            for (uint32_t i = 0; i < v->spc; i++) { if (v->dir_sectors == DIR_SECTORS_MAX) return false; v->dir_lba[v->dir_sectors++] = cluster_lba(v, cluster) + i; }
            v->dir_last_cluster = cluster;
            uint32_t next; if (!fat_get(v, cluster, &next)) return false;
            if (is_eoc(v, next)) break;
            cluster = next;
        }
    }
    for (unsigned i = 0; i < v->dir_sectors; i++) if (block_part_read(v->part, v->dir_lba[i], 1, v->dir + 512 * i) < 0) return false;
    return v->dir_sectors > 0;
}
static bool dir_store(struct vol* v, unsigned first_entry, unsigned count) {
    for (unsigned s = first_entry / 16; s <= (first_entry + count - 1) / 16 && s < v->dir_sectors; s++)
        if (block_part_write(v->part, v->dir_lba[s], 1, v->dir + 512 * s) < 0) return false;
    return true;
}
static uint8_t short_checksum(const uint8_t* name11) { uint8_t sum = 0; for (unsigned i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]); return sum; }
static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
/* Find `name` (long name, or 8.3 short name, case-insensitive); returns the short entry index, *lfn_first
 * the first entry of its long name (== the short entry when it has none). -1 when absent. */
static int dir_find(struct vol* v, const char* name, unsigned* lfn_first) {
    unsigned entries = v->dir_sectors * 16; char lfn[64]; int lfn_start = -1; uint8_t lfn_sum = 0; bool lfn_ok = false;
    for (unsigned i = 0; i < entries; i++) {
        uint8_t* e = v->dir + 32 * i;
        if (!e[0]) break;
        if (e[0] == 0xE5) { lfn_start = -1; continue; }
        if (e[11] == 0x0F) {
            unsigned ordinal = e[0] & 0x1F;
            if (e[0] & 0x40) { lfn_start = (int)i; lfn_sum = e[13]; lfn_ok = ordinal >= 1 && ordinal <= 4; memset(lfn, 0, sizeof(lfn)); }
            if (lfn_start < 0 || e[13] != lfn_sum || !ordinal || ordinal > 4) { lfn_ok = false; continue; }
            static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (unsigned k = 0; k < 13; k++) { uint16_t c = le16(e + at[k]); lfn[(ordinal - 1) * 13 + k] = c == 0xFFFF ? 0 : c < 0x80 ? (char)c : '?'; }
            continue;
        }
        char text[64]; unsigned n = 0;
        if (lfn_start >= 0 && lfn_ok && short_checksum(e) == lfn_sum) { memcpy(text, lfn, sizeof(text)); text[63] = 0; }
        else {
            for (unsigned k = 0; k < 8 && e[k] != ' '; k++) text[n++] = (char)e[k];
            if (e[8] != ' ') { text[n++] = '.'; for (unsigned k = 8; k < 11 && e[k] != ' '; k++) text[n++] = (char)e[k]; }
            text[n] = 0;
        }
        bool match = true; for (unsigned k = 0; match; k++) { if (upper(text[k]) != upper(name[k])) match = false; else if (!name[k]) break; }
        if (match && !(e[11] & 0x08)) { *lfn_first = lfn_start >= 0 && lfn_ok ? (unsigned)lfn_start : i; return (int)i; }
        lfn_start = -1;
    }
    return -1;
}
static uint32_t entry_cluster(const struct vol* v, const uint8_t* e) { return (v->type == 32 ? (uint32_t)le16(e + 20) << 16 : 0) | le16(e + 26); }
static bool free_chain(struct vol* v, uint32_t cluster) {
    for (unsigned guard = 0; cluster >= 2 && cluster < v->clusters + 2 && guard < (1U << 22); guard++) {
        uint32_t next; if (!fat_get(v, cluster, &next) || !fat_set(v, cluster, 0)) return false;
        if (is_eoc(v, next) || !next) break;
        cluster = next;
    }
    return true;
}
/* Allocate `count` clusters as one chain (any free entries, ascending); 0 when the volume is full. */
static uint32_t allocate(struct vol* v, uint32_t count, uint32_t* last) {
    uint32_t first = 0, previous = 0, found = 0;
    for (uint32_t c = 2; c < v->clusters + 2 && found < count; c++) {
        uint32_t value; if (!fat_get(v, c, &value)) return 0;
        if (value) continue;
        if (!fat_set(v, c, eoc(v))) return 0;
        if (previous && !fat_set(v, previous, c)) return 0;
        if (!first) first = c;
        previous = c; found++;
    }
    if (found < count) { if (first) free_chain(v, first); return 0; }
    *last = previous; return first;
}

static bool path_has_kernel(struct vol* v) {
    static const char* const parts[] = {"EFI", "NEXUS", "KERNEL.BIN"};
    uint32_t cluster = v->root_cluster;
    for (unsigned k = 0; k < 3; k++) {
        unsigned first; if (!dir_load(v, cluster)) return false;
        int at = dir_find(v, parts[k], &first);
        if (at < 0) return false;
        const uint8_t* e = v->dir + 32 * at;
        if (k < 2) { if (!(e[11] & 0x10)) return false; cluster = entry_cluster(v, e); if (!cluster) return false; }
        else return !(e[11] & 0x10);
    }
    return false;
}
static void stamp(uint8_t* e) {
    rtc_time_t now; rtc_read(&now);
    uint16_t time = (uint16_t)(now.hour << 11 | now.minute << 5 | now.second / 2);
    uint16_t date = (uint16_t)((now.year >= 1980 ? now.year - 1980 : 0) << 9 | now.month << 5 | now.day);
    put16(e + 14, time); put16(e + 16, date); put16(e + 18, date); put16(e + 22, time); put16(e + 24, date);
}
static int write_file(struct vol* v, const uint8_t* data, uint64_t bytes) {
    static const uint8_t alias[11] = {'N', 'E', 'X', 'U', 'S', 'D', '~', '1', 'T', 'X', 'T'};
    static const char long_name[13] = {'N', 'E', 'X', 'U', 'S', 'D', 'I', 'A', 'G', '.', 'T', 'X', 'T'};
    unsigned first;
    if (!dir_load(v, v->root_cluster)) return -5;
    int old = dir_find(v, "NEXUSDIAG.TXT", &first);
    if (old >= 0) {
        uint32_t cluster = entry_cluster(v, v->dir + 32 * old);
        for (unsigned i = first; i <= (unsigned)old; i++) v->dir[32 * i] = 0xE5;
        if (!dir_store(v, first, (unsigned)old - first + 1) || (cluster && !free_chain(v, cluster))) return -5;
    }
    uint64_t cluster_bytes = 512ULL * v->spc, count = (bytes + cluster_bytes - 1) / cluster_bytes;
    uint32_t last = 0, start = count ? allocate(v, (uint32_t)count, &last) : 0;
    if (count && !start) return -28;
    uint32_t c = start;
    for (uint64_t done = 0; done < bytes; ) {
        for (uint32_t s = 0; s < v->spc; s++) {
            uint64_t n = bytes - done > 512 ? 512 : bytes - done;
            memset(v->sector, 0, 512); if (n) memcpy(v->sector, data + done, n); done += n;
            if (block_part_write(v->part, cluster_lba(v, c) + s, 1, v->sector) < 0) return -5;
        }
        if (done < bytes && (!fat_get(v, c, &c) || is_eoc(v, c))) return -5;
    }
    /* two free entries in one directory sector (long name + alias); a FAT32 root grows by a cluster if needed */
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (!dir_load(v, v->root_cluster)) return -5;
        unsigned entries = v->dir_sectors * 16;
        for (unsigned i = 0; i + 1 < entries; i++) {
            if ((i % 16) == 15) continue;
            uint8_t *a = v->dir + 32 * i, *b = a + 32;
            bool a_free = a[0] == 0 || a[0] == 0xE5, b_free = b[0] == 0 || b[0] == 0xE5;
            if (!a_free || !b_free) continue;
            memset(a, 0, 64);
            a[0] = 0x41; a[11] = 0x0F; a[13] = short_checksum(alias);
            static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (unsigned k = 0; k < 13; k++) put16(a + at[k], (uint16_t)long_name[k]);
            memcpy(b, alias, 11); b[11] = 0x20; stamp(b);
            put16(b + 20, (uint16_t)(start >> 16)); put16(b + 26, (uint16_t)start); put32(b + 28, (uint32_t)bytes);
            if (!dir_store(v, i, 2)) return -5;
            if (v->fsinfo && block_part_read(v->part, v->fsinfo, 1, v->sector) == 0 && le32(v->sector) == 0x41615252U) {
                put32(v->sector + 488, 0xFFFFFFFFU); put32(v->sector + 492, 0xFFFFFFFFU);   /* free count / next free: unknown */
                block_part_write(v->part, v->fsinfo, 1, v->sector);
            }
            return block_flush(v->part->dev) < 0 ? -5 : 1;
        }
        if (v->type != 32) break;
        uint32_t extra_last, extra = allocate(v, 1, &extra_last);
        if (!extra) break;
        memset(v->sector, 0, 512);
        for (uint32_t s = 0; s < v->spc; s++) if (block_part_write(v->part, cluster_lba(v, extra) + s, 1, v->sector) < 0) return -5;
        if (!fat_set(v, v->dir_last_cluster, extra)) return -5;
    }
    if (start) free_chain(v, start);
    return -28;
}

static void copy(char* out, unsigned size, const char* a, const char* b, const char* c) {
    unsigned n = 0;
    for (const char* parts[3] = {a, b, c}, **p = parts; p < parts + 3; p++) for (const char* t = *p; t && *t && n + 1 < size; t++) out[n++] = *t;
    out[n] = 0;
}
/* 1 written (where = volume), else the reason. */
int diagfat_write(const uint8_t* data, uint64_t bytes, char* where, unsigned size) {
    uint64_t pages = arena_alloc(4096 + DIR_SECTORS_MAX * 512);
    if (!pages) { copy(where, size, "no memory", NULL, NULL); return -12; }
    struct vol v; memset(&v, 0, sizeof(v));
    v.sector = (uint8_t*)(uintptr_t)pages; v.fat = v.sector + 512; v.dir = v.sector + 4096;
    int result = 0;
    for (unsigned i = 0; i < block_partition_count() && result <= 0; i++) {
        const struct block_partition* part = block_partition_get(i);
        if (part == block_root() || !vol_open(part, &v) || !path_has_kernel(&v)) continue;
        result = write_file(&v, data, bytes);
        copy(where, size, part->label, result > 0 ? (v.type == 32 ? " (FAT32 boot medium)" : " (FAT16 boot medium)") : " (boot medium): write failed",
             result == -28 ? " - volume or root directory full" : NULL);
    }
    if (!result) copy(where, size, "no reachable NexusOS boot medium (a FAT volume with EFI\\NEXUS\\KERNEL.BIN); "
                      "a USB stick needs a USB storage driver - the UEFI loader's firmware-only NEXUSDIAG.TXT stays there", NULL, NULL);
    arena_free(pages, 4096 + DIR_SECTORS_MAX * 512);
    return result;
}
