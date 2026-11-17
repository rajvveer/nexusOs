/* Device mapper (Linux drivers/md/dm-ioctl.c, ioctl interface 4.48.0) with the linear, crypt, zero, error, striped,
 * snapshot-origin, snapshot (dm_snap.inc), verity (dm_verity.inc), integrity (dm_integrity.inc), thin-pool and thin
 * (dm_thin.inc) and raid (raid1, dm_raid.inc) targets, enough for libdevmapper (dmsetup), cryptsetup (LUKS1/LUKS2, plain),
 * veritysetup, integritysetup, thin-provisioning-tools, Debian's lvm2 (linear/striped LVs, snapshots) and lsblk to work unchanged.
 *
 * /dev/mapper/control (CAP_SYS_ADMIN): DM_VERSION, DM_REMOVE_ALL, DM_LIST_DEVICES, DM_DEV_CREATE, DM_DEV_REMOVE
 * (EBUSY while open or mounted; with DM_DEFERRED_REMOVE the device goes at its last close), DM_DEV_RENAME (name, or
 * the uuid once), DM_DEV_SUSPEND (suspend; or resume: the inactive table becomes live), DM_DEV_STATUS, DM_DEV_WAIT
 * (returns at once: these targets raise no table events), DM_TABLE_LOAD, DM_TABLE_CLEAR, DM_TABLE_DEPS,
 * DM_TABLE_STATUS (status, or the table with DM_STATUS_TABLE_FLAG; crypt keys as on Linux: the keyring reference or
 * the key in hex), DM_LIST_VERSIONS, DM_TARGET_MSG (crypt "key wipe" / "key set <key>", "@cancel_deferred_remove"),
 * DM_DEV_SET_GEOMETRY (accepted), DM_DEV_ARM_POLL, DM_GET_TARGET_VERSION.
 * Devices: /dev/dm-N (block 253:N) and /dev/mapper/NAME, /sys/block/dm-N/dm/{name,uuid,suspended}, slaves/ and
 * holders/; create, resume and removal send uevents. The devices a table uses are held (O_EXCL opens and mounts of
 * them fail with EBUSY) and referenced until the table goes; a table that would make a stacking cycle (or a stack
 * deeper than STACK_DEPTH) is refused.
 * Targets: linear "<dev> <offset>"; crypt "<cipher> <key> <iv_offset> <dev> <offset> [<#opts> <opt>...]" with
 * aes-xts-plain64 (= capi:xts(aes)-plain64), aes-xts-plain (32-bit IV), aes-cbc-essiv:sha256, aes-cbc-plain64 or
 * aes-cbc-plain (kcrypto.h; XTS keys 256/384/512 bits, CBC keys 128/192/256 bits) in hex or as
 * ":<size>:logon|user:<description>" from the caller's keyrings (keyring.c), options allow_discards,
 * sector_size:<512..4096>, iv_large_sectors, and same_cpu_crypt, high_priority, submit_from_crypt_cpus,
 * no_read_workqueue, no_write_workqueue (scheduling hints on Linux; crypt work here is synchronous);
 * zero; error. <dev> is a /dev path or major:minor.
 * Crypt I/O: reads decrypt in place, writes encrypt a bounce copy (the caller's buffer is never modified); AES-NI
 * when present, else constant-time software AES (aes.c).
 * Concurrency: every request holds a counted reference on the live table, so a reload (resume) or a removal frees a
 * table only after its last request; key messages need a suspended device with no request in flight.
 * Suspend (dmsetup suspend, cryptsetup luksSuspend): filesystems mounted on the device are frozen at their entry points
 * (mount.c), then new requests are held back until resume (task contexts sleep; contexts that cannot sleep get EIO,
 * write-back keeps its pages dirty); "key wipe" works on a suspended open/mounted volume, and resume is refused
 * (EINVAL) while a crypt key is wiped, as Linux does. The device holding the root filesystem cannot be suspended
 * (EBUSY: nothing could run the resume). A busy device cannot be reloaded with a different logical block size. */
#include "native.h"
#include "../../kernel/vga.h"
#include "storage_util.h"
#include "kcrypto.h"

int keyring_lookup(const char* type, const char* description, uint8_t* out, unsigned capacity);
void mount_freeze_device(struct block_device* dev, bool freeze);   /* mount.c */
struct block_partition* storage_partition(unsigned device);
int device_lookup(const char* path);

#define DM_MAX 255   /* dm-0..dm-254 */
#define TARGETS 32
#define DM_MAJOR 253
#define DM_NAME_LEN 128
#define DM_UUID_LEN 129
#define DM_DATA_OFFSET 305      /* offsetof(struct dm_ioctl, data) */
#define STACK_DEPTH 8
enum { T_LINEAR = 1, T_CRYPT, T_ZERO, T_ERROR, T_STRIPED, T_ORIGIN, T_SNAPSHOT, T_VERITY, T_INTEGRITY, T_THINPOOL, T_THIN, T_RAID };
#define STRIPES 16
struct snapshot;   /* dm_snap.inc */
struct verity;     /* dm_verity.inc */
struct integrity;  /* dm_integrity.inc */
struct pool; struct thin;   /* dm_thin.inc */
struct rset;                /* dm_raid.inc */
enum { DM_READONLY = 1, DM_SUSPEND = 2, DM_PERSISTENT_DEV = 8, DM_STATUS_TABLE = 16, DM_ACTIVE_PRESENT = 32, DM_INACTIVE_PRESENT = 64,
       DM_BUFFER_FULL = 256, DM_SKIP_BDGET = 512, DM_QUERY_INACTIVE = 4096, DM_UEVENT_GENERATED = 8192, DM_UUID = 16384,
       DM_SECURE_DATA = 32768, DM_DATA_OUT = 65536, DM_DEFERRED_REMOVE = 131072 };
enum { O_DISCARDS = 1, O_SAME_CPU = 2, O_HIGH_PRIORITY = 4, O_NO_OFFLOAD = 8, O_NO_READ_WQ = 16, O_NO_WRITE_WQ = 32, O_LARGE_IV = 64 };

struct dm_ioctl {
    uint32_t version[3], data_size, data_start, target_count; int32_t open_count; uint32_t flags, event_nr, padding;
    uint64_t dev; char name[DM_NAME_LEN], uuid[DM_UUID_LEN], data[7];
};
_Static_assert(sizeof(struct dm_ioctl) == 312 && __builtin_offsetof(struct dm_ioctl, data) == DM_DATA_OFFSET, "dm_ioctl ABI");
struct target {
    uint64_t start, length; unsigned type;
    struct block_partition* under; uint64_t offset;
    struct xts_key* key; uint64_t iv_offset; uint32_t sector_size, options; bool iv32, key_wiped; int mode;
    char cipher[64], key_text[192]; unsigned key_bytes;
    struct block_partition* more[STRIPES]; uint64_t more_offset[STRIPES]; unsigned extra, stripes; uint64_t chunk;   /* striped: stripes 1..; snapshot: more[0] = COW */
    struct snapshot* snap; struct verity* verity;   /* verity: under = data, more[0] = hash */
    struct integrity* integ;
    struct pool* pool; struct thin* thin;   /* thin-pool: under = data, more[0] = metadata; thin: under = the pool device */
    struct rset* rset;                      /* raid: the members' data and _rmeta devices in under/more[] */
};
struct table { struct target t[TARGETS]; unsigned count, users; uint64_t sectors; uint32_t block_size, discard_granularity; bool readonly, retired; };
struct mapped {
    struct block_device block;      /* first */
    unsigned number; char name[DM_NAME_LEN], uuid[DM_UUID_LEN];
    struct table* live; struct table* inactive;
    bool suspended, deferred_remove, gone; uint32_t event_nr;
};
static struct mapped* devices[DM_MAX];
static volatile int dm_lock;   /* leaf: the live-table pointers, table users, crypt key state */
static int striped_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);   /* dm_snap.inc */
static int origin_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);
static int snapshot_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);
static int parse_striped(struct target* g, char* params);
static int parse_snapshot(struct target* g, char* params, bool origin_only);
static void snap_free(struct snapshot* s);
static void snap_status(const struct target* g, bool table, char* text);
static struct block_partition* stripe_dev(const struct target* g, unsigned s, uint64_t* offset);
static int verity_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);   /* dm_verity.inc */
static int parse_verity(struct target* g, char* params, bool readonly);
static void verity_free(struct verity* v);
static void verity_status(const struct target* g, bool table, char* text);
static uint32_t verity_block_size(const struct verity* v);
static int integrity_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);   /* dm_integrity.inc */
static int parse_integrity(struct target* g, char* params, bool readonly);
static void integrity_free(struct integrity* x);
static void integrity_status(const struct target* g, bool table, char* text);
static uint32_t integrity_block_size(const struct integrity* x);
static int thin_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);   /* dm_thin.inc */
static int thinpool_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);
static int parse_thinpool(struct target* g, char* params, bool readonly);
static int parse_thin(struct target* g, char* params);
static void thin_target_free(struct target* g);
static void thin_status(const struct target* g, bool table, char* text);
static void thin_flush(struct target* g);
static int thinpool_message(struct target* g, char* text);
static void thinpool_resume(struct target* g);
static int raid1_dm_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write);   /* dm_raid.inc */
static int parse_raid(struct target* g, char* params);
static void raid_free(struct rset* r);
static void raid_status(const struct target* g, bool table, char* text);
static int raid_message(struct target* g, char* text);
static void raid_presuspend(struct target* g);
int dm_kernel_create(const char* name, const char* uuid, const char* const* lines, unsigned count, bool readonly, struct block_device** out);
static unsigned target_devices(const struct target* g, struct block_partition** out) {   /* every device a target uses */
    unsigned n = 0;
    if (g->under) out[n++] = g->under;
    for (unsigned i = 0; i < g->extra; i++) if (g->more[i]) out[n++] = g->more[i];
    return n;
}

