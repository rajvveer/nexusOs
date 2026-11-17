/* exFAT (SD cards, USB sticks > 32 GB, camera media), read/write, any number of instances, like Linux fs/exfat.
 *
 * On-disk: the boot region (checksummed; VolumeFlags.VolumeDirty set while mounted read-write and cleared at a clean
 * unmount, as the specification and Linux do; a volume found dirty is reported and mounted anyway), one FAT (TexFAT
 * volumes with two FATs mount read-only), the allocation bitmap (the authority on free clusters), the up-case table
 * (loaded and checksummed; names compare case-insensitively through it, the default table when it is damaged) and
 * directory entry sets: File (0x85) + Stream Extension (0xC0) + File Name (0xC1) entries, checked with their
 * SetChecksum and NameHash (sets that fail are skipped, fsck.exfat repairs them). Files and directories may be
 * contiguous (NoFatChain: the FAT is not used) or FAT-chained; an object that cannot grow contiguously is converted
 * to a FAT chain first. ValidDataLength is honoured: bytes between it and DataLength read as zeros, writes past it
 * zero the gap. Times are stored in UTC with a valid UtcOffset (0); offsets on disk are applied when reading.
 * Mount options (Linux exfat): uid=, gid=, umask=, dmask=, fmask=, allow_utime=, iocharset=utf8, errors=, discard,
 * keep_last_dots, time_offset= (minutes, for entries without a valid offset), sys_tz, quiet. Modes: 0777 & ~mask;
 * the ReadOnly attribute removes the write bits; chmod may only toggle the write bits (EPERM otherwise, "quiet":
 * ignored). Names: up to 255 UTF-16 units, no control characters and none of " * / : < > ? \ |, trailing dots
 * removed unless keep_last_dots. Every value read from the medium is range-checked (EIO, never a fault).
 * Handles: first/size = FirstCluster/DataLength, iflags bit 0 = NoFatChain, bit 1 = the parent is contiguous,
 * parent = the parent directory's first cluster, entry_index = the File entry's index in it, dir_lba/dir_off = its
 * sector and offset, lfn_count = SecondaryCount; the root has dir_lba = MOUNT_ROOT_LBA. Identity (ino): the File
 * entry's slot, remapped across rename and unlink like vfat. */
#include "mount.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define CACHE_SECTORS 64
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define ATTR_RO 0x01
#define ATTR_DIR 0x10
#define ATTR_ARCHIVE 0x20
#define NOFATCHAIN 1u
#define PARENT_CONTIG 2u
#define EOC 0xFFFFFFFFu
#define MAX_SET 19

struct sector_cache { uint64_t sector; uint8_t* data; uint64_t used; bool valid, dirty; };
struct ident { uint64_t slot, ino; };
struct exfat {
    struct mount* m; struct block_partition* part;
    uint32_t sector_size, scale, cluster_sectors, cluster_bytes, entries_per_cluster;
    uint64_t volume_sectors, fat_offset, fat_length, heap_offset;
    uint32_t clusters, root_cluster, serial, free_count, next_free, epoch;   /* epoch: bumped when clusters are freed */
    bool readonly, found_dirty, marked_dirty, io_failed, quiet, keep_dots;
    uint32_t* bitmap_chain; uint32_t bitmap_clusters; uint64_t bitmap_bytes;
    uint16_t* upcase;
    uint32_t uid, gid; uint16_t fmask, dmask; int64_t offset_ns;
    struct sector_cache cache[CACHE_SECTORS]; uint64_t clock;
    struct ident* idents; unsigned ident_count, ident_capacity; uint64_t generation;
    uint8_t* scratch;   /* one cluster */
    uint8_t* sector_buf;              /* one sector: metadata edits (not on the stack) */
    char label[48];
};
static struct exfat* fs_of(struct mount* m) { return m->sb; }

/* ---- sectors (exFAT sectors, 512..4096 bytes) ---- */
static int raw_read(struct exfat* fs, uint64_t sector, uint32_t count, void* buffer) {
    if (sector >= fs->volume_sectors || count > fs->volume_sectors - sector) return -5;
    return block_part_read(fs->part, sector * fs->scale, count * fs->scale, buffer) < 0 ? -5 : 0;
}
static int raw_write(struct exfat* fs, uint64_t sector, uint32_t count, const void* buffer) {
    if (fs->readonly) return -30;
    if (sector >= fs->volume_sectors || count > fs->volume_sectors - sector) return -5;
    if (block_part_write(fs->part, sector * fs->scale, count * fs->scale, buffer) < 0) { fs->io_failed = true; return -5; }
    return 0;
}
static int flush_entry(struct exfat* fs, struct sector_cache* c) {
    if (!c->valid || !c->dirty) return 0;
    if (raw_write(fs, c->sector, 1, c->data)) return -5;
    c->dirty = false; return 0;
}
static int flush_cache(struct exfat* fs) {
    int e = 0;
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (flush_entry(fs, &fs->cache[i])) e = -5;
    return e;
}
static uint8_t* sector_get(struct exfat* fs, uint64_t sector) {
    struct sector_cache* pick = NULL;
    for (unsigned i = 0; i < CACHE_SECTORS; i++) {
        struct sector_cache* c = &fs->cache[i];
        if (c->valid && c->sector == sector) { c->used = ++fs->clock; return c->data; }
        if (!pick || !c->valid || (pick->valid && c->used < pick->used)) pick = c;
    }
    if (pick->valid && pick->dirty && flush_entry(fs, pick)) return NULL;
    pick->valid = false; pick->dirty = false;
    if (raw_read(fs, sector, 1, pick->data)) return NULL;
    pick->sector = sector; pick->valid = true; pick->used = ++fs->clock;
    return pick->data;
}
static uint8_t* sector_dirty(struct exfat* fs, uint64_t sector) {   /* FAT and bitmap sectors: written back later */
    if (fs->readonly) return NULL;
    uint8_t* s = sector_get(fs, sector); if (!s) return NULL;
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].valid && fs->cache[i].sector == sector) fs->cache[i].dirty = true;
    return s;
}
static int sector_put(struct exfat* fs, uint64_t sector, const uint8_t* data) {   /* directory sectors: written through */
    int e = raw_write(fs, sector, 1, data);
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].valid && fs->cache[i].sector == sector) {
        if (e) fs->cache[i].valid = false; else { if (fs->cache[i].data != data) memcpy(fs->cache[i].data, data, fs->sector_size); fs->cache[i].dirty = false; }
    }
    return e;
}
static void forget_range(struct exfat* fs, uint64_t first, uint64_t count) {
    for (unsigned i = 0; i < CACHE_SECTORS; i++)
        if (fs->cache[i].valid && fs->cache[i].sector >= first && fs->cache[i].sector < first + count) {
            if (fs->cache[i].dirty) flush_entry(fs, &fs->cache[i]);
            fs->cache[i].valid = false;
        }
}

