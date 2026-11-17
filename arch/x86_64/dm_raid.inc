/* dm-raid (Linux drivers/md/dm-raid.c, target 1.15.1) for lvm2's raid1 and mirror LVs (lvcreate --type raid1 -m N,
 * lvs Cpy%Sync / raid_sync_action / raid_mismatch_count, lvchange --syncaction check|repair, lvconvert --repair),
 * included by dm.c. RAID1 only (raid0/4/5/6/10 tables are refused: md.c serves those through mdadm).
 *
 * Table: "raid1 <#params> <chunk 0> [sync|nosync] [rebuild <i>]... [region_size <s>] [daemon_sleep|min/max_recovery_rate|
 * write_mostly|max_write_behind <n>] <#devs> (<meta dev>|- <data dev>|-)...". Each _rmeta device holds dm-raid's
 * superblock (magic "DmRd", version 1.9.0 layout: devices, position, events, failed devices, the device's recovery
 * offset, the array's resync offset, level, sizes); a zeroed one (a new LV: lvm2 wipes it) makes a new array that
 * resyncs from the start unless "nosync". The freshest superblock (events) wins; a member whose superblock is older, is
 * missing or failed in it, or named by "rebuild" is recovered from an in-sync one. I/O as md.c's RAID1: reads from an
 * in-sync member (another after an error, which fails the member), writes to every working member; the set is
 * marked dirty (array_resync_offset 0) at its first write and clean again after 200 ms idle (members flushed first)
 * and when the table goes, so a power cut leads to a resync, as Linux without a bitmap. A kernel thread "dm_raid" runs
 * resync, recovery, check (mismatches counted) and repair. Status as Linux: "raid1 <n> <A|a|D...> <done>/<size>
 * <idle|frozen|resync|recover|check|repair> <mismatches> 0 -". Messages: idle, frozen, resync, recover, check,
 * repair. Not done: the write-intent bitmap in _rmeta (resyncs are whole), write-mostly/write-behind, reshape,
 * takeover; lvm_boot.c does not activate raid1 LVs at boot (lvm2 does). */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */
#define RAIDSET_MAX 8
bool task_kernel_thread(void (*entry)(void), const char* name);
void task_kernel_sleep(void* channel, uint64_t deadline);
#define DMRAID_MAGIC 0x64526D44u
#define DMRAID_STEP 256u                /* sectors per sync step */
#define DMRAID_CHECKPOINT 16384u        /* sectors between superblock checkpoints */
#define DMRAID_NONE (~0ULL)             /* MaxSector: in sync */
enum { RA_IDLE, RA_FROZEN, RA_RESYNC, RA_RECOVER, RA_CHECK, RA_REPAIR };
struct rmember { struct block_partition* meta; struct block_partition* data; bool faulty, missing; uint64_t recovery, events; };
struct rset {
    struct rset* next; unsigned n; struct rmember m[RAIDSET_MAX];
    uint64_t sectors, resync, events, pos, mark, last_write, mismatches; uint32_t region;
    bool dirty, nosync, dying, ready; int action, target; uint8_t* buf; uint8_t* buf2;
    char params[160];   /* the table's parameters before the device list, as given */
    volatile int busy; unsigned users;
};
static struct rset* rsets; static volatile int rsets_lock; static int dmraid_channel; static bool dmraid_thread;
static void rset_lock(struct rset* r) {
    while (__atomic_exchange_n(&r->busy, 1, __ATOMIC_ACQUIRE)) {
        if (dm_may_wait()) kernel_io_sleep((void*)&r->busy, native_clock_ns() + 1000000ULL); else __asm__ volatile("pause");
    }
}
static void rset_unlock(struct rset* r) { __atomic_store_n(&r->busy, 0, __ATOMIC_RELEASE); task_wake_channel((void*)&r->busy); }
static bool rm_working(const struct rmember* x) { return x->data && !x->faulty && !x->missing; }
/* A suspended device below (lvm2 suspends the sub-LVs while it reloads): I/O there would wait for a resume that only
 * the caller can do, so superblock writes skip it and the thread leaves the set alone (Linux writes at presuspend). */