/* ---- tables ---- */
static void free_table(struct table* t) {
    if (!t) return;
    for (unsigned i = 0; i < t->count; i++) {
        struct target* g = &t->t[i];
        if (g->key) { xts_wipe(g->key); arena_free((uint64_t)g->key, sizeof(struct xts_key)); }
        struct block_partition* used[STRIPES + 1]; unsigned n = target_devices(g, used);
        for (unsigned k = 0; k < n; k++) { if (used[k]->holders) used[k]->holders--; block_put(used[k]->dev); }
        snap_free(g->snap); verity_free(g->verity); integrity_free(g->integ); thin_target_free(g); raid_free(g->rset);
    }
    volatile uint8_t* p = (volatile uint8_t*)t; for (uint64_t i = 0; i < sizeof(*t); i++) p[i] = 0;   /* key text */
    arena_free((uint64_t)t, sizeof(*t));
}
static struct table* table_get(struct mapped* d) {   /* the live table, counted, for one request */
    spin_acquire(&dm_lock);
    struct table* t = d->gone ? NULL : d->live;
    if (t) t->users++;
    spin_release(&dm_lock);
    return t;
}
static void table_put(struct table* t) {
    spin_acquire(&dm_lock);
    bool last = !--t->users && t->retired;
    spin_release(&dm_lock);
    if (last) free_table(t);
}
static void table_retire(struct table* t) {   /* unlinked from its device: freed now, or by its last request */
    if (!t) return;
    spin_acquire(&dm_lock);
    t->retired = true; bool idle = !t->users;
    spin_release(&dm_lock);
    if (idle) free_table(t);
}

/* ---- I/O ---- */
static int under_io(struct target* g, uint64_t sector, uint32_t count, uint8_t* data, bool write) {
    return write ? block_part_write(g->under, sector, count, data) : block_part_read(g->under, sector, count, data);
}
static uint64_t crypt_iv(const struct target* g, uint64_t relative) {   /* dm-crypt: (sector + iv_offset) [>> sector_shift] */
    uint64_t iv = relative + g->iv_offset;
    if (g->options & O_LARGE_IV) iv /= g->sector_size / 512;
    return g->iv32 ? (iv & 0xFFFFFFFFULL) : iv;
}
static int target_io(struct target* g, uint64_t sector, uint32_t count, uint8_t* data, bool write) {
    uint64_t relative = sector - g->start, at = g->offset + relative;
    switch (g->type) {
    case T_ZERO: if (!write) memset(data, 0, (uint64_t)count * 512); return 0;
    case T_ERROR: return -1;
    case T_LINEAR: return under_io(g, at, count, data, write);
    case T_STRIPED: return striped_io(g, relative, count, data, write);
    case T_ORIGIN: return origin_io(g, relative, count, data, write);
    case T_SNAPSHOT: return snapshot_io(g, relative, count, data, write);
    case T_VERITY: return verity_io(g, relative, count, data, write);
    case T_INTEGRITY: return integrity_io(g, relative, count, data, write);
    case T_THINPOOL: return thinpool_io(g, relative, count, data, write);
    case T_THIN: return thin_io(g, relative, count, data, write);
    case T_RAID: return raid1_dm_io(g, relative, count, data, write);
    }
    if (__atomic_load_n(&g->key_wiped, __ATOMIC_ACQUIRE)) return -1;
    uint32_t per = g->sector_size / 512;   /* whole data units (the device's logical block size guarantees that) */
    if (relative % per || count % per) return -1;
    if (!write) {
        if (under_io(g, at, count, data, false) < 0) return -1;
        for (uint32_t i = 0; i < count; i += per) xts_crypt(g->key, data + (uint64_t)i * 512, g->sector_size, crypt_iv(g, relative + i), true);
        return 0;
    }
    uint8_t* bounce = (void*)arena_alloc(65536); if (!bounce) return -1;
    int result = 0;
    for (uint32_t done = 0; done < count && !result; ) {
        uint32_t n = count - done > 128 ? 128 : count - done;   /* 128 is a multiple of every data unit */
        memcpy(bounce, data + (uint64_t)done * 512, (uint64_t)n * 512);
        for (uint32_t i = 0; i < n; i += per) xts_crypt(g->key, bounce + (uint64_t)i * 512, g->sector_size, crypt_iv(g, relative + done + i), false);
        if (under_io(g, at + done, n, bounce, true) < 0) result = -1;
        done += n;
    }
    volatile uint8_t* wipe = bounce; for (uint32_t i = 0; i < 65536; i++) wipe[i] = 0;
    arena_free((uint64_t)bounce, 65536);
    return result;
}
static struct target* target_at(struct table* t, uint64_t sector) {
    for (unsigned i = 0; i < t->count; i++) if (sector >= t->t[i].start && sector - t->t[i].start < t->t[i].length) return &t->t[i];
    return NULL;
}
/* A suspended device holds new requests until it resumes (Linux queues them). Only task contexts can wait; others fail
 * (write-back keeps its dirty pages and retries). Filesystems on the device are frozen at entry (mount.c), so a request
 * that waits here never leaves a filesystem operation half done. */
