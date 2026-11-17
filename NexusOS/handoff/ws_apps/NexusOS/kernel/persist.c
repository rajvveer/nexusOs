/* ============================================================================
 * NexusOS — Persistent RAM filesystem (Implementation) — Phase 55
 * ============================================================================
 * See persist.h for the on-disk layout and the save/restore contract.
 *
 * Everything here runs in the cooperative kernel thread (shell idle pump,
 * desktop loop, shell commands, the reboot/shutdown path) — never from an IRQ.
 * I/O is synchronous PIO through the blkdev layer; the payload is streamed
 * through one 4 KB staging buffer in both directions (no big allocations).
 * ============================================================================ */

#include "persist.h"
#include "ramfs.h"
#include "blkdev.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "port.h"
#include "users.h"
#include "pkg.h"       /* pkg_crc32 for the one-shot header CRC */
#include "syslog.h"

extern volatile uint32_t system_ticks;

_Static_assert(PERSIST_LBA > NPFS_MAX_END_LBA, "persist window overlaps the NPFS window");
_Static_assert(PERSIST_MAX_PAYLOAD >= 96u * (4096u + 64u) + 16u * 192u,
               "a slot must hold RAMFS_MAX_FILES full files plus metadata");

/* ============================================================================
 * On-disk structures
 * ============================================================================ */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* PERSIST_MAGIC                                  */
    uint32_t version;        /* PERSIST_VERSION                                */
    uint32_t generation;     /* monotonic; the higher valid slot wins          */
    uint32_t node_count;     /* records in the payload (incl. tombstones)      */
    uint32_t payload_bytes;  /* exact payload length (CRC covers this many)    */
    uint32_t payload_crc;    /* CRC32 (IEEE) over the payload                  */
    uint32_t save_tick;      /* system_ticks at save time (informational)      */
    uint32_t tomb_count;     /* how many of the records are tombstones         */
    uint32_t header_crc;     /* CRC32 over every field above                   */
} persist_hdr_t;
_Static_assert(sizeof(persist_hdr_t) <= 512, "header must fit one sector");

typedef struct __attribute__((packed)) {
    uint32_t parent;         /* index of the parent record; 0xFFFFFFFF = "/"   */
    uint8_t  type;           /* FS_FILE / FS_DIRECTORY / PERSIST_T_TOMB        */
    uint8_t  pad;
    uint16_t mode;           /* rwx bits                                       */
    uint32_t uid;
    uint32_t gid;
    uint32_t size;           /* data bytes that follow (tombstone: path+NUL)   */
    char     name[FS_NAME_MAX];
} persist_rec_t;
_Static_assert(sizeof(persist_rec_t) == 52, "record layout");

#define PERSIST_T_TOMB   0x80u
#define REC_ROOT         0xFFFFFFFFu
#define STAGE_BYTES      4096u
#define STAGE_SECTORS    (STAGE_BYTES / 512u)
#define FLAG_KEEP        0x80u   /* transient per-node mark used during a save */

/* ============================================================================
 * State
 * ============================================================================ */
static int       p_dev        = -1;      /* blkdev index                        */
static bool      p_enabled    = false;   /* window usable                       */
static bool      p_autosave   = true;
static bool      p_dirty      = false;
static bool      p_quiet      = false;   /* restore/export in progress: hooks off */
static bool      p_boot_phase = true;    /* until persist_boot_done()           */
static bool      p_saving     = false;
static uint32_t  p_last_mut   = 0;       /* tick of the last mutation           */
static uint32_t  p_gen        = 0;       /* generation the RAM tree corresponds to */
static int       p_cur_slot   = -1;      /* slot holding p_gen (-1 = none)      */
static uint32_t  p_last_save_tick = 0, p_last_save_us = 0;
static uint32_t  p_last_save_bytes = 0, p_last_save_nodes = 0;
static int       p_boot_slot  = -2;      /* -2 disabled, -1 none found, 0/1 slot */
static uint32_t  p_boot_gen   = 0, p_boot_nodes = 0, p_boot_tombs = 0;
static int       p_fail       = 0;       /* consecutive autosave failures       */
static uint8_t*  p_buf        = NULL;    /* 4 KB staging buffer                 */
static uint32_t* p_crctab     = NULL;    /* 256-entry CRC32 table (1 KB)        */
static char*     p_tomb       = NULL;    /* PERSIST_MAX_TOMBS x FS_PATH_MAX     */
static int       p_ntomb      = 0;
static uint32_t  p_tsc0_lo = 0, p_tsc0_hi = 0, p_ticks0 = 0;  /* TSC calibration */

/* ============================================================================
 * Small output helpers (screen + COM1 so the headless harness can grep)
 * ============================================================================ */
static void pr(const char* s)              { vga_print(s); }
static void prc(const char* s, uint8_t c)  { vga_print_color(s, c); }
static void pr_num(uint32_t n)             { char b[12]; int_to_str((int)n, b); vga_print(b); }

static void ser_putc(char c) { while ((port_byte_in(0x3FD) & 0x20) == 0) { } port_byte_out(0x3F8, c); }
static void ser_puts(const char* s) { while (*s) ser_putc(*s++); }
static void ser_num(uint32_t n) { char b[12]; int_to_str((int)n, b); ser_puts(b); }

