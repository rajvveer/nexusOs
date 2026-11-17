/* vfat for mounted volumes (USB sticks, SD cards, loop images): FAT12/16/32 with VFAT long names, read/write,
 * any number of instances (the root keeps kernel/fat32.c). Linux vfat semantics: owner/group/modes from the
 * mount options (uid, gid, umask, dmask, fmask, showexec, quiet), ATTR_READ_ONLY as missing write bits, no
 * hard links/symlinks/devices (EPERM), 2 s write times, creation time and access date kept, times in UTC
 * (tz=UTC; time_offset=minutes shifts them), short names generated Windows-style (basis~N) with NTRES case
 * bits when a lower-case 8.3 name fits, names compared case-insensitively (ASCII).
 * Metadata: directory sectors are written through; FAT sectors are cached write-back (copy 0 in the cache,
 * every FAT copy written when the sector is flushed: at sync, unmount, eviction), so a sequential write does
 * not cost two sector writes per cluster. The volume's clean bit (FAT[1]) is cleared while mounted
 * read-write and set again at a clean unmount; a volume found dirty is reported and mounted anyway (fsck's
 * job, as on Linux). FSInfo free/next hints are used when sane and rewritten at sync. Every on-disk value is
 * range-checked: a chain leaving the volume, looping or ending early is EIO, never a fault.
 * Handles keep a chain position hint (cursor = cluster, cursor_index = its index, chain_epoch = the volume's
 * free epoch when taken), so sequential I/O does not rewalk the chain. Inode identity is the directory slot
 * of the short entry, remapped across rename and unlink so open handles and later lookups keep agreeing. */
#include "mount.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define CACHE_SECTORS 64
#define ATTR_RO 0x01
#define ATTR_VOLUME 0x08
#define ATTR_DIR 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN 0x0F
#define S_IFDIR 0040000
#define S_IFREG 0100000

struct sector_cache { uint64_t sector; uint8_t* data; uint64_t used; bool valid, dirty; };
struct ident { uint64_t slot, ino; };
struct fatfs {
    struct mount* m; struct block_partition* part;
    uint32_t sector_size, scale, cluster_sectors, cluster_bytes, reserved, fats, fat_sectors, root_entries, root_sectors;
    uint64_t total_sectors, data_start, root_start;
    uint32_t clusters, type, root_cluster, fsinfo, free_count, next_free, epoch;
    bool free_known, fsinfo_dirty, readonly, marked_dirty, found_dirty, io_failed;
    uint32_t serial; char label[12];
    uint32_t uid, gid; uint16_t fmask, dmask; bool showexec, quiet, flush; int64_t offset_ns;
    struct sector_cache cache[CACHE_SECTORS]; uint64_t clock;
    struct ident* idents; unsigned ident_count, ident_capacity; uint64_t generation;
    uint8_t* scratch;                 /* one cluster */
    uint8_t* sector_buf;              /* one sector: metadata edits (not on the stack) */
};
static struct fatfs* fs_of(struct mount* m) { return m->sb; }

/* ---- sectors ---- */
static int raw_read(struct fatfs* fs, uint64_t sector, uint32_t count, void* buffer) {
    if (sector >= fs->total_sectors || count > fs->total_sectors - sector) return -5;
    return block_part_read(fs->part, sector * fs->scale, count * fs->scale, buffer) < 0 ? -5 : 0;
}
static int raw_write(struct fatfs* fs, uint64_t sector, uint32_t count, const void* buffer) {
    if (fs->readonly) return -30;
    if (sector >= fs->total_sectors || count > fs->total_sectors - sector) return -5;
    if (block_part_write(fs->part, sector * fs->scale, count * fs->scale, buffer) < 0) { fs->io_failed = true; return -5; }
    return 0;
}
static int flush_entry(struct fatfs* fs, struct sector_cache* c) {   /* a dirty FAT sector of copy 0 -> every copy */
    if (!c->valid || !c->dirty) return 0;
    for (uint32_t copy = 0; copy < fs->fats; copy++)
        if (raw_write(fs, c->sector + (uint64_t)copy * fs->fat_sectors, 1, c->data)) return -5;
    c->dirty = false; return 0;
}
static int flush_fat(struct fatfs* fs) {
    int e = 0;
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (flush_entry(fs, &fs->cache[i])) e = -5;
    return e;
}
static struct sector_cache* cache_slot(struct fatfs* fs, uint64_t sector, bool* hit) {
    struct sector_cache* pick = NULL;
    for (unsigned i = 0; i < CACHE_SECTORS; i++) {
        struct sector_cache* c = &fs->cache[i];
        if (c->valid && c->sector == sector) { c->used = ++fs->clock; *hit = true; return c; }
        if (!pick || !c->valid || (pick->valid && c->used < pick->used)) pick = c;
    }
    *hit = false;
    if (pick->valid && pick->dirty && flush_entry(fs, pick)) return NULL;
    return pick;
}
static uint8_t* sector_get(struct fatfs* fs, uint64_t sector) {
    bool hit; struct sector_cache* c = cache_slot(fs, sector, &hit);
    if (!c) return NULL;
    if (hit) return c->data;
    c->valid = false; c->dirty = false;
    if (raw_read(fs, sector, 1, c->data)) return NULL;
    c->sector = sector; c->valid = true; c->used = ++fs->clock;
    return c->data;
}
/* Directory sectors and other metadata: written through. */
static int sector_put(struct fatfs* fs, uint64_t sector, const uint8_t* data) {
    int e = raw_write(fs, sector, 1, data);
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].valid && fs->cache[i].sector == sector) {
        if (e) fs->cache[i].valid = false; else if (fs->cache[i].data != data) memcpy(fs->cache[i].data, data, fs->sector_size);
    }
    return e;
}
static void forget_range(struct fatfs* fs, uint64_t first, uint64_t count) {
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].valid && !fs->cache[i].dirty && fs->cache[i].sector >= first && fs->cache[i].sector < first + count) fs->cache[i].valid = false;
}

/* ---- FAT ---- */
static uint32_t eoc(struct fatfs* fs) { return fs->type == 12 ? 0xFFF : fs->type == 16 ? 0xFFFF : 0x0FFFFFFF; }
static bool is_eoc(struct fatfs* fs, uint32_t value) { return value >= (fs->type == 12 ? 0xFF8U : fs->type == 16 ? 0xFFF8U : 0x0FFFFFF8U); }
static bool valid_cluster(struct fatfs* fs, uint32_t c) { return c >= 2 && c < fs->clusters + 2; }
static uint64_t fat_offset(struct fatfs* fs, uint32_t cluster) { return fs->type == 12 ? cluster + cluster / 2 : fs->type == 16 ? cluster * 2ULL : cluster * 4ULL; }
static int fat_get(struct fatfs* fs, uint32_t cluster, uint32_t* value) {
    if (cluster >= fs->clusters + 2) return -5;
    uint64_t offset = fat_offset(fs, cluster);
    uint64_t sector = fs->reserved + offset / fs->sector_size; unsigned at = (unsigned)(offset % fs->sector_size);
    uint8_t* s = sector_get(fs, sector); if (!s) return -5;
    if (fs->type == 32) { *value = get_le32(s + at) & 0x0FFFFFFF; return 0; }
    if (fs->type == 16) { *value = get_le16(s + at); return 0; }
    uint32_t low = s[at], high;
    if (at + 1 < fs->sector_size) high = s[at + 1];
    else { uint8_t* t = sector_get(fs, sector + 1); if (!t) return -5; high = t[0]; }
    uint32_t pair = low | high << 8;
    *value = cluster & 1 ? pair >> 4 : pair & 0xFFF;
    return 0;
}
static uint8_t* fat_dirty(struct fatfs* fs, uint64_t sector) {
    if (fs->readonly) return NULL;
    uint8_t* s = sector_get(fs, sector); if (!s) return NULL;
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].valid && fs->cache[i].sector == sector) fs->cache[i].dirty = true;
    return s;
}
static int fat_set(struct fatfs* fs, uint32_t cluster, uint32_t value) {
    if (cluster >= fs->clusters + 2) return -5;
    uint64_t offset = fat_offset(fs, cluster);
    uint64_t sector = fs->reserved + offset / fs->sector_size; unsigned at = (unsigned)(offset % fs->sector_size);
    uint8_t* s = fat_dirty(fs, sector); if (!s) return fs->readonly ? -30 : -5;
    if (fs->type == 32) { put_le32(s + at, (get_le32(s + at) & 0xF0000000U) | (value & 0x0FFFFFFF)); return 0; }
    if (fs->type == 16) { put_le16(s + at, value); return 0; }
    uint8_t* high;
    if (at + 1 < fs->sector_size) high = s + at + 1;
    else { high = fat_dirty(fs, sector + 1); if (!high) return -5; s = sector_get(fs, sector); if (!s) return -5; }
    if (cluster & 1) { s[at] = (uint8_t)((s[at] & 0x0F) | (value << 4)); *high = (uint8_t)(value >> 4); }
    else { s[at] = (uint8_t)value; *high = (uint8_t)((*high & 0xF0) | ((value >> 8) & 0x0F)); }
    return 0;
}
static int count_free(struct fatfs* fs) {
    if (flush_fat(fs)) return -5;
    uint32_t free = 0;
    if (fs->type == 12) {
        for (uint32_t c = 2; c < fs->clusters + 2; c++) { uint32_t v; if (fat_get(fs, c, &v)) return -5; if (!v) free++; }
    } else {
        uint32_t per = fs->type == 16 ? 2 : 4, entries = fs->clusters + 2;
        uint8_t* chunk = (void*)arena_alloc(65536); if (!chunk) return -12;
        uint32_t chunk_sectors = 65536 / fs->sector_size;
        for (uint64_t sector = 0; (uint64_t)sector * fs->sector_size / per < entries; sector += chunk_sectors) {
            uint32_t n = chunk_sectors; if (sector + n > fs->fat_sectors) n = (uint32_t)(fs->fat_sectors - sector);
            if (!n || raw_read(fs, fs->reserved + sector, n, chunk)) { arena_free((uint64_t)chunk, 65536); return -5; }
            uint64_t first = sector * fs->sector_size / per;
            for (uint64_t i = 0; i < (uint64_t)n * fs->sector_size / per && first + i < entries; i++) {
                if (first + i < 2) continue;
                uint32_t v = per == 2 ? get_le16(chunk + i * 2) : get_le32(chunk + i * 4) & 0x0FFFFFFF;
                if (!v) free++;
            }
        }
        arena_free((uint64_t)chunk, 65536);
    }
    fs->free_count = free; fs->free_known = true; return 0;
}
static int allocate(struct fatfs* fs, uint32_t previous, uint32_t* out) {
    if (fs->free_known && !fs->free_count) return -28;
    uint32_t start = valid_cluster(fs, fs->next_free) ? fs->next_free : 2;
    for (uint32_t step = 0; step < fs->clusters; step++) {
        uint32_t c = start + step; if (c >= fs->clusters + 2) c -= fs->clusters;
        uint32_t v; if (fat_get(fs, c, &v)) return -5;
        if (v) continue;
        if (fat_set(fs, c, eoc(fs))) return -5;
        if (previous && fat_set(fs, previous, c)) { fat_set(fs, c, 0); return -5; }
        if (fs->free_known && fs->free_count) fs->free_count--;
        fs->next_free = c + 1; fs->fsinfo_dirty = true; *out = c;
        return 0;
    }
    fs->free_count = 0; fs->free_known = true;
    return -28;
}
static int free_chain(struct fatfs* fs, uint32_t cluster) {
    fs->epoch++;
    for (uint32_t steps = 0; valid_cluster(fs, cluster) && steps <= fs->clusters; steps++) {
        uint32_t next; if (fat_get(fs, cluster, &next)) return -5;
        if (!next) return 0;   /* already free: a broken chain ends here */
        if (fat_set(fs, cluster, 0)) return -5;
        if (fs->free_known) fs->free_count++;
        fs->fsinfo_dirty = true;
        if (cluster < fs->next_free) fs->next_free = cluster;
        if (is_eoc(fs, next)) return 0;
        cluster = next;
    }
    return 0;
}
/* Cluster number `index` (0-based) of a handle's chain, from its position hint when still valid.
 * 0 found, -1 past the end of the chain, -5 broken. */