void kernel_io_sleep(void* channel, uint64_t deadline);
unsigned spin_held_count(void);
static bool dm_may_wait(void) { return cpu_self()->task && kernel_lock_held() && spin_held_count() == (cpu_self()->vm_held ? 1u : 0u); }
static bool held_back(struct mapped* d) {
    while (__atomic_load_n(&d->suspended, __ATOMIC_ACQUIRE) && !d->gone) {
        if (!dm_may_wait()) return true;
        kernel_io_sleep(&d->suspended, native_clock_ns() + 20000000ULL);
    }
    return false;
}
static int dm_rw(struct block_device* b, uint64_t lba, uint32_t count, void* buffer, bool write) {
    uint32_t block = b->block_size;
    if (held_back(b->driver)) return -1;
    struct table* t = table_get(b->driver);
    if (!t) return -1;
    int result = (write && t->readonly) || t->block_size != block ? -1 : 0;
    uint64_t sector = lba * (block / 512), sectors = (uint64_t)count * (block / 512); uint8_t* data = buffer;
    while (!result && sectors) {
        struct target* g = target_at(t, sector);
        if (!g) { result = -1; break; }
        uint64_t n = g->start + g->length - sector; if (n > sectors) n = sectors; if (n > 0x10000) n = 0x10000;
        if (target_io(g, sector, (uint32_t)n, data, write) < 0) result = -1;
        sector += n; sectors -= n; data += n * 512;
    }
    table_put(t);
    return result;
}
static int dm_read(struct block_device* b, uint64_t lba, uint32_t count, void* buffer) { return dm_rw(b, lba, count, buffer, false); }
static int dm_write(struct block_device* b, uint64_t lba, uint32_t count, const void* buffer) { return dm_rw(b, lba, count, (void*)buffer, true); }
static int dm_flush(struct block_device* b) {
    struct table* t = table_get(b->driver); int result = 0;
    if (!t) return -1;
    for (unsigned i = 0; i < t->count; i++) {
        struct block_partition* used[STRIPES + 1]; unsigned n = target_devices(&t->t[i], used);
        for (unsigned k = 0; k < n; k++) if (block_flush(used[k]->dev) < 0) result = -1;
        if (t->t[i].type == T_THIN || t->t[i].type == T_THINPOOL) thin_flush(&t->t[i]);
    }
    table_put(t);
    return result;
}
static const struct block_ops dm_ops = {dm_read, dm_write, dm_flush};
static int dm_discard(struct block_device* b, uint64_t lba, uint64_t count) {
    uint32_t scale = b->block_size / 512;
    if (held_back(b->driver)) return -5;
    struct table* t = table_get(b->driver);
    if (!t) return -5;
    int result = t->discard_granularity ? 0 : -95;
    uint64_t sector = lba * scale, sectors = count * scale;
    while (!result && sectors) {
        struct target* g = target_at(t, sector);
        if (!g) { result = -22; break; }
        uint64_t n = g->start + g->length - sector; if (n > sectors) n = sectors;
        if (g->type == T_ERROR) result = -5;
        else if (g->under) {   /* linear, or crypt with allow_discards (the table's granularity says both can) */
            struct block_device* u = g->under->dev; uint32_t per = u->block_size / 512;
            uint64_t first = g->under->start * per + g->offset + (sector - g->start), last = first + n;
            first = (first + per - 1) / per; last /= per;   /* whole device blocks inside the range */
            if (last > first && u->extra->discard(u, first, last - first) < 0) result = -5;
        }
        sector += n; sectors -= n;
    }
    table_put(t);
    return result;
}
static int remove_device(struct mapped* d, bool deferred);
static void dm_idle(struct block_device* b) {   /* the last opener went away */
    struct mapped* d = b->driver;
    if (d->deferred_remove && !d->gone) remove_device(d, false);
}
static void dm_release(struct block_device* b) {
    struct mapped* d = b->driver;
    spin_acquire(&dm_lock);
    struct table* live = d->live; struct table* inactive = d->inactive; d->live = d->inactive = NULL;
    spin_release(&dm_lock);
    table_retire(live); table_retire(inactive);
    memset(d, 0, sizeof(*d)); arena_free((uint64_t)d, sizeof(*d));
}
static const struct block_extra_ops dm_extra = {.discard = dm_discard, .release = dm_release, .idle = dm_idle};
static struct mapped* mapped_of(struct block_device* dev) { return dev && dev->ops == &dm_ops ? dev->driver : NULL; }

/* ---- table parsing ---- */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */
static char* word(char** cursor) {   /* the next space-separated word, NUL-terminated in place */
    char* p = *cursor; while (*p == ' ' || *p == '\t') p++;
    if (!*p) return NULL;
    char* start = p; while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    *cursor = p; return start;
}
static struct block_partition* device_of(const char* text) {
    struct block_partition* part = NULL;
    const char* colon = s_strchr(text, ':');
    if (colon && text[0] != '/') {   /* major:minor */
        uint64_t major, minor; const char* end;
        if (!parse_u64(text, &major, &end) || *end != ':' || !parse_u64(colon + 1, &minor, &end) || *end) return NULL;
        for (unsigned i = 0; i < block_device_count() && !part; i++) {
            struct block_device* d = block_device_get(i); if (!d || d->removed) continue;
            struct block_partition* w = block_whole(d);
            if (w && w->major == major && w->minor == minor) part = w;
        }
        for (unsigned i = 0; i < block_partition_count() && !part; i++) {
            struct block_partition* p = (struct block_partition*)block_partition_get(i);
            if (p && p->number && !p->dev->removed && p->major == major && p->minor == minor) part = p;
        }
    } else {
        int n = device_lookup(text);
        if (n >= 0) part = storage_partition((unsigned)n);
    }
    return part && !part->dev->removed ? part : NULL;
}
static int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* A crypt key: hex digits, or ":<size>:<logon|user>:<description>" looked up in the caller's keyrings. Returns its
 * length (material[0..63]) and its canonical text for the table (lower-case hex as Linux prints it). */