static const char* slot_name(int s) { return s == 0 ? "A" : (s == 1 ? "B" : "-"); }
static uint32_t slot_lba(int s) { return PERSIST_LBA + (uint32_t)s * PERSIST_SLOT_SECTORS; }

/* ============================================================================
 * Timing: rdtsc calibrated against the 18.2 Hz PIT tick (no 64-bit division)
 * ============================================================================ */
static inline void rdtsc32(uint32_t* lo, uint32_t* hi) {
    __asm__ volatile("rdtsc" : "=a"(*lo), "=d"(*hi));
}
static uint32_t tsc_lo(void) { uint32_t lo, hi; rdtsc32(&lo, &hi); return lo; }

/* TSC cycles per microsecond, or 0 if we have not observed >= 1 s yet. */
static uint32_t cycles_per_us(void) {
    uint32_t lo, hi; rdtsc32(&lo, &hi);
    uint32_t ticks = system_ticks - p_ticks0;
    if (ticks < 18) return 0;
    /* 64-bit subtraction + constant shift are inlined by GCC (no libgcc). */
    uint64_t now = ((uint64_t)hi << 32) | lo;
    uint64_t then = ((uint64_t)p_tsc0_hi << 32) | p_tsc0_lo;
    uint32_t d16 = (uint32_t)((now - then) >> 16);     /* cycles / 65536 */
    uint32_t cpt16 = d16 / ticks;                       /* per tick / 65536 */
    /* 65536 / 54925.4 us per tick = 1.19318 = 19549 / 16384 */
    return (cpt16 * 19549u) >> 14;
}

/* ============================================================================
 * CRC32 (IEEE 802.3, same polynomial/result as pkg_crc32 and zlib.crc32)
 * ============================================================================ */
#define CRC_INIT      0xFFFFFFFFu
#define CRC_FINAL(c)  (~(c))

static void crc_init_table(void) {
    p_crctab = (uint32_t*)kmalloc(256 * sizeof(uint32_t));
    if (!p_crctab) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
        p_crctab[i] = c;
    }
}

