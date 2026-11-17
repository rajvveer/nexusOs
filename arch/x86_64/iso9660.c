/* ISO 9660 (ECMA-119) for CD/DVD images and devices, read-only, with Rock Ridge (RRIP: POSIX names, modes,
 * owners, link counts, times, symbolic links, relocated deep directories) and Joliet (UCS-2 names) like Linux isofs.
 *
 * Names: Rock Ridge NM when the primary volume has it (SUSP "SP" in the root's "." record), else Joliet (a
 * supplementary descriptor with escape %/@ %/C %/E), else the ISO name with ";1" and a trailing '.' removed and
 * mapped to lower case (map=normal; map=off keeps it). Options: norock, nojoliet, map=normal|off, uid=, gid=,
 * mode= (files), dmode= (directories), overriderockperm, unhide, cruft, check=, utf8, iocharset=, block=2048,
 * session=0/sbsector=0 only. Without Rock Ridge files are 0555 (mode=) and directories 0555 (dmode=), owned by
 * uid=/gid= (default: the mounter). Multi-extent files (> 4 GiB) are joined. Everything read from the medium is
 * bounds-checked: a record crossing a block, a CE loop or an extent outside the volume is EIO, never a fault.
 * Handles: first = extent (2048-byte blocks), size, dir_lba/dir_off = the directory record (root: MOUNT_ROOT_LBA),
 * parent = a directory's extent length in bytes, iflags bit 0 = multi-extent, ino = the record position
 * (directories: their extent, so a directory and its "." agree). */
#include "mount.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define BLOCK 2048u
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define MULTI_EXTENT 1u
#define CE_DEPTH 16

struct isofs {
    struct block_partition* part;
    uint32_t volume_blocks, root_extent, root_size;
    bool rock, joliet, map_off, override_perm;
    unsigned skip;                 /* SUSP bytes to skip in each system use area (SP) */
    uint32_t uid, gid, fmode, dmode; bool fmode_set, dmode_set;
    uint8_t* buffer;               /* one block */
    uint64_t buffered;             /* block in buffer + 1, 0 = none */
    int64_t created;               /* volume creation time (root times) */
};
static struct isofs* fs_of(struct mount* m) { return m->sb; }
static int read_block(struct isofs* fs, uint32_t block, uint8_t** out) {
    if (block >= fs->volume_blocks && fs->volume_blocks) return -5;
    if ((uint64_t)block * 4 + 4 > block_part_sectors(fs->part)) return -5;
    if (fs->buffered != (uint64_t)block + 1) {
        fs->buffered = 0;
        if (block_part_read(fs->part, (uint64_t)block * 4, 4, fs->buffer) < 0) return -5;
        fs->buffered = (uint64_t)block + 1;
    }
    *out = fs->buffer; return 0;
}
/* 7-byte directory record time (years since 1900, month, day, hour, minute, second, GMT offset in 15 min) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static int64_t record_time(const uint8_t* t) {
    if (!t[1] || t[1] > 12 || !t[2] || t[2] > 31) return 0;
    int64_t s = days_from_civil(1900 + t[0], t[1], t[2]) * 86400 + t[3] * 3600 + t[4] * 60 + t[5] - (int64_t)(int8_t)t[6] * 900;
    return s * 1000000000LL;
}
static int64_t long_time(const uint8_t* t) {   /* 17-byte "YYYYMMDDHHMMSScc" + offset */
    unsigned v[7]; const unsigned width[7] = {4, 2, 2, 2, 2, 2, 2}; unsigned at = 0;
    for (unsigned i = 0; i < 7; i++) { v[i] = 0; for (unsigned k = 0; k < width[i]; k++) { char c = (char)t[at++]; if (c < '0' || c > '9') return 0; v[i] = v[i] * 10 + (unsigned)(c - '0'); } }
    if (!v[1] || v[1] > 12 || !v[2] || v[2] > 31) return 0;
    int64_t s = days_from_civil(v[0], v[1], v[2]) * 86400 + v[3] * 3600 + v[4] * 60 + v[5] - (int64_t)(int8_t)t[16] * 900;
    return s * 1000000000LL + (int64_t)v[6] * 10000000LL;
}
static unsigned utf8_put(unsigned c, char* out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 63)); return 2; }
    out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 63)); out[2] = (char)(0x80 | (c & 63)); return 3;
}