static int parse_key(char* key, uint8_t* material, char* text, unsigned capacity) {
    text[0] = 0;
    if (key[0] == ':') {
        char* p = key + 1; const char* size_text = p;
        while (*p && *p != ':') p++;
        if (*p != ':') return -22;
        *p++ = 0; char* type = p;
        while (*p && *p != ':') p++;
        if (*p != ':') return -22;
        *p++ = 0;
        uint64_t size; const char* end;
        if (!parse_u64(size_text, &size, &end) || *end || !size || size > 64 || !*p) return -22;
        if (strcmp(type, "logon") && strcmp(type, "user")) return -22;
        if (strlen(type) + strlen(p) + 8 >= capacity) return -22;
        int bytes = keyring_lookup(type, p, material, 64);
        if (bytes < 0) return bytes;
        if ((uint64_t)bytes != size) { memset(material, 0, 64); return -22; }
        strcpy(text, ":"); append_u64(text, size); strcat(text, ":"); strcat(text, type); strcat(text, ":"); strcat(text, p);
        return bytes;
    }
    unsigned n = strlen(key);
    if (!n || n % 2 || n / 2 > 64 || n >= capacity) return -22;
    for (unsigned i = 0; i < n; i++) if (hex_value(key[i]) < 0) return -22;
    for (unsigned i = 0; i < n / 2; i++) material[i] = (uint8_t)(hex_value(key[2 * i]) << 4 | hex_value(key[2 * i + 1]));
    for (unsigned i = 0; i < n; i++) text[i] = "0123456789abcdef"[hex_value(key[i])];
    text[n] = 0;
    return (int)(n / 2);
}
static int parse_crypt(struct target* g, char* params) {
    char* cursor = params;
    char *cipher = word(&cursor), *key = word(&cursor), *iv = word(&cursor), *device = word(&cursor), *offset = word(&cursor);
    if (!cipher || !key || !iv || !device || !offset || strlen(cipher) >= sizeof(g->cipher)) return -22;
    g->mode = cipher_mode(cipher);   /* aes-xts-plain64/-plain, aes-cbc-essiv:sha256, aes-cbc-plain64/-plain (aes.c) */
    if (!g->mode) return -22;
    g->iv32 = g->mode == CIPHER_XTS_PLAIN || g->mode == CIPHER_CBC_PLAIN;
    strcpy(g->cipher, cipher);
    const char* end;
    if (!parse_u64(iv, &g->iv_offset, &end) || *end || !parse_u64(offset, &g->offset, &end) || *end) return -22;
    g->sector_size = 512;
    char* count_text = word(&cursor);
    if (count_text) {
        uint64_t count; if (!parse_u64(count_text, &count, &end) || *end || count > 16) return -22;
        for (uint64_t i = 0; i < count; i++) {
            char* o = word(&cursor); if (!o) return -22;
            static const char* const flags[6] = {"allow_discards", "same_cpu_crypt", "high_priority", "submit_from_crypt_cpus", "no_read_workqueue", "no_write_workqueue"};
            bool known = false;
            for (unsigned f = 0; f < 6; f++) if (!strcmp(o, flags[f])) { g->options |= 1u << f; known = true; }
            if (known) continue;
            if (!strcmp(o, "iv_large_sectors")) g->options |= O_LARGE_IV;
            else if (!strncmp(o, "sector_size:", 12)) {
                uint64_t v; if (!parse_u64(o + 12, &v, &end) || *end || v < 512 || v > 4096 || (v & (v - 1))) return -22;
                g->sector_size = (uint32_t)v;
            } else return -22;
        }
    }
    if (word(&cursor)) return -22;
    uint32_t per = g->sector_size / 512;
    if (g->start % per || g->length % per || g->offset % per) return -22;   /* Linux: size/offset not multiple of sector_size */
    uint8_t material[64];
    int bytes = parse_key(key, material, g->key_text, sizeof(g->key_text));
    memset(key, '0', strlen(key));
    if (bytes < 0) return bytes;
    g->key = (void*)arena_alloc(sizeof(struct xts_key));
    int e = g->key ? cipher_set_key(g->key, g->mode, material, (unsigned)bytes) : -12;
    memset(material, 0, sizeof(material));
    if (e) return e;
    g->key_bytes = (unsigned)bytes;
    g->under = device_of(device);
    return g->under ? 0 : -6;
}
static bool reaches(struct block_device* dev, const struct mapped* d, unsigned depth) {   /* would d end up below itself? */
    if (dev == &d->block) return true;
    struct mapped* m = mapped_of(dev); if (!m) return false;
    if (depth >= STACK_DEPTH) return true;
    struct table* tables[2] = {m->live, m->inactive};
    for (unsigned k = 0; k < 2; k++) if (tables[k])
        for (unsigned i = 0; i < tables[k]->count; i++) {
            struct block_partition* used[STRIPES + 1]; unsigned n = target_devices(&tables[k]->t[i], used);
            for (unsigned u = 0; u < n; u++) if (reaches(used[u]->dev, d, depth + 1)) return true;
        }
    return false;
}
static int parse_table(struct mapped* d, const struct dm_ioctl* io, const uint8_t* data, uint32_t size, uint32_t first, struct table** out) {
    if (!io->target_count || io->target_count > TARGETS) return -22;
    struct table* t = (void*)arena_alloc(sizeof(*t)); if (!t) return -12;
    memset(t, 0, sizeof(*t));
    t->readonly = (io->flags & DM_READONLY) != 0;
    uint32_t at = first, minimum = sizeof(struct dm_ioctl); int e = 0;
    for (uint32_t n = 0; n < io->target_count && !e; n++) {
        if (at < minimum || at > size || size - at < 40) { e = -22; break; }   /* specs follow each other (Linux next_target) */
        const uint8_t* spec = data + at;
        struct target* g = &t->t[t->count];
        g->start = get_le64(spec); g->length = get_le64(spec + 8); uint32_t next = get_le32(spec + 20);
        char type[17]; memcpy(type, spec + 24, 16); type[16] = 0;
        const char* text = (const char*)spec + 40; uint32_t room = size - at - 40, length = 0;
        while (length < room && text[length]) length++;
        char params[1024];
        if (length == room || length >= sizeof(params)) { e = -22; break; }   /* unterminated, or too long */
        memcpy(params, text, length + 1);
        char shown[100] = ""; if (strcmp(type, "crypt")) { unsigned k = length < sizeof(shown) - 1 ? length : sizeof(shown) - 1; memcpy(shown, text, k); shown[k] = 0; }   /* keys never logged */
        minimum = at + 40 + length + 1;
        if (g->start != t->sectors || !g->length || g->length > ~0ULL / 2 - g->start) { e = -22; break; }   /* contiguous from 0 */
        if (!strcmp(type, "linear")) {
            char* cursor = params; char *device = word(&cursor), *offset = word(&cursor); const char* end;
            g->type = T_LINEAR;
            if (!device || !offset || word(&cursor) || !parse_u64(offset, &g->offset, &end) || *end) e = -22;
            else if (!(g->under = device_of(device))) e = -6;
        } else if (!strcmp(type, "crypt")) { g->type = T_CRYPT; e = parse_crypt(g, params); }
        else if (!strcmp(type, "zero")) { char* cursor = params; g->type = T_ZERO; if (word(&cursor)) e = -22; }
        else if (!strcmp(type, "striped")) { g->type = T_STRIPED; e = parse_striped(g, params); }
        else if (!strcmp(type, "snapshot-origin")) { g->type = T_ORIGIN; e = parse_snapshot(g, params, true); }
        else if (!strcmp(type, "snapshot")) { g->type = T_SNAPSHOT; e = parse_snapshot(g, params, false); }
        else if (!strcmp(type, "verity")) { g->type = T_VERITY; e = parse_verity(g, params, t->readonly); }
        else if (!strcmp(type, "integrity")) { g->type = T_INTEGRITY; e = parse_integrity(g, params, t->readonly); }
        else if (!strcmp(type, "thin-pool")) { g->type = T_THINPOOL; e = parse_thinpool(g, params, t->readonly); }
        else if (!strcmp(type, "thin")) { g->type = T_THIN; e = parse_thin(g, params); }
        else if (!strcmp(type, "raid")) { g->type = T_RAID; e = parse_raid(g, params); }
        else if (!strcmp(type, "error")) g->type = T_ERROR;
        else e = -22;
        volatile char* wipe = params; for (unsigned i = 0; i < sizeof(params); i++) wipe[i] = 0;
        struct block_partition* used[STRIPES + 1]; unsigned uses = e ? 0 : target_devices(g, used);
        for (unsigned k = 0; k < uses && !e; k++) {
            uint64_t room_sectors = block_part_sectors(used[k]), offset = 0, length = g->length;
            struct mapped* below = used[k]->number ? NULL : mapped_of(used[k]->dev);   /* Linux checks sizes at resume: a reload */
            if (below && below->inactive && below->inactive->sectors > room_sectors) room_sectors = below->inactive->sectors;   /* below may grow first */
            if (g->type == T_STRIPED) { length = g->length / g->stripes; stripe_dev(g, k, &offset); }
            else if (((g->type == T_SNAPSHOT || g->type == T_VERITY || g->type == T_THINPOOL) && k) || g->type == T_THIN || g->type == T_RAID) length = 0;   /* COW, hash, pool metadata: their targets check; thin: over-provisioned */
            else if (g->type != T_ORIGIN && g->type != T_SNAPSHOT) offset = g->offset;
            if (reaches(used[k]->dev, d, 0)) e = -22;                                              /* itself, a cycle, too deep */
            else if (room_sectors && (offset > room_sectors || length > room_sectors - offset)) {   /* beyond its device (0: not live yet, as Linux) */
                char n[96] = " beyond "; strcat(n, used[k]->linux_name); strcat(n, ": sectors "); append_u64(n, room_sectors);
                vga_print("[64] DM"); vga_print(n); vga_print("\n"); e = -22;
            }
            else if (!t->readonly && used[k]->dev->read_only) e = -13;
        }
        if (e) {   /* this target is not counted yet: drop what it allocated */
            char why[24] = ""; append_u64(why, (uint64_t)-e);   /* as Linux logs a refused table */
            vga_print("[64] DM "); vga_print(d->name); vga_print(": table refused, target "); vga_print(type); vga_print(" errno "); vga_print(why);
            vga_print(" params '"); vga_print(shown); vga_print("'\n");
            if (g->key) { xts_wipe(g->key); arena_free((uint64_t)g->key, sizeof(struct xts_key)); g->key = NULL; }
            snap_free(g->snap); g->snap = NULL; verity_free(g->verity); g->verity = NULL; integrity_free(g->integ); g->integ = NULL; thin_target_free(g); raid_free(g->rset); g->rset = NULL;
            g->under = NULL; g->extra = 0; memset(g->key_text, 0, sizeof(g->key_text));
            break;
        }
        for (unsigned k = 0; k < uses; k++) { used[k]->holders++; block_get(used[k]->dev); }
        t->sectors += g->length; t->count++;
        at += next;
    }
    if (!e) {   /* logical block size: the largest crypt data unit or underlying block size; every target aligned to it */
        uint32_t block = 512; bool discards = true; uint32_t granularity = 0;
        for (unsigned i = 0; i < t->count; i++) {
            struct target* g = &t->t[i];
            if (g->type == T_CRYPT && g->sector_size > block) block = g->sector_size;
            if (g->type == T_VERITY && verity_block_size(g->verity) > block) block = verity_block_size(g->verity);   /* Linux verity_io_hints */
            if (g->type == T_INTEGRITY && integrity_block_size(g->integ) > block) block = integrity_block_size(g->integ);
            struct block_partition* used[STRIPES + 1]; unsigned uses = target_devices(g, used);
            for (unsigned k = 0; k < uses; k++) if (used[k]->dev->block_size > block) block = used[k]->dev->block_size;
            if (g->type == T_ERROR || g->type >= T_STRIPED || (g->type == T_CRYPT && !(g->options & O_DISCARDS))) discards = false;
            else if (g->under) {
                struct block_device* u = g->under->dev;
                if (!u->discard_granularity || !u->extra || !u->extra->discard) discards = false;
                else if (u->discard_granularity > granularity) granularity = u->discard_granularity;
            }
        }
        for (unsigned i = 0; i < t->count; i++) if (t->t[i].start % (block / 512) || t->t[i].length % (block / 512)) e = -22;
        t->block_size = block; t->discard_granularity = discards ? (granularity ? granularity : block) : 0;
    }
    if (e) { free_table(t); return e; }
    *out = t; return 0;
}

