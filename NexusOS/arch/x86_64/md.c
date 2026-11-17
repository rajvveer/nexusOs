/* Software RAID (Linux drivers/md: md.c, raid0.c, raid1.c; raid5.c, raid10.c and md-bitmap.c in md_raid.inc) for
 * mdadm and for booting from an array.
 *
 * Arrays /dev/md0../dev/md7 (block 9:N), made on first use as Linux md_probe does: by mdadm's mknod of 9:N or by the
 * boot assembly (none exist empty: every array takes one of blockdev.c's 16 device slots, as dm devices do). Metadata:
 * the version-1 superblock mdadm writes (1.2 at 4 KiB, 1.1 at 0, 1.0 near the end): magic a92b4efc, set uuid and
 * name, level, raid_disks, chunk, size, data_offset/size, events, resync_offset, recovery_offset, dev_roles[] and the
 * checksum (sum of 32-bit words folded to 32 bits). RAID1: reads from the first in-sync member (another one after a
 * read error), writes to every member that has a role (a member being rebuilt too); RAID0: chunks round-robin over the
 * members. A failed write or read makes the member faulty (dev_roles 0xfffe) and the array degraded; a RAID1 array
 * runs with one member left. A RAID1 array is marked dirty (resync_offset 0, events+1) at its first write and clean
 * again after 200 ms without writes (Linux safe_mode_delay; members flushed first), at STOP_ARRAY and at shutdown
 * (vfs.c disk_shutdown -> md_shutdown); a dirty RAID1 array (an unclean shutdown, or a
 * new array mdadm created without --assume-clean) is resynchronised by the md_resync kernel thread from its
 * checkpoint, a spare added to a degraded array is rebuilt (recovery_offset checkpoints), both with progress in
 * /proc/mdstat and /sys/block/mdN/md/{sync_action,sync_completed}. Members whose events lag are kicked (unbound) as
 * Linux does; roles of absent members are written as faulty (Linux super_1_sync).
 * mdadm interface (ioctl type 9): RAID_VERSION, GET_ARRAY_INFO, GET_DISK_INFO, SET_ARRAY_INFO (version 1.x only),
 * ADD_NEW_DISK (its superblock is read; on a running array: a spare, rebuilt), HOT_REMOVE_DISK, SET_DISK_FAULTY,
 * RUN_ARRAY, STOP_ARRAY(_RO), RESTART_ARRAY_RW, GET_BITMAP_FILE (none); /proc/mdstat; /sys/block/mdN/md/{level,
 * raid_disks, chunk_size, layout, component_size, metadata_version, array_state, array_size, degraded, sync_action,
 * sync_completed, resync_start, uuid, consistency_policy} and md/dev-<member>/{slot,state,offset,size,errors,
 * recovery_start,block} (mdadm's sysfs_read). Boot (kernel.c md_boot before cryptroot_boot: md -> LUKS -> LVM stacks): the
 * superblocks of every partition and disk, grouped by set uuid, each set assembled as mdN from its name's ":N" (else
 * the first free), root=/dev/mdN or UUID= of the filesystem on it. RAID5/6/10 and internal write-intent bitmaps:
 * md_raid.inc. Not done: RAID4, RAID10 far/offset layouts, external bitmaps, reshape, PPL/journal, external metadata
 * (IMSM/DDF), write-mostly. */
#include "native.h"
#include "boot_info.h"
#include "blockdev.h"
#include "storage_util.h"
#include "../../kernel/vga.h"
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */

#define MD_MAX 8
#define MD_MEMBERS 16
#define MD_MAJOR 9
#define SB_MAGIC 0xa92b4efcu
#define SB_BYTES 1024u
#define NO_SECTOR (~0ULL)
#define FEATURE_RECOVERY 2u
#define SYNC_STEP 256u          /* sectors copied per step (128 KiB) */
#define CHECKPOINT (16384u)     /* sectors between superblock checkpoints (8 MiB) */
#define SAFE_MODE_NS 200000000ULL   /* idle time before a dirty RAID1 array is marked clean */
int block_spec_matches(struct block_partition* p, const char* spec);
void kernel_io_sleep(void* channel, uint64_t deadline);
bool task_kernel_thread(void (*entry)(void), const char* name);
void task_kernel_sleep(void* channel, uint64_t deadline);

struct member {
    struct block_partition* part; uint64_t sb_offset, data_offset, data_size, recovery_offset;
    int role; uint32_t desc; bool faulty, kicked, held, bm_recovery; uint64_t events;   /* bm_recovery: re-added, only bitmap-dirty chunks */
    uint8_t sb[SB_BYTES];
};
struct md {
    struct block_device block; unsigned number; bool active, ro, dirty, configured;
    int level; uint32_t raid_disks, chunk, layout, max_dev, feature, minor_version; uint64_t size, events, recovery_cp, ctime;   /* recovery_cp: resync point, NO_SECTOR = in sync */
    uint8_t uuid[16]; char name[33];
    struct member m[MD_MEMBERS]; unsigned members;
    int sync; uint64_t sync_pos, sync_mark; int sync_target;   /* 1 resync, 2 recovery (member sync_target) */
    uint64_t last_write;
    unsigned near;                     /* RAID10 copies */
    uint8_t* work[MD_MEMBERS + 2];     /* RAID5/6/10: one piece per role + 2 scratch */
    bool bitmap, bm_dirty; int32_t bm_offset; uint32_t bm_bytes; uint64_t bm_chunk, bm_bits, bm_cleared; uint8_t* bm;
    volatile int busy;
};
static struct md* arrays[MD_MAX];
static int resync_channel; static bool thread_started;
static void say(const struct md* a, const char* text) {
    vga_print("[64] MD "); if (a) { vga_print(a->block.linux_name); vga_print(" "); } vga_print(text); vga_print("\n");
}
static void say_member(const struct md* a, const struct member* x, const char* text) {
    vga_print("[64] MD "); vga_print(a->block.linux_name); vga_print(" "); vga_print(x->part->linux_name); vga_print(" "); vga_print(text); vga_print("\n");
}
static void lock(struct md* a) {
    while (__atomic_exchange_n(&a->busy, 1, __ATOMIC_ACQUIRE)) {
        if (cpu_self()->task && kernel_lock_held()) kernel_io_sleep((void*)&a->busy, native_clock_ns() + 1000000ULL); else __asm__ volatile("pause");
    }
}
static void unlock(struct md* a) { __atomic_store_n(&a->busy, 0, __ATOMIC_RELEASE); task_wake_channel((void*)&a->busy); }