/* ---- FAT, bitmap, chains ---- */
static bool valid_cluster(struct exfat* fs, uint32_t c) { return c >= 2 && c < fs->clusters + 2; }
static uint64_t cluster_sector(struct exfat* fs, uint32_t c) { return fs->heap_offset + (uint64_t)(c - 2) * fs->cluster_sectors; }
static int fat_get(struct exfat* fs, uint32_t c, uint32_t* value) {
    if (c >= fs->clusters + 2) return -5;
    uint64_t at = (uint64_t)c * 4, sector = fs->fat_offset + at / fs->sector_size;
    if (sector >= fs->fat_offset + fs->fat_length) return -5;
    uint8_t* s = sector_get(fs, sector); if (!s) return -5;
    *value = get_le32(s + at % fs->sector_size); return 0;
}
static int fat_set(struct exfat* fs, uint32_t c, uint32_t value) {
    if (c >= fs->clusters + 2) return -5;
    uint64_t at = (uint64_t)c * 4, sector = fs->fat_offset + at / fs->sector_size;
    uint8_t* s = sector_dirty(fs, sector); if (!s) return fs->readonly ? -30 : -5;
    put_le32(s + at % fs->sector_size, value); return 0;
}
static int bitmap_locate(struct exfat* fs, uint32_t c, uint64_t* sector, unsigned* byte, unsigned* bit) {
    if (!valid_cluster(fs, c)) return -5;
    uint64_t index = c - 2, offset = index / 8;
    if (offset >= fs->bitmap_bytes) return -5;
    uint32_t which = (uint32_t)(offset / fs->cluster_bytes); if (which >= fs->bitmap_clusters) return -5;
    uint64_t within = offset % fs->cluster_bytes;
    *sector = cluster_sector(fs, fs->bitmap_chain[which]) + within / fs->sector_size; *byte = (unsigned)(within % fs->sector_size); *bit = index % 8;
    return 0;
}
static int bitmap_get(struct exfat* fs, uint32_t c, bool* used) {
    uint64_t sector; unsigned byte, bit; if (bitmap_locate(fs, c, &sector, &byte, &bit)) return -5;
    uint8_t* s = sector_get(fs, sector); if (!s) return -5;
    *used = (s[byte] >> bit) & 1; return 0;
}
static int bitmap_set(struct exfat* fs, uint32_t c, bool used) {
    uint64_t sector; unsigned byte, bit; if (bitmap_locate(fs, c, &sector, &byte, &bit)) return -5;
    uint8_t* s = sector_dirty(fs, sector); if (!s) return fs->readonly ? -30 : -5;
    bool was = (s[byte] >> bit) & 1;
    if (used) s[byte] |= (uint8_t)(1u << bit); else s[byte] &= (uint8_t)~(1u << bit);
    if (was != used) { if (used) fs->free_count--; else fs->free_count++; }
    return 0;
}
static int count_free(struct exfat* fs) {
    uint32_t used = 0;
    for (uint32_t which = 0; which < fs->bitmap_clusters; which++)
        for (uint32_t s = 0; s < fs->cluster_sectors; s++) {
            uint64_t first_bit = ((uint64_t)which * fs->cluster_bytes + (uint64_t)s * fs->sector_size) * 8;
            if (first_bit >= fs->clusters) break;
            uint8_t* data = sector_get(fs, cluster_sector(fs, fs->bitmap_chain[which]) + s); if (!data) return -5;
            for (uint32_t i = 0; i < fs->sector_size; i++) {
                uint64_t bit0 = first_bit + (uint64_t)i * 8; if (bit0 >= fs->clusters) break;
                uint8_t v = data[i]; if (fs->clusters - bit0 < 8) v &= (uint8_t)((1u << (fs->clusters - bit0)) - 1);
                used += (uint32_t)__builtin_popcount(v);
            }
        }
    fs->free_count = fs->clusters - used; return 0;
}
/* One free cluster, from the hint on (wrapping); 0 or -ENOSPC/-EIO. */
static int allocate_one(struct exfat* fs, uint32_t hint, uint32_t* out) {
    if (!fs->free_count) return -28;
    if (!valid_cluster(fs, hint)) hint = fs->next_free;
    if (!valid_cluster(fs, hint)) hint = 2;
    for (uint32_t n = 0; n < fs->clusters; n++) {
        uint32_t c = 2 + (hint - 2 + n) % fs->clusters;
        bool used; if (bitmap_get(fs, c, &used)) return -5;
        if (used) continue;
        if (bitmap_set(fs, c, true)) return -5;
        fs->next_free = c + 1; *out = c; return 0;
    }
    return -28;
}
static int cluster_at(struct exfat* fs, fat32_file_t* f, uint64_t index, uint32_t* out) {
    if (!valid_cluster(fs, f->first)) return -5;
    if (f->iflags & NOFATCHAIN) {
        if (index >= fs->clusters || f->first + index >= (uint64_t)fs->clusters + 2) return -5;
        *out = (uint32_t)(f->first + index); return 0;
    }
    uint32_t c = f->first; uint64_t at = 0;
    if (f->chain_epoch == fs->epoch + 1 && f->cursor_index <= index && valid_cluster(fs, f->cursor)) { c = f->cursor; at = f->cursor_index; }
    for (; at < index; at++) {
        uint32_t next; if (fat_get(fs, c, &next) || !valid_cluster(fs, next)) return -5;
        c = next;
        if (at > fs->clusters) return -5;   /* a loop */
    }
    if (index <= 0xFFFFFFFFULL) { f->cursor = c; f->cursor_index = (uint32_t)index; f->chain_epoch = fs->epoch + 1; }
    *out = c; return 0;
}
static int free_clusters(struct exfat* fs, uint32_t first, uint64_t count, bool contig) {   /* count: for NoFatChain objects */
    if (!valid_cluster(fs, first)) return 0;
    fs->epoch++;   /* chain position hints may name freed clusters */
    if (contig) {
        for (uint64_t i = 0; i < count; i++) { if (!valid_cluster(fs, first + (uint32_t)i)) return -5; if (bitmap_set(fs, first + (uint32_t)i, false)) return -5; }
        return 0;
    }
    uint32_t c = first;
    for (uint64_t n = 0; n <= fs->clusters; n++) {
        uint32_t next; if (fat_get(fs, c, &next)) return -5;
        if (bitmap_set(fs, c, false) || fat_set(fs, c, 0)) return -5;
        if (next == EOC || !valid_cluster(fs, next)) return 0;
        c = next;
    }
    return -5;
}
static uint64_t clusters_for(struct exfat* fs, uint64_t bytes) { return (bytes + fs->cluster_bytes - 1) / fs->cluster_bytes; }

/* ---- times ---- */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static int64_t exfat_time(struct exfat* fs, uint32_t stamp, uint8_t tenms, uint8_t offset) {
    unsigned sec = (stamp & 31) * 2, min = (stamp >> 5) & 63, hour = (stamp >> 11) & 31, day = (stamp >> 16) & 31, month = (stamp >> 21) & 15, year = 1980 + (stamp >> 25);
    if (!day || !month || month > 12) return 0;
    int64_t s = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
    int64_t ns = s * 1000000000LL + (int64_t)(tenms < 200 ? tenms : 0) * 10000000LL;
    if (offset & 0x80) { int off = offset & 0x7F; if (off & 0x40) off -= 0x80; ns -= (int64_t)off * 15 * 60 * 1000000000LL; }
    else ns -= fs->offset_ns;
    return ns;
}
static void exfat_stamp(int64_t ns, uint32_t* stamp, uint8_t* tenms) {   /* UTC */
    int64_t s = ns / 1000000000LL; if (ns < 0 && ns % 1000000000LL) s--;
    if (s < 315532800LL) s = 315532800LL;
    int64_t days = s / 86400, rem = s % 86400;
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    if (y > 2107) y = 2107;
    *stamp = (uint32_t)((y - 1980) << 25 | m << 21 | d << 16 | (uint32_t)(rem / 3600) << 11 | (uint32_t)(rem % 3600 / 60) << 5 | (uint32_t)(rem % 60 / 2));
    if (tenms) { int64_t sub = ns % 1000000000LL; if (sub < 0) sub += 1000000000LL; *tenms = (uint8_t)((rem % 2) * 100 + sub / 10000000LL); }
}

/* ---- names ---- */
static unsigned utf8_decode(const char* s, unsigned* used) {
    const uint8_t* u = (const uint8_t*)s;
    if (u[0] < 0x80) { *used = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) { *used = 2; return (u[0] & 31u) << 6 | (u[1] & 63u); }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) { *used = 3; return (u[0] & 15u) << 12 | (u[1] & 63u) << 6 | (u[2] & 63u); }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) { *used = 4; return (u[0] & 7u) << 18 | (u[1] & 63u) << 12 | (u[2] & 63u) << 6 | (u[3] & 63u); }
    *used = 1; return 0xFFFD;
}
static int to_utf16(const char* name, uint16_t* out, unsigned capacity) {   /* units, or -EINVAL / -ENAMETOOLONG */
    unsigned n = 0;
    for (const char* p = name; *p; ) {
        unsigned used, c = utf8_decode(p, &used); p += used;
        if (c >= 0x10000) { if (n + 2 > capacity) return -36; c -= 0x10000; out[n++] = (uint16_t)(0xD800 | c >> 10); out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF)); }
        else { if (n + 1 > capacity) return -36; out[n++] = (uint16_t)c; }
    }
    return (int)n;
}
static void from_utf16(const uint16_t* in, unsigned count, char* out, unsigned capacity) {
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned c = in[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00); i++; }
        char b[4]; unsigned k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); k = 3; }
        else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); k = 4; }
        if (n + k + 1 > capacity) break;
        memcpy(out + n, b, k); n += k;
    }
    out[n] = 0;
}
static uint16_t up(struct exfat* fs, uint16_t c) { return fs->upcase ? fs->upcase[c] : (c >= 'a' && c <= 'z' ? (uint16_t)(c - 32) : c); }
static uint16_t name_hash(struct exfat* fs, const uint16_t* name, unsigned n) {
    uint16_t hash = 0;
    for (unsigned i = 0; i < n; i++) {
        uint16_t c = up(fs, name[i]);
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c & 0xFF));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c >> 8));
    }
    return hash;
}
static uint16_t set_checksum(const uint8_t* entries, unsigned count) {
    uint16_t sum = 0;
    for (unsigned i = 0; i < count * 32; i++) { if (i == 2 || i == 3) continue; sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + entries[i]); }
    return sum;
}
static bool same_utf16(struct exfat* fs, const uint16_t* a, unsigned an, const uint16_t* b, unsigned bn) {
    if (an != bn) return false;
    for (unsigned i = 0; i < an; i++) if (up(fs, a[i]) != up(fs, b[i])) return false;
    return true;
}