static int chain_at(struct fatfs* fs, fat32_file_t* f, uint32_t first, uint64_t index, uint32_t* out) {
    uint32_t c = first; uint64_t i = 0;
    if (f && f->chain_epoch == fs->epoch + 1 && f->first == first && valid_cluster(fs, f->cursor) && f->cursor_index <= index) { c = f->cursor; i = f->cursor_index; }
    for (; i < index; i++) {
        if (!valid_cluster(fs, c) || i > fs->clusters) return -5;
        uint32_t next; if (fat_get(fs, c, &next)) return -5;
        if (is_eoc(fs, next)) return -1;
        if (!valid_cluster(fs, next)) return -5;
        c = next;
    }
    if (!valid_cluster(fs, c)) return -5;
    if (f && index <= 0xFFFFFFFFULL) { f->cursor = c; f->cursor_index = (uint32_t)index; f->chain_epoch = fs->epoch + 1; }
    *out = c; return 0;
}
/* Number of clusters in a chain (bounded; -5 when broken). */
static int64_t chain_length(struct fatfs* fs, fat32_file_t* f, uint32_t first, uint32_t* last_out) {
    if (!valid_cluster(fs, first)) { if (last_out) *last_out = 0; return 0; }
    uint32_t c = first; uint64_t i = 0;
    if (f && f->chain_epoch == fs->epoch + 1 && f->first == first && valid_cluster(fs, f->cursor)) { c = f->cursor; i = f->cursor_index; }
    for (;; i++) {
        if (i > fs->clusters) return -5;
        uint32_t next; if (fat_get(fs, c, &next)) return -5;
        if (is_eoc(fs, next)) break;
        if (!valid_cluster(fs, next)) return -5;
        c = next;
    }
    if (f && i <= 0xFFFFFFFFULL) { f->cursor = c; f->cursor_index = (uint32_t)i; f->chain_epoch = fs->epoch + 1; }
    if (last_out) *last_out = c;
    return (int64_t)i + 1;
}
static uint64_t cluster_sector(struct fatfs* fs, uint32_t c) { return fs->data_start + (uint64_t)(c - 2) * fs->cluster_sectors; }

/* ---- times ---- */
static void unix_to_fat(int64_t ns, uint16_t* date, uint16_t* time, uint8_t* tenths) {
    int64_t seconds = ns / 1000000000LL;
    if (seconds < 315532800) seconds = 315532800;
    if (seconds > 4354819198LL) seconds = 4354819198LL;   /* 2107-12-31 23:59:58 */
    int64_t days = seconds / 86400 + 719468, rest = seconds % 86400, era = days / 146097;
    unsigned doe = (unsigned)(days - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    unsigned mo = mp < 10 ? mp + 3 : mp - 9; int64_t y = (int64_t)yoe + era * 400 + (mo <= 2);
    *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *time = (uint16_t)(((rest / 3600) << 11) | (((rest / 60) % 60) << 5) | ((rest % 60) / 2));
    if (tenths) *tenths = (uint8_t)((rest % 2) * 100 + (ns % 1000000000LL) / 10000000LL);
}
static int64_t fat_to_unix(uint16_t date, uint16_t time, uint8_t tenths) {
    unsigned y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (!m || m > 12 || !d) return 315532800LL * 1000000000LL;
    unsigned hh = time >> 11, mm = (time >> 5) & 63, ss = (time & 31) * 2;
    if (tenths > 199) tenths = 0;
    int64_t yy = (int64_t)y - (m <= 2); int64_t era = yy / 400; unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    int64_t seconds = days * 86400 + hh * 3600 + mm * 60 + ss + tenths / 100;
    return seconds * 1000000000LL + (int64_t)(tenths % 100) * 10000000LL;
}

/* ---- identity ---- */
static uint64_t ident_get(struct fatfs* fs, uint64_t slot) {
    for (unsigned i = 0; i < fs->ident_count; i++) if (fs->idents[i].slot == slot) return fs->idents[i].ino;
    return slot + 2;   /* the slot itself (root = 1) */
}
static void ident_set(struct fatfs* fs, uint64_t slot, uint64_t ino) {
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
static uint64_t fresh_ino(struct fatfs* fs) { return (1ULL << 42) + ++fs->generation; }
static uint64_t slot_of(struct fatfs* fs, uint64_t sector, unsigned offset) { return sector * (fs->sector_size / 32) + offset / 32; }

/* ---- names ---- */
static unsigned utf8_decode(const char* s, unsigned* used) {
    const uint8_t* p = (const uint8_t*)s;
    if (p[0] < 0x80) { *used = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *used = 2; return (p[0] & 0x1FU) << 6 | (p[1] & 0x3F); }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { *used = 3; return (p[0] & 0x0FU) << 12 | (p[1] & 0x3FU) << 6 | (p[2] & 0x3F); }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *used = 4; return (p[0] & 0x07U) << 18 | (p[1] & 0x3FU) << 12 | (p[2] & 0x3FU) << 6 | (p[3] & 0x3F);
    }
    *used = 1; return 0xFFFD;
}
static unsigned utf8_encode(unsigned c, char* out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 63)); out[2] = (char)(0x80 | (c & 63)); return 3; }
    out[0] = (char)(0xF0 | c >> 18); out[1] = (char)(0x80 | ((c >> 12) & 63)); out[2] = (char)(0x80 | ((c >> 6) & 63)); out[3] = (char)(0x80 | (c & 63)); return 4;
}
/* UTF-8 name -> UTF-16 units (no terminator); 0 if invalid or too long */
static unsigned to_utf16(const char* name, uint16_t* out, unsigned capacity) {
    unsigned n = 0;
    while (*name) {
        unsigned used, c = utf8_decode(name, &used); name += used;
        if (c == 0xFFFD || c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|' || c == 0x7F) return 0;
        if (c >= 0x10000) { if (n + 2 > capacity) return 0; c -= 0x10000; out[n++] = (uint16_t)(0xD800 | (c >> 10)); out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF)); }
        else { if (n + 1 > capacity) return 0; out[n++] = (uint16_t)c; }
    }
    return n;
}
static void from_utf16(const uint16_t* in, unsigned count, char* out, unsigned capacity) {
    unsigned n = 0;
    for (unsigned i = 0; i < count && n + 5 < capacity; i++) {
        unsigned c = in[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00); i++; }
        else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
        if (!c || c == '/') c = '_';
        n += utf8_encode(c, out + n);
    }
    out[n] = 0;
}
static char fold(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static bool same_name(const char* a, const char* b) {
    for (; *a && *b; a++, b++) if (fold(*a) != fold(*b)) return false;
    return !*a && !*b;
}
static void short_display(const uint8_t* e, char* out) {   /* "NAME.EXT" with NTRES case bits applied */
    unsigned n = 0; bool lower_base = (e[12] & 0x08) != 0, lower_ext = (e[12] & 0x10) != 0;
    for (unsigned i = 0; i < 8 && e[i] != ' '; i++) {
        uint8_t c = i == 0 && e[0] == 0x05 ? 0xE5 : e[i];
        if (c >= 0x80 || c < 0x20 || c == '/') c = '_';
        out[n++] = lower_base && c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8; i < 11 && e[i] != ' '; i++) { uint8_t c = e[i]; if (c >= 0x80 || c < 0x20 || c == '/') c = '_'; out[n++] = lower_ext && c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c; }
    }
    if (!n) out[n++] = '_';
    out[n] = 0;
}
static uint8_t lfn_checksum(const uint8_t* name11) { uint8_t sum = 0; for (unsigned i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]); return sum; }
static bool short_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || s_strchr("!#$%&'()-@^_`{}~", c) != NULL;
}
/* An 8.3 name for `name` without a long entry, when it fits exactly (case through NTRES). */
static bool exact_short(const char* name, uint8_t* out, uint8_t* ntres) {
    unsigned length = strlen(name); const char* dot = NULL;
    for (unsigned i = 0; i < length; i++) if (name[i] == '.') { if (dot) return false; dot = name + i; }
    unsigned base = dot ? (unsigned)(dot - name) : length, ext = dot ? length - base - 1 : 0;
    if (!base || base > 8 || ext > 3 || (dot && !ext)) return false;
    bool upper_b = false, lower_b = false, upper_e = false, lower_e = false;
    memset(out, ' ', 11);
    for (unsigned i = 0; i < base; i++) {
        char c = name[i]; if (!short_char(c)) return false;
        if (c >= 'a' && c <= 'z') lower_b = true; if (c >= 'A' && c <= 'Z') upper_b = true; out[i] = (uint8_t)fold(c);
    }
    for (unsigned i = 0; i < ext; i++) {
        char c = dot[1 + i]; if (!short_char(c)) return false;
        if (c >= 'a' && c <= 'z') lower_e = true; if (c >= 'A' && c <= 'Z') upper_e = true; out[8 + i] = (uint8_t)fold(c);
    }
    if ((lower_b && upper_b) || (lower_e && upper_e)) return false;
    *ntres = (uint8_t)((lower_b ? 0x08 : 0) | (lower_e ? 0x10 : 0));
    return true;
}