/* ---- superblocks ---- */
static uint32_t sb_csum(const uint8_t* sb, uint32_t max_dev) {
    uint64_t sum = 0; uint32_t size = 256 + max_dev * 2, i = 0;
    for (; size - i >= 4; i += 4) sum += i == 216 ? 0 : get_le32(sb + i);
    if (size - i == 2) sum += (uint32_t)sb[i] | (uint32_t)sb[i + 1] << 8;
    return (uint32_t)((sum & 0xFFFFFFFFULL) + (sum >> 32));
}
static bool sb_read(struct block_partition* part, struct member* x, unsigned* minor) {   /* 1.2, 1.1, 1.0 */
    uint64_t sectors = block_part_sectors(part);
    if (sectors < 64) return false;
    uint64_t at[3] = {8, 0, (sectors - 16) & ~7ULL}; const unsigned minors[3] = {2, 1, 0};
    for (unsigned k = 0; k < 3; k++) {
        if (block_part_read(part, at[k], 2, x->sb) < 0) return false;
        const uint8_t* sb = x->sb;
        if (get_le32(sb) != SB_MAGIC || get_le32(sb + 4) != 1) continue;
        uint32_t max_dev = get_le32(sb + 220);
        uint64_t data_offset = get_le64(sb + 128), data_size = get_le64(sb + 136);
        bool overlap = k == 2 ? data_offset + data_size > at[k] : data_offset < at[k] + 2;   /* data over the superblock */
        if (max_dev > 384 || get_le64(sb + 144) != at[k] || sb_csum(sb, max_dev) != get_le32(sb + 216) ||
            data_offset >= sectors || data_size > sectors - data_offset || !data_size || overlap) { vga_print("[64] MD bad superblock on "); vga_print(part->linux_name); vga_print("\n"); return false; }
        x->part = part; x->sb_offset = at[k]; x->data_offset = data_offset; x->data_size = data_size;
        x->desc = get_le32(sb + 160); x->events = get_le64(sb + 200);
        x->recovery_offset = get_le32(sb + 8) & FEATURE_RECOVERY ? get_le64(sb + 152) : NO_SECTOR;
        uint16_t role = x->desc < max_dev ? (uint16_t)(sb[256 + 2 * x->desc] | sb[257 + 2 * x->desc] << 8) : 0xFFFF;
        x->faulty = role == 0xFFFE; x->role = role >= 0xFFFE ? -1 : (int)role; x->kicked = false;
        *minor = minors[k]; return true;
    }
    return false;
}
static void sb_update(struct md* a, struct member* x) {   /* the array's state into one member's superblock copy */
    uint8_t* sb = x->sb;
    put_le64(sb + 192, native_realtime_ns() / 1000000000ULL); put_le64(sb + 200, a->events); put_le64(sb + 208, a->dirty ? 0 : a->recovery_cp);   /* Linux: dirty -> 0 */
    uint32_t feature = get_le32(sb + 8) & ~FEATURE_RECOVERY;
    if (x->recovery_offset != NO_SECTOR && x->role >= 0) { feature |= FEATURE_RECOVERY; put_le64(sb + 152, x->recovery_offset); }
    put_le32(sb + 8, feature);
    uint32_t max_dev = get_le32(sb + 220);
    for (unsigned d = 0; d < max_dev; d++) { sb[256 + 2 * d] = 0xFE; sb[257 + 2 * d] = 0xFF; }   /* absent: faulty */
    for (unsigned k = 0; k < a->members; k++) {
        const struct member* y = &a->m[k]; if (y->desc >= max_dev) continue;
        uint16_t role = y->faulty ? 0xFFFE : y->role < 0 || y->kicked ? 0xFFFF : (uint16_t)y->role;
        sb[256 + 2 * y->desc] = (uint8_t)role; sb[257 + 2 * y->desc] = (uint8_t)(role >> 8);
    }
    put_le32(sb + 216, sb_csum(sb, max_dev));
}
static void bm_write_all(struct md* a);   /* md_raid.inc */
static void write_supers(struct md* a) {
    for (unsigned k = 0; k < a->members; k++) {
        struct member* x = &a->m[k];
        if (x->faulty || x->kicked) continue;
        sb_update(a, x);
        if (block_part_write(x->part, x->sb_offset, 2, x->sb) < 0 || block_flush(x->part->dev) < 0) { x->faulty = true; say_member(a, x, "superblock write failed: faulty"); }
    }
    bm_write_all(a);   /* the bitmap's events follow the superblocks' */
}