/* ---- sysfs and holders (blkdev_node.c) ---- */
bool dm_sysfs_attr(struct block_device* dev, unsigned index, char* text) {
    struct mapped* d = mapped_of(dev); if (!d) return false;
    switch (index) { case 0: strcat(text, d->name); return true; case 1: strcat(text, d->uuid); return true;
                     case 2: strcat(text, d->suspended ? "1" : "0"); return true; case 3: strcat(text, "1"); return true; }
    return false;
}
unsigned dm_sysfs_slaves(struct block_device* dev, struct block_device** out, unsigned capacity) {
    struct mapped* d = mapped_of(dev); unsigned n = 0;
    if (d && d->live) for (unsigned i = 0; i < d->live->count && n < capacity; i++) {
        struct block_partition* used[STRIPES + 1]; unsigned uses = target_devices(&d->live->t[i], used);
        for (unsigned j = 0; j < uses && n < capacity; j++) {
            bool seen = false; for (unsigned k = 0; k < n; k++) if (out[k] == used[j]->dev) seen = true;
            if (!seen) out[n++] = used[j]->dev;
        }
    }
    return n;
}
unsigned storage_holders(struct block_device* dev, struct block_device** out, unsigned capacity) {
    unsigned n = 0;
    for (unsigned k = 0; k < DM_MAX && n < capacity; k++) {
        struct mapped* d = devices[k]; if (!d || !d->live) continue;
        bool uses = false;
        for (unsigned i = 0; i < d->live->count && !uses; i++) {
            struct block_partition* used[STRIPES + 1]; unsigned count = target_devices(&d->live->t[i], used);
            for (unsigned j = 0; j < count; j++) if (used[j]->dev == dev) uses = true;
        }
        if (uses) out[n++] = &d->block;
    }
    return n;
}