/* ---- directories ----
 * A directory is the fixed root region (FAT12/16 root: first == 0) or a cluster chain; entry i lives at byte i * 32. */
static int dir_sector(struct fatfs* fs, fat32_file_t* hint, uint32_t first, uint64_t index, uint64_t* sector, unsigned* offset) {
    uint64_t byte = index * 32;
    if (!first && fs->type != 32) {
        if (index >= fs->root_entries) return -1;
        *sector = fs->root_start + byte / fs->sector_size; *offset = (unsigned)(byte % fs->sector_size); return 0;
    }
    if (!first) first = fs->root_cluster;
    uint32_t c; int e = chain_at(fs, hint, first, byte / fs->cluster_bytes, &c);
    if (e) return e;
    uint64_t within = byte % fs->cluster_bytes;
    *sector = cluster_sector(fs, c) + within / fs->sector_size; *offset = (unsigned)(within % fs->sector_size);
    return 0;
}
struct dirent_info {
    uint8_t raw[32]; uint64_t index, sector; unsigned offset, lfn_count; uint64_t lfn_first;
    char name[FAT32_LFN_MAX * 3 + 1];
};
/* Next live entry at or after *index (volume labels skipped, dot entries only with `dots`); 1, 0 end, -errno. */
static int dir_next(struct fatfs* fs, uint32_t first, uint64_t* index, struct dirent_info* out, bool dots) {
    uint16_t lfn[260]; unsigned lfn_units = 0, lfn_expect = 0, lfn_count = 0; uint8_t lfn_sum = 0; uint64_t lfn_first = 0;
    fat32_file_t hint; memset(&hint, 0, sizeof(hint)); hint.first = first ? first : fs->root_cluster;
    for (uint64_t guard = 0; guard < 2097152; guard++) {
        uint64_t sector; unsigned offset;
        int e = dir_sector(fs, &hint, first, *index, &sector, &offset);
        if (e == -1) return 0; if (e) return e;
        uint8_t* s = sector_get(fs, sector); if (!s) return -5;
        const uint8_t* d = s + offset;
        uint64_t this = (*index)++;
        if (d[0] == 0x00) { (*index)--; return 0; }
        if (d[0] == 0xE5) { lfn_expect = 0; lfn_count = 0; continue; }
        if (d[11] == ATTR_LFN) {
            unsigned ord = d[0] & 0x1F;
            if (d[0] & 0x40) {
                if (!ord || ord > 20) { lfn_expect = 0; lfn_count = 0; continue; }
                lfn_expect = ord; lfn_sum = d[13]; lfn_units = ord * 13; lfn_count = 0; lfn_first = this;
                for (unsigned k = 0; k < 260; k++) lfn[k] = 0xFFFF;
            } else if (!lfn_expect || ord != lfn_expect || d[13] != lfn_sum) { lfn_expect = 0; lfn_count = 0; continue; }
            unsigned base = (ord - 1) * 13;
            static const unsigned at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (unsigned k = 0; k < 13 && base + k < 260; k++) lfn[base + k] = get_le16(d + at[k]);
            lfn_expect--; lfn_count++;
            continue;
        }
        if (d[11] & ATTR_VOLUME) { lfn_expect = 0; lfn_count = 0; continue; }
        bool dot = d[0] == '.' && (d[1] == ' ' || (d[1] == '.' && d[2] == ' '));
        if (dot && !dots) { lfn_count = 0; continue; }
        memcpy(out->raw, d, 32); out->index = this; out->sector = sector; out->offset = offset; out->lfn_count = 0; out->lfn_first = this;
        if (lfn_count && !lfn_expect && lfn_checksum(d) == lfn_sum) {
            unsigned units = 0; while (units < lfn_units && units < 260 && lfn[units] && lfn[units] != 0xFFFF) units++;
            from_utf16(lfn, units, out->name, sizeof(out->name));
            out->lfn_count = lfn_count; out->lfn_first = lfn_first;
        } else if (dot) strcpy(out->name, d[1] == '.' ? ".." : ".");
        else short_display(d, out->name);
        if (!out->name[0]) short_display(d, out->name);
        return 1;
    }
    return -5;
}
static uint32_t entry_cluster(struct fatfs* fs, const uint8_t* e) { return (fs->type == 32 ? (uint32_t)get_le16(e + 20) << 16 : 0) | get_le16(e + 26); }
static void fill_file(struct fatfs* fs, const struct dirent_info* info, uint32_t parent, fat32_file_t* f) {
    memset(f, 0, sizeof(*f));
    const uint8_t* e = info->raw;
    f->first = entry_cluster(fs, e); f->size = get_le32(e + 28); f->is_dir = (e[11] & ATTR_DIR) != 0;
    if (f->is_dir) f->size = 0;
    f->read_only = (e[11] & ATTR_RO) != 0 && !f->is_dir;
    f->dir_lba = (uint32_t)info->sector; f->dir_off = info->offset; f->parent = parent; f->entry_index = (uint32_t)info->index;
    f->lfn_count = (uint8_t)info->lfn_count;
    f->wdate = get_le16(e + 24); f->wtime = get_le16(e + 22);
    strncpy(f->name, info->name, FAT32_LFN_MAX); f->name[FAT32_LFN_MAX] = 0;
    short_display(e, f->short_name);
    uint64_t ino = ident_get(fs, slot_of(fs, info->sector, info->offset));
    f->ino = (uint32_t)ino; f->generation = (uint32_t)(ino >> 32);
    f->blocks = f->is_dir ? fs->cluster_bytes / 512 : ((uint64_t)f->size + fs->cluster_bytes - 1) / fs->cluster_bytes * (fs->cluster_bytes / 512);
    unsigned mode = f->is_dir ? 0777 & ~fs->dmask : 0777 & ~fs->fmask;
    if (!f->is_dir && fs->showexec) {
        unsigned n = strlen(f->name);
        bool exe = n > 4 && (same_name(f->name + n - 4, ".exe") || same_name(f->name + n - 4, ".com") || same_name(f->name + n - 4, ".bat"));
        if (!exe) mode &= ~0111U;
    }
    if (f->read_only) mode &= ~0222U;
    f->mode = (f->is_dir ? S_IFDIR : S_IFREG) | mode; f->uid = fs->uid; f->gid = fs->gid; f->nlink = f->is_dir ? 2 : 1;
    int64_t mtime = fat_to_unix(f->wdate, f->wtime, 0) - fs->offset_ns;
    f->mtime = mtime; f->ctime = mtime;
    f->atime = fat_to_unix(get_le16(e + 18), 0, 0) - fs->offset_ns;
    f->btime = fat_to_unix(get_le16(e + 16), get_le16(e + 14), e[13]) - fs->offset_ns;
}
static void root_file(struct fatfs* fs, fat32_file_t* f) {
    memset(f, 0, sizeof(*f));
    f->is_dir = true; f->first = fs->type == 32 ? fs->root_cluster : 0; f->dir_lba = MOUNT_ROOT_LBA; f->ino = 1;
    f->mode = S_IFDIR | (0777 & ~fs->dmask); f->uid = fs->uid; f->gid = fs->gid; f->nlink = 2; f->blocks = fs->cluster_bytes / 512;
    f->mtime = f->ctime = f->atime = f->btime = 315532800LL * 1000000000LL;
}
static uint32_t dir_first(struct fatfs* fs, const fat32_file_t* dir) { return dir->dir_lba == MOUNT_ROOT_LBA ? (fs->type == 32 ? fs->root_cluster : 0) : dir->first; }
static int find(struct fatfs* fs, uint32_t first, const char* name, struct dirent_info* out) {
    uint64_t index = 0; int r;
    while ((r = dir_next(fs, first, &index, out, false)) == 1) {
        if (same_name(out->name, name)) return 0;
        char shortname[16]; short_display(out->raw, shortname);
        if (same_name(shortname, name)) return 0;
    }
    return r < 0 ? r : -2;
}
static void leaf_name(const char* leaf, char* name, unsigned capacity) {
    unsigned n = 0; while (leaf[n] && leaf[n] != '/' && n + 1 < capacity) { name[n] = leaf[n]; n++; } name[n] = 0;
}
/* Walk a relative path to its last component's directory. */
static int walk(struct fatfs* fs, const char* path, fat32_file_t* dir, const char** leaf) {
    root_file(fs, dir);
    char part[FAT32_LFN_MAX * 3 + 1];
    for (const char* p = path;;) {
        while (*p == '/') p++;
        const char* end = p; while (*end && *end != '/') end++;
        const char* after = end; while (*after == '/') after++;
        if (!*after) { *leaf = p; return 0; }
        unsigned n = (unsigned)(end - p); if (n >= sizeof(part)) return -36;
        memcpy(part, p, n); part[n] = 0;
        struct dirent_info info; int e = find(fs, dir_first(fs, dir), part, &info); if (e) return e;
        if (!(info.raw[11] & ATTR_DIR)) return -20;
        uint32_t parent = dir_first(fs, dir);
        fill_file(fs, &info, parent, dir);
        p = after;
    }
}
static int fat_lookup(struct mount* m, const char* path, fat32_file_t* out) {
    struct fatfs* fs = fs_of(m);
    if (!path[0]) { root_file(fs, out); return 0; }
    fat32_file_t dir; const char* leaf; int e = walk(fs, path, &dir, &leaf); if (e) return e;
    char name[FAT32_LFN_MAX * 3 + 1]; leaf_name(leaf, name, sizeof(name));
    if (!name[0]) { *out = dir; return 0; }
    struct dirent_info info; e = find(fs, dir_first(fs, &dir), name, &info); if (e) return e;
    fill_file(fs, &info, dir_first(fs, &dir), out);
    return 0;
}
static int fat_refresh(struct mount* m, fat32_file_t* f) {
    struct fatfs* fs = fs_of(m);
    char name[FAT32_LFN_MAX + 1]; strcpy(name, f->name);
    if (f->dir_lba == MOUNT_ROOT_LBA) { root_file(fs, f); strcpy(f->name, name); return 0; }
    uint8_t* s = sector_get(fs, f->dir_lba); if (!s) return -5;
    const uint8_t* e = s + f->dir_off;
    if (e[0] == 0 || e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & ATTR_VOLUME)) return -2;
    struct dirent_info info; memcpy(info.raw, e, 32); info.sector = f->dir_lba; info.offset = f->dir_off; info.index = f->entry_index;
    info.lfn_count = f->lfn_count; strcpy(info.name, name);
    uint32_t parent = f->parent, cursor = f->cursor, cursor_index = f->cursor_index, epoch = f->chain_epoch;
    fill_file(fs, &info, parent, f);
    f->cursor = cursor; f->cursor_index = cursor_index; f->chain_epoch = epoch;
    return 0;
}
static int write_entry(struct fatfs* fs, const fat32_file_t* f, void (*edit)(struct fatfs*, uint8_t*, const void*), const void* context) {
    if (f->dir_lba == MOUNT_ROOT_LBA || f->detached) return 0;
    uint8_t* s = sector_get(fs, f->dir_lba); if (!s) return -5;
    uint8_t* data = fs->sector_buf; memcpy(data, s, fs->sector_size);
    uint8_t* e = data + f->dir_off;
    if (e[0] == 0 || e[0] == 0xE5) return -2;
    edit(fs, e, context);
    return sector_put(fs, f->dir_lba, data);
}
static void edit_size(struct fatfs* fs, uint8_t* e, const void* context) {
    const fat32_file_t* f = context;
    put_le32(e + 28, f->is_dir ? 0 : (uint32_t)f->size); put_le16(e + 26, f->first & 0xFFFF);
    if (fs->type == 32) put_le16(e + 20, f->first >> 16);
    e[11] |= ATTR_ARCHIVE;
}