/* ---- members and I/O ---- */
static bool usable(const struct md* a, const struct member* x) { return x->part && !x->faulty && !x->kicked && x->role >= 0 && x->role < (int)a->raid_disks; }
static bool in_sync(const struct md* a, const struct member* x) { return usable(a, x) && x->recovery_offset == NO_SECTOR; }
static unsigned working(const struct md* a) { unsigned n = 0; for (unsigned k = 0; k < a->members; k++) if (in_sync(a, &a->m[k])) n++; return n; }
static void fail_member(struct md* a, struct member* x, const char* why) {
    if (x->faulty) return;
    x->faulty = true; a->events++;
    if (a->sync == 2 && a->sync_target == (int)(x - a->m)) a->sync = 0;
    say_member(a, x, why);
    write_supers(a);
}
static int member_io(struct member* x, uint64_t sector, uint32_t count, uint8_t* data, bool write) {
    return write ? block_part_write(x->part, x->data_offset + sector, count, data) : block_part_read(x->part, x->data_offset + sector, count, data);
}
static void mark_dirty(struct md* a) {
    if (a->dirty) return;
    a->dirty = true; a->events++;   /* resync_offset 0 on disk until a clean stop: an unclean one resyncs everything */
    write_supers(a);
}
static int raid0_io(struct md* a, uint64_t sector, uint32_t count, uint8_t* data, bool write) {
    struct member* by_role[MD_MEMBERS] = {0};
    for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k])) by_role[a->m[k].role] = &a->m[k];
    while (count) {
        uint64_t chunk = sector / a->chunk, within = sector % a->chunk;
        struct member* x = by_role[chunk % a->raid_disks]; if (!x) return -1;
        uint32_t n = a->chunk - within < count ? (uint32_t)(a->chunk - within) : count;
        if (member_io(x, chunk / a->raid_disks * a->chunk + within, n, data, write) < 0) { fail_member(a, x, "I/O error: faulty (RAID0 lost)"); return -1; }
        sector += n; count -= n; data += (uint64_t)n * 512;
    }
    return 0;
}
static int raid1_io(struct md* a, uint64_t sector, uint32_t count, uint8_t* data, bool write) {
    if (!write) {
        for (unsigned k = 0; k < a->members; k++) {
            struct member* x = &a->m[k];
            bool ok = in_sync(a, x) || (usable(a, x) && x->recovery_offset != NO_SECTOR && sector + count <= x->recovery_offset);
            if (!ok) continue;
            if (member_io(x, sector, count, data, false) == 0) return 0;
            fail_member(a, x, "read error: faulty");
        }
        return -1;
    }
    unsigned done = 0;
    for (unsigned k = 0; k < a->members; k++) {
        struct member* x = &a->m[k];
        if (!usable(a, x)) continue;
        if (member_io(x, sector, count, data, true) == 0) { if (x->recovery_offset == NO_SECTOR) done++; }
        else fail_member(a, x, "write error: faulty");
    }
    return done ? 0 : -1;
}
#include "md_raid.inc"   /* RAID5/6/10, bitmaps */
static int md_rw(struct block_device* b, uint64_t lba, uint32_t count, void* buffer, bool write) {
    struct md* a = b->driver;
    if (!a->active || (write && a->ro)) return -1;
    if (lba + count > b->blocks) return -1;
    lock(a);
    if (write && a->level >= 1) { mark_dirty(a); a->last_write = native_clock_ns(); }   /* RAID0 is never dirty */
    if (write && a->level == 1) bm_mark(a, lba, lba + count);
    int e = a->level == 0 ? raid0_io(a, lba, count, buffer, write) : a->level == 1 ? raid1_io(a, lba, count, buffer, write) : raid_io(a, lba, count, buffer, write);
    unlock(a);
    return e;
}
static int md_read(struct block_device* b, uint64_t lba, uint32_t count, void* buffer) { return md_rw(b, lba, count, buffer, false); }
static int md_write(struct block_device* b, uint64_t lba, uint32_t count, const void* buffer) { return md_rw(b, lba, count, (void*)buffer, true); }
static int md_flush(struct block_device* b) {
    struct md* a = b->driver; int e = 0;
    for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k]) && block_flush(a->m[k].part->dev) < 0) e = -1;
    return e;
}
static const struct block_ops md_ops = {md_read, md_write, md_flush};

/* ---- assembling ---- */
static void work_free(struct md* a) {
    for (unsigned i = 0; i < MD_MEMBERS + 2; i++) if (a->work[i]) { arena_free((uint64_t)a->work[i], PIECE * 512); a->work[i] = NULL; }
}
static void release_members(struct md* a) {
    for (unsigned k = 0; k < a->members; k++) {
        struct member* x = &a->m[k];
        if (x->held) { if (x->part->holders) x->part->holders--; block_put(x->part->dev); x->held = false; }
    }
    a->members = 0;
}
static int add_member(struct md* a, struct block_partition* part) {   /* an inactive array: its superblock joins it */
    if (a->members == MD_MEMBERS) return -28;
    for (unsigned k = 0; k < a->members; k++) if (a->m[k].part == part) return -16;
    if (part->holders) return -16;
    struct member* x = &a->m[a->members]; memset(x, 0, sizeof(*x)); unsigned minor;
    if (!sb_read(part, x, &minor)) return -22;
    const uint8_t* sb = x->sb;
    int level = (int)get_le32(sb + 72); uint32_t raid_disks = get_le32(sb + 92), chunk = get_le32(sb + 88);
    if (!a->members) {   /* the first one defines the array */
        uint32_t layout = get_le32(sb + 76), near = layout & 0xFF, far = (layout >> 8) & 0xFF;
        bool chunk_ok = chunk && !(chunk & (chunk - 1)) && chunk <= 1u << 16, ok = raid_disks && raid_disks <= MD_MEMBERS;
        if (level == 0) ok = ok && chunk_ok;
        else if (level == 5 || level == 6) ok = ok && raid_disks >= (level == 5 ? 2u : 4u) && chunk_ok && chunk >= 8 && layout <= 3;
        else if (level == 10) ok = ok && chunk_ok && chunk >= 8 && far == 1 && !(layout >> 16) && near >= 2 && near <= raid_disks;
        else if (level != 1) ok = false;
        if (!ok) { say(a, "unsupported RAID level, member count, layout or chunk size"); return -22; }
        a->near = level == 10 ? near : 0;
        memcpy(a->uuid, sb + 16, 16); memcpy(a->name, sb + 32, 32); a->name[32] = 0;
        a->level = level; a->raid_disks = raid_disks; a->chunk = chunk; a->layout = get_le32(sb + 76); a->size = get_le64(sb + 80);
        a->max_dev = get_le32(sb + 220); a->feature = get_le32(sb + 8); a->minor_version = minor; a->ctime = get_le64(sb + 64);
        a->events = x->events; a->recovery_cp = get_le64(sb + 208); a->configured = true;
    } else if (memcmp(a->uuid, sb + 16, 16) || level != a->level || raid_disks != a->raid_disks) return -22;
    if (x->events > a->events) { a->events = x->events; a->recovery_cp = get_le64(sb + 208); }
    x->part = part; part->holders++; block_get(part->dev); x->held = true;
    a->members++;
    return 0;
}
static void announce(struct md* a) { if (block_event_hook) block_event_hook(&a->block, NULL, "change"); }
static void start_sync(struct md* a);
static int run_array(struct md* a) {
    if (a->active) return -16;
    if (!a->members) return -22;
    for (unsigned k = 0; k < a->members; ) {   /* non-fresh members (Linux: kicking non-fresh, then unbinding them) */
        struct member* x = &a->m[k];
        if (x->events + 1 >= a->events || x->role < 0) { k++; continue; }
        say_member(a, x, "kicked: its events are older than the array's");
        if (x->held) { if (x->part->holders) x->part->holders--; block_put(x->part->dev); }
        a->m[k] = a->m[--a->members];
    }
    bool roles[MD_MEMBERS] = {0}; unsigned present = 0;
    for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k]) && !roles[a->m[k].role]) { roles[a->m[k].role] = true; present++; }
    uint64_t sectors;
    bool synced[MD_MEMBERS] = {0}; unsigned present_sync = 0;   /* roles with an in-sync member */
    for (unsigned k = 0; k < a->members; k++) if (in_sync(a, &a->m[k]) && !synced[a->m[k].role]) { synced[a->m[k].role] = true; present_sync++; }
    if (a->level == 0) {
        if (present != a->raid_disks) { say(a, "RAID0 needs every member"); return -22; }
        uint64_t smallest = ~0ULL; for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k]) && a->m[k].data_size < smallest) smallest = a->m[k].data_size;
        sectors = smallest / a->chunk * a->chunk * a->raid_disks;
    } else if (a->level >= 5) {   /* RAID5/6/10 */
        bool enough = a->level == 10 || present_sync + parity_count(a) >= a->raid_disks;
        for (unsigned c = 0; a->level == 10 && c < a->raid_disks; c++) {   /* every group of copies keeps one */
            bool one = false; for (unsigned k = 0; k < a->near; k++) if (synced[(c * a->near + k) % a->raid_disks]) one = true;
            if (!one) enough = false;
        }
        if (!enough) { say(a, "not enough working members"); return -22; }
        for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k]) && a->m[k].data_size < a->size) { say(a, "a member is smaller than the array"); return -22; }
        sectors = a->level == 10 ? a->size / a->chunk * a->raid_disks / a->near * a->chunk : a->size / a->chunk * a->chunk * data_count(a);
        if (a->level != 10 && a->recovery_cp != NO_SECTOR && present_sync < a->raid_disks &&
            !boot_option("md_mod.start_dirty_degraded=1") && !boot_option("md-mod.start_dirty_degraded=1")) {
            say(a, "cannot start dirty degraded array (md_mod.start_dirty_degraded=1 overrides)"); return -22;
        }
        if (a->level == 6) gf_init();
        for (unsigned i = 0; i < a->raid_disks + 2; i++) if (!(a->work[i] = (void*)arena_alloc(PIECE * 512))) { work_free(a); return -12; }
    } else {
        if (!working(a) && !present) { say(a, "no member is usable"); return -22; }
        bool any = false; for (unsigned k = 0; k < a->members; k++) if (in_sync(a, &a->m[k])) any = true;
        if (!any) { say(a, "no in-sync member"); return -22; }
        sectors = a->size;
        for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k]) && a->m[k].data_size < sectors) { say(a, "a member is smaller than the array"); return -22; }
    }
    if (!sectors) { work_free(a); return -22; }
    a->block.blocks = sectors;
    if (a->level >= 1 && !bm_load(a)) { work_free(a); a->block.blocks = 0; return -22; }
    a->block.block_size = 512; a->active = true; a->dirty = false;
    a->block.generation++;
    char line[112] = "active raid"; append_u64(line, (uint64_t)a->level); strcat(line, " "); append_u64(line, present); strcat(line, "/"); append_u64(line, a->raid_disks);
    strcat(line, " members, "); append_u64(line, sectors / 2); strcat(line, " KiB");
    if (present < a->raid_disks) strcat(line, " (degraded)");
    if (a->bitmap) strcat(line, ", internal bitmap");
    say(a, line);
    start_sync(a);
    return 0;   /* the caller announces it, unlocked (the uevent reads the array) */
}
static int stop_array(struct md* a, bool readonly, unsigned caller) {   /* caller: the ioctl's own open of the array */
    if (!a->active) { release_members(a); a->configured = false; return 0; }
    if (a->block.references > 1 + caller) return -16;   /* mounted, held by dm/another array, or open elsewhere */
    lock(a);
    if (a->sync == 1) a->recovery_cp = a->sync_pos;
    else if (a->sync == 2 && a->sync_target >= 0) a->m[a->sync_target].recovery_offset = a->sync_pos;
    if (readonly) { if (a->dirty) { a->dirty = false; a->events++; write_supers(a); } a->ro = true; a->sync = 0; unlock(a); return 0; }
    if (a->dirty || a->sync) { a->dirty = false; a->events++; write_supers(a); }
    a->sync = 0; bm_clear(a);
    a->active = false; a->ro = false; a->dirty = false;
    release_members(a); a->configured = false; a->block.blocks = 0; work_free(a); bm_free(a);
    unlock(a);
    say(a, "stopped");
    announce(a);
    return 0;
}