/* ---- directories ---- */
struct dir_iter { uint32_t first, cluster; uint64_t index, limit; bool contig; };   /* limit: entries (DataLength / 32) */
struct entry_set {
    uint8_t raw[MAX_SET * 32]; unsigned count;                 /* File + secondaries */
    uint64_t sectors[MAX_SET]; uint16_t offsets[MAX_SET];      /* where each entry lives */
    uint64_t index;                                            /* the File entry's index */
    uint16_t name[256]; unsigned name_length;
};
static void iter_begin(struct exfat* fs, struct dir_iter* it, uint32_t first, bool contig, uint64_t length) {
    it->first = first; it->cluster = first; it->index = 0; it->contig = contig;
    it->limit = length ? length / 32 : (uint64_t)fs->clusters * fs->entries_per_cluster;
}
/* Location of entry `index` (advancing the iterator's cluster: indexes only grow). */
static int iter_locate(struct exfat* fs, struct dir_iter* it, uint64_t index, uint64_t* sector, unsigned* offset) {
    if (index >= it->limit) return -61;   /* ENODATA: past the end */
    uint64_t want = index / fs->entries_per_cluster, have = it->index / fs->entries_per_cluster;
    if (want < have || !valid_cluster(fs, it->cluster)) { it->cluster = it->first; have = 0; }
    while (have < want) {
        uint32_t next;
        if (it->contig) next = it->cluster + 1;
        else if (fat_get(fs, it->cluster, &next)) return -5;
        if (!valid_cluster(fs, next)) return -61;
        it->cluster = next; have++;
    }
    it->index = index;
    uint64_t byte = (index % fs->entries_per_cluster) * 32;
    *sector = cluster_sector(fs, it->cluster) + byte / fs->sector_size; *offset = (unsigned)(byte % fs->sector_size);
    return 0;
}
/* The next valid entry set at or after *index; 1 found (index past it), 0 end, -errno. */
static int next_set(struct exfat* fs, struct dir_iter* it, uint64_t* index, struct entry_set* set) {
    for (;;) {
        uint64_t sector; unsigned offset;
        int e = iter_locate(fs, it, *index, &sector, &offset);
        if (e == -61) return 0;
        if (e) return e;
        uint8_t* s = sector_get(fs, sector); if (!s) return -5;
        uint8_t type = s[offset];
        if (!type) return 0;                          /* end of directory */
        if (type != 0x85) { (*index)++; continue; }   /* deleted, bitmap, up-case, label, GUID, ... */
        unsigned count = s[offset + 1] + 1u;
        if (count < 3 || count > MAX_SET) { (*index)++; continue; }
        set->index = *index; set->count = count;
        bool ok = true;
        for (unsigned i = 0; i < count; i++) {
            uint64_t sec; unsigned off;
            if (iter_locate(fs, it, *index + i, &sec, &off)) { ok = false; break; }
            uint8_t* d = sector_get(fs, sec); if (!d) return -5;
            memcpy(set->raw + i * 32, d + off, 32); set->sectors[i] = sec; set->offsets[i] = (uint16_t)off;
        }
        if (!ok) return 0;
        uint8_t* stream = set->raw + 32;
        bool valid = stream[0] == 0xC0 && get_le16(set->raw + 2) == set_checksum(set->raw, count);
        unsigned length = valid ? stream[3] : 0;
        if (valid && (!length || (length + 14) / 15 > count - 2)) valid = false;
        for (unsigned i = 2; valid && i < count; i++) if (set->raw[i * 32] != 0xC1 && set->raw[i * 32] < 0xE0) valid = false;
        if (valid) {
            unsigned n = 0;
            for (unsigned i = 2; i < count && n < length; i++) {
                if (set->raw[i * 32] != 0xC1) continue;
                for (unsigned k = 0; k < 15 && n < length; k++) set->name[n++] = get_le16(set->raw + i * 32 + 2 + k * 2);
            }
            set->name_length = n;
            if (n != length || name_hash(fs, set->name, n) != get_le16(stream + 4)) valid = false;
        }
        *index += count;
        if (valid) return 1;
        vga_print("[64] exfat: skipping a damaged directory entry set (run fsck.exfat)\n");
    }
}
static uint64_t slot_of(struct exfat* fs, const struct entry_set* set) { return set->sectors[0] * (fs->sector_size / 32) + set->offsets[0] / 32; }
static uint64_t ident_get(struct exfat* fs, uint64_t slot) {
    for (unsigned i = 0; i < fs->ident_count; i++) if (fs->idents[i].slot == slot) return fs->idents[i].ino;
    return slot + 2;
}
static void ident_set(struct exfat* fs, uint64_t slot, uint64_t ino) {
    for (unsigned i = 0; i < fs->ident_count; i++) if (fs->idents[i].slot == slot) {
        if (ino == slot + 2) { fs->idents[i] = fs->idents[--fs->ident_count]; return; }
        fs->idents[i].ino = ino; return;
    }
    if (ino == slot + 2) return;
    if (fs->ident_count == fs->ident_capacity) {
        unsigned capacity = fs->ident_capacity ? fs->ident_capacity * 2 : 256;
        struct ident* bigger = (void*)arena_alloc(capacity * sizeof(struct ident)); if (!bigger) return;
        if (fs->idents) { memcpy(bigger, fs->idents, fs->ident_count * sizeof(struct ident)); arena_free((uint64_t)fs->idents, fs->ident_capacity * sizeof(struct ident)); }
        fs->idents = bigger; fs->ident_capacity = capacity;
    }
    fs->idents[fs->ident_count++] = (struct ident){slot, ino};
}
static uint64_t fresh_ino(struct exfat* fs) { return (1ULL << 42) + ++fs->generation; }
static void fill(struct exfat* fs, const struct entry_set* set, const fat32_file_t* dir, fat32_file_t* f) {
    memset(f, 0, sizeof(*f));
    const uint8_t* e = set->raw; const uint8_t* stream = set->raw + 32;
    uint16_t attr = get_le16(e + 4);
    f->is_dir = (attr & ATTR_DIR) != 0;
    f->first = get_le32(stream + 20); f->size = get_le64(stream + 24);
    f->iflags = (stream[1] & 2 ? NOFATCHAIN : 0) | (dir->iflags & NOFATCHAIN ? PARENT_CONTIG : 0);
    f->read_only = (attr & ATTR_RO) && !f->is_dir;
    f->dir_lba = (uint32_t)set->sectors[0]; f->dir_off = set->offsets[0];
    f->parent = dir->first; f->entry_index = (uint32_t)set->index; f->lfn_count = (uint8_t)(set->count - 1);
    from_utf16(set->name, set->name_length, f->name, sizeof(f->name));
    uint64_t ino = ident_get(fs, slot_of(fs, set)); f->ino = (uint32_t)ino; f->generation = (uint32_t)(ino >> 32);
    f->blocks = clusters_for(fs, f->size) * (fs->cluster_bytes / 512);
    unsigned mode = 0777 & ~(f->is_dir ? fs->dmask : fs->fmask);
    if (f->read_only) mode &= ~0222u;
    f->mode = (f->is_dir ? S_IFDIR : S_IFREG) | mode; f->uid = fs->uid; f->gid = fs->gid; f->nlink = f->is_dir ? 2 : 1;
    f->btime = exfat_time(fs, get_le32(e + 8), e[20], e[22]);
    f->mtime = f->ctime = exfat_time(fs, get_le32(e + 12), e[21], e[23]);
    f->atime = exfat_time(fs, get_le32(e + 16), 0, e[24]);
}
static void root_file(struct exfat* fs, fat32_file_t* f) {
    memset(f, 0, sizeof(*f));
    f->is_dir = true; f->first = fs->root_cluster; f->dir_lba = MOUNT_ROOT_LBA; f->ino = 1;
    f->mode = S_IFDIR | (0777 & ~fs->dmask); f->uid = fs->uid; f->gid = fs->gid; f->nlink = 2;
    f->blocks = fs->cluster_bytes / 512; f->mtime = f->ctime = f->atime = f->btime = 315532800LL * 1000000000LL;
}
static void dir_iter_of(struct exfat* fs, const fat32_file_t* dir, struct dir_iter* it) {
    iter_begin(fs, it, dir->first, (dir->iflags & NOFATCHAIN) != 0, dir->dir_lba == MOUNT_ROOT_LBA ? 0 : dir->size);
}
static int find(struct exfat* fs, const fat32_file_t* dir, const char* name, struct entry_set* set) {
    uint16_t wanted[256]; int n = to_utf16(name, wanted, 255); if (n <= 0) return n < 0 ? n : -2;
    uint16_t hash = name_hash(fs, wanted, (unsigned)n);
    struct dir_iter it; dir_iter_of(fs, dir, &it);
    uint64_t index = 0; int r;
    while ((r = next_set(fs, &it, &index, set)) == 1)
        if (get_le16(set->raw + 32 + 4) == hash && same_utf16(fs, set->name, set->name_length, wanted, (unsigned)n)) return 0;
    return r < 0 ? r : -2;
}
static int walk(struct exfat* fs, const char* path, fat32_file_t* dir, const char** leaf) {
    root_file(fs, dir);
    char part[1024];
    for (const char* p = path;;) {
        while (*p == '/') p++;
        const char* end = p; while (*end && *end != '/') end++;
        const char* after = end; while (*after == '/') after++;
        if (!*after) { *leaf = p; return 0; }
        unsigned n = (unsigned)(end - p); if (n >= sizeof(part)) return -36;
        memcpy(part, p, n); part[n] = 0;
        struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
        int e = find(fs, dir, part, set);
        if (!e && !(get_le16(set->raw + 4) & ATTR_DIR)) e = -20;
        if (!e) { fat32_file_t child; fill(fs, set, dir, &child); *dir = child; }
        arena_free((uint64_t)set, sizeof(*set));
        if (e) return e;
        p = after;
    }
}
static void leaf_name(const char* leaf, char* name, unsigned capacity) {
    unsigned n = 0; while (leaf[n] && leaf[n] != '/' && n + 1 < capacity) { name[n] = leaf[n]; n++; } name[n] = 0;
}
static int ex_lookup(struct mount* m, const char* path, fat32_file_t* out) {
    struct exfat* fs = fs_of(m);
    if (!path[0]) { root_file(fs, out); return 0; }
    fat32_file_t dir; const char* leaf; int e = walk(fs, path, &dir, &leaf); if (e) return e;
    char name[1024]; leaf_name(leaf, name, sizeof(name));
    if (!name[0]) { *out = dir; return 0; }
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    e = find(fs, &dir, name, set);
    if (!e) fill(fs, set, &dir, out);
    arena_free((uint64_t)set, sizeof(*set));
    return e;
}
/* The entry set of a handle, re-read from disk (sectors from the parent's chain). */
static int load_set(struct exfat* fs, const fat32_file_t* f, struct entry_set* set) {
    struct dir_iter it; iter_begin(fs, &it, f->parent, (f->iflags & PARENT_CONTIG) != 0, 0);
    unsigned count = f->lfn_count + 1u;
    if (count < 3 || count > MAX_SET) return -5;
    for (unsigned i = 0; i < count; i++) {
        uint64_t sector; unsigned offset;
        if (i == 0) { sector = f->dir_lba; offset = f->dir_off; }
        else if (f->dir_off + i * 32 < fs->sector_size) { sector = f->dir_lba; offset = f->dir_off + i * 32; }
        else if (iter_locate(fs, &it, (uint64_t)f->entry_index + i, &sector, &offset)) return -5;
        uint8_t* d = sector_get(fs, sector); if (!d) return -5;
        memcpy(set->raw + i * 32, d + offset, 32); set->sectors[i] = sector; set->offsets[i] = (uint16_t)offset;
    }
    if (set->raw[0] != 0x85 || set->raw[32] != 0xC0 || set->raw[1] + 1u != count) return -2;
    set->count = count; set->index = f->entry_index; return 0;
}
static int store_set(struct exfat* fs, struct entry_set* set) {   /* checksum, then each touched sector once */
    put_le16(set->raw + 2, set_checksum(set->raw, set->count));
    for (unsigned i = 0; i < set->count; i++) {
        if (i && set->sectors[i] == set->sectors[i - 1]) continue;
        uint8_t* d = sector_get(fs, set->sectors[i]); if (!d) return -5;
        uint8_t* data = fs->sector_buf; memcpy(data, d, fs->sector_size);
        for (unsigned k = i; k < set->count && set->sectors[k] == set->sectors[i]; k++) memcpy(data + set->offsets[k], set->raw + k * 32, 32);
        if (sector_put(fs, set->sectors[i], data)) return -5;
    }
    return 0;
}
static int ex_refresh(struct mount* m, fat32_file_t* f) {
    struct exfat* fs = fs_of(m);
    char name[FAT32_LFN_MAX + 1]; strcpy(name, f->name);
    if (f->dir_lba == MOUNT_ROOT_LBA) { root_file(fs, f); strcpy(f->name, name); return 0; }
    if (f->detached) return 0;
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    int e = load_set(fs, f, set);
    if (!e) {
        unsigned n = 0, length = set->raw[32 + 3];
        for (unsigned i = 2; i < set->count && n < length; i++) for (unsigned k = 0; k < 15 && n < length; k++) set->name[n++] = get_le16(set->raw + i * 32 + 2 + k * 2);
        set->name_length = n;
        fat32_file_t dir; memset(&dir, 0, sizeof(dir)); dir.first = f->parent; dir.iflags = f->iflags & PARENT_CONTIG ? NOFATCHAIN : 0;
        uint32_t cursor = f->cursor, cursor_index = f->cursor_index, epoch = f->chain_epoch;
        fill(fs, set, &dir, f);
        f->cursor = cursor; f->cursor_index = cursor_index; f->chain_epoch = epoch;
    }
    arena_free((uint64_t)set, sizeof(*set));
    return e;
}