/* ---- control ---- */
static struct mapped* lookup(const struct dm_ioctl* io) {   /* Linux find_device: uuid, else name, else dev */
    for (unsigned i = 0; i < DM_MAX; i++) {
        struct mapped* d = devices[i]; if (!d || d->gone) continue;
        if (io->uuid[0]) { if (!strcmp(io->uuid, d->uuid)) return d; }
        else if (io->name[0]) { if (!strcmp(io->name, d->name)) return d; }
        else if (io->dev && io->dev == storage_linux_dev(DM_MAJOR, d->number)) return d;
    }
    return NULL;
}
static void info(struct mapped* d, struct dm_ioctl* io) {
    io->dev = storage_linux_dev(DM_MAJOR, d->number);
    memset(io->name, 0, DM_NAME_LEN); memset(io->uuid, 0, DM_UUID_LEN);
    strcpy(io->name, d->name); strcpy(io->uuid, d->uuid);
    io->open_count = d->block.references > 1 ? (int32_t)d->block.references - 1 : 0;
    io->event_nr = d->event_nr;
    io->flags &= ~(DM_SUSPEND | DM_READONLY | DM_ACTIVE_PRESENT | DM_INACTIVE_PRESENT | DM_DEFERRED_REMOVE);
    if (d->suspended) io->flags |= DM_SUSPEND;
    if (d->live) { io->flags |= DM_ACTIVE_PRESENT; if (d->live->readonly) io->flags |= DM_READONLY; }
    if (d->inactive) io->flags |= DM_INACTIVE_PRESENT;
    if (d->deferred_remove) io->flags |= DM_DEFERRED_REMOVE;
    io->target_count = d->live ? d->live->count : 0;
}
static bool name_ok(const char* name) {
    if (!name[0] || strlen(name) >= DM_NAME_LEN || !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "control")) return false;
    for (const char* p = name; *p; p++) if (*p == '/' || (unsigned char)*p < 0x20) return false;
    return true;
}
static void announce(struct mapped* d, const char* action) { if (block_event_hook) block_event_hook(&d->block, NULL, action); }
static int create(struct dm_ioctl* io) {
    if (!name_ok(io->name)) return -22;
    for (unsigned i = 0; i < DM_MAX; i++)
        if (devices[i] && !devices[i]->gone && (!strcmp(devices[i]->name, io->name) || (io->uuid[0] && !strcmp(devices[i]->uuid, io->uuid)))) return -16;
    unsigned number = 0; while (number < DM_MAX && devices[number]) number++;
    if (number == DM_MAX) return -12;
    struct mapped* d = (void*)arena_alloc(sizeof(*d)); if (!d) return -12;
    memset(d, 0, sizeof(*d)); d->number = number;
    strcpy(d->name, io->name); strcpy(d->uuid, io->uuid);
    struct block_device* b = &d->block;
    block_assign_name(b, "dm-", number, DM_MAJOR); strcpy(b->name, b->linux_name); strcpy(b->model, "Device mapper");
    b->kind = "DM"; b->block_size = 512; b->blocks = 0; b->max_blocks = 128; b->ops = &dm_ops; b->driver = d; b->extra = &dm_extra;
    b->read_only = (io->flags & DM_READONLY) != 0;
    devices[number] = d;
    if (block_hot_add(b) < 0) { devices[number] = NULL; arena_free((uint64_t)d, sizeof(*d)); return -12; }
    if (storage_node_alias(b, d->name) < 0) { remove_device(d, false); return -12; }
    info(d, io); io->flags |= DM_UEVENT_GENERATED; return 0;
}
static int resume(struct mapped* d, struct dm_ioctl* io) {
    struct table* fresh = d->inactive;
    if (!fresh && !d->live) return -22;   /* Linux dm_resume: nothing to resume */
    struct table* next = fresh ? fresh : d->live;   /* Linux crypt_preresume: "aborting resume - crypt key is not set" */
    for (unsigned i = 0; i < next->count; i++) if (next->t[i].type == T_CRYPT && __atomic_load_n(&next->t[i].key_wiped, __ATOMIC_ACQUIRE)) return -22;
    if (fresh) {
        if (d->block.references > 1 && d->live && fresh->block_size != d->block.block_size) return -16;
        spin_acquire(&dm_lock);
        struct table* old = d->live; d->live = fresh; d->inactive = NULL;
        spin_release(&dm_lock);
        d->block.block_size = fresh->block_size; d->block.blocks = fresh->sectors / (fresh->block_size / 512);
        d->block.read_only = fresh->readonly; d->block.discard_granularity = fresh->discard_granularity;
        d->block.generation++;   /* identities (fsprobe) are probed again */
        table_retire(old);
        for (unsigned i = 0; i < fresh->count; i++) if (fresh->t[i].type == T_THINPOOL) thinpool_resume(&fresh->t[i]);   /* the data device grew */
    }
    bool was = d->suspended;
    __atomic_store_n(&d->suspended, false, __ATOMIC_RELEASE);
    if (was) { mount_freeze_device(&d->block, false); task_wake_channel(&d->suspended); }   /* held requests and frozen mounts go on */
    announce(d, "change"); io->flags |= DM_UEVENT_GENERATED;
    return 0;
}
const struct block_partition* ext4_dev_partition(void);
static bool holds_root(struct mapped* d) {   /* the root filesystem cannot be frozen here (nothing could resume it) */
    const struct block_partition* fat = block_root(); const struct block_partition* ext4 = ext4_dev_partition();
    return (fat && fat->dev == &d->block) || (ext4 && ext4->dev == &d->block);
}
static int remove_device(struct mapped* d, bool deferred) {
    if (d->block.references > 1) { if (deferred) { d->deferred_remove = true; return 0; } return -16; }
    d->gone = true; d->deferred_remove = false;
    storage_node_unalias(&d->block);
    if (devices[d->number] == d) devices[d->number] = NULL;
    block_hot_remove(&d->block);   /* "remove" uevent; the last reference frees d (dm_release) */
    return 0;
}
/* output: data after the header (data_start = 312, as Linux aligns it) */
struct out { uint8_t* base; uint32_t capacity, used; bool full; };
static uint8_t* reserve(struct out* o, uint32_t bytes) {
    uint32_t aligned = (bytes + 7) & ~7u;
    if (o->full || aligned > o->capacity - o->used) { o->full = true; return NULL; }
    uint8_t* p = o->base + o->used; memset(p, 0, aligned); o->used += aligned; return p;
}
static void finish_output(struct dm_ioctl* io, struct out* o) {
    io->flags |= o->full ? DM_BUFFER_FULL : DM_DATA_OUT;
    io->data_size = sizeof(struct dm_ioctl) + o->used;
}
static void put_number(char* text, uint64_t value) { char number[24] = ""; append_u64(number, value); strcat(text, number); }
static void target_status(const struct target* g, bool table, char* text) {   /* Linux linear_status / crypt_status */
    text[0] = 0;
    if (g->type == T_VERITY) { verity_status(g, table, text); return; }
    if (g->type == T_INTEGRITY) { integrity_status(g, table, text); return; }
    if (g->type == T_THINPOOL || g->type == T_THIN) { thin_status(g, table, text); return; }
    if (g->type == T_RAID) { raid_status(g, table, text); return; }
    if (g->type >= T_STRIPED) { snap_status(g, table, text); return; }
    if (!table || g->type == T_ZERO || g->type == T_ERROR) return;   /* STATUSTYPE_INFO is empty for these targets */
    if (g->type == T_CRYPT) {
        strcat(text, g->cipher); strcat(text, " ");
        if (__atomic_load_n(&g->key_wiped, __ATOMIC_ACQUIRE) && g->key_text[0] != ':') for (unsigned i = 0; i < g->key_bytes * 2; i++) strcat(text, "0");
        else strcat(text, g->key_text);
        strcat(text, " "); put_number(text, g->iv_offset); strcat(text, " ");
    }
    put_number(text, g->under->major); strcat(text, ":"); put_number(text, g->under->minor); strcat(text, " "); put_number(text, g->offset);
    if (g->type != T_CRYPT) return;
    static const char* const flags[6] = {"allow_discards", "same_cpu_crypt", "high_priority", "submit_from_crypt_cpus", "no_read_workqueue", "no_write_workqueue"};
    unsigned count = (g->sector_size != 512) + ((g->options & O_LARGE_IV) != 0);
    for (unsigned f = 0; f < 6; f++) count += (g->options >> f) & 1;
    if (!count) return;
    strcat(text, " "); put_number(text, count);
    for (unsigned f = 0; f < 6; f++) if (g->options & (1u << f)) { strcat(text, " "); strcat(text, flags[f]); }
    if (g->sector_size != 512) { strcat(text, " sector_size:"); put_number(text, g->sector_size); }
    if (g->options & O_LARGE_IV) strcat(text, " iv_large_sectors");
}
#include "dm_snap.inc"   /* striped, snapshot-origin, snapshot (LVM2) */
#include "dm_verity.inc" /* verity (veritysetup; roothash= at boot) */
#include "dm_integrity.inc" /* integrity (integritysetup) */
#include "dm_thin.inc"   /* thin-pool, thin (lvm2 thin pools, thin_check) */
#include "dm_raid.inc"   /* raid (lvm2 raid1/mirror LVs) */
static const char* input_string(uint8_t* data, uint32_t size, uint32_t at) {   /* a NUL-terminated string at `at` */
    if (at < DM_DATA_OFFSET || at >= size) return NULL;
    for (uint32_t i = at; i < size; i++) if (!data[i]) return (const char*)data + at;
    return NULL;
}
static int crypt_message(struct mapped* d, struct target* g, char* text) {   /* "key wipe", "key set <key>" */
    char* cursor = text; char *what = word(&cursor), *how = word(&cursor), *key = word(&cursor);
    if (!what || !how || strcmp(what, "key")) return -22;
    if (!d->suspended) return -22;   /* Linux: "not suspended during key manipulation" */
    /* open or mounted is fine: new requests are held back while suspended and none is in flight (users, below) */
    if (!strcmp(how, "wipe") && !key) {
        spin_acquire(&dm_lock);
        bool busy = d->live->users != 0;
        if (!busy) __atomic_store_n(&g->key_wiped, true, __ATOMIC_RELEASE);
        spin_release(&dm_lock);
        if (busy) return -16;
        xts_wipe(g->key);
        return 0;
    }
    if (strcmp(how, "set") || !key || word(&cursor)) return -22;
    uint8_t material[64]; char canonical[sizeof(g->key_text)];
    int bytes = parse_key(key, material, canonical, sizeof(canonical));
    memset(key, '0', strlen(key));
    if (bytes < 0) return bytes;
    struct xts_key* fresh = (void*)arena_alloc(sizeof(struct xts_key));
    int e = (unsigned)bytes != g->key_bytes ? -22 : !fresh ? -12 : cipher_set_key(fresh, g->mode, material, (unsigned)bytes);   /* the key size may not change */
    memset(material, 0, sizeof(material));
    if (!e) {
        spin_acquire(&dm_lock);
        if (d->live->users) e = -16;
        else { struct xts_key* old = g->key; g->key = fresh; fresh = old; strcpy(g->key_text, canonical); __atomic_store_n(&g->key_wiped, false, __ATOMIC_RELEASE); }
        spin_release(&dm_lock);
    }
    if (fresh) { xts_wipe(fresh); arena_free((uint64_t)fresh, sizeof(struct xts_key)); }
    memset(canonical, 0, sizeof(canonical));
    return e;
}
static int control(struct dm_ioctl* io, uint8_t* data, uint32_t size, unsigned command) {
    uint32_t input = io->data_start;   /* where TABLE_LOAD, DEV_RENAME and TARGET_MSG data starts */
    io->version[0] = 4; io->version[1] = 48; io->version[2] = 0;
    io->flags &= ~(DM_BUFFER_FULL | DM_DATA_OUT | DM_UEVENT_GENERATED);
    io->data_start = sizeof(struct dm_ioctl); io->data_size = sizeof(struct dm_ioctl);
    struct out o = {data + sizeof(struct dm_ioctl), size - (uint32_t)sizeof(struct dm_ioctl), 0, false};
    switch (command) {
    case 0: return 0;                                            /* VERSION */
    case 1:                                                      /* REMOVE_ALL: the unused ones (deferred: the rest later) */
        for (unsigned i = 0; i < DM_MAX; i++) if (devices[i] && !devices[i]->gone) remove_device(devices[i], (io->flags & DM_DEFERRED_REMOVE) != 0);
        return 0;
    case 2: {                                                    /* LIST_DEVICES: struct dm_name_list chain */
        bool uuids = (io->flags & DM_UUID) != 0; uint8_t* previous = NULL;
        for (unsigned i = 0; i < DM_MAX; i++) {
            struct mapped* m = devices[i]; if (!m || m->gone) continue;
            uint32_t names = (12 + (uint32_t)strlen(m->name) + 1 + 7) & ~7u, ul = (uint32_t)strlen(m->uuid) + 1;
            uint8_t* e = reserve(&o, names + 8 + (uuids && m->uuid[0] ? ul : 0)); if (!e) break;
            put_le64(e, storage_linux_dev(DM_MAJOR, m->number)); strcpy((char*)e + 12, m->name);
            put_le32(e + names, m->event_nr);                     /* then flags, then the uuid when asked for */
            if (uuids) { put_le32(e + names + 4, m->uuid[0] ? 1 : 2); if (m->uuid[0]) strcpy((char*)e + names + 8, m->uuid); }
            if (previous) put_le32(previous + 8, (uint32_t)(e - previous));
            previous = e;
        }
        finish_output(io, &o); return 0;
    }
    case 3: return create(io);                                   /* DEV_CREATE */
    case 13: case 17: {                                          /* LIST_VERSIONS, GET_TARGET_VERSION (name in io->name) */
        static const char* const names[12] = {"linear", "crypt", "zero", "error", "striped", "snapshot-origin", "snapshot", "verity", "integrity", "thin-pool", "thin", "raid"};
        static const uint32_t versions[12][3] = {{1, 4, 0}, {1, 24, 0}, {1, 2, 0}, {1, 6, 0}, {1, 6, 0}, {1, 9, 0}, {1, 16, 0}, {1, 10, 0}, {1, 10, 0}, {1, 23, 0}, {1, 23, 0}, {1, 15, 1}};
        uint8_t* previous = NULL; bool found = false;
        for (unsigned i = 0; i < 12; i++) {
            if (command == 17 && strcmp(io->name, names[i])) continue;
            found = true;
            uint8_t* e = reserve(&o, 16 + (uint32_t)strlen(names[i]) + 1); if (!e) break;
            put_le32(e + 4, versions[i][0]); put_le32(e + 8, versions[i][1]); put_le32(e + 12, versions[i][2]); strcpy((char*)e + 16, names[i]);
            if (previous) put_le32(previous, (uint32_t)(e - previous));
            previous = e;
        }
        if (!found) return -22;
        finish_output(io, &o); return 0;
    }
    }
    struct mapped* d = lookup(io);
    if (!d) return -6;
    switch (command) {
    case 4: return remove_device(d, (io->flags & DM_DEFERRED_REMOVE) != 0);   /* DEV_REMOVE */
    case 5: {                                                                   /* DEV_RENAME: the new name (or uuid) as data */
        const char* fresh = input_string(data, size, input);
        bool uuid = (io->flags & DM_UUID) != 0;
        if (!fresh || !fresh[0] || strlen(fresh) >= (uuid ? DM_UUID_LEN : DM_NAME_LEN)) return -22;
        if (uuid) {
            if (d->uuid[0]) return -22;   /* a uuid is set once */
            for (unsigned i = 0; i < DM_MAX; i++) if (devices[i] && !devices[i]->gone && !strcmp(devices[i]->uuid, fresh)) return -16;
            strcpy(d->uuid, fresh);
        } else {
            if (!name_ok(fresh)) return -22;
            for (unsigned i = 0; i < DM_MAX; i++) if (devices[i] && !devices[i]->gone && !strcmp(devices[i]->name, fresh)) return -16;
            storage_node_unalias(&d->block); strcpy(d->name, fresh);
            if (storage_node_alias(&d->block, d->name) < 0) vga_print("[64] DM: no node for the renamed device\n");
        }
        announce(d, "change"); info(d, io); io->flags |= DM_UEVENT_GENERATED; return 0;
    }
    case 6:                                                                     /* DEV_SUSPEND: suspend, or resume */
        if (io->flags & DM_SUSPEND) {
            if (!d->suspended && holds_root(d)) return -16;
            if (!d->suspended && d->live) for (unsigned i = 0; i < d->live->count; i++) if (d->live->t[i].type == T_RAID) raid_presuspend(&d->live->t[i]);
            if (!d->suspended) { mount_freeze_device(&d->block, true); __atomic_store_n(&d->suspended, true, __ATOMIC_RELEASE); }
            if (d->live) block_flush(&d->block);
        }
        else { int e = resume(d, io); if (e) return e; }
        info(d, io); return 0;
    case 7: case 8: case 16: info(d, io); return 0;                              /* DEV_STATUS, DEV_WAIT, DEV_ARM_POLL */
    case 9: {                                                                   /* TABLE_LOAD */
        struct table* t; int e = parse_table(d, io, data, size, input, &t); if (e) return e;
        free_table(d->inactive); d->inactive = t; info(d, io); return 0;
    }
    case 10: free_table(d->inactive); d->inactive = NULL; info(d, io); return 0;   /* TABLE_CLEAR */
    case 11: {                                                                  /* TABLE_DEPS: count, padding, distinct dev_t[] */
        struct table* t = (io->flags & DM_QUERY_INACTIVE) ? d->inactive : d->live;
        uint8_t* head = reserve(&o, 8); uint32_t count = 0;
        if (head && t) for (unsigned i = 0; i < t->count; i++) {
            struct block_partition* used[STRIPES + 1]; unsigned uses = target_devices(&t->t[i], used);
            for (unsigned j = 0; j < uses; j++) {
                uint64_t dev = storage_linux_dev(used[j]->major, used[j]->minor); bool seen = false;
                for (uint32_t k = 0; k < count; k++) if (get_le64(head + 8 + 8 * k) == dev) seen = true;
                if (seen) continue;
                uint8_t* slot = reserve(&o, 8); if (!slot) break;
                put_le64(slot, dev); count++;
            }
        }
        if (head) put_le32(head, count);
        info(d, io); finish_output(io, &o); return 0;
    }
    case 12: {                                                                  /* TABLE_STATUS */
        struct table* t = (io->flags & DM_QUERY_INACTIVE) ? d->inactive : d->live;
        bool table = (io->flags & DM_STATUS_TABLE) != 0;
        info(d, io); io->target_count = t ? t->count : 0;
        static const char* const types[13] = {"", "linear", "crypt", "zero", "error", "striped", "snapshot-origin", "snapshot", "verity", "integrity", "thin-pool", "thin", "raid"};
        char text[1024];
        if (t) for (unsigned i = 0; i < t->count && !o.full; i++) {
            const struct target* g = &t->t[i];
            target_status(g, table, text);
            uint32_t length = (uint32_t)strlen(text) + 1;
            uint8_t* e = reserve(&o, 40 + length); if (!e) break;
            put_le64(e, g->start); put_le64(e + 8, g->length); strcpy((char*)e + 24, types[g->type]);
            memcpy(e + 40, text, length);
            put_le32(e + 20, o.used);   /* Linux: the next spec's offset from the start of the output */
        }
        volatile char* wipe = text; for (unsigned i = 0; i < sizeof(text); i++) wipe[i] = 0;
        finish_output(io, &o); return 0;
    }
    case 14: {                                                                  /* TARGET_MSG: struct dm_target_msg */
        if (input < DM_DATA_OFFSET || input > size || size - input < 9) return -22;
        uint64_t sector = get_le64(data + input);
        char* text = (char*)input_string(data, size, input + 8);
        if (!text) return -22;
        if (!strcmp(text, "@cancel_deferred_remove")) { if (!d->deferred_remove) return -6; d->deferred_remove = false; return 0; }
        struct target* g = d->live ? target_at(d->live, sector) : NULL;
        if (g && g->type == T_THINPOOL) return thinpool_message(g, text);
        if (g && g->type == T_RAID) return raid_message(g, text);
        if (!g || g->type != T_CRYPT) return -22;
        return crypt_message(d, g, text);
    }
    case 15: return 0;                                                          /* DEV_SET_GEOMETRY */
    }
    return -25;
}
static int control_open(uint64_t flags, struct input_reader* reader) { (void)flags; (void)reader; return 0; }
static int control_ioctl(struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)reader; (void)flags;
    if (((command >> 8) & 0xFF) != 0xFD || (command & 0xFF) > 17) return -25;   /* _IOWR(0xfd, nr, struct dm_ioctl) */
    if (!credentials_capable(credentials_current(), NULL, 21)) return -13;
    struct dm_ioctl header;
    if (!accessible(argument, sizeof(header), true)) return -14;
    copy_from_user(&header, (const void*)argument, sizeof(header));
    if (header.version[0] != 4) {   /* Linux check_version: say which interface this is */
        uint32_t version[3] = {4, 48, 0}; copy_to_user((void*)argument, version, sizeof(version)); return -22;
    }
    uint32_t size = header.data_size;
    if (size < sizeof(header) || size > (1u << 20)) return -22;
    if (!accessible(argument, size, true)) return -14;
    uint32_t capacity = (size + 4095) / 4096 * 4096;
    uint8_t* data = (void*)arena_alloc(capacity); if (!data) return -12;
    copy_from_user(data, (const void*)argument, size);
    struct dm_ioctl* io = (struct dm_ioctl*)data;
    io->name[DM_NAME_LEN - 1] = 0; io->uuid[DM_UUID_LEN - 1] = 0;
    int e = control(io, data, size, (unsigned)(command & 0xFF));
    if (!e) { if (copy_to_user((void*)argument, data, io->data_size <= size ? io->data_size : size)) e = -14; }
    else copy_to_user((void*)argument, io->version, sizeof(io->version));
    volatile uint8_t* wipe = data; for (uint32_t i = 0; i < size; i++) wipe[i] = 0;   /* keys passed through */
    arena_free((uint64_t)data, capacity);
    return e;
}
static const struct storage_char_ops control_ops = {.open = control_open, .ioctl = control_ioctl};
/* Kernel callers (LVM activation at boot, lvm_boot.c): create NAME with `count` targets ("<start> <length> <type>
 * <params>" lines as dmsetup takes them) and make it live: DM_DEV_CREATE + DM_TABLE_LOAD + DM_DEV_SUSPEND (resume). */