/* ---- resync and recovery ---- */
static int source_member(const struct md* a) { for (unsigned k = 0; k < a->members; k++) if (in_sync(a, &a->m[k])) return (int)k; return -1; }
static void start_sync(struct md* a) {
    if (!a->active || a->level == 0 || a->ro) return;
    if (a->sync == 0 && a->recovery_cp != NO_SECTOR && (a->level == 1 ? working(a) > 1 : !degraded(a))) { a->sync = 1; a->sync_pos = a->recovery_cp; a->sync_mark = a->sync_pos; say(a, "resync started"); }
    if (a->sync == 0) {   /* a member being rebuilt, or a spare for a missing role */
        bool roles[MD_MEMBERS] = {0};
        for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k])) roles[a->m[k].role] = true;
        for (unsigned k = 0; k < a->members && !a->sync; k++) {
            struct member* x = &a->m[k];
            if (usable(a, x) && x->recovery_offset != NO_SECTOR) { a->sync = 2; a->sync_target = (int)k; a->sync_pos = x->recovery_offset; }
            else if (x->role < 0 && !x->faulty && !x->kicked && x->part && x->data_size >= a->size) {
                for (unsigned r = 0; r < a->raid_disks; r++) if (!roles[r]) { x->role = (int)r; x->recovery_offset = 0; a->sync = 2; a->sync_target = (int)k; a->sync_pos = 0; a->events++; write_supers(a); break; }
            }
        }
        if (a->sync == 2) { a->sync_mark = a->sync_pos; say_member(a, &a->m[a->sync_target], "recovery started"); }
    }
    if (a->sync) task_wake_channel(&resync_channel);
}
static void sync_step(struct md* a, uint8_t* buffer) {   /* one piece of the running resync or recovery (array locked) */
    struct member* target = a->sync == 2 && a->sync_target >= 0 ? &a->m[a->sync_target] : NULL;
    if (a->sync == 2 && (!target || !usable(a, target))) { a->sync = 0; return; }
    uint64_t end = sync_end(a);
    bool limited = a->bitmap && (a->sync == 1 || target->bm_recovery);   /* only the bitmap's dirty chunks */
    if (a->sync_pos < end && limited && !bm_test(a, a->sync_pos)) {
        uint64_t next = (a->sync_pos / a->bm_chunk + 1) * a->bm_chunk; a->sync_pos = next < end ? next : end;
    } else if (a->sync_pos < end && a->level == 1) {
        int source = source_member(a);
        if (source < 0) { a->sync = 0; return; }
        uint64_t n = end - a->sync_pos < SYNC_STEP ? end - a->sync_pos : SYNC_STEP;
        if (member_io(&a->m[source], a->sync_pos, (uint32_t)n, buffer, false) < 0) { fail_member(a, &a->m[source], "read error during sync: faulty"); return; }
        for (unsigned k = 0; k < a->members; k++) {
            struct member* x = &a->m[k];
            if ((int)k == source || !usable(a, x) || (a->sync == 2 && (int)k != a->sync_target)) continue;
            if (member_io(x, a->sync_pos, (uint32_t)n, buffer, true) < 0) fail_member(a, x, "write error during sync: faulty");
        }
        a->sync_pos += n;
    } else if (a->sync_pos < end) {
        uint32_t n = raid_sync_piece(a, a->sync_pos, a->sync == 2 ? a->sync_target : -1);
        if (!n) { say(a, "sync stopped: not enough working members"); a->sync = 0; return; }
        a->sync_pos += n;
    }
    bool done = a->sync_pos >= end;
    if (done || a->sync_pos - a->sync_mark >= CHECKPOINT) {   /* the checkpoint survives a power cut */
        a->sync_mark = a->sync_pos;
        if (a->sync == 1) a->recovery_cp = done ? NO_SECTOR : a->sync_pos;
        else if (target) target->recovery_offset = done ? NO_SECTOR : a->level == 10 ? 0 : a->sync_pos;   /* RAID10: array sectors, restarts */
        if (done) { a->events++; if (target) target->bm_recovery = false; }
        write_supers(a);
        if (done) {
            say(a, a->sync == 1 ? (limited ? "resync done (bitmap: dirty chunks only)" : "resync done") : limited ? "recovery done (bitmap: dirty chunks only)" : "recovery done");
            a->sync = 0; start_sync(a);   /* another spare may follow */
            if (!a->sync) { for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k])) block_flush(a->m[k].part->dev); bm_clear(a); }
        }
    }
}
static void safe_mode(void) {   /* dirty RAID1 arrays idle for SAFE_MODE_NS: members flushed, then marked clean */
    uint64_t now = native_clock_ns(); bool idle = false;
    for (unsigned i = 0; i < MD_MAX; i++) { struct md* a = arrays[i]; if (a && a->active && (a->dirty || (a->bm_dirty && !degraded(a))) && !a->sync && now - a->last_write >= SAFE_MODE_NS) idle = true; }
    if (!idle) return;
    kernel_lock_acquire(); block_io_barrier();
    for (unsigned i = 0; i < MD_MAX; i++) {
        struct md* a = arrays[i]; if (!a || !a->active) continue;
        lock(a);
        if (!a->sync && native_clock_ns() - a->last_write >= SAFE_MODE_NS) {
            if (a->dirty || a->bm_dirty) md_flush(&a->block);   /* the data is on every member before anything says clean */
            if (a->dirty) { a->dirty = false; a->events++; write_supers(a); }
            bm_clear(a);
        }
        unlock(a);
    }
    kernel_lock_release();
}
static void resync_main(void) {
    uint8_t* buffer = NULL;
    for (;;) {
        task_kernel_sleep(&resync_channel, native_clock_ns() + SAFE_MODE_NS);
        safe_mode();
        if (!buffer) buffer = (void*)arena_alloc(SYNC_STEP * 512);
        if (!buffer) continue;
        for (bool again = true; again; ) {
            again = false;
            kernel_lock_acquire(); block_io_barrier();
            for (unsigned i = 0; i < MD_MAX; i++) {
                struct md* a = arrays[i]; if (!a || !a->active || !a->sync) continue;
                lock(a);
                for (unsigned s = 0; s < 32 && a->sync; s++) sync_step(a, buffer);   /* 4 MiB, then let others run */
                if (a->sync) again = true;
                unlock(a);
            }
            kernel_lock_release();
            if (again) task_kernel_sleep(&resync_channel, native_clock_ns() + 1000000ULL);
        }
    }
}