/* What one directory record says, Rock Ridge applied. */
struct record {
    uint32_t extent, block, offset;   /* block/offset: where the record is */
    uint64_t size;                    /* multi-extent files: the sum of their parts */
    uint8_t flags; char name[256]; bool dot;
    bool have_px, relocated, child; uint32_t mode, nlink, uid, gid, child_extent;
    int64_t mtime, atime, ctime, btime; bool symlink; char target[1024];
};
/* Rock Ridge system use entries of one record, following CE continuations. */
static void rock_ridge(struct isofs* fs, const uint8_t* su, unsigned length, struct record* r) {
    char name[256]; unsigned name_length = 0; bool have_name = false;
    unsigned target_length = 0; bool component_open = false;
    uint8_t* area = (void*)arena_alloc(BLOCK); if (!area) return;
    unsigned depth = 0;
    for (;;) {
        uint32_t ce_block = 0, ce_offset = 0, ce_length = 0;
        for (unsigned at = 0; at + 4 <= length; ) {
            const uint8_t* e = su + at; unsigned len = e[2];
            if (len < 4 || at + len > length) break;
            if (e[0] == 'C' && e[1] == 'E' && len >= 28) { ce_block = get_le32(e + 4); ce_offset = get_le32(e + 12); ce_length = get_le32(e + 20); }
            else if (e[0] == 'N' && e[1] == 'M' && len >= 5) {
                if (e[4] & 6) { have_name = false; }
                else for (unsigned i = 5; i < len && name_length + 1 < sizeof(name); i++) { name[name_length++] = (char)e[i]; have_name = true; }
            } else if (e[0] == 'P' && e[1] == 'X' && len >= 36) {
                r->have_px = true; r->mode = get_le32(e + 4); r->nlink = get_le32(e + 12); r->uid = get_le32(e + 20); r->gid = get_le32(e + 28);
            } else if (e[0] == 'T' && e[1] == 'F' && len >= 5) {
                unsigned flags = e[4], size = flags & 0x80 ? 17 : 7, p = 5;
                int64_t* order[4] = {&r->btime, &r->mtime, &r->atime, &r->ctime};
                for (unsigned bit = 0; bit < 4; bit++) if (flags & (1u << bit)) {
                    if (p + size > len) break;
                    *order[bit] = size == 17 ? long_time(e + p) : record_time(e + p); p += size;
                }
            } else if (e[0] == 'S' && e[1] == 'L' && len >= 5) {
                r->symlink = true;
                for (unsigned p = 5; p + 2 <= len; ) {
                    unsigned cflags = e[p], clen = e[p + 1];
                    if (p + 2 + clen > len) break;
                    if (!component_open && target_length && target_length + 1 < sizeof(r->target)) r->target[target_length++] = '/';
                    const char* text = cflags & 2 ? "." : cflags & 4 ? ".." : NULL;
                    if (cflags & 8) { target_length = 0; r->target[target_length++] = '/'; component_open = true; p += 2; continue; }
                    if (text) for (unsigned k = 0; text[k] && target_length + 1 < sizeof(r->target); k++) r->target[target_length++] = text[k];
                    else for (unsigned k = 0; k < clen && target_length + 1 < sizeof(r->target); k++) r->target[target_length++] = (char)e[p + 2 + k];
                    component_open = (cflags & 1) != 0;
                    p += 2 + clen;
                }
                if (target_length == 1 && r->target[0] == '/') component_open = true;
            } else if (e[0] == 'R' && e[1] == 'E') r->relocated = true;
            else if (e[0] == 'C' && e[1] == 'L' && len >= 12) { r->child = true; r->child_extent = get_le32(e + 4); }
            else if (e[0] == 'S' && e[1] == 'T') break;
            at += len;
        }
        if (!ce_length || ++depth > CE_DEPTH || ce_offset >= BLOCK || ce_length > BLOCK - ce_offset) break;
        uint8_t* b; if (read_block(fs, ce_block, &b)) break;
        memcpy(area, b + ce_offset, ce_length); su = area; length = ce_length;
    }
    if (have_name) { memcpy(r->name, name, name_length); r->name[name_length] = 0; }
    r->target[target_length] = 0;
    arena_free((uint64_t)area, BLOCK);
}
/* Parse the record at b + offset (len checked by the caller). */
static void parse(struct isofs* fs, const uint8_t* d, uint32_t block, uint32_t offset, bool joliet, struct record* r) {
    memset(r, 0, sizeof(*r));
    r->extent = get_le32(d + 2) + d[1]; r->size = get_le32(d + 10); r->flags = d[25]; r->block = block; r->offset = offset;
    r->mtime = r->atime = r->ctime = r->btime = record_time(d + 18);
    unsigned name_length = d[32];
    const uint8_t* id = d + 33;
    if (name_length == 1 && (id[0] == 0 || id[0] == 1)) { r->dot = true; r->name[0] = id[0] ? '.' : 0; }
    else if (joliet) {
        unsigned n = 0;
        for (unsigned i = 0; i + 1 < name_length && n + 4 < sizeof(r->name); i += 2) {
            unsigned c = (unsigned)id[i] << 8 | id[i + 1];
            if (c == ';') break;
            n += utf8_put(c ? c : '_', r->name + n);
        }
        r->name[n] = 0;
    } else {
        unsigned n = 0;
        for (unsigned i = 0; i < name_length && n + 1 < sizeof(r->name); i++) {
            char c = (char)id[i];
            if (c == ';' && !fs->map_off) break;
            if (!fs->map_off && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            r->name[n++] = c;
        }
        if (!fs->map_off && n > 1 && r->name[n - 1] == '.') n--;
        r->name[n] = 0;
    }
    if (fs->rock && !joliet) {
        unsigned su = 33 + name_length + (name_length & 1 ? 0 : 1) + fs->skip;
        if (su < d[0]) rock_ridge(fs, d + su, d[0] - su, r);
    }
}
static uint64_t ino_of(const struct record* r) {
    if (r->flags & 2) return (uint64_t)r->extent << 11 | 1;
    return (uint64_t)r->block << 11 | r->offset;
}
static int fill(struct isofs* fs, const struct record* r, fat32_file_t* f) {
    memset(f, 0, sizeof(*f));
    bool dir = (r->flags & 2) || r->child;
    f->is_dir = dir; f->first = r->child ? r->child_extent : r->extent; f->size = dir ? 0 : r->size;
    if (dir && r->child) {   /* a relocated directory: its length from its own "." record */
        uint8_t* b; if (read_block(fs, f->first, &b) || b[0] < 34) return -5;
        f->parent = get_le32(b + 10);
    } else if (dir) f->parent = (uint32_t)r->size;
    f->dir_lba = r->block; f->dir_off = r->offset; f->iflags = r->flags & 0x80 ? MULTI_EXTENT : 0;
    strncpy(f->name, r->name, FAT32_LFN_MAX); f->name[FAT32_LFN_MAX] = 0;
    uint64_t ino = ino_of(r); f->ino = (uint32_t)ino; f->generation = (uint32_t)(ino >> 32);
    if (r->have_px && !fs->override_perm) {
        f->mode = r->mode; f->uid = r->uid; f->gid = r->gid; f->nlink = r->nlink ? r->nlink : 1;
        if (r->child) f->mode = S_IFDIR | (r->mode & 07777);
        uint32_t k; if (credentials_kernel_from_visible(r->uid, false, &k)) f->uid = k;
        if (credentials_kernel_from_visible(r->gid, true, &k)) f->gid = k;
    } else {
        f->mode = dir ? S_IFDIR | fs->dmode : S_IFREG | fs->fmode; f->uid = fs->uid; f->gid = fs->gid; f->nlink = dir ? 2 : 1;
        if (r->have_px && (r->mode & S_IFMT) == S_IFLNK) f->mode = S_IFLNK | 0777;
    }
    if (r->symlink && (f->mode & S_IFMT) == S_IFLNK) f->size = strlen(r->target);
    f->blocks = ((uint64_t)(dir ? f->parent : f->size) + BLOCK - 1) / BLOCK * 4;
    f->mtime = r->mtime; f->atime = r->atime; f->ctime = r->ctime; f->btime = r->btime;
    return 0;
}
static void root(struct isofs* fs, fat32_file_t* f) {
    memset(f, 0, sizeof(*f));
    f->is_dir = true; f->first = fs->root_extent; f->parent = fs->root_size; f->dir_lba = MOUNT_ROOT_LBA; f->ino = 1;
    f->mode = S_IFDIR | fs->dmode; f->uid = fs->uid; f->gid = fs->gid; f->nlink = 2; f->blocks = 4;
    f->mtime = f->atime = f->ctime = f->btime = fs->created;
    if (fs->rock) {   /* the root's "." record carries its Rock Ridge attributes */
        uint8_t* b; struct record r;
        if (!read_block(fs, fs->root_extent, &b) && b[0] >= 34) {
            uint8_t copy[256]; memcpy(copy, b, b[0]);
            parse(fs, copy, fs->root_extent, 0, false, &r);
            if (r.have_px && !fs->override_perm) {
                f->mode = S_IFDIR | (r.mode & 07777); f->nlink = r.nlink ? r.nlink : 2;
                uint32_t k; f->uid = credentials_kernel_from_visible(r.uid, false, &k) ? k : r.uid; f->gid = credentials_kernel_from_visible(r.gid, true, &k) ? k : r.gid;
            }
            f->mtime = r.mtime; f->atime = r.atime; f->ctime = r.ctime;
        }
    }
}
/* The record at *position (byte offset in the directory extent) -> r; 1, 0 at the end, -errno. */
static int next_record(struct isofs* fs, uint32_t extent, uint32_t size, uint64_t* position, struct record* r) {
    while (*position < size) {
        uint32_t block = extent + (uint32_t)(*position / BLOCK), within = (uint32_t)(*position % BLOCK);
        uint8_t* b; if (read_block(fs, block, &b)) return -5;
        unsigned len = b[within];
        if (!len) { *position = (*position / BLOCK + 1) * BLOCK; continue; }
        if (len < 34 || within + len > BLOCK || 33u + b[within + 32] > len) return -5;
        uint8_t record[256]; memcpy(record, b + within, len);
        *position += len;
        parse(fs, record, block, within, fs->joliet, r);
        if (r->dot || (r->flags & 4) || r->relocated) continue;   /* ".", "..", associated files, relocated directories */
        if (!r->name[0]) continue;
        if ((r->flags & 0x80)) {   /* multi-extent: the size of every following part of the same file */
            uint64_t total = r->size, at = *position; struct record more;
            while (at < size) {
                uint32_t mb = extent + (uint32_t)(at / BLOCK), mw = (uint32_t)(at % BLOCK);
                uint8_t* x; if (read_block(fs, mb, &x)) return -5;
                unsigned l = x[mw]; if (!l) { at = (at / BLOCK + 1) * BLOCK; continue; }
                if (l < 34 || mw + l > BLOCK) return -5;
                uint8_t rec[256]; memcpy(rec, x + mw, l); at += l;
                parse(fs, rec, mb, mw, fs->joliet, &more);
                total += more.size; *position = at;
                if (!(more.flags & 0x80)) break;
            }
            r->size = total;
            r->flags |= 0x80;
        }
        return 1;
    }
    return 0;
}
static uint32_t dir_length(struct isofs* fs, const fat32_file_t* dir) { return dir->dir_lba == MOUNT_ROOT_LBA ? fs->root_size : dir->parent; }
static int find(struct isofs* fs, const fat32_file_t* dir, const char* name, struct record* out) {
    uint64_t position = 0; int r;
    while ((r = next_record(fs, dir->first, dir_length(fs, dir), &position, out)) == 1) {
        if (!strcmp(out->name, name)) return 0;
        if (!fs->rock && !fs->joliet) {   /* plain ISO names: also case-insensitively (Linux map=normal checks) */
            unsigned i = 0; for (; name[i] && out->name[i]; i++) { char a = name[i], c = out->name[i]; if (a >= 'A' && a <= 'Z') a += 32; if (c >= 'A' && c <= 'Z') c += 32; if (a != c) break; }
            if (!name[i] && !out->name[i]) return 0;
        }
    }
    return r < 0 ? r : -2;
}
static int iso_lookup(struct mount* m, const char* path, fat32_file_t* out) {
    struct isofs* fs = fs_of(m);
    fat32_file_t dir; root(fs, &dir);
    if (!path[0]) { *out = dir; return 0; }
    char part[256];
    for (const char* p = path;;) {
        while (*p == '/') p++;
        const char* end = p; while (*end && *end != '/') end++;
        unsigned n = (unsigned)(end - p); if (!n) { *out = dir; return 0; }
        if (n >= sizeof(part)) return -36;
        memcpy(part, p, n); part[n] = 0;
        if (!dir.is_dir) return -20;
        struct record* r = (void*)arena_alloc(sizeof(struct record)); if (!r) return -12;
        int e = find(fs, &dir, part, r);
        if (!e) e = fill(fs, r, &dir);
        arena_free((uint64_t)r, sizeof(struct record));
        if (e) return e;
        p = end; while (*p == '/') p++;
        if (!*p) { *out = dir; return 0; }
    }
}
static int iso_refresh(struct mount* m, fat32_file_t* f) { (void)m; (void)f; return 0; }   /* nothing changes on a read-only medium */
static int iso_readdir(struct mount* m, const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) {
    struct isofs* fs = fs_of(m);
    if (!dir->is_dir) return -20;
    struct record* r = (void*)arena_alloc(sizeof(struct record)); if (!r) return -12;
    uint64_t position = *cookie;
    int e = next_record(fs, dir->first, dir_length(fs, dir), &position, r);
    if (e == 1 && fill(fs, r, out)) e = -5;
    arena_free((uint64_t)r, sizeof(struct record));
    if (e != 1) return e;
    *cookie = position; return 1;
}
static int64_t extent_read(struct isofs* fs, uint32_t extent, uint64_t extent_size, uint64_t offset, uint64_t length, uint8_t* out) {
    if (offset >= extent_size) return 0;
    if (length > extent_size - offset) length = extent_size - offset;
    uint64_t done = 0;
    while (done < length) {
        uint64_t at = offset + done; uint32_t block = extent + (uint32_t)(at / BLOCK), within = (uint32_t)(at % BLOCK);
        if (!within && length - done >= BLOCK) {   /* whole blocks straight to the caller */
            uint64_t blocks = (length - done) / BLOCK; if (blocks > 64) blocks = 64;
            if (((uint64_t)block + blocks) * 4 > block_part_sectors(fs->part)) return -5;
            if (block_part_read(fs->part, (uint64_t)block * 4, (uint32_t)(blocks * 4), out + done) < 0) return -5;
            done += blocks * BLOCK; continue;
        }
        uint8_t* b; if (read_block(fs, block, &b)) return -5;
        uint64_t take = BLOCK - within; if (take > length - done) take = length - done;
        memcpy(out + done, b + within, take); done += take;
    }
    return (int64_t)done;
}
static int64_t iso_read(struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, void* buffer) {
    struct isofs* fs = fs_of(m);
    if (f->is_dir) return -21;
    if ((f->mode & S_IFMT) == S_IFLNK) {   /* the link target, from the record's SL entries */
        uint8_t* b; if (read_block(fs, f->dir_lba, &b)) return -5;
        uint8_t record[256]; unsigned len = b[f->dir_off]; if (len < 34 || f->dir_off + len > BLOCK) return -5;
        memcpy(record, b + f->dir_off, len);
        struct record* r = (void*)arena_alloc(sizeof(struct record)); if (!r) return -12;
        parse(fs, record, f->dir_lba, f->dir_off, false, r);
        uint64_t n = strlen(r->target); int64_t result = 0;
        if (offset < n) { result = (int64_t)(length < n - offset ? length : n - offset); memcpy(buffer, r->target + offset, (size_t)result); }
        arena_free((uint64_t)r, sizeof(struct record)); return result;
    }
    if (offset >= f->size) return 0;
    if (length > f->size - offset) length = f->size - offset;
    if (!(f->iflags & MULTI_EXTENT)) return extent_read(fs, f->first, f->size, offset, length, buffer);
    /* multi-extent: walk the parts from the first record */
    uint64_t done = 0, base = 0; uint32_t block = f->dir_lba, within = f->dir_off;
    while (done < length) {
        uint8_t* b; if (read_block(fs, block, &b)) return -5;
        unsigned len = b[within];
        if (!len) { block++; within = 0; continue; }
        if (len < 34 || within + len > BLOCK) return -5;
        uint32_t extent = get_le32(b + within + 2) + b[within + 1], size = get_le32(b + within + 10); uint8_t flags = b[within + 25];
        if (offset + done < base + size) {
            int64_t n = extent_read(fs, extent, size, offset + done - base, length - done, (uint8_t*)buffer + done);
            if (n <= 0) return n < 0 ? n : (int64_t)done;
            done += (uint64_t)n;
        }
        base += size; within += len;
        if (!(flags & 0x80)) break;
        if (within >= BLOCK) { block++; within = 0; }
    }
    return (int64_t)done;
}
static int readonly_error(void) { return -30; }
static int64_t iso_write(struct mount* m, fat32_file_t* f, uint64_t o, uint64_t l, const void* b) { (void)m; (void)f; (void)o; (void)l; (void)b; return readonly_error(); }
static int iso_truncate(struct mount* m, fat32_file_t* f, uint64_t s) { (void)m; (void)f; (void)s; return -30; }
static int iso_create(struct mount* m, const char* p, uint32_t mode, uint32_t u, uint32_t g, uint32_t r, const char* t, fat32_file_t* o) { (void)m; (void)p; (void)mode; (void)u; (void)g; (void)r; (void)t; (void)o; return -30; }
static int iso_unlink(struct mount* m, const char* p, int d, fat32_file_t* v) { (void)m; (void)p; (void)d; (void)v; return -30; }
static int iso_release(struct mount* m, fat32_file_t* f) { (void)m; (void)f; return 0; }
static int iso_rename(struct mount* m, const char* a, const char* b, unsigned fl, fat32_file_t* x, fat32_file_t* y) { (void)m; (void)a; (void)b; (void)fl; (void)x; (void)y; return -30; }
static int iso_setattr(struct mount* m, fat32_file_t* f, const struct mount_attr* a) { (void)m; (void)f; (void)a; return -30; }
static int iso_sync(struct mount* m) { (void)m; return 0; }
static int iso_fsync(struct mount* m, fat32_file_t* f) { (void)m; (void)f; return 0; }
static int iso_statfs(struct mount* m, struct mount_statfs* s) {
    struct isofs* fs = fs_of(m);
    s->type = 0x9660; s->block_size = BLOCK; s->blocks = fs->volume_blocks; s->free = s->available = 0;
    s->files = 0; s->free_files = 0; s->name_max = fs->joliet ? 64 : 255; return 0;
}
static int iso_remount(struct mount* m, uint64_t flags, char* options) { (void)options; return flags & MS_RDONLY ? 0 : (m->flags |= MS_RDONLY, 0); }
static int parse_options(struct isofs* fs, char* options, bool* rock, bool* joliet) {
    fs->uid = mount_current_uid(); fs->gid = mount_current_gid(); fs->fmode = 0555; fs->dmode = 0555;
    char* cursor = options; char key[32], value[64];
    while (mount_option(&cursor, key, sizeof(key), value, sizeof(value))) {
        uint64_t v = 0; const char* end = value; bool number = value[0] && parse_u64(value, &v, &end) && !*end && v <= 0xFFFFFFFFULL;
        bool octal = value[0] != 0; uint64_t o = 0;
        for (const char* p = value; *p; p++) { if (*p < '0' || *p > '7' || o > 07777) { octal = false; break; } o = o * 8 + (uint64_t)(*p - '0'); }
        if (!strcmp(key, "norock")) *rock = false;
        else if (!strcmp(key, "nojoliet")) *joliet = false;
        else if (!strcmp(key, "map")) { if (value[0] == 'o') fs->map_off = true; else if (value[0] != 'n' && value[0] != 'a') return -22; }
        else if (!strcmp(key, "uid") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, false, &k)) return -22; fs->uid = k; }
        else if (!strcmp(key, "gid") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, true, &k)) return -22; fs->gid = k; }
        else if (!strcmp(key, "mode") && octal) { fs->fmode = (uint32_t)(o & 07777); fs->fmode_set = true; }
        else if (!strcmp(key, "dmode") && octal) { fs->dmode = (uint32_t)(o & 07777); fs->dmode_set = true; }
        else if (!strcmp(key, "overriderockperm")) fs->override_perm = true;
        else if (!strcmp(key, "block")) { if (strcmp(value, "2048") && strcmp(value, "1024") && strcmp(value, "512")) return -22; }
        else if (!strcmp(key, "session") || !strcmp(key, "sbsector")) { if (strcmp(value, "0")) { vga_print("[64] iso9660: only the first session is supported\n"); return -22; } }
        else if (!strcmp(key, "unhide") || !strcmp(key, "cruft") || !strcmp(key, "check") || !strcmp(key, "utf8") || !strcmp(key, "iocharset") ||
                 !strcmp(key, "hide") || !strcmp(key, "nocompress") || !strcmp(key, "conv") || !strcmp(key, "showassoc")) {}
        else { vga_print("[64] iso9660: unrecognized mount option "); vga_print(key); vga_print("\n"); return -22; }
    }
    return 0;
}
static void show_options(struct isofs* fs, char* out, bool rock_wanted, bool joliet_wanted) {
    out[0] = 0;
    if (!rock_wanted) strcat(out, "norock,");
    if (!joliet_wanted) strcat(out, "nojoliet,");
    strcat(out, "check=s,map="); strcat(out, fs->map_off ? "o" : "n"); strcat(out, ",blocksize=2048");
    if (fs->uid) { strcat(out, ",uid="); append_u64(out, credentials_visible_id(fs->uid, false)); }
    if (fs->gid) { strcat(out, ",gid="); append_u64(out, credentials_visible_id(fs->gid, true)); }
    if (fs->fmode_set) { strcat(out, ",fmode="); char t[8] = ""; append_u64(t, fs->fmode); strcat(out, t); }
    if (fs->dmode_set) { strcat(out, ",dmode="); char t[8] = ""; append_u64(t, fs->dmode); strcat(out, t); }
    strcat(out, ",iocharset=utf8");
}
static int iso_mount(struct mount* m, char* options) {
    struct isofs* fs = (void*)arena_alloc(sizeof(*fs)); if (!fs) return -12;
    memset(fs, 0, sizeof(*fs)); fs->part = m->part;
    bool rock_wanted = true, joliet_wanted = true;
    int e = parse_options(fs, options, &rock_wanted, &joliet_wanted);
    fs->buffer = e ? NULL : (void*)arena_alloc(BLOCK);
    if (!e && !fs->buffer) e = -12;
    if (e) { arena_free((uint64_t)fs, sizeof(*fs)); return e; }
    uint8_t pvd[BLOCK]; bool have_pvd = false; uint32_t joliet_root = 0, joliet_size = 0;
    for (uint32_t block = 16; block < 16 + 64; block++) {
        uint8_t* b; if (read_block(fs, block, &b)) { e = -5; goto failed; }
        if (memcmp(b + 1, "CD001", 5)) { if (have_pvd) break; e = -22; goto failed; }
        if (b[0] == 255) break;
        if (b[0] == 1 && !have_pvd) { memcpy(pvd, b, BLOCK); have_pvd = true; }
        if (b[0] == 2 && b[88] == '%' && b[89] == '/' && (b[90] == '@' || b[90] == 'C' || b[90] == 'E') && get_le16(b + 128) == BLOCK) {
            joliet_root = get_le32(b + 156 + 2) + b[156 + 1]; joliet_size = get_le32(b + 156 + 10);
        }
    }
    if (!have_pvd || get_le16(pvd + 128) != BLOCK || pvd[156] < 34) { e = -22; goto failed; }
    fs->volume_blocks = get_le32(pvd + 80);
    if ((uint64_t)fs->volume_blocks * 4 > block_part_sectors(fs->part)) fs->volume_blocks = (uint32_t)(block_part_sectors(fs->part) / 4);
    fs->root_extent = get_le32(pvd + 156 + 2) + pvd[156 + 1]; fs->root_size = get_le32(pvd + 156 + 10);
    fs->created = long_time(pvd + 813);
    if (rock_wanted) {   /* SUSP "SP" in the first system use field of the root's "." record */
        uint8_t* b;
        if (!read_block(fs, fs->root_extent, &b) && b[0] >= 34 + 7) {
            unsigned su = 33 + b[32] + (b[32] & 1 ? 0 : 1);
            if (su + 7 <= b[0] && b[su] == 'S' && b[su + 1] == 'P' && b[su + 4] == 0xBE && b[su + 5] == 0xEF) { fs->rock = true; fs->skip = b[su + 6]; }
        }
    }
    if (!fs->rock && joliet_wanted && joliet_root) { fs->joliet = true; fs->root_extent = joliet_root; fs->root_size = joliet_size; }
    if (!fs->root_size || fs->root_extent >= fs->volume_blocks) { e = -22; goto failed; }
    m->flags |= MS_RDONLY; m->sb = fs;
    show_options(fs, m->options, rock_wanted, joliet_wanted);
    char volume[33]; memcpy(volume, pvd + 40, 32); volume[32] = 0; for (int i = 31; i >= 0 && volume[i] == ' '; i--) volume[i] = 0;
    vga_print("[64] iso9660: \""); vga_print(volume); vga_print(fs->rock ? "\" Rock Ridge" : fs->joliet ? "\" Joliet" : "\" plain ISO 9660");
    char n[24] = ""; append_u64(n, fs->volume_blocks); vga_print(" blocks="); vga_print(n); vga_print("\n");
    return 0;
failed:
    if (fs->buffer) arena_free((uint64_t)fs->buffer, BLOCK);
    arena_free((uint64_t)fs, sizeof(*fs));
    if (e == -22) vga_print("[64] iso9660: no ISO 9660 volume\n");
    return e;
}
static int iso_unmount(struct mount* m, bool force) {
    (void)force; struct isofs* fs = fs_of(m); if (!fs) return 0;
    arena_free((uint64_t)fs->buffer, BLOCK); arena_free((uint64_t)fs, sizeof(*fs)); m->sb = NULL; return 0;
}
const struct mount_ops iso9660_mount_ops = {
    .name = "iso9660", .flags = MOUNT_NEEDS_BLOCK | MOUNT_USER_OK,
    .mount = iso_mount, .unmount = iso_unmount, .sync = iso_sync, .statfs = iso_statfs, .remount = iso_remount,
    .lookup = iso_lookup, .refresh = iso_refresh, .read = iso_read, .write = iso_write, .truncate = iso_truncate, .readdir = iso_readdir,
    .create = iso_create, .unlink = iso_unlink, .release = iso_release, .rename = iso_rename, .link = NULL, .setattr = iso_setattr,
    .fsync = iso_fsync, .max_file_size = 0xFFFFFFFFULL * 4,
};