/* ---- data ---- */
/* Transfer [offset, offset + length) of a chain: runs of consecutive clusters go to the device in one command,
 * partial sectors through the cache (write: read-modify-write). */
static int64_t transfer(struct fatfs* fs, fat32_file_t* f, uint64_t offset, uint64_t length, uint8_t* data, bool write) {
    uint64_t done = 0; uint32_t c;
    int e = chain_at(fs, f, f->first, offset / fs->cluster_bytes, &c); if (e) return -5;
    while (done < length) {
        uint32_t run = 1, last = c;
        uint64_t within = (offset + done) % fs->cluster_bytes, want = length - done + within;
        while ((uint64_t)run * fs->cluster_bytes < want && run < 256) {
            uint32_t next; if (fat_get(fs, last, &next)) return -5;
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
        if (done < length) {
            uint32_t next; if (fat_get(fs, last, &next) || !valid_cluster(fs, next)) return -5;
            c = next;
            uint64_t index = (offset + done) / fs->cluster_bytes;
            if (index <= 0xFFFFFFFFULL) { f->cursor = c; f->cursor_index = (uint32_t)index; f->chain_epoch = fs->epoch + 1; }
        }
    }
    return (int64_t)done;
}
static int64_t fat_read(struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, void* buffer) {
    struct fatfs* fs = fs_of(m);
    if (f->is_dir) return -21;
    if (offset >= f->size) return 0;
    if (length > f->size - offset) length = f->size - offset;
    int64_t r = transfer(fs, f, offset, length, buffer, false);
    return r;
}
static int zero_cluster(struct fatfs* fs, uint32_t c) {
    memset(fs->scratch, 0, fs->cluster_bytes); forget_range(fs, cluster_sector(fs, c), fs->cluster_sectors);
    return raw_write(fs, cluster_sector(fs, c), fs->cluster_sectors, fs->scratch);
}
/* Make the chain cover `bytes`; bytes between f->size and `zero_before` read back as zeros (FAT has no holes).
 * Fresh clusters starting at or after `zero_before` are left as they are: the caller writes them from their start
 * (what lies past the new end is slack, zeroed by a later growth through the stale range below). */
static int grow(struct fatfs* fs, fat32_file_t* f, uint64_t bytes, uint64_t zero_before) {
    uint32_t last; int64_t have = chain_length(fs, f, f->first, &last);
    if (have < 0) return -5;
    uint64_t have_bytes = (uint64_t)have * fs->cluster_bytes;
    uint64_t need = (bytes + fs->cluster_bytes - 1) / fs->cluster_bytes;
    for (uint64_t count = (uint64_t)have; count < need; count++) {
        uint32_t fresh; int e = allocate(fs, last, &fresh); if (e) return e;
        if (count * fs->cluster_bytes < zero_before && (e = zero_cluster(fs, fresh))) return e;
        if (!f->first) f->first = fresh;
        last = fresh;
        if (count <= 0xFFFFFFFFULL) { f->cursor = fresh; f->cursor_index = (uint32_t)count; f->chain_epoch = fs->epoch + 1; }
    }
    if (zero_before > bytes) zero_before = bytes;
    uint64_t stale = have_bytes < zero_before ? have_bytes : zero_before;   /* already allocated bytes past the old size */
    if (f->size < stale) {
        uint64_t from = f->size, length = stale - from;
        for (uint64_t at = 0; at < length;) {
            uint64_t n = length - at < fs->cluster_bytes ? length - at : fs->cluster_bytes;
            memset(fs->scratch, 0, n);
            if (transfer(fs, f, from + at, n, fs->scratch, true) < 0) return -5;
            at += n;
        }
    }
    return 0;
}
static int64_t fat_write(struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, const void* buffer) {
    struct fatfs* fs = fs_of(m);
    if (f->is_dir) return -21;
    if (f->detached) return (int64_t)length;
    if (offset + length > 0xFFFFFFFFULL) return -27;
    if (!length) return 0;
    uint64_t old_size = f->size; uint32_t old_first = f->first;
    int e = grow(fs, f, offset + length, offset);
    if (e) return e;
    int64_t done = transfer(fs, f, offset, length, (uint8_t*)buffer, true);
    if (done < 0) return done;
    if (offset + length > f->size) f->size = offset + length;
    if (f->size != old_size || f->first != old_first) { e = write_entry(fs, f, edit_size, f); if (e) return e; }
    if (fs->flush) flush_fat(fs);
    return done;
}
static int fat_truncate(struct mount* m, fat32_file_t* f, uint64_t size) {
    struct fatfs* fs = fs_of(m);
    if (f->is_dir) return -21;
    if (size > 0xFFFFFFFFULL) return -27;
    if (size == f->size) return 0;
    if (size > f->size) {
        int e = grow(fs, f, size, size); if (e) return e;
        f->size = size;
        return write_entry(fs, f, edit_size, f);
    }
    if (!size) {
        uint32_t first = f->first; f->first = 0; f->size = 0;
        int e = write_entry(fs, f, edit_size, f); if (e) return e;
        return valid_cluster(fs, first) ? free_chain(fs, first) : 0;
    }
    uint64_t keep = (size + fs->cluster_bytes - 1) / fs->cluster_bytes;
    uint32_t last; if (chain_at(fs, f, f->first, keep - 1, &last)) return -5;
    uint32_t next; if (fat_get(fs, last, &next)) return -5;
    f->size = size;
    int e = write_entry(fs, f, edit_size, f); if (e) return e;
    if (!is_eoc(fs, next)) { if (fat_set(fs, last, eoc(fs))) return -5; if (valid_cluster(fs, next)) return free_chain(fs, next); }
    return 0;
}
static int fat_readdir(struct mount* m, const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) {
    struct fatfs* fs = fs_of(m);
    if (!dir->is_dir) return -20;
    struct dirent_info info; uint64_t index = *cookie;
    int r = dir_next(fs, dir_first(fs, dir), &index, &info, false);
    if (r != 1) return r;
    fill_file(fs, &info, dir_first(fs, dir), out);
    *cookie = index;
    return 1;
}

/* ---- creation, removal, rename ---- */
/* `count` consecutive free entries in directory `first` (growing a cluster directory); index of the first. */
static int find_free_run(struct fatfs* fs, uint32_t first, unsigned count, uint64_t* out) {
    uint64_t run_start = 0; unsigned run = 0;
    fat32_file_t hint; memset(&hint, 0, sizeof(hint)); hint.first = first ? first : fs->root_cluster;
    for (uint64_t index = 0; index < 65536; index++) {
        uint64_t sector; unsigned offset;
        int e = dir_sector(fs, &hint, first, index, &sector, &offset);
        if (e == -1) {
            if (!first && fs->type != 32) return -28;   /* the FAT12/16 root is full */
            uint32_t last; int64_t length = chain_length(fs, NULL, first ? first : fs->root_cluster, &last);
            if (length < 0) return -5;
            uint32_t fresh; if ((e = allocate(fs, last, &fresh))) return e;
            if ((e = zero_cluster(fs, fresh))) return e;
            index--; continue;
        }
        if (e) return e;
        uint8_t* s = sector_get(fs, sector); if (!s) return -5;
        uint8_t b = s[offset];
        if (b == 0x00 || b == 0xE5) { if (!run) run_start = index; if (++run == count) { *out = run_start; return 0; } }
        else run = 0;
    }
    return -28;
}
static bool short_taken(struct fatfs* fs, uint32_t first, const uint8_t* name11) {
    uint64_t index = 0; struct dirent_info info;
    while (dir_next(fs, first, &index, &info, true) == 1) if (!memcmp(info.raw, name11, 11)) return true;
    return false;
}
static void basis_name(const char* name, uint8_t* out) {   /* Windows basis-name: uppercase, invalid -> '_' */
    memset(out, ' ', 11);
    const char* dot = s_strrchr(name, '.'); if (dot == name) dot = NULL;
    unsigned n = 0;
    for (const char* p = name; *p && p != dot && n < 8; p++) {
        char c = *p; if (c == ' ' || c == '.') continue;
        if ((unsigned char)c >= 0x80) c = '_';
        c = fold(c); if (!short_char(c)) c = '_';
        out[n++] = (uint8_t)c;
    }
    if (!n) out[n++] = '_';
    n = 0;
    if (dot) for (const char* p = dot + 1; *p && n < 3; p++) {
        char c = *p; if (c == ' ') continue;
        if ((unsigned char)c >= 0x80) c = '_';
        c = fold(c); if (!short_char(c)) c = '_';
        out[8 + n++] = (uint8_t)c;
    }
}
static int unique_short(struct fatfs* fs, uint32_t first, const char* name, uint8_t* out) {
    basis_name(name, out);
    unsigned base = 0; while (base < 8 && out[base] != ' ') base++;
    for (unsigned tail = 1; tail < 1000000; tail++) {
        char digits[8], reverse[8]; unsigned d = 0, t = tail; do { reverse[d++] = (char)('0' + t % 10); t /= 10; } while (t);
        unsigned k = 0; digits[k++] = '~'; while (d) digits[k++] = reverse[--d];
        unsigned keep = base + k > 8 ? 8 - k : base;
        uint8_t candidate[11]; memcpy(candidate, out, 11);
        for (unsigned i = keep; i < 8; i++) candidate[i] = ' ';
        for (unsigned i = 0; i < k; i++) candidate[keep + i] = (uint8_t)digits[i];
        if (!short_taken(fs, first, candidate)) { memcpy(out, candidate, 11); return 0; }
    }
    return -17;
}
static int clean_name(const char* path_leaf, char* name, unsigned capacity) {   /* trailing dots and spaces go (Windows rule) */
    unsigned n = 0; while (path_leaf[n] && path_leaf[n] != '/') n++;
    if (!n) return -2;
    if (n >= capacity) return -36;
    memcpy(name, path_leaf, n); name[n] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -22;
    while (n && (name[n - 1] == '.' || name[n - 1] == ' ')) name[--n] = 0;
    return n ? 0 : -22;
}
static int insert_entry(struct fatfs* fs, uint32_t dir_cluster, const char* name, const uint8_t* entry_template, struct dirent_info* out) {
    uint16_t units[260]; unsigned count = to_utf16(name, units, 255);
    if (!count) return strlen(name) > 255 ? -36 : -22;
    uint8_t short_name[11], ntres = 0; bool lfn = !exact_short(name, short_name, &ntres);
    if (!lfn && short_taken(fs, dir_cluster, short_name)) return -17;
    if (lfn) { int e = unique_short(fs, dir_cluster, name, short_name); if (e) return e; }
    if (short_name[0] == 0xE5) short_name[0] = 0x05;
    unsigned lfn_entries = lfn ? (count + 12) / 13 : 0;
    uint64_t start;
    int e = find_free_run(fs, dir_cluster, lfn_entries + 1, &start); if (e) return e;
    uint8_t sum = lfn_checksum(short_name);
    fat32_file_t hint; memset(&hint, 0, sizeof(hint)); hint.first = dir_cluster ? dir_cluster : fs->root_cluster;
    for (unsigned k = 0; k <= lfn_entries; k++) {
        uint64_t sector; unsigned offset;
        if ((e = dir_sector(fs, &hint, dir_cluster, start + k, &sector, &offset))) return -5;
        uint8_t* s = sector_get(fs, sector); if (!s) return -5;
        uint8_t* data = fs->sector_buf; memcpy(data, s, fs->sector_size); uint8_t* d = data + offset;
        if (k < lfn_entries) {
            unsigned ord = lfn_entries - k; memset(d, 0, 32);
            d[0] = (uint8_t)(ord | (k == 0 ? 0x40 : 0)); d[11] = ATTR_LFN; d[13] = sum;
            static const unsigned at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (unsigned j = 0; j < 13; j++) {
                unsigned i = (ord - 1) * 13 + j;
                put_le16(d + at[j], i < count ? units[i] : i == count ? 0x0000 : 0xFFFF);
            }
        } else {
            memcpy(d, entry_template, 32); memcpy(d, short_name, 11); d[12] = (uint8_t)((d[12] & ~0x18) | ntres);
            if (out) {
                memcpy(out->raw, d, 32); out->index = start + k; out->sector = sector; out->offset = offset; out->lfn_count = lfn_entries; out->lfn_first = start;
                strncpy(out->name, name, sizeof(out->name) - 1); out->name[sizeof(out->name) - 1] = 0;
            }
        }
        if ((e = sector_put(fs, sector, data))) return e;
    }
    return 0;
}
static int erase_entry(struct fatfs* fs, uint32_t dir_cluster, uint64_t first_index, unsigned count) {
    fat32_file_t hint; memset(&hint, 0, sizeof(hint)); hint.first = dir_cluster ? dir_cluster : fs->root_cluster;
    for (unsigned k = 0; k < count; k++) {
        uint64_t sector; unsigned offset;
        int e = dir_sector(fs, &hint, dir_cluster, first_index + k, &sector, &offset); if (e) return -5;
        uint8_t* s = sector_get(fs, sector); if (!s) return -5;
        uint8_t* data = fs->sector_buf; memcpy(data, s, fs->sector_size); data[offset] = 0xE5;
        if ((e = sector_put(fs, sector, data))) return e;
    }
    return 0;
}
static void template_entry(struct fatfs* fs, uint8_t* e, uint8_t attr, uint32_t first, uint32_t size) {
    memset(e, 0, 32); memset(e, ' ', 11); e[11] = attr;
    uint16_t date, time; uint8_t tenths;
    unix_to_fat((int64_t)native_realtime_ns() + fs->offset_ns, &date, &time, &tenths);
    e[13] = tenths; put_le16(e + 14, time); put_le16(e + 16, date); put_le16(e + 18, date); put_le16(e + 22, time); put_le16(e + 24, date);
    put_le16(e + 26, first & 0xFFFF); if (fs->type == 32) put_le16(e + 20, first >> 16);
    put_le32(e + 28, size);
}
static int fat_create(struct mount* m, const char* path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, const char* target, fat32_file_t* out) {
    (void)uid; (void)gid; (void)rdev; (void)target;
    struct fatfs* fs = fs_of(m);
    uint32_t type = mode & 0170000;
    if (type != S_IFREG && type != S_IFDIR) return -1;     /* vfat: no symlinks, sockets, fifos or devices */
    fat32_file_t dir; const char* leaf; int e = walk(fs, path, &dir, &leaf); if (e) return e;
    char name[FAT32_LFN_MAX * 3 + 1]; if ((e = clean_name(leaf, name, sizeof(name)))) return e;
    struct dirent_info info;
    uint32_t parent = dir_first(fs, &dir);
    if (!find(fs, parent, name, &info)) return -17;
    uint8_t entry[32]; uint32_t cluster = 0;
    if (type == S_IFDIR) {
        if ((e = allocate(fs, 0, &cluster))) return e;
        memset(fs->scratch, 0, fs->cluster_bytes);
        template_entry(fs, fs->scratch, ATTR_DIR, cluster, 0); memcpy(fs->scratch, ".          ", 11);
        template_entry(fs, fs->scratch + 32, ATTR_DIR, fs->type == 32 && parent == fs->root_cluster ? 0 : parent, 0); memcpy(fs->scratch + 32, "..         ", 11);
        forget_range(fs, cluster_sector(fs, cluster), fs->cluster_sectors);
        if ((e = raw_write(fs, cluster_sector(fs, cluster), fs->cluster_sectors, fs->scratch))) { free_chain(fs, cluster); return e; }
    }
    template_entry(fs, entry, type == S_IFDIR ? ATTR_DIR : ATTR_ARCHIVE, cluster, 0);
    if (!(mode & 0222) && type == S_IFREG) entry[11] |= ATTR_RO;
    if ((e = insert_entry(fs, parent, name, entry, &info))) { if (cluster) free_chain(fs, cluster); return e; }
    ident_set(fs, slot_of(fs, info.sector, info.offset), fresh_ino(fs));   /* never the identity of an earlier file in this slot */
    fill_file(fs, &info, parent, out);
    if (fs->flush) flush_fat(fs);
    return 0;
}
static int fat_unlink(struct mount* m, const char* path, int directory, fat32_file_t* victim) {
    struct fatfs* fs = fs_of(m);
    fat32_file_t dir; const char* leaf; int e = walk(fs, path, &dir, &leaf); if (e) return e;
    char name[FAT32_LFN_MAX * 3 + 1]; leaf_name(leaf, name, sizeof(name));
    struct dirent_info info; uint32_t parent = dir_first(fs, &dir);
    if ((e = find(fs, parent, name, &info))) return e;
    fill_file(fs, &info, parent, victim);
    if (directory == 1 && !victim->is_dir) return -20;
    if (directory == 0 && victim->is_dir) return -21;
    if (victim->is_dir) {   /* only "." and ".." may remain */
        uint64_t index = 0; struct dirent_info child;
        int r = dir_next(fs, victim->first, &index, &child, false);
        if (r < 0) return r;
        if (r == 1) return -39;
    }
    if ((e = erase_entry(fs, parent, info.lfn_first, info.lfn_count + 1))) return e;
    ident_set(fs, slot_of(fs, info.sector, info.offset), fresh_ino(fs));
    victim->detached = true;
    return 0;
}
static int fat_release(struct mount* m, fat32_file_t* f) {
    struct fatfs* fs = fs_of(m);
    if (!f->detached || fs->readonly) return 0;
    uint32_t first = f->first; f->first = 0;
    int e = valid_cluster(fs, first) ? free_chain(fs, first) : 0;
    if (fs->flush) flush_fat(fs);
    return e;
}
static int fat_rename(struct mount* m, const char* from, const char* to, unsigned flags, fat32_file_t* moved, fat32_file_t* replaced) {
    struct fatfs* fs = fs_of(m);
    fat32_file_t sdir, tdir; const char *sleaf, *tleaf;
    int e = walk(fs, from, &sdir, &sleaf); if (e) return e;
    if ((e = walk(fs, to, &tdir, &tleaf))) return e;
    char sname[FAT32_LFN_MAX * 3 + 1], tname[FAT32_LFN_MAX * 3 + 1];
    leaf_name(sleaf, sname, sizeof(sname));
    if ((e = clean_name(tleaf, tname, sizeof(tname)))) return e;
    uint32_t sparent = dir_first(fs, &sdir), tparent = dir_first(fs, &tdir);
    struct dirent_info source, target;
    if ((e = find(fs, sparent, sname, &source))) return e;
    fat32_file_t sfile; fill_file(fs, &source, sparent, &sfile);
    if (sfile.is_dir) {   /* not into itself or a descendant: walk tparent's ".." chain */
        uint32_t walker = tparent;
        for (unsigned depth = 0; depth < 1024; depth++) {
            if (walker == sfile.first) return -22;
            if (!walker || (fs->type == 32 && walker == fs->root_cluster)) break;
            uint64_t index = 0; struct dirent_info up; int r;
            uint32_t next = 0; bool found = false;
            while ((r = dir_next(fs, walker, &index, &up, true)) == 1) if (!memcmp(up.raw, "..         ", 11)) { next = entry_cluster(fs, up.raw); found = true; break; }
            if (!found) break;
            walker = next;
        }
    }
    bool exists = !find(fs, tparent, tname, &target);
    memset(replaced, 0, sizeof(*replaced));
    if (exists && target.sector == source.sector && target.offset == source.offset) {
        if (!strcmp(target.name, tname)) { *moved = sfile; return 0; }   /* same name: nothing to do */
        exists = false;                                                    /* case-only change of one entry */
    } else if (exists) {
        if (flags & 1) return -17;
        fat32_file_t tfile; fill_file(fs, &target, tparent, &tfile);
        if (tfile.is_dir != sfile.is_dir) return tfile.is_dir ? -21 : -20;
        if (tfile.is_dir) { uint64_t index = 0; struct dirent_info child; int r = dir_next(fs, tfile.first, &index, &child, false); if (r < 0) return r; if (r == 1) return -39; }
        if ((e = erase_entry(fs, tparent, target.lfn_first, target.lfn_count + 1))) return e;
        ident_set(fs, slot_of(fs, target.sector, target.offset), fresh_ino(fs));
        *replaced = tfile; replaced->detached = true;
    }
    uint64_t sslot = slot_of(fs, source.sector, source.offset);
    uint64_t identity = ident_get(fs, sslot);
    uint8_t entry[32]; memcpy(entry, source.raw, 32);
    if ((e = erase_entry(fs, sparent, source.lfn_first, source.lfn_count + 1))) return e;
    struct dirent_info placed;
    if ((e = insert_entry(fs, tparent, tname, entry, &placed))) { insert_entry(fs, sparent, source.name, entry, NULL); return e; }
    ident_set(fs, sslot, fresh_ino(fs));
    ident_set(fs, slot_of(fs, placed.sector, placed.offset), identity);
    if (sfile.is_dir && sparent != tparent) {   /* the moved directory's '..' */
        uint64_t sector; unsigned offset;
        if (!dir_sector(fs, NULL, sfile.first, 1, &sector, &offset)) {
            uint8_t* s = sector_get(fs, sector);
            if (s && !memcmp(s + offset, "..         ", 11)) {
                uint8_t* data = fs->sector_buf; memcpy(data, s, fs->sector_size);
                uint32_t up = fs->type == 32 && tparent == fs->root_cluster ? 0 : tparent;
                put_le16(data + offset + 26, up & 0xFFFF); if (fs->type == 32) put_le16(data + offset + 20, up >> 16);
                if ((e = sector_put(fs, sector, data))) return e;
            }
        }
    }
    fill_file(fs, &placed, tparent, moved);
    if (fs->flush) flush_fat(fs);
    return 0;
}
struct times_edit { int64_t mtime, atime; bool set_m, set_a; uint8_t attr_set, attr_clear; };
static void edit_times(struct fatfs* fs, uint8_t* e, const void* context) {
    const struct times_edit* t = context; uint16_t date, time;
    if (t->set_m) { unix_to_fat(t->mtime + fs->offset_ns, &date, &time, NULL); put_le16(e + 22, time); put_le16(e + 24, date); }
    if (t->set_a) { unix_to_fat(t->atime + fs->offset_ns, &date, &time, NULL); put_le16(e + 18, date); }
    e[11] = (uint8_t)((e[11] | t->attr_set) & ~t->attr_clear);
}
static int fat_setattr(struct mount* m, fat32_file_t* f, const struct mount_attr* a) {
    struct fatfs* fs = fs_of(m);
    struct times_edit t = {0};
    if (a->valid & EXT4_ATTR_MODE) {
        /* Linux fat_sanitize_mode_change: r/x bits must stay as they are; w bits all (subject to the mask) or none on
         * files (ATTR_READ_ONLY), unchanged on directories; anything else is EPERM, or ignored with "quiet". */
        unsigned mask = f->is_dir ? fs->dmask : fs->fmask, perm = a->mode & 0777 & ~mask;
        bool ok = !(a->mode & 07000) && (perm & 0555) == (f->mode & 0555);
        if (ok) ok = f->is_dir ? (perm & 0222) == (0222 & ~mask) : !(perm & 0222) || (perm & 0222) == (0222 & ~mask);
        if (!ok) { if (!fs->quiet) return -1; }
        else if (!f->is_dir) { if (perm & 0222) t.attr_clear |= ATTR_RO; else t.attr_set |= ATTR_RO; }
    }
    if ((a->valid & EXT4_ATTR_UID) && a->uid != fs->uid && !fs->quiet) return -1;
    if ((a->valid & EXT4_ATTR_GID) && a->gid != fs->gid && !fs->quiet) return -1;
    if (a->valid & EXT4_ATTR_MTIME) { t.set_m = true; t.mtime = a->mtime; }
    if (a->valid & EXT4_ATTR_ATIME) { t.set_a = true; t.atime = a->atime; }
    if (a->valid & MOUNT_ATTR_SIZE) { int e = fat_truncate(m, f, a->size); if (e) return e; }
    if (!t.set_m && !t.set_a && !t.attr_set && !t.attr_clear) return 0;
    if (f->dir_lba == MOUNT_ROOT_LBA || f->detached) return 0;   /* the root has no entry; an unlinked file no longer has one */
    int e = write_entry(fs, f, edit_times, &t); if (e) return e;
    return fat_refresh(m, f);
}
static int fat_statfs(struct mount* m, struct mount_statfs* s) {
    struct fatfs* fs = fs_of(m);
    if (!fs->free_known) count_free(fs);
    s->type = 0x4D44; s->block_size = fs->cluster_bytes; s->blocks = fs->clusters; s->free = s->available = fs->free_known ? fs->free_count : 0;
    s->files = 0; s->free_files = 0; s->name_max = 260; s->fsid[0] = fs->serial; s->fsid[1] = 0;
    return 0;
}
static int write_fsinfo(struct fatfs* fs) {
    if (fs->type != 32 || !fs->fsinfo || !fs->fsinfo_dirty || fs->readonly) return 0;
    uint8_t* s = sector_get(fs, fs->fsinfo); if (!s) return -5;
    if (get_le32(s) != 0x41615252 || get_le32(s + 484) != 0x61417272) return 0;
    uint8_t* data = fs->sector_buf; memcpy(data, s, fs->sector_size);
    put_le32(data + 488, fs->free_known ? fs->free_count : 0xFFFFFFFF); put_le32(data + 492, fs->next_free);
    int e = sector_put(fs, fs->fsinfo, data); if (!e) fs->fsinfo_dirty = false;
    return e;
}
static int fat_sync(struct mount* m) {
    struct fatfs* fs = fs_of(m);
    if (fs->readonly) return 0;
    int e = flush_fat(fs);
    if (write_fsinfo(fs)) e = -5;
    if (block_flush(fs->part->dev) < 0) e = -5;
    return e;
}
static int set_clean(struct fatfs* fs, bool clean) {   /* FAT[1] clean-shutdown bit (FAT16 bit 15, FAT32 bit 27) */
    if (fs->type == 12) return 0;
    uint32_t value; if (fat_get(fs, 1, &value)) return -5;
    uint32_t bit = fs->type == 16 ? 0x8000 : 0x08000000;
    uint32_t next = clean ? value | bit : value & ~bit;
    if (next == value) return 0;
    if (fat_set(fs, 1, next)) return -5;
    return flush_fat(fs);
}
static void free_fs(struct fatfs* fs) {
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].data) arena_free((uint64_t)fs->cache[i].data, 4096);
    if (fs->sector_buf) arena_free((uint64_t)fs->sector_buf, 4096);
    if (fs->scratch) arena_free((uint64_t)fs->scratch, fs->cluster_bytes);
    if (fs->idents) arena_free((uint64_t)fs->idents, fs->ident_capacity * sizeof(struct ident));
    arena_free((uint64_t)fs, sizeof(*fs));
}
static int fat_unmount(struct mount* m, bool force) {
    struct fatfs* fs = fs_of(m);
    if (!fs) return 0;
    if (!fs->readonly && !fs->part->dev->removed && (!force || !m->dead)) {
        flush_fat(fs); write_fsinfo(fs);
        if (fs->marked_dirty && !fs->found_dirty && !fs->io_failed) set_clean(fs, true);
        block_flush(fs->part->dev);
    }
    free_fs(fs); m->sb = NULL;
    return 0;
}
static int fat_remount(struct mount* m, uint64_t flags, char* options) {
    (void)options; struct fatfs* fs = fs_of(m);
    bool ro = (flags & MS_RDONLY) != 0;
    if (ro && !fs->readonly) {
        flush_fat(fs); write_fsinfo(fs);
        if (fs->marked_dirty && !fs->found_dirty) set_clean(fs, true);
        block_flush(fs->part->dev); fs->readonly = true; fs->marked_dirty = false;
    } else if (!ro && fs->readonly) {
        if (fs->part->dev->read_only) return -13;
        fs->readonly = false;
        if (!fs->found_dirty && !set_clean(fs, false)) fs->marked_dirty = true;
    }
    return 0;
}
static int parse_options(struct fatfs* fs, char* options) {
    fs->uid = mount_current_uid(); fs->gid = mount_current_gid();
    unsigned umask = filesystem_umask(); fs->fmask = fs->dmask = (uint16_t)umask;
    char* cursor = options; char key[32], value[64];
    while (mount_option(&cursor, key, sizeof(key), value, sizeof(value))) {
        uint64_t v = 0; const char* end = value;
        bool number = value[0] && parse_u64(value, &v, &end) && !*end && v <= 0xFFFFFFFFULL;
        bool octal = value[0] != 0; uint64_t o = 0;
        for (const char* p = value; *p; p++) { if (*p < '0' || *p > '7' || o > 07777) { octal = false; break; } o = o * 8 + (uint64_t)(*p - '0'); }
        if (!strcmp(key, "uid") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, false, &k)) return -22; fs->uid = k; }
        else if (!strcmp(key, "gid") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, true, &k)) return -22; fs->gid = k; }
        else if (!strcmp(key, "umask") && octal) fs->fmask = fs->dmask = (uint16_t)(o & 0777);
        else if (!strcmp(key, "fmask") && octal) fs->fmask = (uint16_t)(o & 0777);
        else if (!strcmp(key, "dmask") && octal) fs->dmask = (uint16_t)(o & 0777);
        else if (!strcmp(key, "showexec") && !value[0]) fs->showexec = true;
        else if (!strcmp(key, "quiet") && !value[0]) fs->quiet = true;
        else if (!strcmp(key, "flush") && !value[0]) fs->flush = true;
        else if (!strcmp(key, "time_offset") && value[0]) {
            bool negative = value[0] == '-'; uint64_t minutes; const char* e2;
            if (!parse_u64(value + negative, &minutes, &e2) || *e2 || minutes > 24 * 60) return -22;
            fs->offset_ns = (negative ? -(int64_t)minutes : (int64_t)minutes) * 60000000000LL;
        }
        else if (!strcmp(key, "tz") && !strcmp(value, "UTC")) fs->offset_ns = 0;
        else if (!strcmp(key, "utf8") || !strcmp(key, "iocharset") || !strcmp(key, "codepage") || !strcmp(key, "shortname") ||
                 !strcmp(key, "errors") || !strcmp(key, "discard") || !strcmp(key, "nfs") || !strcmp(key, "usefree") ||
                 !strcmp(key, "allow_utime") || !strcmp(key, "check") || !strcmp(key, "dos1xfloppy") || !strcmp(key, "rodir") || !strcmp(key, "sys_immutable")) {}
        else { vga_print("[64] vfat: unrecognized mount option "); vga_print(key); vga_print("\n"); return -22; }
    }
    return 0;
}
/* /proc/mounts superblock options, in Linux vfat_show_options order. */
static void show_options(struct fatfs* fs, char* out) {
    out[0] = 0;
    if (fs->uid) { strcat(out, "uid="); append_u64(out, credentials_visible_id(fs->uid, false)); strcat(out, ","); }
    if (fs->gid) { strcat(out, "gid="); append_u64(out, credentials_visible_id(fs->gid, true)); strcat(out, ","); }
    char mask[8] = "0000";
    mask[1] = (char)('0' + ((fs->fmask >> 6) & 7)); mask[2] = (char)('0' + ((fs->fmask >> 3) & 7)); mask[3] = (char)('0' + (fs->fmask & 7));
    strcat(out, "fmask="); strcat(out, mask);
    mask[1] = (char)('0' + ((fs->dmask >> 6) & 7)); mask[2] = (char)('0' + ((fs->dmask >> 3) & 7)); mask[3] = (char)('0' + (fs->dmask & 7));
    strcat(out, ",dmask="); strcat(out, mask);
    strcat(out, ",codepage=437,iocharset=utf8,shortname=mixed");
    if (fs->showexec) strcat(out, ",showexec");
    if (fs->quiet) strcat(out, ",quiet");
    if (fs->flush) strcat(out, ",flush");
    if (fs->offset_ns) {
        int64_t minutes = fs->offset_ns / 60000000000LL;
        strcat(out, ",time_offset="); if (minutes < 0) { strcat(out, "-"); minutes = -minutes; } append_u64(out, (uint64_t)minutes);
    }
    strcat(out, ",utf8,errors=remount-ro");
}
static int fat_mount(struct mount* m, char* options) {
    struct fatfs* fs = (void*)arena_alloc(sizeof(*fs)); if (!fs) return -12;
    memset(fs, 0, sizeof(*fs)); fs->m = m; fs->part = m->part;
    int e = parse_options(fs, options);
    if (e) { arena_free((uint64_t)fs, sizeof(*fs)); return e; }
    show_options(fs, m->options);
    uint8_t* bs = (void*)arena_alloc(4096); if (!bs) { arena_free((uint64_t)fs, sizeof(*fs)); return -12; }
    e = -22;
    if (block_part_read(fs->part, 0, 1, bs) < 0) { e = -5; goto failed; }
    uint32_t ss = get_le16(bs + 11), spc = bs[13], reserved = get_le16(bs + 14), fats = bs[16], root_entries = get_le16(bs + 17);
    uint64_t total = get_le16(bs + 19) ? get_le16(bs + 19) : get_le32(bs + 32);
    uint32_t fat16 = get_le16(bs + 22), fat32 = get_le32(bs + 36);
    if ((bs[0] != 0xEB && bs[0] != 0xE9) || (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096) || !spc || (spc & (spc - 1)) || !reserved || !fats || fats > 4 ||
        bs[510] != 0x55 || bs[511] != 0xAA || !total || (uint64_t)spc * ss > 131072) goto failed;
    uint32_t fat_size = fat16 ? fat16 : fat32;
    if (!fat_size) goto failed;
    fs->sector_size = ss; fs->scale = ss / 512; fs->cluster_sectors = spc; fs->cluster_bytes = spc * ss; fs->reserved = reserved; fs->fats = fats;
    fs->fat_sectors = fat_size; fs->root_entries = root_entries; fs->root_sectors = (root_entries * 32 + ss - 1) / ss;
    if (total * fs->scale > block_part_sectors(fs->part)) { vga_print("[64] vfat: volume larger than its partition; using the partition size\n"); total = block_part_sectors(fs->part) / fs->scale; }
    fs->total_sectors = total;
    uint64_t meta = (uint64_t)reserved + (uint64_t)fats * fat_size + fs->root_sectors;
    if (meta >= total) goto failed;
    fs->root_start = (uint64_t)reserved + (uint64_t)fats * fat_size; fs->data_start = meta;
    uint64_t clusters = (total - meta) / spc;
    if (clusters > 0x0FFFFFF5) clusters = 0x0FFFFFF5;
    fs->clusters = (uint32_t)clusters;
    fs->type = !fat16 ? 32 : fs->clusters < 4085 ? 12 : 16;   /* BPB layout decides FAT32; the cluster count FAT12 vs 16 */
    uint64_t needed = fs->type == 12 ? ((uint64_t)fs->clusters + 2) * 3 / 2 + 1 : ((uint64_t)fs->clusters + 2) * (fs->type == 16 ? 2 : 4);
    if ((uint64_t)fat_size * ss < needed) {   /* fewer FAT entries than data clusters: only what the FAT maps is usable */
        uint64_t entries = fs->type == 12 ? (uint64_t)fat_size * ss * 2 / 3 : (uint64_t)fat_size * ss / (fs->type == 16 ? 2 : 4);
        if (entries < 3) goto failed;
        fs->clusters = (uint32_t)(entries - 2);
    }
    if (fs->type == 32) {
        fs->root_cluster = get_le32(bs + 44); fs->fsinfo = get_le16(bs + 48);
        if (!valid_cluster(fs, fs->root_cluster) || get_le16(bs + 42) || root_entries) goto failed;   /* FAT32 version 0.0 only */
        fs->serial = get_le32(bs + 67); memcpy(fs->label, bs + 71, 11);
    } else { fs->serial = get_le32(bs + 39); memcpy(fs->label, bs + 43, 11); if (!root_entries) goto failed; }
    fs->label[11] = 0;
    for (unsigned i = 0; i < CACHE_SECTORS; i++) { fs->cache[i].data = (void*)arena_alloc(4096); if (!fs->cache[i].data) { e = -12; goto failed; } }
    fs->scratch = (void*)arena_alloc(fs->cluster_bytes); if (!fs->scratch) { e = -12; goto failed; }
    fs->sector_buf = (void*)arena_alloc(4096); if (!fs->sector_buf) { e = -12; goto failed; }
    fs->readonly = (m->flags & MS_RDONLY) || fs->part->dev->read_only;
    fs->next_free = 2;
    if (fs->type == 32 && fs->fsinfo && fs->fsinfo < reserved) {
        uint8_t* info = sector_get(fs, fs->fsinfo);
        if (info && get_le32(info) == 0x41615252 && get_le32(info + 484) == 0x61417272) {
            uint32_t free = get_le32(info + 488), next = get_le32(info + 492);
            if (free <= fs->clusters) { fs->free_count = free; fs->free_known = true; }
            if (valid_cluster(fs, next)) fs->next_free = next;
        } else fs->fsinfo = 0;
    } else fs->fsinfo = 0;
    if (!fs->free_known && (e = count_free(fs))) goto failed;
    uint32_t fat0; if (fat_get(fs, 0, &fat0)) { e = -5; goto failed; }
    if ((fat0 & 0xFF) != bs[21] && (fat0 & 0xFF) < 0xF0) { vga_print("[64] vfat: FAT[0] does not match the media descriptor\n"); e = -22; goto failed; }
    if (fs->type != 12) {
        uint32_t fat1; if (fat_get(fs, 1, &fat1)) { e = -5; goto failed; }
        fs->found_dirty = !(fat1 & (fs->type == 16 ? 0x8000U : 0x08000000U));
        if (fs->found_dirty) vga_print("[64] vfat: volume was not properly unmounted; some data may be corrupt (run fsck)\n");
        if (!fs->readonly && !fs->found_dirty) { if (set_clean(fs, false)) { e = -5; goto failed; } fs->marked_dirty = true; }
    }
    arena_free((uint64_t)bs, 4096);
    m->sb = fs;
    char n[24];
    vga_print("[64] vfat: FAT"); vga_print(fs->type == 12 ? "12" : fs->type == 16 ? "16" : "32"); vga_print(" clusters="); number64(fs->clusters, n); vga_print(n);
    vga_print(" cluster_bytes="); number64(fs->cluster_bytes, n); vga_print(n); vga_print(" free="); number64(fs->free_count, n); vga_print(n); vga_print("\n");
    return 0;
failed:
    arena_free((uint64_t)bs, 4096);
    for (unsigned i = 0; i < CACHE_SECTORS; i++) if (fs->cache[i].data) arena_free((uint64_t)fs->cache[i].data, 4096);
    if (fs->scratch) arena_free((uint64_t)fs->scratch, fs->cluster_bytes);
    if (fs->sector_buf) arena_free((uint64_t)fs->sector_buf, 4096);
    arena_free((uint64_t)fs, sizeof(*fs));
    if (e == -22) vga_print("[64] vfat: not a valid FAT volume\n");
    return e;
}
static int fat_fsync(struct mount* m, fat32_file_t* f) { (void)f; return fat_sync(m); }
const struct mount_ops vfat_mount_ops = {
    .name = "vfat", .flags = MOUNT_NEEDS_BLOCK | MOUNT_USER_OK,
    .mount = fat_mount, .unmount = fat_unmount, .sync = fat_sync, .statfs = fat_statfs, .remount = fat_remount,
    .lookup = fat_lookup, .refresh = fat_refresh, .read = fat_read, .write = fat_write, .truncate = fat_truncate, .readdir = fat_readdir,
    .create = fat_create, .unlink = fat_unlink, .release = fat_release, .rename = fat_rename, .link = NULL, .setattr = fat_setattr,
    .fsync = fat_fsync, .max_file_size = 0xFFFFFFFFULL,
};
/* Identity for /dev/disk/by-* and fstab (fsprobe.c): serial "XXXX-XXXX" and label of a FAT boot sector. */
bool fatfs_identity(const uint8_t* bs, char* uuid, char* label, char* type) {
    if ((bs[0] != 0xEB && bs[0] != 0xE9) || bs[510] != 0x55 || bs[511] != 0xAA) return false;
    uint32_t ss = get_le16(bs + 11), spc = bs[13];
    if ((ss != 512 && ss != 1024 && ss != 2048 && ss != 4096) || !spc || (spc & (spc - 1)) || !get_le16(bs + 14) || !bs[16] || bs[16] > 4) return false;
    bool fat32 = !get_le16(bs + 22);
    if (fat32 && !get_le32(bs + 36)) return false;
    if (!memcmp(bs + 3, "NTFS    ", 8) || !memcmp(bs + 3, "EXFAT   ", 8)) return false;
    const uint8_t* ext = bs + (fat32 ? 64 : 36);
    uint32_t serial = 0; label[0] = 0;
    if (ext[2] == 0x29 || ext[2] == 0x28) {
        serial = get_le32(ext + 3);
        if (ext[2] == 0x29) { memcpy(label, ext + 7, 11); label[11] = 0; unsigned n = 11; while (n && label[n - 1] == ' ') label[--n] = 0; if (!strcmp(label, "NO NAME")) label[0] = 0; }
    }
    uuid[0] = 0; append_hex(uuid, serial >> 16, 4); strcat(uuid, "-"); append_hex(uuid, serial & 0xFFFF, 4);
    for (char* p = uuid; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
    strcpy(type, "vfat");
    return true;
}