/* ---- mdadm's ioctls ---- */
static struct md* md_of(struct block_device* b) { return b && b->ops == &md_ops ? b->driver : NULL; }
static int copy_in(void* to, uint64_t from, uint64_t size) { if (!accessible(from, size, false)) return -14; return copy_from_user(to, (const void*)from, size) ? -14 : 0; }
static int copy_out(uint64_t to, const void* from, uint64_t size) { if (!accessible(to, size, true)) return -14; return copy_to_user((void*)to, from, size) ? -14 : 0; }
static struct block_partition* partition_of(uint32_t major, uint32_t minor) {
    for (unsigned i = 0; i < block_partition_count(); i++) {
        struct block_partition* p = (struct block_partition*)block_partition_get(i);
        if (p && p->number && !p->dev->removed && p->major == major && p->minor == minor) return p;
    }
    for (unsigned i = 0; i < block_device_count(); i++) {
        struct block_device* d = block_device_get(i); if (!d || d->removed) continue;
        struct block_partition* w = block_whole(d); if (w && w->major == major && w->minor == minor) return w;
    }
    return NULL;
}
static uint32_t dev_major(uint64_t dev) { return (uint32_t)((dev >> 8) & 0xFFF) | (uint32_t)((dev >> 32) & ~0xFFFU); }
static uint32_t dev_minor(uint64_t dev) { return (uint32_t)(dev & 0xFF) | (uint32_t)((dev >> 12) & ~0xFFU); }
static int md_ioctl(struct block_device* b, uint64_t command, uint64_t argument) {
    struct md* a = md_of(b); if (!a || ((command >> 8) & 0xFF) != MD_MAJOR) return -25;
    if (!credentials_capable(credentials_current(), NULL, 21)) return -13;
    unsigned nr = command & 0xFF;
    switch (nr) {
    case 0x10: { int32_t v[3] = {0, 90, 3}; return copy_out(argument, v, sizeof(v)); }   /* RAID_VERSION */
    case 0x11: {                                                                        /* GET_ARRAY_INFO */
        if (!a->configured) return -6;
        int32_t v[18]; memset(v, 0, sizeof(v)); unsigned failed = 0, spares = 0, active = 0;
        for (unsigned k = 0; k < a->members; k++) { const struct member* x = &a->m[k]; if (x->faulty) failed++; else if (in_sync(a, x)) active++; else if (!x->kicked) spares++; }
        v[0] = 1; v[1] = (int32_t)a->minor_version; v[3] = (int32_t)a->ctime; v[4] = a->level; v[5] = (int32_t)(a->size / 2);
        v[6] = (int32_t)a->members; v[7] = (int32_t)a->raid_disks; v[8] = (int32_t)a->number; v[10] = (int32_t)(native_realtime_ns() / 1000000000ULL);
        v[11] = (a->dirty ? 0 : 1) | (a->bitmap ? 1 << 8 : 0); v[12] = (int32_t)active; v[13] = (int32_t)(active + spares); v[14] = (int32_t)failed; v[15] = (int32_t)spares;
        v[16] = (int32_t)a->layout; v[17] = (int32_t)(a->chunk * 512);
        return copy_out(argument, v, sizeof(v));
    }
    case 0x12: {                                                                        /* GET_DISK_INFO */
        int32_t v[5]; int e = copy_in(v, argument, sizeof(v)); if (e) return e;
        const struct member* x = NULL;
        for (unsigned k = 0; k < a->members; k++) if ((int32_t)a->m[k].desc == v[0] && !a->m[k].kicked) x = &a->m[k];
        if (!x) { v[1] = v[2] = 0; v[3] = -1; v[4] = 1 << 3; }
        else { v[1] = (int32_t)x->part->major; v[2] = (int32_t)x->part->minor; v[3] = usable(a, x) ? x->role : -1;
               v[4] = x->faulty ? 1 : in_sync(a, x) ? (1 << 1 | 1 << 2) : 0; }
        return copy_out(argument, v, sizeof(v));
    }
    case 0x15: { static const uint8_t none[4096]; return copy_out(argument, none, sizeof(none)); }   /* GET_BITMAP_FILE: none */
    case 0x23: {                                                                        /* SET_ARRAY_INFO */
        if (a->active) return -16;
        int32_t v[18]; int e = copy_in(v, argument, sizeof(v)); if (e) return e;
        if (v[0] != 1 || v[1] < 0 || v[1] > 2) return -22;   /* version-1 superblocks only: the members carry the rest */
        release_members(a); memset(a->uuid, 0, 16); a->configured = false; a->minor_version = (uint32_t)v[1];
        return 0;
    }
    case 0x21: {                                                                        /* ADD_NEW_DISK */
        int32_t v[5]; int e = copy_in(v, argument, sizeof(v)); if (e) return e;
        struct block_partition* p = partition_of((uint32_t)v[1], (uint32_t)v[2]); if (!p) return -6;
        if (!a->active) return add_member(a, p);
        lock(a);   /* a running array: a spare, rebuilt into a missing role */
        e = -22;
        if (a->members < MD_MEMBERS && !p->holders) {
            struct member* x = &a->m[a->members]; unsigned minor;
            if (sb_read(p, x, &minor) && !memcmp(x->sb + 16, a->uuid, 16) && x->data_size >= a->size) {
                int old = (v[4] & (1 << 2)) && v[3] >= 0 && v[3] < (int)a->raid_disks ? v[3] : -1;   /* MD_DISK_SYNC: a re-add into its old role */
                if (old >= 0 && role_member(a, (unsigned)old)) old = -1;
                bool partial = old >= 0 && a->bitmap && x->events >= a->bm_cleared && x->events <= a->events;
                x->role = partial ? old : -1; x->faulty = false; x->kicked = false; x->recovery_offset = partial ? 0 : NO_SECTOR; x->bm_recovery = partial;
                p->holders++; block_get(p->dev); x->held = true; a->members++; a->events++;
                say_member(a, x, partial ? "re-added: recovery of the bitmap's dirty chunks" : "added as a spare"); write_supers(a); start_sync(a); e = 0;
            }
        }
        unlock(a);
        return e;
    }
    case 0x22: {                                                                        /* HOT_REMOVE_DISK (dev_t) */
        lock(a); int e = -6;
        for (unsigned k = 0; k < a->members; k++) {
            struct member* x = &a->m[k];
            if (x->part->major != dev_major(argument) || x->part->minor != dev_minor(argument)) continue;
            if (usable(a, x) && in_sync(a, x)) { e = -16; break; }   /* only faulty or spare members */
            if (x->held) { if (x->part->holders) x->part->holders--; block_put(x->part->dev); }
            say_member(a, x, "removed");
            a->m[k] = a->m[--a->members]; a->events++; write_supers(a); e = 0; break;
        }
        unlock(a); return e;
    }
    case 0x29: {                                                                        /* SET_DISK_FAULTY (dev_t) */
        lock(a); int e = -6;
        for (unsigned k = 0; k < a->members; k++) {
            struct member* x = &a->m[k];
            if (x->part->major == dev_major(argument) && x->part->minor == dev_minor(argument)) { fail_member(a, x, "marked faulty"); e = 0; break; }
        }
        unlock(a); return e;
    }
    case 0x30: { int e = run_array(a); if (!e) announce(a); return e; }               /* RUN_ARRAY */
    case 0x32: return stop_array(a, false, 1);                                          /* STOP_ARRAY */
    case 0x33: return stop_array(a, true, 1);                                           /* STOP_ARRAY_RO */
    case 0x34: if (!a->active) return -6; a->ro = false; start_sync(a); return 0;       /* RESTART_ARRAY_RW */
    case 0x28: return -22;                                                              /* HOT_ADD_DISK (0.90 only) */
    }
    return -25;
}
static const struct block_extra_ops md_extra = {.ioctl = md_ioctl};