int dm_kernel_create(const char* name, const char* uuid, const char* const* lines, unsigned count, bool readonly, struct block_device** out) {
    if (!name || !count || count > TARGETS || strlen(name) >= DM_NAME_LEN || (uuid && strlen(uuid) >= DM_UUID_LEN)) return -22;
    uint32_t size = 16384;
    uint8_t* data = (void*)arena_alloc(size); if (!data) return -12;
    memset(data, 0, size);
    struct dm_ioctl* io = (struct dm_ioctl*)data;
    io->version[0] = 4; io->data_size = size; strcpy(io->name, name); if (uuid) strcpy(io->uuid, uuid);
    int e = control(io, data, size, 3);
    struct mapped* d = e ? NULL : lookup(io);
    if (!e && !d) e = -6;
    if (!e) {
        memset(data + sizeof(struct dm_ioctl), 0, size - sizeof(struct dm_ioctl));
        io->data_start = sizeof(struct dm_ioctl); io->target_count = count; io->flags = readonly ? DM_READONLY : 0;
        uint32_t at = sizeof(struct dm_ioctl);
        for (unsigned i = 0; i < count && !e; i++) {   /* struct dm_target_spec + parameters, 8-aligned */
            char line[1024]; strncpy(line, lines[i], sizeof(line) - 1); line[sizeof(line) - 1] = 0;
            char* cursor = line; char* start = word(&cursor); char* length = word(&cursor); char* type = word(&cursor);
            uint64_t s, n; const char* end;
            while (*cursor == ' ') cursor++;
            if (!start || !length || !type || strlen(type) >= 16 || !parse_u64(start, &s, &end) || *end || !parse_u64(length, &n, &end) || *end) { e = -22; break; }
            uint32_t need = (40 + (uint32_t)strlen(cursor) + 1 + 7) & ~7u;
            if (at + need > size) { e = -22; break; }
            uint8_t* spec = data + at;
            put_le64(spec, s); put_le64(spec + 8, n); memcpy(spec + 24, type, strlen(type)); strcpy((char*)spec + 40, cursor);
            put_le32(spec + 20, need); at += need;
        }
        if (!e) e = control(io, data, size, 9);
        if (!e) { io->flags = 0; io->data_start = sizeof(struct dm_ioctl); e = control(io, data, size, 6); }
        if (e) remove_device(d, false);
    }
    arena_free((uint64_t)data, size);
    if (!e && out) *out = &d->block;
    return e;
}
/* Kernel callers (the encrypted root, luks.c): create NAME with one crypt target over `under` and make it live, the
 * same path DM_DEV_CREATE + DM_TABLE_LOAD + DM_DEV_SUSPEND(resume) take. The key text is wiped afterwards. */