static bool part_suspended(const struct block_partition* p) { struct mapped* m = p ? mapped_of(p->dev) : NULL; return m && __atomic_load_n(&m->suspended, __ATOMIC_ACQUIRE); }
static bool rset_suspended(const struct rset* r) {
    for (unsigned i = 0; i < r->n; i++) if (rm_working(&r->m[i]) && (part_suspended(r->m[i].data) || part_suspended(r->m[i].meta))) return true;
    return false;
}
static bool rm_in_sync(const struct rmember* x) { return rm_working(x) && x->recovery == DMRAID_NONE; }
static void rset_write_supers(struct rset* r) {   /* dm-raid superblock (sector 0 of every working _rmeta) */
    uint64_t failed = 0; for (unsigned i = 0; i < r->n; i++) if (!rm_working(&r->m[i])) failed |= 1ULL << i;
    uint8_t* sb = r->buf2;
    for (unsigned i = 0; i < r->n; i++) {
        struct rmember* x = &r->m[i]; if (!rm_working(x) || !x->meta || part_suspended(x->meta)) continue;
        memset(sb, 0, 512);
        put_le32(sb, DMRAID_MAGIC); put_le32(sb + 4, 1); put_le32(sb + 8, r->n); put_le32(sb + 12, i);
        put_le64(sb + 16, r->events); put_le64(sb + 24, failed); put_le64(sb + 32, x->recovery);
        put_le64(sb + 40, r->dirty ? 0 : r->resync); put_le32(sb + 48, 1);
        put_le32(sb + 72, 1); put_le64(sb + 88, r->sectors); put_le64(sb + 112, r->sectors);
        if (block_part_write(x->meta, 0, 1, sb) < 0 || block_flush(x->meta->dev) < 0) { x->faulty = true; vga_print("[64] DM raid: metadata write failed, member faulty\n"); }
        x->events = r->events;
    }
}
static void rset_fail(struct rset* r, struct rmember* x, const char* why) {
    if (x->faulty) return;
    x->faulty = true; r->events++;
    char line[96] = "[64] DM raid: member "; put_number(line, (uint64_t)(x - r->m)); strcat(line, " "); strcat(line, why); strcat(line, "\n"); vga_print(line);
    if (r->action == RA_RECOVER && r->target == (int)(x - r->m)) r->action = RA_IDLE;
    rset_write_supers(r);
}
static void rset_start(struct rset* r) {   /* resync first (a dirty or new set), then recovery of a member */
    if (r->action == RA_FROZEN || r->action == RA_CHECK || r->action == RA_REPAIR) return;
    unsigned in = 0; for (unsigned i = 0; i < r->n; i++) in += rm_in_sync(&r->m[i]);
    if (!in) return;
    if (r->resync != DMRAID_NONE && in > 1) { r->action = RA_RESYNC; r->pos = r->mark = r->resync; }
    else for (unsigned i = 0; i < r->n && r->action == RA_IDLE; i++)
        if (rm_working(&r->m[i]) && r->m[i].recovery != DMRAID_NONE) { r->action = RA_RECOVER; r->target = (int)i; r->pos = r->mark = r->m[i].recovery; }
    if (r->action != RA_IDLE) task_wake_channel(&dmraid_channel);
}
static int source_of(struct rset* r, int not) { for (unsigned i = 0; i < r->n; i++) if ((int)i != not && rm_in_sync(&r->m[i])) return (int)i; return -1; }
static void rset_step(struct rset* r) {   /* one piece of the running sync action (set locked) */
    uint64_t n = r->sectors - r->pos < DMRAID_STEP ? r->sectors - r->pos : DMRAID_STEP;
    int src = source_of(r, r->action == RA_RECOVER ? r->target : -1);
    if (src < 0) { r->action = RA_IDLE; return; }
    if (n && block_part_read(r->m[src].data, r->pos, (uint32_t)n, r->buf) < 0) { rset_fail(r, &r->m[src], "read error during sync: faulty"); return; }
    for (unsigned i = 0; n && i < r->n; i++) {
        struct rmember* x = &r->m[i];
        if ((int)i == src || !rm_working(x) || (r->action == RA_RECOVER && (int)i != r->target)) continue;
        if (r->action == RA_CHECK || r->action == RA_REPAIR) {
            if (block_part_read(x->data, r->pos, (uint32_t)n, r->buf2) < 0) { rset_fail(r, x, "read error during check: faulty"); continue; }
            bool same = !memcmp(r->buf, r->buf2, n * 512);
            if (same) continue;
            r->mismatches += n;
            if (r->action == RA_CHECK) continue;
        }
        if (block_part_write(x->data, r->pos, (uint32_t)n, r->buf) < 0) rset_fail(r, x, "write error during sync: faulty");
    }
    r->pos += n;
    bool done = r->pos >= r->sectors;
    if (r->action == RA_CHECK) { if (done) { vga_print("[64] DM raid: check done\n"); r->action = RA_IDLE; } return; }
    if (done || r->pos - r->mark >= DMRAID_CHECKPOINT) {
        r->mark = r->pos;
        if (r->action == RA_RESYNC || r->action == RA_REPAIR) r->resync = done ? DMRAID_NONE : r->pos;
        else r->m[r->target].recovery = done ? DMRAID_NONE : r->pos;
        if (done) r->events++;
        rset_write_supers(r);
        if (done) {
            vga_print(r->action == RA_RESYNC ? "[64] DM raid: resync done\n" : r->action == RA_REPAIR ? "[64] DM raid: repair done\n" : "[64] DM raid: recovery done\n");
            r->action = RA_IDLE; rset_start(r);
        }
    }
}
static void rset_destroy(struct rset* r);
static void dmraid_main(void) {
    for (;;) {
        task_kernel_sleep(&dmraid_channel, native_clock_ns() + 200000000ULL);
        for (unsigned round = 0; round < 1000000; round++) {
            struct rset* r = NULL; uint64_t now = native_clock_ns();
            spin_acquire(&rsets_lock);
            for (struct rset* x = rsets; x && !r; x = x->next)
                if (!x->dying && !rset_suspended(x) && ((x->action != RA_IDLE && x->action != RA_FROZEN) || (x->dirty && now - x->last_write >= 200000000ULL))) { r = x; r->users++; }
            spin_release(&rsets_lock);
            if (!r) break;
            kernel_lock_acquire(); block_io_barrier();
            rset_lock(r);
            if (r->action != RA_IDLE && r->action != RA_FROZEN) for (unsigned s = 0; s < 32 && r->action != RA_IDLE && r->action != RA_FROZEN; s++) rset_step(r);
            else if (r->dirty && native_clock_ns() - r->last_write >= 200000000ULL) {   /* idle: clean on disk again */
                for (unsigned i = 0; i < r->n; i++) if (rm_working(&r->m[i])) block_flush(r->m[i].data->dev);
                r->dirty = false; r->events++; rset_write_supers(r);
            }
            rset_unlock(r);
            kernel_lock_release();
            spin_acquire(&rsets_lock); r->users--; bool destroy = r->dying && !r->users; spin_release(&rsets_lock);
            if (destroy) rset_destroy(r);
            task_kernel_sleep(&dmraid_channel, native_clock_ns() + 1000000ULL);
        }
    }
}
static int raid1_dm_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {
    struct rset* r = g->rset; int e = -1;
    rset_lock(r);
    if (!write) {
        for (unsigned i = 0; i < r->n && e; i++) {
            struct rmember* x = &r->m[i];
            if (!rm_working(x) || (x->recovery != DMRAID_NONE && relative + count > x->recovery)) continue;
            if (r->action == RA_RESYNC && r->resync != DMRAID_NONE && source_of(r, -1) != (int)i && relative + count > r->pos) continue;   /* resync: the source */
            if (block_part_read(x->data, relative, count, data) == 0) e = 0; else rset_fail(r, x, "read error: faulty");
        }
    } else {
        if (!r->dirty) { r->dirty = true; r->events++; rset_write_supers(r); }
        r->last_write = native_clock_ns();
        for (unsigned i = 0; i < r->n; i++) {
            struct rmember* x = &r->m[i]; if (!rm_working(x)) continue;
            if (block_part_write(x->data, relative, count, data) == 0) { if (x->recovery == DMRAID_NONE) e = 0; } else rset_fail(r, x, "write error: faulty");
        }
    }
    rset_unlock(r);
    return e;
}
static int parse_raid(struct target* g, char* params) {
    char* c = params; char* type = word(&c); char* count_text = word(&c); uint64_t count, v; const char* end;
    if (!type || !count_text || !parse_u64(count_text, &count, &end) || *end || count > 32) return -22;
    if (strcmp(type, "raid1")) return -22;   /* raid0/4/5/6/10 through dm-raid: not done (mdadm and md.c serve them) */
    struct rset* r = (void*)arena_alloc(sizeof(*r)); if (!r) return -12;
    memset(r, 0, sizeof(*r)); g->rset = r; r->region = 1024; r->target = -1;
    bool rebuild[RAIDSET_MAX] = {0};
    strcpy(r->params, "raid1 "); append_u64(r->params, count);
    for (uint64_t i = 0; i < count; i++) {
        char* o = word(&c); if (!o) return -22;
        if (strlen(r->params) + strlen(o) + 2 < sizeof(r->params)) { strcat(r->params, " "); strcat(r->params, o); }
        if (i == 0) { if (!parse_u64(o, &v, &end) || *end) return -22; continue; }   /* chunk size: 0 for raid1 */
        if (!strcmp(o, "sync")) continue;
        if (!strcmp(o, "nosync")) { r->nosync = true; continue; }
        static const char* const valued[7] = {"rebuild", "daemon_sleep", "min_recovery_rate", "max_recovery_rate", "write_mostly", "max_write_behind", "region_size"};
        int k = -1; for (unsigned q = 0; q < 7; q++) if (!strcmp(o, valued[q])) k = (int)q;
        char* arg = k >= 0 && i + 1 < count ? word(&c) : NULL;
        if (!arg || !parse_u64(arg, &v, &end) || *end) return -22;   /* stripe_cache, raid10_*, data_offset, delta_disks, journal_*: not raid1 */
        if (strlen(r->params) + strlen(arg) + 2 < sizeof(r->params)) { strcat(r->params, " "); strcat(r->params, arg); }
        i++;
        if (k == 0) { if (v >= RAIDSET_MAX) return -22; rebuild[v] = true; }
        if (k == 6) { if (!v || (v & (v - 1))) return -22; r->region = (uint32_t)v; }
    }
    char* devs_text = word(&c); uint64_t devs;
    if (!devs_text || !parse_u64(devs_text, &devs, &end) || *end || devs < 1 || devs > RAIDSET_MAX) return -22;
    r->n = (unsigned)devs; r->sectors = g->length;
    unsigned slot = 0;
    for (unsigned i = 0; i < r->n; i++) {
        char* meta = word(&c); char* data = word(&c); if (!meta || !data) return -22;
        struct rmember* x = &r->m[i];
        if (strcmp(meta, "-")) { if (!(x->meta = device_of(meta))) return -6; }
        if (strcmp(data, "-")) { if (!(x->data = device_of(data))) return -6; } else x->missing = true;
        if (x->data && block_part_sectors(x->data) < r->sectors) return -22;
        struct block_partition* list[2] = {x->data, x->meta};
        for (unsigned k = 0; k < 2; k++) if (list[k]) { if (!g->under) g->under = list[k]; else { if (slot == STRIPES) return -22; g->more[slot++] = list[k]; } }
    }
    g->extra = slot;
    if (word(&c)) return -22;
    if (!(r->buf = (void*)arena_alloc(DMRAID_STEP * 512)) || !(r->buf2 = (void*)arena_alloc(DMRAID_STEP * 512))) return -12;
    uint64_t best = 0; int fresh = -1; uint8_t* sb = r->buf;   /* the superblocks */
    for (unsigned i = 0; i < r->n; i++) {
        struct rmember* x = &r->m[i]; x->recovery = DMRAID_NONE; x->events = 0;
        if (!x->meta || !x->data) continue;
        if (block_part_read(x->meta, 0, 1, sb) < 0) { x->faulty = true; continue; }
        if (get_le32(sb) != DMRAID_MAGIC) continue;
        if (get_le32(sb + 48) != 1 || !get_le32(sb + 8) || get_le32(sb + 8) > 64) { vga_print("[64] DM raid: superblock of another level, member rebuilt\n"); continue; }
        x->events = get_le64(sb + 16);
        if (fresh < 0 || x->events > best) { best = x->events; fresh = (int)i; }
    }
    if (fresh < 0) {   /* a new set: in sync with nosync, else a resync from the start */
        r->events = 1; r->resync = r->nosync ? DMRAID_NONE : 0;
    } else {
        block_part_read(r->m[fresh].meta, 0, 1, sb);
        r->events = best; r->resync = get_le64(sb + 40); uint64_t failed = get_le64(sb + 24);
        for (unsigned i = 0; i < r->n; i++) {
            struct rmember* x = &r->m[i]; if (!x->data || x->missing) continue;
            uint8_t own[512]; uint64_t own_recovery = 0;
            if (x->meta && block_part_read(x->meta, 0, 1, own) == 0 && get_le32(own) == DMRAID_MAGIC && get_le32(own + 48) == 1) own_recovery = get_le64(own + 32);
            else { x->recovery = 0; continue; }   /* a new leg (lvconvert -m +1, --repair): recovered */
            bool same_layout = get_le32(sb + 8) == r->n;   /* positions only mean something for the same leg count */
            if (x->events < best || (same_layout && (failed >> i & 1))) x->recovery = 0;   /* stale, new, or failed in the freshest view: rebuilt */
            else x->recovery = own_recovery;
        }
    }
    for (unsigned i = 0; i < r->n; i++) if (rebuild[i] && r->m[i].data) r->m[i].recovery = 0;
    bool any = false; for (unsigned i = 0; i < r->n; i++) if (rm_in_sync(&r->m[i])) any = true;
    if (!any) { vga_print("[64] DM raid: no in-sync member\n"); return -22; }
    rset_write_supers(r); r->ready = true;
    spin_acquire(&rsets_lock); r->next = rsets; rsets = r; spin_release(&rsets_lock);
    if (!dmraid_thread) dmraid_thread = task_kernel_thread(dmraid_main, "dm_raid");
    rset_start(r);
    return 0;
}
static void rset_destroy(struct rset* r) {
    if (r->buf && r->ready) {   /* clean on disk, the sync position kept */
        if (r->action == RA_RESYNC || r->action == RA_REPAIR) r->resync = r->pos;
        else if (r->action == RA_RECOVER && r->target >= 0) r->m[r->target].recovery = r->pos;
        for (unsigned i = 0; i < r->n; i++) if (rm_working(&r->m[i]) && !part_suspended(r->m[i].data)) block_flush(r->m[i].data->dev);
        if (r->dirty) { r->dirty = false; r->events++; }
        rset_write_supers(r);
    }
    if (r->buf) arena_free((uint64_t)r->buf, DMRAID_STEP * 512);
    if (r->buf2) arena_free((uint64_t)r->buf2, DMRAID_STEP * 512);
    arena_free((uint64_t)r, sizeof(*r));
}
static void raid_free(struct rset* r) {   /* the table goes: now, or by the dm_raid thread when it lets go of the set */
    if (!r) return;
    spin_acquire(&rsets_lock);
    for (struct rset** q = &rsets; *q; q = &(*q)->next) if (*q == r) { *q = r->next; break; }
    r->dying = true; bool now = !r->users;
    spin_release(&rsets_lock);
    if (now) rset_destroy(r);
}
static void raid_presuspend(struct target* g) {   /* dm suspend of the raid LV: clean on disk while the sub-LVs are live */
    struct rset* r = g->rset; rset_lock(r);
    if (r->action == RA_RESYNC || r->action == RA_REPAIR) r->resync = r->pos;
    else if (r->action == RA_RECOVER && r->target >= 0) r->m[r->target].recovery = r->pos;
    if (r->dirty) {
        for (unsigned i = 0; i < r->n; i++) if (rm_working(&r->m[i]) && !part_suspended(r->m[i].data)) block_flush(r->m[i].data->dev);
        r->dirty = false; r->events++;
    }
    rset_write_supers(r);
    rset_unlock(r);
}
static int raid_message(struct target* g, char* text) {
    struct rset* r = g->rset; char* c = text; char* cmd = word(&c);
    if (!cmd || word(&c)) return -22;
    rset_lock(r);
    int e = 0;
    if (!strcmp(cmd, "idle") || !strcmp(cmd, "frozen")) {
        if (r->action == RA_RESYNC || r->action == RA_REPAIR) r->resync = r->pos;
        else if (r->action == RA_RECOVER) r->m[r->target].recovery = r->pos;
        r->action = !strcmp(cmd, "frozen") ? RA_FROZEN : RA_IDLE;
        if (r->action == RA_IDLE) rset_start(r);
    } else if (!strcmp(cmd, "resync") || !strcmp(cmd, "recover")) { if (r->action == RA_FROZEN) r->action = RA_IDLE; if (!strcmp(cmd, "resync")) r->resync = 0; rset_start(r); }
    else if (!strcmp(cmd, "check") || !strcmp(cmd, "repair")) {
        if (r->action != RA_IDLE && r->action != RA_FROZEN) e = -16;
        else { r->action = !strcmp(cmd, "check") ? RA_CHECK : RA_REPAIR; r->pos = r->mark = 0; r->mismatches = 0; task_wake_channel(&dmraid_channel); }
    } else e = -22;
    rset_unlock(r);
    return e;
}
static void raid_status(const struct target* g, bool table, char* text) {
    struct rset* r = g->rset;
    if (table) {
        strcat(text, r->params); strcat(text, " "); put_number(text, r->n);
        for (unsigned i = 0; i < r->n; i++) {
            const struct rmember* x = &r->m[i];
            strcat(text, " "); if (x->meta) { put_number(text, x->meta->major); strcat(text, ":"); put_number(text, x->meta->minor); } else strcat(text, "-");
            strcat(text, " "); if (x->data && !x->missing) { put_number(text, x->data->major); strcat(text, ":"); put_number(text, x->data->minor); } else strcat(text, "-");
        }
        return;
    }
    strcat(text, "raid1 "); put_number(text, r->n); strcat(text, " ");
    for (unsigned i = 0; i < r->n; i++) {
        const struct rmember* x = &r->m[i];
        char ch[2] = {!rm_working(x) ? 'D' : (r->action == RA_RESYNC || x->recovery != DMRAID_NONE) ? 'a' : 'A', 0}; strcat(text, ch);
    }
    uint64_t done = r->action == RA_IDLE || r->action == RA_FROZEN ? (r->resync != DMRAID_NONE ? r->resync : r->sectors) : r->pos;
    for (unsigned i = 0; (r->action == RA_IDLE || r->action == RA_FROZEN) && i < r->n; i++) if (rm_working(&r->m[i]) && r->m[i].recovery != DMRAID_NONE && r->m[i].recovery < done) done = r->m[i].recovery;
    static const char* const actions[6] = {"idle", "frozen", "resync", "recover", "check", "repair"};
    strcat(text, " "); put_number(text, done); strcat(text, "/"); put_number(text, r->sectors);
    strcat(text, " "); strcat(text, actions[r->action]); strcat(text, " "); put_number(text, r->mismatches); strcat(text, " 0 -");
}
#pragma clang attribute pop