/* ---- devices ---- */
static struct md* array_number(unsigned number) {   /* /dev/mdN, made on first use */
    if (number >= MD_MAX) return NULL;
    if (arrays[number]) return arrays[number];
    struct md* a = (void*)arena_alloc(sizeof(*a)); if (!a) return NULL;
    memset(a, 0, sizeof(*a)); a->number = number;
    struct block_device* b = &a->block;
    block_assign_name(b, "md", number, MD_MAJOR); strcpy(b->name, b->linux_name); strcpy(b->model, "Linux Software RAID");
    b->kind = "MD"; b->block_size = 512; b->blocks = 0; b->max_blocks = 128; b->ops = &md_ops; b->driver = a; b->extra = &md_extra;
    arrays[number] = a;
    if (block_hot_add(b) < 0) { arrays[number] = NULL; arena_free((uint64_t)a, sizeof(*a)); return NULL; }
    if (!thread_started) thread_started = task_kernel_thread(resync_main, "md_resync");
    return a;
}
void md_init(void) { }   /* blkdev_node.c storage_nodes_init: no array exists before it is used (Linux md_probe) */
bool md_probe(unsigned minor) { return array_number(minor) != NULL; }   /* blkdev_node.c: mknod of 9:N (Linux md_probe) */

/* ---- /proc/mdstat, sysfs ---- */
static void put(char* out, unsigned capacity, const char* text) { unsigned n = strlen(out); while (*text && n + 1 < capacity) out[n++] = *text++; out[n] = 0; }
static void put_number(char* out, unsigned capacity, uint64_t v) { char n[24] = ""; append_u64(n, v); put(out, capacity, n); }
unsigned md_proc_mdstat(char* out, unsigned capacity) {
    out[0] = 0; put(out, capacity, "Personalities : [raid0] [raid1] [raid6] [raid5] [raid4] [raid10] \n");
    for (unsigned i = 0; i < MD_MAX; i++) {
        struct md* a = arrays[i]; if (!a || !a->members) continue;
        put(out, capacity, a->block.linux_name); put(out, capacity, a->active ? (a->ro ? " : active (read-only) " : " : active ") : " : inactive ");
        if (a->active) { put(out, capacity, "raid"); put_number(out, capacity, (uint64_t)a->level); }
        for (unsigned k = a->members; k-- > 0; ) {
            const struct member* x = &a->m[k]; if (x->kicked) continue;
            put(out, capacity, " "); put(out, capacity, x->part->linux_name); put(out, capacity, "["); put_number(out, capacity, x->desc); put(out, capacity, "]");
            if (x->faulty) put(out, capacity, "(F)"); else if (x->role < 0) put(out, capacity, "(S)");
        }
        put(out, capacity, "\n      ");
        put_number(out, capacity, a->active ? a->block.blocks / 2 : 0); put(out, capacity, " blocks super 1."); put_number(out, capacity, a->minor_version);
        if (a->active && a->level == 0) { put(out, capacity, " "); put_number(out, capacity, a->chunk / 2); put(out, capacity, "k chunks"); }
        if (a->active && (a->level == 5 || a->level == 6)) {
            put(out, capacity, " level "); put_number(out, capacity, (uint64_t)a->level); put(out, capacity, ", "); put_number(out, capacity, a->chunk / 2);
            put(out, capacity, "k chunk, algorithm "); put_number(out, capacity, a->layout);
        }
        if (a->active && a->level == 10) { put(out, capacity, " "); put_number(out, capacity, a->chunk / 2); put(out, capacity, "K chunks "); put_number(out, capacity, a->near); put(out, capacity, " near-copies"); }
        if (a->active && a->level >= 1) {
            put(out, capacity, " ["); put_number(out, capacity, a->raid_disks); put(out, capacity, "/"); put_number(out, capacity, working(a)); put(out, capacity, "] [");
            for (unsigned r = 0; r < a->raid_disks; r++) {
                bool up = false; for (unsigned k = 0; k < a->members; k++) if (in_sync(a, &a->m[k]) && a->m[k].role == (int)r) up = true;
                put(out, capacity, up ? "U" : "_");
            }
            put(out, capacity, "]");
        }
        put(out, capacity, "\n");
        if (a->active && a->sync) {
            uint64_t per_mille = sync_end(a) ? a->sync_pos * 1000 / sync_end(a) : 0, bars = per_mille / 50;
            put(out, capacity, "      [");
            for (unsigned k = 0; k < 20; k++) put(out, capacity, k < bars ? "=" : k == bars ? ">" : ".");
            put(out, capacity, a->sync == 1 ? "]  resync = " : "]  recovery = "); put_number(out, capacity, per_mille / 10); put(out, capacity, ".");
            put_number(out, capacity, per_mille % 10); put(out, capacity, "% ("); put_number(out, capacity, a->sync_pos / 2); put(out, capacity, "/");
            put_number(out, capacity, sync_end(a) / 2); put(out, capacity, ") finish=0.1min speed=100000K/sec\n");
        }
        if (a->active && a->bitmap) {
            uint64_t dirty = 0; for (uint64_t b = 0; b < a->bm_bits; b++) dirty += a->bm[256 + b / 8] >> (b % 8) & 1;
            put(out, capacity, "      bitmap: "); put_number(out, capacity, dirty ? 1 : 0); put(out, capacity, "/1 pages [");
            put_number(out, capacity, dirty ? 4 : 0); put(out, capacity, "KB], "); put_number(out, capacity, a->bm_chunk / 2); put(out, capacity, "KB chunk\n");
        }
        put(out, capacity, "      \n");
    }
    put(out, capacity, "unused devices: <none>\n");
    return strlen(out);
}
/* /sys/block/mdN/md/<attr> (blkdev_node.c): level raid_disks chunk_size layout component_size metadata_version
 * array_state array_size degraded sync_action sync_completed resync_start uuid */