static uint32_t crc_feed(uint32_t crc, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    if (p_crctab) {
        for (uint32_t i = 0; i < len; i++) crc = p_crctab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    } else {
        for (uint32_t i = 0; i < len; i++) {
            crc ^= p[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc;
}

/* ============================================================================
 * Block I/O through the blkdev layer
 * ============================================================================ */
static int rd(uint32_t lba, uint32_t n, void* buf)        { return blkdev_read(p_dev, lba, n, (uint8_t*)buf); }
static int wr(uint32_t lba, uint32_t n, const void* buf)  { return blkdev_write(p_dev, lba, n, (const uint8_t*)buf); }

static int find_dev(const char* name) {
    for (int i = 0; i < blkdev_count(); i++) {
        blkdev_t* d = blkdev_get(i);
        if (d && strcmp(d->name, name) == 0) return i;
    }
    return -1;
}

/* ============================================================================
 * Streaming payload writer (4 KB staging -> whole sectors, running CRC)
 * ============================================================================ */
/* `mem` (optional, heap, cap bytes = whole sectors) collects the entire
 * payload so it goes to the drive in ONE blkdev_write — ata_write_sectors
 * flushes the write cache once per call, and that flush (a host fsync under
 * QEMU) costs far more than the PIO itself. If the heap cannot spare it the
 * writer streams through the 4 KB staging buffer instead (one flush / 4 KB). */
typedef struct { uint32_t lba, fill, total, crc; int err; uint8_t* mem; uint32_t cap; } wstate_t;

static void w_begin(wstate_t* w, uint32_t lba, uint8_t* mem, uint32_t cap) {
    w->lba = lba; w->fill = 0; w->total = 0; w->crc = CRC_INIT; w->err = 0;
    w->mem = mem; w->cap = cap;
}
static void w_put(wstate_t* w, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    if (w->mem) {
        if (w->total + n > w->cap) { w->err = -1; return; }   /* cannot happen: sized by pass 1 */
        memcpy(w->mem + w->total, p, n);
        w->crc = crc_feed(w->crc, p, n);
        w->total += n;
        return;
    }
    while (n) {
        uint32_t room = STAGE_BYTES - w->fill;
        uint32_t k = n < room ? n : room;
        memcpy(p_buf + w->fill, p, k);
        w->crc = crc_feed(w->crc, p, k);
        w->fill += k; w->total += k; p += k; n -= k;
        if (w->fill == STAGE_BYTES) {
            if (!w->err && wr(w->lba, STAGE_SECTORS, p_buf) != 0) w->err = -1;
            w->lba += STAGE_SECTORS; w->fill = 0;
        }
    }
}
static void w_pad4(wstate_t* w) {
    static const uint8_t z[4] = { 0, 0, 0, 0 };
    uint32_t r = w->total & 3u;
    if (r) w_put(w, z, 4u - r);
}
static int w_finish(wstate_t* w) {
    if (w->mem) {
        uint32_t secs = (w->total + 511u) / 512u;
        memset(w->mem + w->total, 0, w->cap - w->total);      /* zero tail: not part of the payload */
        if (secs && !w->err && wr(w->lba, secs, w->mem) != 0) w->err = -1;
        w->lba += secs;
        return w->err;
    }
    if (w->fill) {
        uint32_t secs = (w->fill + 511u) / 512u;
        memset(p_buf + w->fill, 0, secs * 512u - w->fill);   /* zero tail: not part of the payload */
        if (!w->err && wr(w->lba, secs, p_buf) != 0) w->err = -1;
        w->lba += secs; w->fill = 0;
    }
    return w->err;
}

/* ============================================================================
 * Streaming payload reader (bounded to its slot, running CRC)
 * ============================================================================ */
typedef struct { uint32_t lba, end_lba, pos, avail, remaining, crc; int err; } rstate_t;

static void r_begin(rstate_t* r, int slot, uint32_t bytes) {
    r->lba = slot_lba(slot) + 1; r->end_lba = slot_lba(slot) + PERSIST_SLOT_SECTORS;
    r->pos = 0; r->avail = 0; r->remaining = bytes; r->crc = CRC_INIT; r->err = 0;
}
/* Copy n payload bytes into out (NULL = skip). Returns 0 or -1. */
static int r_get(rstate_t* r, void* out, uint32_t n) {
    uint8_t* o = (uint8_t*)out;
    if (r->err || n > r->remaining) { r->err = -1; return -1; }
    while (n) {
        if (r->pos == r->avail) {
            uint32_t secs = STAGE_SECTORS;
            if (r->lba >= r->end_lba) { r->err = -1; return -1; }
            if (r->end_lba - r->lba < secs) secs = r->end_lba - r->lba;
            if (rd(r->lba, secs, p_buf) != 0) { r->err = -1; return -1; }
            r->lba += secs; r->pos = 0; r->avail = secs * 512u;
        }
        uint32_t k = r->avail - r->pos;
        if (k > n) k = n;
        if (o) { memcpy(o, p_buf + r->pos, k); o += k; }
        r->crc = crc_feed(r->crc, p_buf + r->pos, k);
        r->pos += k; n -= k; r->remaining -= k;
    }
    return 0;
}

/* ============================================================================
 * Header helpers
 * ============================================================================ */
static bool hdr_valid(const persist_hdr_t* h) {
    if (h->magic != PERSIST_MAGIC || h->version != PERSIST_VERSION) return false;
    if (h->payload_bytes > PERSIST_MAX_PAYLOAD) return false;
    if (h->node_count > 4096u || h->tomb_count > h->node_count) return false;
    return pkg_crc32(h, sizeof(*h) - sizeof(uint32_t)) == h->header_crc;
}

/* 0 = valid header, -1 = empty/invalid, -2 = I/O error */
static int read_hdr(int slot, persist_hdr_t* h) {
    if (rd(slot_lba(slot), 1, p_buf) != 0) return -2;
    memcpy(h, p_buf, sizeof(*h));
    return hdr_valid(h) ? 0 : -1;
}

/* Stream the whole payload once and compare its CRC with the header's. */
static bool payload_valid(int slot, const persist_hdr_t* h) {
    rstate_t r; r_begin(&r, slot, h->payload_bytes);
    if (r_get(&r, NULL, h->payload_bytes) != 0) return false;
    return CRC_FINAL(r.crc) == h->payload_crc;
}

/* ============================================================================
 * Tombstones (absolute paths of deleted built-ins)
 * ============================================================================ */
static char* tomb_at(int i) { return p_tomb + i * FS_PATH_MAX; }

static void tomb_add(const char* path) {
    if (!p_tomb || !path || !path[0]) return;
    for (int i = 0; i < p_ntomb; i++) if (strcmp(tomb_at(i), path) == 0) return;
    if (p_ntomb >= PERSIST_MAX_TOMBS) return;             /* table full: deletion will not stick */
    strncpy(tomb_at(p_ntomb), path, FS_PATH_MAX - 1);
    tomb_at(p_ntomb)[FS_PATH_MAX - 1] = '\0';
    p_ntomb++;
}
static void tomb_remove(const char* path) {
    if (!p_tomb || !path) return;
    for (int i = 0; i < p_ntomb; i++) {
        if (strcmp(tomb_at(i), path) == 0) {
            for (int j = i; j < p_ntomb - 1; j++) memcpy(tomb_at(j), tomb_at(j + 1), FS_PATH_MAX);
            p_ntomb--;
            return;
        }
    }
}
/* Re-delete a built-in named by a tombstone (file, or empty directory). */
static void tomb_apply(const char* path) {
    fs_node_t* n = ramfs_resolve(path, NULL);
    if (!n || !n->parent) return;
    if (n->type == FS_DIRECTORY) { if (!n->children) ramfs_rmdir(path, NULL); }
    else ramfs_rm_path(path, NULL);
}

/* ============================================================================
 * Mutation hooks
 * ============================================================================ */
static void touched(fs_node_t* n) {
    if (n) n->flags &= (uint8_t)~FS_FLAG_BUILTIN;
    p_dirty = true;
    p_last_mut = system_ticks;
}

void persist_note_create(fs_node_t* n) {
    if (p_quiet || !n) return;
    if (p_boot_phase) { n->flags |= FS_FLAG_BUILTIN; return; }
    char path[FS_PATH_MAX];
    ramfs_abspath(n, path, sizeof(path));
    tomb_remove(path);                 /* re-created a deleted built-in: forget the tombstone */
    touched(n);
}
void persist_note_write(fs_node_t* n) {
    if (p_quiet || !n) return;
    if (p_boot_phase) { n->flags |= FS_FLAG_BUILTIN; return; }
    touched(n);
}
void persist_note_meta(fs_node_t* n) {
    if (p_quiet || !n || p_boot_phase) return;
    touched(n);
}
void persist_note_unlink(fs_node_t* n) {
    if (p_quiet || !n || p_boot_phase) return;
    if (n->flags & FS_FLAG_BUILTIN) {
        char path[FS_PATH_MAX];
        ramfs_abspath(n, path, sizeof(path));
        tomb_add(path);
    }
    p_dirty = true;
    p_last_mut = system_ticks;
}
void persist_note_rename(fs_node_t* n) {
    if (p_quiet || !n || p_boot_phase) return;
    if (n->flags & FS_FLAG_BUILTIN) {
        char path[FS_PATH_MAX];
        ramfs_abspath(n, path, sizeof(path));
        tomb_add(path);                /* the old name must disappear on restore */
    }
    touched(n);
}
void persist_note_users(void) {
    if (p_quiet || p_boot_phase) return;
    p_dirty = true;
    p_last_mut = system_ticks;
}

/* ============================================================================
 * User table <-> /etc/passwd.db (mode 0600, root)
 * ============================================================================ */
#define PW_MAGIC 0x5750584Eu   /* "NXPW" */
typedef struct __attribute__((packed)) { uint32_t magic, count; } pw_hdr_t;
typedef struct __attribute__((packed)) {
    uint32_t uid, gid;
    char     name[USER_NAME_MAX];
    uint32_t salt, h0, h1;
} pw_rec_t;
_Static_assert(sizeof(pw_hdr_t) + USER_MAX * sizeof(pw_rec_t) <= 1024, "passwd.db fits a KB");

/* Serialize the live account table into /etc/passwd.db (quiet: no dirtying). */
static void users_export(void) {
    fs_node_t* root = vfs_get_root();
    fs_node_t* etc = ramfs_resolve("/etc", NULL);
    if (!etc) {
        etc = ramfs_mknode(root, "etc", FS_DIRECTORY);
        if (etc) { etc->uid = UID_ROOT; etc->gid = 0; etc->mode = 0755; }
    }
    if (!etc || etc->type != FS_DIRECTORY) return;
    fs_node_t* pw = vfs_finddir(etc, "passwd.db");
    if (!pw) pw = ramfs_mknode(etc, "passwd.db", FS_FILE);
    if (!pw || pw->type != FS_FILE || !pw->write) return;

    uint8_t buf[sizeof(pw_hdr_t) + USER_MAX * sizeof(pw_rec_t)];
    pw_hdr_t* h = (pw_hdr_t*)buf;
    pw_rec_t* r = (pw_rec_t*)(buf + sizeof(pw_hdr_t));
    user_t* t = users_table();
    uint32_t n = 0;
    for (int i = 0; i < USER_MAX; i++) {
        if (!t[i].used) continue;
        memset(&r[n], 0, sizeof(pw_rec_t));
        r[n].uid = t[i].uid; r[n].gid = t[i].gid;
        strncpy(r[n].name, t[i].name, USER_NAME_MAX - 1);
        r[n].salt = t[i].pw.salt; r[n].h0 = t[i].pw.h0; r[n].h1 = t[i].pw.h1;
        n++;
    }
    h->magic = PW_MAGIC; h->count = n;
    uint32_t len = sizeof(pw_hdr_t) + n * sizeof(pw_rec_t);
    pw->size = 0;
    pw->write(pw, 0, len, buf);
    pw->uid = UID_ROOT; pw->gid = 0; pw->mode = 0600;
    etc->flags &= (uint8_t)~FS_FLAG_BUILTIN;      /* always part of the snapshot */
    pw->flags  &= (uint8_t)~FS_FLAG_BUILTIN;
}

/* Load /etc/passwd.db (if present and sane) into the account table. */
static int users_import_from_fs(void) {
    fs_node_t* pw = ramfs_resolve("/etc/passwd.db", NULL);
    if (!pw || pw->type != FS_FILE || !pw->data || pw->size < sizeof(pw_hdr_t)) return -1;
    const pw_hdr_t* h = (const pw_hdr_t*)pw->data;
    if (h->magic != PW_MAGIC || h->count == 0 || h->count > USER_MAX) return -1;
    if (pw->size < sizeof(pw_hdr_t) + h->count * sizeof(pw_rec_t)) return -1;
    const pw_rec_t* r = (const pw_rec_t*)(pw->data + sizeof(pw_hdr_t));
    user_t tmp[USER_MAX];
    for (uint32_t i = 0; i < h->count; i++) {
        memset(&tmp[i], 0, sizeof(user_t));
        tmp[i].uid = r[i].uid; tmp[i].gid = r[i].gid;
        memcpy(tmp[i].name, r[i].name, USER_NAME_MAX);
        tmp[i].name[USER_NAME_MAX - 1] = '\0';
        tmp[i].pw.salt = r[i].salt; tmp[i].pw.h0 = r[i].h0; tmp[i].pw.h1 = r[i].h1;
        tmp[i].used = true;
    }
    return users_import(tmp, (int)h->count);
}

/* ============================================================================
 * Tree walk — pass 1: mark what to keep, count records and bytes
 * ============================================================================ */
static uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }

static bool mark_keep(fs_node_t* n, uint32_t* nodes, uint32_t* bytes) {
    bool keep = !(n->flags & FS_FLAG_BUILTIN);
    if (n->type == FS_DIRECTORY)
        for (fs_node_t* c = n->children; c; c = c->next)
            if (mark_keep(c, nodes, bytes)) keep = true;     /* chain dir */
    if (keep) {
        n->flags |= FLAG_KEEP;
        (*nodes)++;
        *bytes += sizeof(persist_rec_t);
        if (n->type == FS_FILE && n->data) *bytes += pad4(n->size);
    } else {
        n->flags &= (uint8_t)~FLAG_KEEP;
    }
    return keep;
}
static void clear_keep(fs_node_t* n) {
    n->flags &= (uint8_t)~FLAG_KEEP;
    if (n->type == FS_DIRECTORY)
        for (fs_node_t* c = n->children; c; c = c->next) clear_keep(c);
}

/* ============================================================================
 * Tree walk — pass 2: emit records (parent before child). Siblings are
 * emitted last-to-first because ramfs_mknode PREPENDS, so a restored
 * directory lists in the same order it was saved.
 * ============================================================================ */
static void emit_children_rev(wstate_t* w, fs_node_t* c, uint32_t parent_idx, uint32_t* idx);

static void emit_node(wstate_t* w, fs_node_t* n, uint32_t parent_idx, uint32_t* idx) {
    if (!(n->flags & FLAG_KEEP)) return;
    persist_rec_t r;
    memset(&r, 0, sizeof(r));
    r.parent = parent_idx;
    r.type   = n->type;
    r.mode   = n->mode;
    r.uid    = n->uid;
    r.gid    = n->gid;
    r.size   = (n->type == FS_FILE && n->data) ? n->size : 0;
    strncpy(r.name, n->name, FS_NAME_MAX - 1);
    uint32_t my = (*idx)++;
    w_put(w, &r, sizeof(r));
    if (r.size) { w_put(w, n->data, r.size); w_pad4(w); }
    n->flags &= (uint8_t)~FLAG_KEEP;
    if (n->type == FS_DIRECTORY) emit_children_rev(w, n->children, my, idx);
}
static void emit_children_rev(wstate_t* w, fs_node_t* c, uint32_t parent_idx, uint32_t* idx) {
    if (!c) return;
    emit_children_rev(w, c->next, parent_idx, idx);
    emit_node(w, c, parent_idx, idx);
}

/* ============================================================================
 * Save
 * ============================================================================ */
int persist_save_ex(bool verbose) {
    if (!p_enabled) {
        if (verbose) prc("  Persistence is disabled (no usable disk).\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return -1;
    }
    if (p_saving) return -1;
    p_saving = true;
    uint32_t c0 = tsc_lo();
    fs_node_t* root = vfs_get_root();

    /* 1. Accounts -> /etc/passwd.db (quiet so this does not re-dirty the tree). */
    bool was_quiet = p_quiet;
    p_quiet = true;
    users_export();
    p_quiet = was_quiet;

    /* 2. Pass 1: what to save and how big it is. */
    uint32_t nodes = 0, bytes = 0;
    for (fs_node_t* c = root->children; c; c = c->next) mark_keep(c, &nodes, &bytes);
    for (int i = 0; i < p_ntomb; i++) { nodes++; bytes += sizeof(persist_rec_t) + pad4(strlen(tomb_at(i)) + 1); }
    if (bytes > PERSIST_MAX_PAYLOAD) {
        clear_keep(root);
        if (verbose) prc("  Snapshot too large for a slot.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        p_saving = false;
        return -1;
    }

    /* 3. Pick the target slot: an invalid one, else the OLDER (lower gen). */
    persist_hdr_t ha, hb;
    bool va = read_hdr(0, &ha) == 0, vb = read_hdr(1, &hb) == 0;
    uint32_t ga = va ? ha.generation : 0, gb = vb ? hb.generation : 0;
    int target = !va ? 0 : (!vb ? 1 : (ga <= gb ? 0 : 1));
    uint32_t newgen = (ga > gb ? ga : gb) + 1;
    if (newgen <= p_gen) newgen = p_gen + 1;

    /* 4. Payload first: tombstones, then the tree. One write call if the
     *    heap can hold the whole payload (one cache flush), else streamed. */
    uint32_t cap = (bytes + 511u) & ~511u;
    uint8_t* mem = cap ? (uint8_t*)kmalloc(cap) : NULL;
    wstate_t w;
    w_begin(&w, slot_lba(target) + 1, mem, cap);
    for (int i = 0; i < p_ntomb; i++) {
        persist_rec_t r;
        memset(&r, 0, sizeof(r));
        r.parent = REC_ROOT; r.type = PERSIST_T_TOMB;
        r.size = (uint32_t)strlen(tomb_at(i)) + 1;
        w_put(&w, &r, sizeof(r));
        w_put(&w, tomb_at(i), r.size);
        w_pad4(&w);
    }
    /* Record indexes are absolute payload positions (tombstones occupy
     * 0..p_ntomb-1), so a child's `parent` field can be used directly as an
     * index into the restore map. */
    uint32_t idx = (uint32_t)p_ntomb;
    emit_children_rev(&w, root->children, REC_ROOT, &idx);
    int wrc = w_finish(&w);
    if (mem) kfree(mem);
    if (wrc != 0) {
        clear_keep(root);
        if (verbose) prc("  Disk write failed; snapshot not saved.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        ser_puts("PERSIST: save failed (payload I/O)\n");
        p_saving = false;
        return -1;
    }

    /* 5. Header LAST — the durability point for this generation. */
    persist_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = PERSIST_MAGIC; h.version = PERSIST_VERSION; h.generation = newgen;
    h.node_count = idx; h.payload_bytes = w.total;   /* idx already counts the tombstones */
    h.payload_crc = CRC_FINAL(w.crc); h.save_tick = system_ticks; h.tomb_count = (uint32_t)p_ntomb;
    h.header_crc = pkg_crc32(&h, sizeof(h) - sizeof(uint32_t));
    memset(p_buf, 0, 512);
    memcpy(p_buf, &h, sizeof(h));
    if (wr(slot_lba(target), 1, p_buf) != 0) {
        if (verbose) prc("  Header write failed; snapshot not saved.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        ser_puts("PERSIST: save failed (header I/O)\n");
        p_saving = false;
        return -1;
    }

    /* 6. Bookkeeping. */
    uint32_t dc = tsc_lo() - c0, cpu = cycles_per_us();
    p_last_save_us = cpu ? dc / cpu : 0;
    p_gen = newgen; p_cur_slot = target; p_dirty = false; p_fail = 0;
    p_last_save_tick = system_ticks; p_last_save_bytes = w.total; p_last_save_nodes = h.node_count;

    ser_puts("PERSIST: saved "); ser_num(h.node_count); ser_puts(" nodes "); ser_num(w.total);
    ser_puts(" bytes slot "); ser_puts(slot_name(target)); ser_puts(" gen "); ser_num(newgen);
    ser_puts(" us "); ser_num(p_last_save_us); ser_puts(mem ? " (1 write)\n" : " (streamed)\n");
    if (verbose) {
        char b[12];
        prc("  Persist: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        pr("saved "); pr_num(h.node_count); pr(" nodes, "); pr_num(w.total); pr(" bytes, slot ");
        pr(slot_name(target)); pr(" gen "); pr_num(newgen);
        if (p_ntomb) { pr(" ("); pr_num((uint32_t)p_ntomb); pr(" tombstone"); if (p_ntomb > 1) pr("s"); pr(")"); }
        pr(" in "); int_to_str((int)p_last_save_us, b); pr(b); pr(" us\n");
    } else {
        char m[48];
        strcpy(m, "Persist: autosaved gen "); { char b[12]; int_to_str((int)newgen, b); strcat(m, b); }
        syslog_add(m);
    }
    p_saving = false;
    return (int)h.node_count;
}

void persist_flush(void) {
    if (p_enabled && p_dirty && !p_boot_phase && !p_saving) persist_save_ex(false);
}

void persist_poll(void) {
    if (!p_enabled || !p_autosave || !p_dirty || p_boot_phase || p_saving) return;
    if ((uint32_t)(system_ticks - p_last_mut) < PERSIST_DEBOUNCE_TICKS) return;
    if (persist_save_ex(false) < 0) {
        p_last_mut = system_ticks;            /* back off 2 s, then retry */
        if (++p_fail >= 3) {
            p_autosave = false;
            ser_puts("PERSIST: autosave disabled after 3 failures\n");
            syslog_add("Persist: autosave disabled (disk errors)");
        }
    }
}

/* ============================================================================
 * Restore
 * ============================================================================ */
static int apply_slot(int slot, const persist_hdr_t* h, uint32_t* out_tombs) {
    fs_node_t* root = vfs_get_root();
    uint32_t cnt = h->node_count ? h->node_count : 1;
    fs_node_t** map = (fs_node_t**)kcalloc(cnt, sizeof(fs_node_t*));
    if (!map) return -1;

    bool was_quiet = p_quiet;
    p_quiet = true;
    p_ntomb = 0;                              /* the snapshot's list replaces ours */
    uint32_t tombs = 0;

    rstate_t r;
    r_begin(&r, slot, h->payload_bytes);
    for (uint32_t i = 0; i < h->node_count && !r.err; i++) {
        persist_rec_t rec;
        if (r_get(&r, &rec, sizeof(rec)) != 0) break;
        uint32_t padded = pad4(rec.size);
        map[i] = NULL;

        if (rec.type == PERSIST_T_TOMB) {
            char path[FS_PATH_MAX];
            if (rec.size == 0 || rec.size > FS_PATH_MAX) { r_get(&r, NULL, padded); continue; }
            if (r_get(&r, path, rec.size) != 0) break;
            path[rec.size - 1] = '\0';
            r_get(&r, NULL, padded - rec.size);
            tomb_add(path);
            tomb_apply(path);
            tombs++;
            continue;
        }

        rec.name[FS_NAME_MAX - 1] = '\0';
        fs_node_t* parent = (rec.parent == REC_ROOT) ? root
                          : (rec.parent < i ? map[rec.parent] : NULL);
        if (!parent || parent->type != FS_DIRECTORY || !rec.name[0] ||
            (rec.type != FS_FILE && rec.type != FS_DIRECTORY) || rec.size > RAMFS_MAX_FILE_SIZE) {
            r_get(&r, NULL, padded);
            continue;
        }
        fs_node_t* n = vfs_finddir(parent, rec.name);
        if (n && n->type != rec.type) { r_get(&r, NULL, padded); continue; }
        if (!n) n = ramfs_mknode(parent, rec.name, rec.type);
        if (!n) { r_get(&r, NULL, padded); continue; }       /* table full */
        map[i] = n;

        if (rec.type == FS_FILE) {
            n->size = 0;                                      /* replace content */
            uint32_t off = 0, left = rec.size;
            uint8_t tmp[512];
            while (left && !r.err) {
                uint32_t k = left < sizeof(tmp) ? left : (uint32_t)sizeof(tmp);
                if (r_get(&r, tmp, k) != 0) break;
                if (n->write) n->write(n, off, k, tmp);
                off += k; left -= k;
            }
            r_get(&r, NULL, padded - rec.size);
        }
        n->uid = rec.uid; n->gid = rec.gid; n->mode = (uint16_t)(rec.mode & 0777);
        n->flags &= (uint8_t)~FS_FLAG_BUILTIN;                /* it is user state now */
    }
    kfree(map);
    users_import_from_fs();
    p_quiet = was_quiet;
    if (out_tombs) *out_tombs = tombs;
    return r.err ? -1 : 0;
}

int persist_restore(void) {
    if (!p_enabled) return -1;
    persist_hdr_t ha, hb;
    bool va = read_hdr(0, &ha) == 0 && payload_valid(0, &ha);
    bool vb = read_hdr(1, &hb) == 0 && payload_valid(1, &hb);
    if (!va && !vb) { p_boot_slot = -1; ser_puts("PERSIST: no valid snapshot\n"); return -2; }
    int slot = (va && (!vb || ha.generation >= hb.generation)) ? 0 : 1;
    const persist_hdr_t* h = slot == 0 ? &ha : &hb;

    uint32_t tombs = 0;
    int rc = apply_slot(slot, h, &tombs);
    p_gen = h->generation; p_cur_slot = slot; p_dirty = false; p_last_mut = system_ticks;
    p_boot_slot = slot; p_boot_gen = h->generation; p_boot_nodes = h->node_count; p_boot_tombs = tombs;

    ser_puts("PERSIST: restored gen "); ser_num(h->generation); ser_puts(" slot "); ser_puts(slot_name(slot));
    ser_puts(" nodes "); ser_num(h->node_count); ser_puts(" tombs "); ser_num(tombs);
    ser_puts(rc == 0 ? "\n" : " (truncated)\n");
    return rc;
}

/* ============================================================================
 * Boot sequence
 * ============================================================================ */
static void stamp_builtin(fs_node_t* n) {
    n->flags = (uint8_t)((n->flags | FS_FLAG_BUILTIN) & ~FLAG_KEEP);
    if (n->type == FS_DIRECTORY)
        for (fs_node_t* c = n->children; c; c = c->next) stamp_builtin(c);
}

void persist_mark_builtins(void) {
    fs_node_t* root = vfs_get_root();
    if (root) stamp_builtin(root);
}

void persist_init(void) {
    rdtsc32(&p_tsc0_lo, &p_tsc0_hi);
    p_ticks0 = system_ticks;
    crc_init_table();
    p_tomb = (char*)kcalloc(PERSIST_MAX_TOMBS, FS_PATH_MAX);
    p_ntomb = 0;

    int dev = find_dev("hda");
    if (dev < 0) {
        prc("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        prc("Persistence: no disk\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        ser_puts("PERSIST: no disk\n");
        return;
    }
    blkdev_t* d = blkdev_get(dev);
    if (!d || d->sector_count < PERSIST_END_LBA) {
        prc("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        pr("Persistence: disk too small ("); pr_num(d ? d->sector_count : 0);
        pr(" sectors, need "); pr_num(PERSIST_END_LBA); pr(")\n");
        ser_puts("PERSIST: disk too small\n");
        return;
    }
    p_buf = (uint8_t*)kmalloc(STAGE_BYTES);
    if (!p_buf || !p_tomb) {
        prc("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        pr("Persistence: out of memory\n");
        return;
    }
    p_dev = dev;
    p_enabled = true;

    int rc = persist_restore();
    prc("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr("Persistence: ");
    if (rc == -2) {
        pr("ready on "); pr(d->name); pr(" (window LBA "); pr_num(PERSIST_LBA); pr(", no snapshot yet)\n");
    } else {
        pr("restored gen "); pr_num(p_boot_gen); pr(" from slot "); pr(slot_name(p_boot_slot));
        pr(" ("); pr_num(p_boot_nodes); pr(" nodes");
        if (p_boot_tombs) { pr(", "); pr_num(p_boot_tombs); pr(" deleted built-in"); if (p_boot_tombs > 1) pr("s"); }
        pr(rc == 0 ? ")\n" : ", truncated)\n");
    }
}

void persist_boot_done(void) {
    p_boot_phase = false;
    /* Demo binaries installed by uproc_init() after login may have revived a
     * built-in the user deleted: re-apply the tombstones quietly. */
    bool was_quiet = p_quiet;
    p_quiet = true;
    for (int i = 0; i < p_ntomb; i++) tomb_apply(tomb_at(i));
    p_quiet = was_quiet;
    p_dirty = false;
    p_last_mut = system_ticks;
}

/* ============================================================================
 * Introspection / control
 * ============================================================================ */
bool persist_is_dirty(void)        { return p_dirty; }
bool persist_available(void)       { return p_enabled; }
bool persist_autosave(void)        { return p_autosave; }
void persist_set_autosave(bool on) { p_autosave = on; p_fail = 0; }

int persist_set_device(const char* name) {
    int dev = find_dev(name);
    if (dev < 0) return -1;
    blkdev_t* d = blkdev_get(dev);
    if (!d || d->sector_count < PERSIST_END_LBA) return -2;
    if (!p_buf) { p_buf = (uint8_t*)kmalloc(STAGE_BYTES); if (!p_buf) return -3; }
    if (!p_tomb) { p_tomb = (char*)kcalloc(PERSIST_MAX_TOMBS, FS_PATH_MAX); if (!p_tomb) return -3; }
    p_dev = dev; p_enabled = true; p_fail = 0;
    p_gen = 0; p_cur_slot = -1;                /* generations are per device */
    return 0;
}

static void status_slot(int slot) {
    persist_hdr_t h;
    pr("  Slot "); pr(slot_name(slot)); pr(":     ");
    int rc = read_hdr(slot, &h);
    if (rc == -2) { prc("I/O error\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return; }
    if (rc == -1) { prc("empty / invalid header\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK)); return; }
    pr("gen "); pr_num(h.generation); pr("  "); pr_num(h.node_count); pr(" nodes  ");
    pr_num(h.payload_bytes); pr(" bytes  ");
    if (payload_valid(slot, &h)) prc("crc ok", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    else prc("PAYLOAD CRC BAD", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    if (slot == p_cur_slot) prc("  [current]", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr("\n");
}

void persist_status(void) {
    prc("\n  Persistence - Phase 55 (ramfs snapshot on disk)\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  ==============================================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("  Window:     LBA "); pr_num(PERSIST_LBA); pr(".."); pr_num(PERSIST_END_LBA - 1);
    pr("  (2 slots x "); pr_num(PERSIST_SLOT_SECTORS); pr(" sectors = 1 MiB each)\n");
    pr("  NPFS end:   LBA "); pr_num(NPFS_MAX_END_LBA); pr("  (gap of "); pr_num(PERSIST_LBA - NPFS_MAX_END_LBA);
    pr(" sectors; FAT32 ends at LBA "); pr_num(NPFS_WINDOW_LBA - 1); pr(")\n");
    if (!p_enabled) {
        pr("  State:      "); prc("disabled", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(" - no usable block device (CD/ISO boot?)\n\n");
        return;
    }
    blkdev_t* d = blkdev_get(p_dev);
    pr("  Device:     "); pr(d ? d->name : "?"); pr(" ("); pr_num(d ? d->sector_count : 0); pr(" sectors)\n");
    status_slot(0);
    status_slot(1);
    pr("  In memory:  gen "); pr_num(p_gen);
    if (p_cur_slot >= 0) { pr(" (slot "); pr(slot_name(p_cur_slot)); pr(")"); } else pr(" (nothing saved yet)");
    pr("\n");
    pr("  At boot:    ");
    if (p_boot_slot >= 0) {
        pr("restored gen "); pr_num(p_boot_gen); pr(" from slot "); pr(slot_name(p_boot_slot));
        pr(" ("); pr_num(p_boot_nodes); pr(" nodes, "); pr_num(p_boot_tombs); pr(" tombstones)\n");
    } else pr("no snapshot found (fresh disk)\n");
    pr("  Last save:  ");
    if (p_last_save_tick) {
        pr("tick "); pr_num(p_last_save_tick); pr(" ("); pr_num((system_ticks - p_last_save_tick) / 18);
        pr(" s ago)  "); pr_num(p_last_save_nodes); pr(" nodes  "); pr_num(p_last_save_bytes);
        pr(" bytes  "); pr_num(p_last_save_us); pr(" us\n");
    } else pr("none this session\n");
    pr("  Dirty:      ");
    if (p_dirty) {
        prc("yes", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        if (p_autosave) {
            uint32_t el = system_ticks - p_last_mut;
            pr("  (autosave in "); pr_num(el >= PERSIST_DEBOUNCE_TICKS ? 0 : PERSIST_DEBOUNCE_TICKS - el); pr(" ticks)");
        }
        pr("\n");
    } else prc("no\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr("  Autosave:   "); pr(p_autosave ? "on" : "off"); pr("  (2 s after the last change; also on reboot/shutdown)\n");
    pr("  Tombstones: "); pr_num((uint32_t)p_ntomb);
    for (int i = 0; i < p_ntomb; i++) { pr(i ? ", " : "  "); pr(tomb_at(i)); }
    pr("\n\n");
}