int dm_crypt_create(const char* name, const char* uuid, const struct block_partition* under, uint64_t offset_sectors, uint64_t sectors,
                    const char* cipher, const uint8_t* key, unsigned key_bytes, uint32_t sector_size, uint64_t iv_offset,
                    unsigned flags, struct block_device** out) {   /* flags: 1 allow_discards, 2 read-only (crypttab) */
    if (!name || !under || !key || !key_bytes || key_bytes > 64 || !sectors || strlen(name) >= DM_NAME_LEN || (uuid && strlen(uuid) >= DM_UUID_LEN)) return -22;
    uint32_t size = 4096;
    uint8_t* data = (void*)arena_alloc(size); if (!data) return -12;
    memset(data, 0, size);
    struct dm_ioctl* io = (struct dm_ioctl*)data;
    io->version[0] = 4; io->data_size = size; strcpy(io->name, name); if (uuid) strcpy(io->uuid, uuid);
    int e = control(io, data, size, 3);
    struct mapped* d = e ? NULL : lookup(io);
    if (!e && !d) e = -6;
    if (!e) {   /* the table: one crypt target as Linux receives it (struct dm_target_spec + parameters) */
        memset(data + sizeof(struct dm_ioctl), 0, size - sizeof(struct dm_ioctl));
        io->data_start = sizeof(struct dm_ioctl); io->target_count = 1; io->flags = flags & 2 ? DM_READONLY : 0;
        uint8_t* spec = data + sizeof(struct dm_ioctl);
        put_le64(spec, 0); put_le64(spec + 8, sectors); memcpy(spec + 24, "crypt", 6);
        char* p = (char*)spec + 40;
        strcpy(p, cipher); strcat(p, " ");
        for (unsigned i = 0; i < key_bytes; i++) append_hex(p, key[i], 2);
        strcat(p, " "); append_u64(p, iv_offset); strcat(p, " ");
        append_u64(p, under->major); strcat(p, ":"); append_u64(p, under->minor); strcat(p, " "); append_u64(p, offset_sectors);
        unsigned options = (sector_size != 512) + ((flags & 1) != 0);   /* as cryptsetup loads LUKS: IVs in 512-byte sectors */
        if (options) { strcat(p, " "); append_u64(p, options); }
        if (flags & 1) strcat(p, " allow_discards");
        if (sector_size != 512) { strcat(p, " sector_size:"); append_u64(p, sector_size); }
        put_le32(spec + 20, 0);
        e = control(io, data, size, 9);
        if (!e) { io->flags = 0; io->data_start = sizeof(struct dm_ioctl); e = control(io, data, size, 6); }
        if (e) remove_device(d, false);
    }
    crypto_wipe(data, size); arena_free((uint64_t)data, size);
    if (!e && out) *out = &d->block;
    return e;
}
int dm_remove_name(const char* name) {   /* kernel callers: remove an unused mapping (the installer's temporary one) */
    for (unsigned i = 0; i < DM_MAX; i++) if (devices[i] && !devices[i]->gone && !strcmp(devices[i]->name, name)) return remove_device(devices[i], false);
    return -6;
}
bool dm_name_exists(const char* name) {   /* crypttab.c: is /dev/mapper/NAME there already? */
    for (unsigned i = 0; i < DM_MAX; i++) if (devices[i] && !devices[i]->gone && !strcmp(devices[i]->name, name)) return true;
    return false;
}
/* Any dm-crypt mapping live? (hibernation must not write memory, keys included, to an unencrypted place) */
bool dm_crypt_in_use(void) {
    for (unsigned i = 0; i < DM_MAX; i++) {
        struct mapped* d = devices[i]; if (!d || d->gone || !d->live) continue;
        for (unsigned k = 0; k < d->live->count; k++) if (d->live->t[k].type == T_CRYPT) return true;
    }
    return false;
}
const char* dm_device_name(const struct block_device* dev) { struct mapped* d = mapped_of((struct block_device*)dev); return d ? d->name : NULL; }
bool dm_device_suspended(const struct block_device* dev) { struct mapped* d = mapped_of((struct block_device*)dev); return d && __atomic_load_n(&d->suspended, __ATOMIC_ACQUIRE); }   /* blkdev_node.c uevents */
void dm_init(void) {
    if (!aes_selftest()) { vga_print("[64] DM: /dev/mapper/control disabled (AES self-test failed)\n"); return; }
    storage_mapper_control_ops = &control_ops;
}
#pragma clang attribute pop