bool md_sysfs_attr(struct block_device* dev, unsigned index, char* text) {
    struct md* a = md_of(dev); if (!a) return false;
    char n[64] = "";
    switch (index) {
    case 0: if (a->configured) { strcat(text, "raid"); append_u64(text, (uint64_t)a->level); } return true;
    case 1: append_u64(n, a->raid_disks); break;
    case 2: append_u64(n, (uint64_t)a->chunk * 512); break;
    case 3: append_u64(n, a->layout); break;
    case 4: append_u64(n, a->size / 2); break;
    case 5: if (a->configured) { strcpy(n, "1."); append_u64(n, a->minor_version); } else strcpy(n, "none"); break;
    case 6: strcpy(n, !a->configured ? "clear" : !a->active ? "inactive" : a->ro ? "readonly" : a->dirty ? "active" : "clean"); break;
    case 7: append_u64(n, a->active ? a->block.blocks / 2 : 0); break;
    case 8: append_u64(n, a->active && a->level == 1 ? a->raid_disks - working(a) : 0); break;
    case 9: strcpy(n, !a->active ? "idle" : a->sync == 1 ? "resync" : a->sync == 2 ? "recover" : "idle"); break;
    case 10: if (a->sync) { append_u64(n, a->sync_pos); strcat(n, " / "); append_u64(n, sync_end(a)); } else strcpy(n, "none"); break;
    case 11: if (a->recovery_cp == NO_SECTOR) strcpy(n, "none"); else append_u64(n, a->recovery_cp); break;
    case 12: for (unsigned i = 0; i < 16; i++) { if (i == 4 || i == 8 || i == 12) strcat(n, ":"); append_hex(n, a->uuid[i], 2); } break;
    case 13: strcpy(n, !a->configured || !a->level ? "none" : a->bitmap ? "bitmap" : "resync"); break;
    default: return false;
    }
    strcat(text, n); return true;
}
/* /sys/block/mdN/md/dev-<member>/{slot,state,offset,size,errors,recovery_start} (kicked members are not bound) */
static struct member* listed(struct md* a, unsigned index) {
    for (unsigned k = 0; k < a->members; k++) if (!a->m[k].kicked && !index--) return &a->m[k];
    return NULL;
}
unsigned md_sysfs_members(struct block_device* dev, struct block_partition** out, unsigned capacity) {
    struct md* a = md_of(dev); unsigned n = 0; struct member* x;
    while (a && n < capacity && (x = listed(a, n))) out[n++] = x->part;
    return n;
}
bool md_sysfs_member_attr(struct block_device* dev, unsigned member, unsigned index, char* text) {
    struct md* a = md_of(dev); struct member* x = a ? listed(a, member) : NULL; if (!x) return false;
    char n[64] = ""; bool rebuilding = a->sync == 2 && a->sync_target == (int)(x - a->m);
    switch (index) {
    case 0: if (usable(a, x)) append_u64(n, (uint64_t)x->role); else strcpy(n, "none"); break;
    case 1: strcpy(n, x->faulty ? "faulty" : in_sync(a, x) ? "in_sync" : "spare"); break;
    case 2: append_u64(n, x->data_offset); break;
    case 3: append_u64(n, x->data_size / 2); break;
    case 4: strcpy(n, "0"); break;
    case 5: if (in_sync(a, x) || x->faulty) strcpy(n, "none"); else append_u64(n, rebuilding ? a->sync_pos : x->recovery_offset == NO_SECTOR ? 0 : x->recovery_offset); break;
    default: return false;
    }
    strcat(text, n); return true;
}