/* ---- data ---- */
static uint64_t valid_length(struct exfat* fs, const fat32_file_t* f) {
    if (f->dir_lba == MOUNT_ROOT_LBA || f->is_dir) return f->size;
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return 0;
    uint64_t v = load_set(fs, f, set) ? 0 : get_le64(set->raw + 32 + 8);
    arena_free((uint64_t)set, sizeof(*set));
    return v > f->size ? f->size : v;
}
static int64_t transfer(struct exfat* fs, fat32_file_t* f, uint64_t offset, uint64_t length, uint8_t* data, bool write) {
    uint64_t done = 0;
    while (done < length) {
        uint64_t at = offset + done; uint32_t c;
        if (cluster_at(fs, f, at / fs->cluster_bytes, &c)) return -5;
        uint32_t run = 1, last = c; uint64_t within = at % fs->cluster_bytes, want = length - done + within;
        while ((uint64_t)run * fs->cluster_bytes < want && run < 256) {
            uint32_t next;
            if (f->iflags & NOFATCHAIN) next = last + 1; else if (fat_get(fs, last, &next)) return -5;
            if (next != last + 1 || !valid_cluster(fs, next)) break;
            last = next; run++;
        }
        uint64_t bytes = (uint64_t)run * fs->cluster_bytes - within; if (bytes > length - done) bytes = length - done;
        uint64_t sector = cluster_sector(fs, c) + within / fs->sector_size; unsigned skip = (unsigned)(within % fs->sector_size);
        uint64_t sectors = (skip + bytes + fs->sector_size - 1) / fs->sector_size;
        if (!skip && !(bytes % fs->sector_size)) {
            if (write) { forget_range(fs, sector, sectors); if (raw_write(fs, sector, (uint32_t)sectors, data + done)) return -5; }
            else if (raw_read(fs, sector, (uint32_t)sectors, data + done)) return -5;
        } else {
            for (uint64_t s = 0, copied = 0; s < sectors; s++) {
                uint8_t* old = sector_get(fs, sector + s); if (!old) return -5;
                unsigned from = s ? 0 : skip; uint64_t take = fs->sector_size - from; if (take > bytes - copied) take = bytes - copied;
                if (write) {
                    uint8_t* block = fs->sector_buf; memcpy(block, old, fs->sector_size); memcpy(block + from, data + done + copied, take);
                    if (sector_put(fs, sector + s, block)) return -5;
                } else memcpy(data + done + copied, old + from, take);
                copied += take;
            }
        }
        done += bytes;
    }
    return (int64_t)done;
}
static int64_t ex_read(struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, void* buffer) {
    struct exfat* fs = fs_of(m);
    if (f->is_dir) return -21;
    if (offset >= f->size) return 0;
    if (length > f->size - offset) length = f->size - offset;
    uint64_t valid = valid_length(fs, f), readable = offset < valid ? (valid - offset < length ? valid - offset : length) : 0;
    if (readable) { int64_t r = transfer(fs, f, offset, readable, buffer, false); if (r < 0) return r; }
    if (readable < length) memset((uint8_t*)buffer + readable, 0, length - readable);   /* past ValidDataLength */
    return (int64_t)length;
}
/* A NoFatChain object that cannot stay contiguous gets FAT entries for its clusters. */
static int to_fat_chain(struct exfat* fs, fat32_file_t* f, uint64_t have) {
    if (!(f->iflags & NOFATCHAIN)) return 0;
    for (uint64_t i = 0; i < have; i++) if (fat_set(fs, f->first + (uint32_t)i, i + 1 < have ? f->first + (uint32_t)i + 1 : EOC)) return -5;
    f->iflags &= ~NOFATCHAIN; f->chain_epoch = 0; return 0;
}
static int zero_cluster(struct exfat* fs, uint32_t c) {
    memset(fs->scratch, 0, fs->cluster_bytes); forget_range(fs, cluster_sector(fs, c), fs->cluster_sectors);
    return raw_write(fs, cluster_sector(fs, c), fs->cluster_sectors, fs->scratch);
}
/* Make the allocation cover `bytes` (zeroing new clusters when asked: directories). */
static int grow_to(struct exfat* fs, fat32_file_t* f, uint64_t bytes, bool zero) {
    uint64_t have = f->first ? clusters_for(fs, f->size) : 0, need = clusters_for(fs, bytes);
    if (f->first && !valid_cluster(fs, f->first)) return -5;
    uint32_t last = 0;
    if (have && cluster_at(fs, f, have - 1, &last)) return -5;
    if (!have) f->iflags &= ~NOFATCHAIN;   /* a new allocation is FAT-chained */
    for (uint64_t count = have; count < need; count++) {
        uint32_t fresh;
        if (last && (f->iflags & NOFATCHAIN)) {   /* contiguous: take the next cluster, else become a FAT chain */
            bool used = true;
            if (valid_cluster(fs, last + 1) && !bitmap_get(fs, last + 1, &used) && !used) {
                if (bitmap_set(fs, last + 1, true)) return -5;
                fresh = last + 1;
                if (zero && zero_cluster(fs, fresh)) return -5;
                last = fresh; continue;
            }
            if (to_fat_chain(fs, f, count)) return -5;
        }
        int e = allocate_one(fs, last ? last + 1 : 0, &fresh); if (e) return e;
        if (zero && zero_cluster(fs, fresh)) return -5;
        if (fat_set(fs, fresh, EOC)) return -5;
        if (last) { if (fat_set(fs, last, fresh)) return -5; } else f->first = fresh;
        last = fresh;
    }
    return 0;
}
static int write_stream(struct exfat* fs, fat32_file_t* f, uint64_t valid, bool touch) {
    if (f->dir_lba == MOUNT_ROOT_LBA || f->detached) return 0;
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    int e = load_set(fs, f, set);
    if (!e) {
        uint8_t* stream = set->raw + 32;
        stream[1] = (uint8_t)((stream[1] & ~2u) | 1u | (f->iflags & NOFATCHAIN ? 2u : 0)); if (!f->first) stream[1] &= (uint8_t)~3u;
        put_le32(stream + 20, f->first); put_le64(stream + 24, f->size); put_le64(stream + 8, valid > f->size ? f->size : valid);
        if (touch) {
            uint32_t stamp; uint8_t tenms; exfat_stamp(native_realtime_ns(), &stamp, &tenms);
            put_le32(set->raw + 12, stamp); set->raw[21] = tenms; set->raw[23] = 0x80;
            put_le32(set->raw + 16, stamp); set->raw[24] = 0x80;
            put_le16(set->raw + 4, get_le16(set->raw + 4) | ATTR_ARCHIVE);
        }
        e = store_set(fs, set);
    }
    arena_free((uint64_t)set, sizeof(*set));
    return e;
}
static int64_t ex_write(struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, const void* buffer) {
    struct exfat* fs = fs_of(m);
    if (f->is_dir) return -21;
    if (f->detached) return (int64_t)length;
    if (!length) return 0;
    if (offset + length < offset) return -27;
    uint64_t end = offset + length, valid = valid_length(fs, f);
    int e = grow_to(fs, f, end > f->size ? end : f->size, false); if (e) return e;
    if (end > f->size) f->size = end;
    if (offset > valid) {   /* the gap between ValidDataLength and the write reads as zeros: make it so on disk */
        for (uint64_t at = valid; at < offset; ) {
            uint64_t n = offset - at < fs->cluster_bytes ? offset - at : fs->cluster_bytes;
            memset(fs->scratch, 0, n);
            if (transfer(fs, f, at, n, fs->scratch, true) < 0) return -5;
            at += n;
        }
    }
    int64_t done = transfer(fs, f, offset, length, (uint8_t*)buffer, true);
    if (done < 0) return done;
    if ((e = write_stream(fs, f, end > valid ? end : valid, true))) return e;
    return done;
}
static int ex_truncate(struct mount* m, fat32_file_t* f, uint64_t size) {
    struct exfat* fs = fs_of(m);
    if (f->is_dir) return -21;
    if (size == f->size) return 0;
    uint64_t valid = valid_length(fs, f);
    if (size > f->size) {   /* growth stays beyond ValidDataLength: reads zeros, nothing written */
        int e = grow_to(fs, f, size, false); if (e) return e;
        f->size = size; return write_stream(fs, f, valid, true);
    }
    uint64_t keep = clusters_for(fs, size), have = clusters_for(fs, f->size);
    uint32_t first = f->first; bool contig = (f->iflags & NOFATCHAIN) != 0;
    if (!keep) {
        f->first = 0; f->size = 0; f->iflags &= ~NOFATCHAIN;
        int e = write_stream(fs, f, 0, true); if (e) return e;
        return free_clusters(fs, first, have, contig);
    }
    uint32_t last; if (cluster_at(fs, f, keep - 1, &last)) return -5;
    f->size = size;
    int e = write_stream(fs, f, valid < size ? valid : size, true); if (e) return e;
    if (contig) return free_clusters(fs, last + 1, have - keep, true);
    uint32_t next; if (fat_get(fs, last, &next)) return -5;
    if (fat_set(fs, last, EOC)) return -5;
    return next != EOC && valid_cluster(fs, next) ? free_clusters(fs, next, 0, false) : 0;
}
static int ex_readdir(struct mount* m, const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) {
    struct exfat* fs = fs_of(m);
    if (!dir->is_dir) return -20;
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    struct dir_iter it; dir_iter_of(fs, dir, &it);
    uint64_t index = *cookie;
    int r = next_set(fs, &it, &index, set);
    if (r == 1) { fill(fs, set, dir, out); *cookie = index; }
    arena_free((uint64_t)set, sizeof(*set));
    return r;
}