/* ---- boot: assemble every complete (or RAID1-degraded) set ---- */
static struct block_partition* candidate(unsigned index) {
    unsigned parts = block_partition_count();
    if (index < parts) return (struct block_partition*)block_partition_get(index);
    struct block_device* d = block_device_get(index - parts);
    return d && !d->removed ? block_whole(d) : NULL;
}
static unsigned number_for(const char* name) {   /* "host:3" -> md3, else the first free slot */
    const char* colon = s_strrchr(name, ':'); const char* digits = colon ? colon + 1 : name; uint64_t v; const char* end;
    if (parse_u64(digits, &v, &end) && !*end && v < MD_MAX && (!arrays[v] || !arrays[v]->configured)) return (unsigned)v;
    for (unsigned i = 0; i < MD_MAX; i++) if (!arrays[i] || !arrays[i]->configured) return i;
    return MD_MAX;
}
static bool set_running(const uint8_t* uuid) {   /* a member left out (kicked, failed) must not start a second array */
    for (unsigned i = 0; i < MD_MAX; i++) if (arrays[i] && arrays[i]->configured && !memcmp(arrays[i]->uuid, uuid, 16)) return true;
    return false;
}
static unsigned assemble_all(void) {
    if (boot_option("md=0") || boot_option("rd.md=0") || boot_option("raid=noautodetect")) return 0;
    unsigned started = 0, refused_count = 0; struct member probe; unsigned minor; uint8_t refused[16][16];   /* sets tried once */
    unsigned total = block_partition_count() + block_device_count();
    for (unsigned i = 0; i < total; i++) {
        struct block_partition* p = candidate(i);
        if (!p || p->dev->removed || p->holders || (p->dev->kind && !strcmp(p->dev->kind, "MD")) || !sb_read(p, &probe, &minor)) continue;
        uint8_t uuid[16]; memcpy(uuid, probe.sb + 16, 16);
        if (set_running(uuid)) { vga_print("[64] MD "); vga_print(p->linux_name); vga_print(": its array is assembled already, left out\n"); continue; }
        bool tried = false; for (unsigned r = 0; r < refused_count; r++) if (!memcmp(refused[r], uuid, 16)) tried = true;
        if (tried) continue;
        char name[33]; memcpy(name, probe.sb + 32, 32); name[32] = 0;
        unsigned number = number_for(name);
        struct md* a = number < MD_MAX ? array_number(number) : NULL; if (!a) continue;
        lock(a);
        for (unsigned j = i; j < total; j++) {   /* every member of this set */
            struct block_partition* q = candidate(j);
            if (!q || q->dev->removed || q->holders || (q->dev->kind && !strcmp(q->dev->kind, "MD")) || !sb_read(q, &probe, &minor) || memcmp(probe.sb + 16, uuid, 16)) continue;
            if (add_member(a, q)) vga_print("[64] MD a member was refused\n");
        }
        int e = a->members ? run_array(a) : -22;
        if (e) { say(a, "not started"); release_members(a); a->configured = false; if (refused_count < 16) memcpy(refused[refused_count++], uuid, 16); }
        else started++;
        unlock(a);
        if (!e) announce(a);
    }
    return started;
}
/* kernel.c before cryptroot_boot: arrays, then the root on one (root=/dev/mdN or a filesystem UUID/LABEL) */
bool md_boot(void) {
    if (!assemble_all()) return false;
    char spec[96];
    if (!boot_value("root", spec, sizeof(spec))) return false;
    for (unsigned i = 0; i < MD_MAX; i++) {
        struct md* a = arrays[i]; if (!a || !a->active) continue;
        struct block_partition* w = block_whole(&a->block); char path[24] = "/dev/"; strcat(path, a->block.linux_name);
        if (!strcmp(spec, path) || block_spec_matches(w, spec) == 1) { block_unlocked_root = w; say(a, "holds the root"); return true; }
    }
    return false;
}
void md_boot_late(void) { assemble_all(); }   /* kernel.c, before LVM's late activation and /etc/fstab */
void md_shutdown(void) {   /* vfs.c disk_shutdown: every array clean on disk */
    for (unsigned i = 0; i < MD_MAX; i++) {
        struct md* a = arrays[i]; if (!a || !a->active || !a->dirty) continue;
        lock(a);
        if (a->sync == 1) a->recovery_cp = a->sync_pos;   /* a resync goes on from here at the next start */
        else if (a->sync == 2 && a->sync_target >= 0) a->m[a->sync_target].recovery_offset = a->sync_pos;
        a->dirty = false; a->events++; write_supers(a);   /* dirty first cleared: sb_update writes the resync point */
        if (!a->sync) bm_clear(a);
        unlock(a);
        say(a, "clean");
    }
}
#pragma clang attribute pop