/* ---- creation, removal, rename ---- */
static int clean_name(struct exfat* fs, const char* leaf, char* name, unsigned capacity) {
    unsigned n = 0; while (leaf[n] && leaf[n] != '/' && n + 1 < capacity) { name[n] = leaf[n]; n++; }
    if (leaf[n] && leaf[n] != '/') return -36;
    if (!fs->keep_dots) while (n && name[n - 1] == '.') n--;   /* Linux exfat drops trailing dots (keep_last_dots keeps them) */
    name[n] = 0;
    if (!n || !strcmp(name, ".") || !strcmp(name, "..")) return -22;
    for (unsigned i = 0; i < n; i++) { unsigned char c = (unsigned char)name[i]; if (c < 0x20 || c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') return -22; }
    return 0;
}
/* `count` free entries in a row (growing the directory by a cluster when needed); index of the first. */
static int free_run(struct exfat* fs, fat32_file_t* dir, unsigned count, uint64_t* out) {
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        struct dir_iter it; dir_iter_of(fs, dir, &it);
        uint64_t run_start = 0; unsigned run = 0;
        for (uint64_t index = 0;; index++) {
            uint64_t sector; unsigned offset;
            int e = iter_locate(fs, &it, index, &sector, &offset);
            if (e == -61) break;
            if (e) return e;
            uint8_t* s = sector_get(fs, sector); if (!s) return -5;
            if (s[offset] < 0x80) { if (!run) run_start = index; if (++run == count) { *out = run_start; return 0; } }
            else run = 0;
        }
        if (dir->dir_lba == MOUNT_ROOT_LBA) {   /* the root: a FAT chain without a size entry */
            uint32_t last = dir->first, next;
            for (uint32_t n = 0; n <= fs->clusters; n++) { if (fat_get(fs, last, &next)) return -5; if (next == EOC || !valid_cluster(fs, next)) break; last = next; }
            uint32_t fresh; int e = allocate_one(fs, last + 1, &fresh); if (e) return e;
            if (zero_cluster(fs, fresh) || fat_set(fs, fresh, EOC) || fat_set(fs, last, fresh)) return -5;
        } else {
            int e = grow_to(fs, dir, dir->size + fs->cluster_bytes, true); if (e) return e;
            dir->size += fs->cluster_bytes;
            if ((e = write_stream(fs, dir, dir->size, true))) return e;
        }
    }
    return -28;
}
static int place_set(struct exfat* fs, fat32_file_t* dir, struct entry_set* set) {
    uint64_t first; int e = free_run(fs, dir, set->count, &first); if (e) return e;
    struct dir_iter it; dir_iter_of(fs, dir, &it);
    for (unsigned i = 0; i < set->count; i++) {
        unsigned offset; if (iter_locate(fs, &it, first + i, &set->sectors[i], &offset)) return -5;
        set->offsets[i] = (uint16_t)offset;
    }
    set->index = first;
    return store_set(fs, set);
}
static int build_set(struct exfat* fs, const char* name, uint16_t attributes, struct entry_set* set) {
    uint16_t units[256]; int n = to_utf16(name, units, 255); if (n <= 0) return n < 0 ? n : -22;
    unsigned names = ((unsigned)n + 14) / 15;
    memset(set, 0, sizeof(*set)); set->count = 2 + names;
    uint8_t* e = set->raw; e[0] = 0x85; e[1] = (uint8_t)(1 + names); put_le16(e + 4, attributes);
    uint32_t stamp; uint8_t tenms; exfat_stamp(native_realtime_ns(), &stamp, &tenms);
    put_le32(e + 8, stamp); put_le32(e + 12, stamp); put_le32(e + 16, stamp); e[20] = tenms; e[21] = tenms; e[22] = e[23] = e[24] = 0x80;
    uint8_t* stream = set->raw + 32; stream[0] = 0xC0; stream[1] = 1; stream[3] = (uint8_t)n; put_le16(stream + 4, name_hash(fs, units, (unsigned)n));
    for (unsigned i = 0; i < names; i++) {
        uint8_t* x = set->raw + (2 + i) * 32; x[0] = 0xC1;
        for (unsigned k = 0; k < 15 && i * 15 + k < (unsigned)n; k++) put_le16(x + 2 + k * 2, units[i * 15 + k]);
    }
    memcpy(set->name, units, (size_t)n * 2); set->name_length = (unsigned)n;
    return 0;
}
static int ex_create(struct mount* m, const char* path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, const char* target, fat32_file_t* out) {
    (void)uid; (void)gid; (void)rdev; (void)target;
    struct exfat* fs = fs_of(m);
    uint32_t type = mode & 0170000;
    if (type != S_IFREG && type != S_IFDIR) return -1;   /* exfat: no symlinks, sockets, fifos or devices */
    fat32_file_t dir; const char* leaf; int e = walk(fs, path, &dir, &leaf); if (e) return e;
    char name[1024]; if ((e = clean_name(fs, leaf, name, sizeof(name)))) return e;
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    if (!find(fs, &dir, name, set)) { arena_free((uint64_t)set, sizeof(*set)); return -17; }
    uint16_t attributes = type == S_IFDIR ? ATTR_DIR : ATTR_ARCHIVE;
    if (type == S_IFREG && !(mode & 0222)) attributes |= ATTR_RO;
    e = build_set(fs, name, attributes, set);
    uint32_t cluster = 0;
    if (!e && type == S_IFDIR) {   /* one zeroed cluster, FAT-chained */
        if (!(e = allocate_one(fs, 0, &cluster))) {
            if (zero_cluster(fs, cluster) || fat_set(fs, cluster, EOC)) e = -5;
            uint8_t* stream = set->raw + 32; stream[1] = 1; put_le32(stream + 20, cluster);
            put_le64(stream + 8, fs->cluster_bytes); put_le64(stream + 24, fs->cluster_bytes);
        }
    }
    if (!e) e = place_set(fs, &dir, set);
    if (e && cluster) free_clusters(fs, cluster, 1, false);
    if (!e) { ident_set(fs, slot_of(fs, set), fresh_ino(fs)); fill(fs, set, &dir, out); }
    arena_free((uint64_t)set, sizeof(*set));
    return e;
}
static int erase_set(struct exfat* fs, struct entry_set* set) {
    for (unsigned i = 0; i < set->count; i++) set->raw[i * 32] &= 0x7F;
    for (unsigned i = 0; i < set->count; i++) {   /* store without recomputing: the entries are no longer in use */
        if (i && set->sectors[i] == set->sectors[i - 1]) continue;
        uint8_t* d = sector_get(fs, set->sectors[i]); if (!d) return -5;
        uint8_t* data = fs->sector_buf; memcpy(data, d, fs->sector_size);
        for (unsigned k = i; k < set->count && set->sectors[k] == set->sectors[i]; k++) data[set->offsets[k]] = set->raw[k * 32];
        if (sector_put(fs, set->sectors[i], data)) return -5;
    }
    return 0;
}
static int dir_empty(struct exfat* fs, const fat32_file_t* dir) {   /* 1 empty, 0 not, -errno */
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    struct dir_iter it; dir_iter_of(fs, dir, &it); uint64_t index = 0;
    int r = next_set(fs, &it, &index, set);
    arena_free((uint64_t)set, sizeof(*set));
    return r < 0 ? r : !r;
}
static int ex_unlink(struct mount* m, const char* path, int directory, fat32_file_t* victim) {
    struct exfat* fs = fs_of(m);
    fat32_file_t dir; const char* leaf; int e = walk(fs, path, &dir, &leaf); if (e) return e;
    char name[1024]; leaf_name(leaf, name, sizeof(name));
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    e = find(fs, &dir, name, set);
    if (!e) {
        fill(fs, set, &dir, victim);
        if (directory == 1 && !victim->is_dir) e = -20;
        else if (directory == 0 && victim->is_dir) e = -21;
        else if (victim->is_dir) { int r = dir_empty(fs, victim); if (r < 0) e = r; else if (!r) e = -39; }
    }
    if (!e) e = erase_set(fs, set);
    if (!e) { ident_set(fs, slot_of(fs, set), fresh_ino(fs)); victim->detached = true; }
    arena_free((uint64_t)set, sizeof(*set));
    return e;
}
static int ex_release(struct mount* m, fat32_file_t* f) {
    struct exfat* fs = fs_of(m);
    if (!f->detached || fs->readonly) return 0;
    uint32_t first = f->first; f->first = 0;
    return valid_cluster(fs, first) ? free_clusters(fs, first, clusters_for(fs, f->size), (f->iflags & NOFATCHAIN) != 0) : 0;
}
static int ex_rename(struct mount* m, const char* from, const char* to, unsigned flags, fat32_file_t* moved, fat32_file_t* replaced) {
    struct exfat* fs = fs_of(m);
    fat32_file_t sdir, tdir; const char *sleaf, *tleaf;
    int e = walk(fs, from, &sdir, &sleaf); if (e) return e;
    if ((e = walk(fs, to, &tdir, &tleaf))) return e;
    char sname[1024], tname[1024]; leaf_name(sleaf, sname, sizeof(sname));
    if ((e = clean_name(fs, tleaf, tname, sizeof(tname)))) return e;
    struct entry_set *source = (void*)arena_alloc(sizeof(struct entry_set)), *target = (void*)arena_alloc(sizeof(struct entry_set));
    if (!source || !target) { if (source) arena_free((uint64_t)source, sizeof(*source)); if (target) arena_free((uint64_t)target, sizeof(*target)); return -12; }
    memset(replaced, 0, sizeof(*replaced));
    fat32_file_t sfile;
    if ((e = find(fs, &sdir, sname, source))) goto done;
    fill(fs, source, &sdir, &sfile);
    if (sfile.is_dir) {   /* not into itself or below: walk the target's path components */
        unsigned n = strlen(from);
        if (!strncmp(to, from, n) && (to[n] == '/' || !to[n]) && strcmp(from, to)) { e = -22; goto done; }
    }
    bool exists = !find(fs, &tdir, tname, target);
    if (exists && target->sectors[0] == source->sectors[0] && target->offsets[0] == source->offsets[0]) {
        uint16_t units[256]; int n = to_utf16(tname, units, 255);
        if (n == (int)source->name_length && !memcmp(units, source->name, (size_t)n * 2)) { *moved = sfile; e = 0; goto done; }
        exists = false;   /* a case-only change of the same entry */
    } else if (exists) {
        if (flags & 1) { e = -17; goto done; }
        fat32_file_t tfile; fill(fs, target, &tdir, &tfile);
        if (tfile.is_dir != sfile.is_dir) { e = tfile.is_dir ? -21 : -20; goto done; }
        if (tfile.is_dir) { int r = dir_empty(fs, &tfile); if (r < 0) { e = r; goto done; } if (!r) { e = -39; goto done; } }
        if ((e = erase_set(fs, target))) goto done;
        ident_set(fs, slot_of(fs, target), fresh_ino(fs));
        *replaced = tfile; replaced->detached = true;
    }
    uint64_t sslot = slot_of(fs, source), identity = ident_get(fs, sslot);
    struct entry_set* placed = target;
    if ((e = build_set(fs, tname, get_le16(source->raw + 4), placed))) goto done;
    memcpy(placed->raw + 8, source->raw + 8, 24 - 8 + 1);    /* times and offsets */
    uint8_t* ps = placed->raw + 32; const uint8_t* ss = source->raw + 32;
    ps[1] = ss[1]; memcpy(ps + 8, ss + 8, 8); memcpy(ps + 20, ss + 20, 12);   /* flags, ValidDataLength, FirstCluster, DataLength */
    if ((e = erase_set(fs, source))) goto done;
    if ((e = place_set(fs, &tdir, placed))) {   /* put the old name back */
        for (unsigned i = 0; i < source->count; i++) source->raw[i * 32] |= 0x80;
        store_set(fs, source); goto done;
    }
    ident_set(fs, sslot, fresh_ino(fs));
    ident_set(fs, slot_of(fs, placed), identity);
    fill(fs, placed, &tdir, moved);
done:
    arena_free((uint64_t)source, sizeof(*source)); arena_free((uint64_t)target, sizeof(*target));
    return e;
}
static int ex_setattr(struct mount* m, fat32_file_t* f, const struct mount_attr* a) {
    struct exfat* fs = fs_of(m);
    int toggle = -1;
    if (a->valid & EXT4_ATTR_MODE) {   /* only the write bits may change (Linux exfat_sanitize_mode) */
        unsigned mask = f->is_dir ? fs->dmask : fs->fmask, perm = a->mode & 0777 & ~mask;
        bool ok = !(a->mode & 07000) && (perm & 0555) == (f->mode & 0555);
        if (ok) ok = f->is_dir ? (perm & 0222) == (0222 & ~mask) : !(perm & 0222) || (perm & 0222) == (0222 & ~mask);
        if (!ok) { if (!fs->quiet) return -1; }
        else if (!f->is_dir) toggle = (perm & 0222) ? 0 : 1;
    }
    if ((a->valid & EXT4_ATTR_UID) && a->uid != fs->uid && !fs->quiet) return -1;
    if ((a->valid & EXT4_ATTR_GID) && a->gid != fs->gid && !fs->quiet) return -1;
    if (a->valid & MOUNT_ATTR_SIZE) { int e = ex_truncate(m, f, a->size); if (e) return e; }
    bool times = (a->valid & (EXT4_ATTR_MTIME | EXT4_ATTR_ATIME)) != 0;
    if ((toggle < 0 && !times) || f->dir_lba == MOUNT_ROOT_LBA || f->detached) return 0;
    struct entry_set* set = (void*)arena_alloc(sizeof(*set)); if (!set) return -12;
    int e = load_set(fs, f, set);
    if (!e) {
        uint32_t stamp; uint8_t tenms;
        if (a->valid & EXT4_ATTR_MTIME) { exfat_stamp(a->mtime, &stamp, &tenms); put_le32(set->raw + 12, stamp); set->raw[21] = tenms; set->raw[23] = 0x80; }
        if (a->valid & EXT4_ATTR_ATIME) { exfat_stamp(a->atime, &stamp, NULL); put_le32(set->raw + 16, stamp); set->raw[24] = 0x80; }
        uint16_t attr = get_le16(set->raw + 4);
        if (toggle == 1) attr |= ATTR_RO; else if (toggle == 0) attr &= (uint16_t)~ATTR_RO;
        put_le16(set->raw + 4, attr);
        e = store_set(fs, set);
    }
    arena_free((uint64_t)set, sizeof(*set));
    return e ? e : ex_refresh(m, f);
}

/* ---- volume ---- */
static int set_dirty(struct exfat* fs, bool dirty) {   /* VolumeFlags bit 1 in the main boot sector (not in the checksum) */
    uint8_t* s = (void*)arena_alloc(4096); if (!s) return -12;
    int e = raw_read(fs, 0, 1, s);
    if (!e) { uint16_t flags = get_le16(s + 106); flags = dirty ? flags | 2 : flags & ~2u; put_le16(s + 106, flags); e = raw_write(fs, 0, 1, s); }
    arena_free((uint64_t)s, 4096);
    if (!e) e = block_flush(fs->part->dev) < 0 ? -5 : 0;
    return e;
}
static int ex_sync(struct mount* m) {
    struct exfat* fs = fs_of(m);
    if (fs->readonly) return 0;
    int e = flush_cache(fs);
    if (block_flush(fs->part->dev) < 0) e = -5;
    return e;
}
static int ex_fsync(struct mount* m, fat32_file_t* f) { (void)f; return ex_sync(m); }
static int ex_statfs(struct mount* m, struct mount_statfs* s) {
    struct exfat* fs = fs_of(m);
    s->type = 0x2011BAB0; s->block_size = fs->cluster_bytes; s->blocks = fs->clusters; s->free = s->available = fs->free_count;
    s->files = 0; s->free_files = 0; s->name_max = 255; s->fsid[0] = fs->serial; s->fsid[1] = 0; return 0;
}
static void free_fs(struct exfat* fs) {
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].data) arena_free((uint64_t)fs->cache[i].data, 4096);
    if (fs->sector_buf) arena_free((uint64_t)fs->sector_buf, 4096);
    if (fs->scratch) arena_free((uint64_t)fs->scratch, fs->cluster_bytes);
    if (fs->upcase) arena_free((uint64_t)fs->upcase, 65536 * 2);
    if (fs->bitmap_chain) arena_free((uint64_t)fs->bitmap_chain, ((uint64_t)fs->bitmap_clusters * 4 + 4095) / 4096 * 4096);
    if (fs->idents) arena_free((uint64_t)fs->idents, fs->ident_capacity * sizeof(struct ident));
    arena_free((uint64_t)fs, sizeof(*fs));
}
static int ex_unmount(struct mount* m, bool force) {
    struct exfat* fs = fs_of(m); if (!fs) return 0;
    if (!fs->readonly && !fs->part->dev->removed && (!force || !m->dead)) {
        flush_cache(fs);
        if (fs->marked_dirty && !fs->io_failed) set_dirty(fs, false);
        block_flush(fs->part->dev);
    }
    free_fs(fs); m->sb = NULL; return 0;
}
static int ex_remount(struct mount* m, uint64_t flags, char* options) {
    (void)options; struct exfat* fs = fs_of(m);
    bool ro = (flags & MS_RDONLY) != 0;
    if (ro && !fs->readonly) { flush_cache(fs); if (fs->marked_dirty) set_dirty(fs, false); block_flush(fs->part->dev); fs->readonly = true; fs->marked_dirty = false; }
    else if (!ro && fs->readonly) { if (fs->part->dev->read_only) return -13; fs->readonly = false; if (!fs->found_dirty && !set_dirty(fs, true)) fs->marked_dirty = true; }
    return 0;
}
static int parse_options(struct exfat* fs, char* options) {
    fs->uid = mount_current_uid(); fs->gid = mount_current_gid();
    unsigned umask = filesystem_umask(); fs->fmask = fs->dmask = (uint16_t)umask;
    char* cursor = options; char key[32], value[64];
    while (mount_option(&cursor, key, sizeof(key), value, sizeof(value))) {
        uint64_t v = 0; const char* end = value; bool number = value[0] && parse_u64(value, &v, &end) && !*end && v <= 0xFFFFFFFFULL;
        bool octal = value[0] != 0; uint64_t o = 0;
        for (const char* p = value; *p; p++) { if (*p < '0' || *p > '7' || o > 07777) { octal = false; break; } o = o * 8 + (uint64_t)(*p - '0'); }
        if (!strcmp(key, "uid") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, false, &k)) return -22; fs->uid = k; }
        else if (!strcmp(key, "gid") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, true, &k)) return -22; fs->gid = k; }
        else if (!strcmp(key, "umask") && octal) fs->fmask = fs->dmask = (uint16_t)(o & 0777);
        else if (!strcmp(key, "fmask") && octal) fs->fmask = (uint16_t)(o & 0777);
        else if (!strcmp(key, "dmask") && octal) fs->dmask = (uint16_t)(o & 0777);
        else if (!strcmp(key, "quiet") && !value[0]) fs->quiet = true;
        else if (!strcmp(key, "keep_last_dots") && !value[0]) fs->keep_dots = true;
        else if (!strcmp(key, "time_offset") && value[0]) {
            bool negative = value[0] == '-'; uint64_t minutes; const char* e2;
            if (!parse_u64(value + negative, &minutes, &e2) || *e2 || minutes > 24 * 60) return -22;
            fs->offset_ns = (negative ? -(int64_t)minutes : (int64_t)minutes) * 60000000000LL;
        }
        else if (!strcmp(key, "iocharset") || !strcmp(key, "errors") || !strcmp(key, "discard") || !strcmp(key, "allow_utime") ||
                 !strcmp(key, "sys_tz") || !strcmp(key, "utf8") || !strcmp(key, "namecase")) {}
        else { vga_print("[64] exfat: unrecognized mount option "); vga_print(key); vga_print("\n"); return -22; }
    }
    return 0;
}
static void show_options(struct exfat* fs, char* out) {
    out[0] = 0;
    if (fs->uid) { strcat(out, "uid="); append_u64(out, credentials_visible_id(fs->uid, false)); strcat(out, ","); }
    if (fs->gid) { strcat(out, "gid="); append_u64(out, credentials_visible_id(fs->gid, true)); strcat(out, ","); }
    char mask[8] = "0000";
    mask[1] = (char)('0' + ((fs->fmask >> 6) & 7)); mask[2] = (char)('0' + ((fs->fmask >> 3) & 7)); mask[3] = (char)('0' + (fs->fmask & 7));
    strcat(out, "fmask="); strcat(out, mask);
    mask[1] = (char)('0' + ((fs->dmask >> 6) & 7)); mask[2] = (char)('0' + ((fs->dmask >> 3) & 7)); mask[3] = (char)('0' + (fs->dmask & 7));
    strcat(out, ",dmask="); strcat(out, mask); strcat(out, ",iocharset=utf8");
    if (fs->offset_ns) { int64_t minutes = fs->offset_ns / 60000000000LL; strcat(out, ",time_offset="); if (minutes < 0) { strcat(out, "-"); minutes = -minutes; } append_u64(out, (uint64_t)minutes); }
    if (fs->keep_dots) strcat(out, ",keep_last_dots");
    strcat(out, ",errors=remount-ro");
}
static uint32_t boot_checksum(const uint8_t* region, unsigned sector_size) {   /* sectors 0..10, skipping VolumeFlags and PercentInUse */
    uint32_t sum = 0;
    for (unsigned i = 0; i < sector_size * 11; i++) { if (i == 106 || i == 107 || i == 112) continue; sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + region[i]; }
    return sum;
}
/* Reads a FAT-chained critical object (bitmap/up-case) into `out` up to `length` bytes. */
static int read_chain(struct exfat* fs, uint32_t first, uint64_t length, uint8_t* out) {
    uint32_t c = first;
    for (uint64_t at = 0; at < length; ) {
        if (!valid_cluster(fs, c)) return -5;
        uint64_t n = length - at < fs->cluster_bytes ? length - at : fs->cluster_bytes;
        uint64_t sectors = (n + fs->sector_size - 1) / fs->sector_size;
        if (n == fs->cluster_bytes || !(n % fs->sector_size)) { if (raw_read(fs, cluster_sector(fs, c), (uint32_t)sectors, out + at)) return -5; }
        else for (uint64_t s = 0; s < sectors; s++) {
            uint8_t* d = sector_get(fs, cluster_sector(fs, c) + s); if (!d) return -5;
            uint64_t take = n - s * fs->sector_size < fs->sector_size ? n - s * fs->sector_size : fs->sector_size;
            memcpy(out + at + s * fs->sector_size, d, take);
        }
        at += n;
        if (at < length && fat_get(fs, c, &c)) return -5;
    }
    return 0;
}
static void default_upcase(uint16_t* table) {
    for (unsigned c = 0; c < 65536; c++) table[c] = (uint16_t)(c >= 'a' && c <= 'z' ? c - 32 : (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 32 : c);
}
static int load_upcase(struct exfat* fs, uint32_t first, uint64_t length, uint32_t checksum) {
    fs->upcase = (void*)arena_alloc(65536 * 2); if (!fs->upcase) return -12;
    for (unsigned c = 0; c < 65536; c++) fs->upcase[c] = (uint16_t)c;
    if (!first || length < 2 || length > 131072 || (length & 1)) { default_upcase(fs->upcase); return 0; }
    uint8_t* raw = (void*)arena_alloc((length + 4095) / 4096 * 4096); if (!raw) return -12;
    int e = read_chain(fs, first, length, raw); uint32_t sum = 0;
    for (uint64_t i = 0; !e && i < length; i++) sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + raw[i];
    if (!e && sum == checksum) {
        unsigned c = 0;
        for (uint64_t i = 0; i + 1 < length && c < 65536; i += 2) {
            uint16_t v = get_le16(raw + i);
            if (v == 0xFFFF && i + 3 < length) { c += get_le16(raw + i + 2); i += 2; continue; }   /* identity run */
            fs->upcase[c++] = v;
        }
    } else { vga_print("[64] exfat: up-case table damaged; using the default table\n"); default_upcase(fs->upcase); }
    arena_free((uint64_t)raw, (length + 4095) / 4096 * 4096);
    return e;
}
static int ex_mount(struct mount* m, char* options) {
    struct exfat* fs = (void*)arena_alloc(sizeof(*fs)); if (!fs) return -12;
    memset(fs, 0, sizeof(*fs)); fs->m = m; fs->part = m->part;
    int e = parse_options(fs, options);
    if (e) { arena_free((uint64_t)fs, sizeof(*fs)); return e; }
    show_options(fs, m->options);
    uint8_t* region = (void*)arena_alloc(12 * 4096); if (!region) { arena_free((uint64_t)fs, sizeof(*fs)); return -12; }
    e = -22;
    if (block_part_read(fs->part, 0, 1, region) < 0) { e = -5; goto failed; }
    uint8_t* bs = region;
    unsigned sector_shift = bs[108], cluster_shift = bs[109];
    if (memcmp(bs + 3, "EXFAT   ", 8) || bs[510] != 0x55 || bs[511] != 0xAA || sector_shift < 9 || sector_shift > 12 || cluster_shift > 25 - sector_shift) goto failed;
    for (unsigned i = 11; i < 64; i++) if (bs[i]) goto failed;   /* MustBeZero: not a FAT BPB */
    fs->sector_size = 1u << sector_shift; fs->scale = fs->sector_size / 512; fs->cluster_sectors = 1u << cluster_shift;
    fs->cluster_bytes = fs->sector_size << cluster_shift; fs->entries_per_cluster = fs->cluster_bytes / 32;
    if (block_part_read(fs->part, 0, 12 * fs->scale, region) < 0) { e = -5; goto failed; }
    uint32_t sum = boot_checksum(region, fs->sector_size);
    for (unsigned i = 0; i < fs->sector_size / 4; i++) if (get_le32(region + 11 * fs->sector_size + i * 4) != sum) { vga_print("[64] exfat: boot region checksum mismatch\n"); goto failed; }
    fs->volume_sectors = get_le64(bs + 72); fs->fat_offset = get_le32(bs + 80); fs->fat_length = get_le32(bs + 84);
    fs->heap_offset = get_le32(bs + 88); fs->clusters = get_le32(bs + 92); fs->root_cluster = get_le32(bs + 96); fs->serial = get_le32(bs + 100);
    uint16_t flags = get_le16(bs + 106); unsigned fats = bs[110];
    if (fs->volume_sectors * fs->scale > block_part_sectors(fs->part)) { vga_print("[64] exfat: volume larger than its partition; using the partition size\n"); fs->volume_sectors = block_part_sectors(fs->part) / fs->scale; }
    if (get_le16(bs + 104) >> 8 != 1 || fats < 1 || fats > 2 || fs->fat_offset < 24 || !fs->fat_length || fs->heap_offset < fs->fat_offset + fs->fat_length * fats ||
        !fs->clusters || (uint64_t)fs->clusters * 4 + 8 > (uint64_t)fs->fat_length * fs->sector_size ||
        fs->heap_offset + (uint64_t)fs->clusters * fs->cluster_sectors > fs->volume_sectors || !valid_cluster(fs, fs->root_cluster)) goto failed;
    if (fats == 2 && (flags & 1)) fs->fat_offset += fs->fat_length;   /* TexFAT: the active FAT */
    for (unsigned i = 0; i < CACHE_SECTORS; i++) { fs->cache[i].data = (void*)arena_alloc(4096); if (!fs->cache[i].data) { e = -12; goto failed; } }
    fs->scratch = (void*)arena_alloc(fs->cluster_bytes); if (!fs->scratch) { e = -12; goto failed; }
    fs->sector_buf = (void*)arena_alloc(4096); if (!fs->sector_buf) { e = -12; goto failed; }
    fs->readonly = true;   /* until the volume checks out */
    /* the root directory: allocation bitmap, up-case table, label */
    uint32_t bitmap_first = 0, upcase_first = 0, upcase_checksum = 0; uint64_t bitmap_length = 0, upcase_length = 0;
    struct dir_iter it; iter_begin(fs, &it, fs->root_cluster, false, 0);
    for (uint64_t index = 0; index < 1u << 16; index++) {
        uint64_t sector; unsigned offset;
        int r = iter_locate(fs, &it, index, &sector, &offset); if (r == -61) break; if (r) { e = r; goto failed; }
        uint8_t* s = sector_get(fs, sector); if (!s) { e = -5; goto failed; }
        uint8_t* d = s + offset;
        if (!d[0]) break;
        if (d[0] == 0x81 && !bitmap_first && !(d[1] & 1)) { bitmap_first = get_le32(d + 20); bitmap_length = get_le64(d + 24); }
        else if (d[0] == 0x82 && !upcase_first) { upcase_checksum = get_le32(d + 4); upcase_first = get_le32(d + 20); upcase_length = get_le64(d + 24); }
        else if (d[0] == 0x83) { uint16_t u[11]; unsigned n = d[1] > 11 ? 11 : d[1]; for (unsigned k = 0; k < n; k++) u[k] = get_le16(d + 2 + k * 2); from_utf16(u, n, fs->label, sizeof(fs->label)); }
    }
    if (!valid_cluster(fs, bitmap_first) || bitmap_length < (fs->clusters + 7) / 8 || bitmap_length > (uint64_t)fs->clusters * fs->cluster_bytes) { vga_print("[64] exfat: no allocation bitmap\n"); goto failed; }
    fs->bitmap_bytes = bitmap_length; fs->bitmap_clusters = (uint32_t)clusters_for(fs, bitmap_length);
    fs->bitmap_chain = (void*)arena_alloc(((uint64_t)fs->bitmap_clusters * 4 + 4095) / 4096 * 4096); if (!fs->bitmap_chain) { e = -12; goto failed; }
    for (uint32_t i = 0, c = bitmap_first; i < fs->bitmap_clusters; i++) {
        if (!valid_cluster(fs, c)) { e = -5; goto failed; }
        fs->bitmap_chain[i] = c;
        if (i + 1 < fs->bitmap_clusters && fat_get(fs, c, &c)) { e = -5; goto failed; }
    }
    if ((e = load_upcase(fs, upcase_first, upcase_length, upcase_checksum))) goto failed;
    if ((e = count_free(fs))) goto failed;
    fs->next_free = 2;
    fs->readonly = (m->flags & MS_RDONLY) || fs->part->dev->read_only || fats == 2;
    if (fats == 2 && !(m->flags & MS_RDONLY)) { vga_print("[64] exfat: TexFAT volume (two FATs): mounted read-only\n"); m->flags |= MS_RDONLY; }
    fs->found_dirty = (flags & 2) != 0;
    if (fs->found_dirty) vga_print("[64] exfat: volume was not properly unmounted; some data may be corrupt (run fsck.exfat)\n");
    if (!fs->readonly && !fs->found_dirty) { if ((e = set_dirty(fs, true))) goto failed; fs->marked_dirty = true; }
    arena_free((uint64_t)region, 12 * 4096);
    m->sb = fs;
    char n[24] = ""; append_u64(n, fs->clusters);
    vga_print("[64] exfat: \""); vga_print(fs->label); vga_print("\" clusters="); vga_print(n);
    n[0] = 0; append_u64(n, fs->cluster_bytes); vga_print(" cluster_bytes="); vga_print(n);
    n[0] = 0; append_u64(n, fs->free_count); vga_print(" free="); vga_print(n); vga_print("\n");
    return 0;
failed:
    arena_free((uint64_t)region, 12 * 4096);
    free_fs(fs);
    if (e == -22) vga_print("[64] exfat: not a valid exFAT volume\n");
    return e;
}
const struct mount_ops exfat_mount_ops = {
    .name = "exfat", .flags = MOUNT_NEEDS_BLOCK | MOUNT_USER_OK,
    .mount = ex_mount, .unmount = ex_unmount, .sync = ex_sync, .statfs = ex_statfs, .remount = ex_remount,
    .lookup = ex_lookup, .refresh = ex_refresh, .read = ex_read, .write = ex_write, .truncate = ex_truncate, .readdir = ex_readdir,
    .create = ex_create, .unlink = ex_unlink, .release = ex_release, .rename = ex_rename, .link = NULL, .setattr = ex_setattr,
    .fsync = ex_fsync, .max_file_size = 0x7FFFFFFFFFFFFFFFULL,
};
/* fsprobe.c: UUID "XXXX-XXXX" (VolumeSerialNumber) and the label from the root directory's label entry. */
bool exfat_identity(struct block_partition* part, const uint8_t* bs, char* uuid, char* label) {
    if (memcmp(bs + 3, "EXFAT   ", 8)) return false;
    uint32_t serial = get_le32(bs + 100);
    uuid[0] = 0; append_hex(uuid, serial >> 16, 4); strcat(uuid, "-"); append_hex(uuid, serial & 0xFFFF, 4);
    for (char* p = uuid; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
    label[0] = 0;
    unsigned ss = bs[108], cs = bs[109];
    if (ss < 9 || ss > 12 || cs > 25 - ss) return true;
    uint64_t scale = (1u << ss) / 512, root = get_le32(bs + 96), heap = get_le32(bs + 88);
    if (root < 2) return true;
    uint8_t* s = (void*)arena_alloc(4096); if (!s) return true;
    uint64_t sector = (heap + ((root - 2) << cs)) * scale;
    if (sector + scale <= block_part_sectors(part) && block_part_read(part, sector, (uint32_t)scale, s) == 0)
        for (unsigned i = 0; i < (1u << ss) / 32; i++) {
            uint8_t* d = s + i * 32; if (!d[0]) break;
            if (d[0] == 0x83) { uint16_t u[11]; unsigned n = d[1] > 11 ? 11 : d[1]; for (unsigned k = 0; k < n; k++) u[k] = get_le16(d + 2 + k * 2); from_utf16(u, n, label, 48); break; }
        }
    arena_free((uint64_t)s, 4096);
    return true;
}
