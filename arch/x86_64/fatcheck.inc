/* FAT32 root volume: dirty tracking and the boot-time check/repair (native kernel; included at the end of
 * kernel/fat32.c, so it uses that driver's state and sector helpers directly).
 *
 * Dirty tracking (Windows/Linux semantics): the first write after the mount clears the clean-shutdown bit of FAT[1]
 * in every FAT copy (written through and flushed before that write), and fat32_unmount_clean() sets it again at an
 * orderly power-off/reboot after the pending FAT sectors, FSInfo and every orphan (open-unlinked file, disposed by
 * vfs.c disk_shutdown) are on the disk. A crash, a power cut or a killed VM leaves the bit clear.
 *
 * Boot check (fat32_boot_check, from fat32_init): a volume found dirty (FAT[1] clean bit clear in any copy, or the
 * boot sector's Linux dirty bit) is checked before the mount, like fsck.fat -a / chkdsk:
 *   FAT copies that differ (the copy with fewer invalid entries wins, the first on a tie; all copies rewritten),
 *   FAT entries outside the volume (-> end of chain), the directory tree (chains leaving the volume, running into a
 *   free/bad cluster, looping or cross-linked: truncated; a file entry that starts on another file's first cluster
 *   is a duplicate left by an interrupted rename: removed), file sizes against chain lengths, directories with a
 *   size, '.'/'..', orphaned long-name slots, unpublished NexusOS staged entries (T#######.TMP), and finally lost
 *   clusters (allocated but unreachable: freed -- this is where the clusters of files that were still open but
 *   unlinked at a crash come back), FSInfo, the clean flags.
 * Kernel options: fsck.mode=auto (default: check when dirty) | force (always) | skip (never; a dirty volume is then
 * mounted read-only), fsck.repair=preen|yes (default: repair) | no (check only; any problem -> read-only).
 * Write order: directory sectors, FAT sectors (all copies), flush, FSInfo + clean flags, flush; an interrupted check
 * leaves the volume dirty and the next boot runs it again (every repair is idempotent).
 * Memory: the first FAT (4 bytes per cluster) and two bitmaps live in page-allocated two-level vectors for the
 * duration of the check only (an 8 GiB volume with 4 KiB clusters: ~8.5 MiB). */
#include "boot_info.h"

uint64_t page_alloc(void);
void page_free(uint64_t address);
uint64_t native_clock_ns(void);

#define FC_PTRS 512ULL
struct fc_vec { uint64_t* top; uint64_t pages; };   /* top page -> mid pages -> data pages (512 x 512 x 4 KiB) */
static struct fc_vec fc_fat, fc_claim, fc_head, fc_dirty, fc_stack, fc_hidden;
static struct {
    uint32_t files, dirs, lost, lost_chains, cross, bad, size, dirfix, lfn, pending, hidden, invalid, fatdiff, dup;
    uint64_t referenced, problems, free;
} fc;
static bool fc_repair, fc_failed;
static uint32_t fc_cur_lba, fc_messages, fc_stack_used, fc_hidden_used;
static uint8_t state_sector[512];   /* FAT[1] updates: may run inside volume_write of any other buffer */

static void fc_vec_free(struct fc_vec* v) {
    if (!v->top) return;
    for (uint64_t i = 0; i < FC_PTRS; i++) {
        uint64_t* mid = (uint64_t*)v->top[i]; if (!mid) continue;
        for (uint64_t j = 0; j < FC_PTRS; j++) if (mid[j]) page_free(mid[j]);
        page_free((uint64_t)mid);
    }
    page_free((uint64_t)v->top); v->top = NULL; v->pages = 0;
}
static bool fc_vec_page(struct fc_vec* v, uint64_t p) {   /* make data page p present (zeroed) */
    if (p >= FC_PTRS * FC_PTRS) return false;
    if (!v->top) { v->top = (uint64_t*)page_alloc(); if (!v->top) return false; memset(v->top, 0, 4096); }
    uint64_t* mid = (uint64_t*)v->top[p / FC_PTRS];
    if (!mid) { mid = (uint64_t*)page_alloc(); if (!mid) return false; memset(mid, 0, 4096); v->top[p / FC_PTRS] = (uint64_t)mid; }
    if (!mid[p % FC_PTRS]) {
        uint64_t page = page_alloc(); if (!page) return false;
        memset((void*)page, 0, 4096); mid[p % FC_PTRS] = page;
    }
    if (p >= v->pages) v->pages = p + 1;
    return true;
}
static bool fc_vec_alloc(struct fc_vec* v, uint64_t pages) {
    for (uint64_t p = 0; p < pages; p++) if (!fc_vec_page(v, p)) return false;
    return true;
}
static inline uint8_t* fc_page(const struct fc_vec* v, uint64_t p) { return (uint8_t*)((uint64_t*)v->top[p / FC_PTRS])[p % FC_PTRS]; }
static inline bool fc_bit(const struct fc_vec* v, uint64_t i) { return (fc_page(v, i >> 15)[(i >> 3) & 4095] >> (i & 7)) & 1; }
static inline void fc_bit_put(struct fc_vec* v, uint64_t i, bool on) {
    uint8_t* b = &fc_page(v, i >> 15)[(i >> 3) & 4095];
    if (on) *b = (uint8_t)(*b | (1u << (i & 7))); else *b = (uint8_t)(*b & ~(1u << (i & 7)));
}
static inline uint32_t* fc_entry(uint32_t c) { return &((uint32_t*)fc_page(&fc_fat, c >> 10))[c & 1023]; }
static inline uint32_t fc_get(uint32_t c) { return *fc_entry(c) & 0x0FFFFFFF; }
static void fc_set(uint32_t c, uint32_t value) {
    uint32_t* e = fc_entry(c), next = (*e & 0xF0000000) | (value & 0x0FFFFFFF);
    if (next == *e) return;
    *e = next; fc_bit_put(&fc_dirty, c / 128, true);
}
static bool fc_push(struct fc_vec* v, uint32_t* used, uint32_t a, uint32_t b) {   /* pairs of words */
    uint64_t at = (uint64_t)*used * 2;
    if (!fc_vec_page(v, at / 1024)) return false;
    uint32_t* w = (uint32_t*)fc_page(v, at / 1024); w[at % 1024] = a; w[at % 1024 + 1] = b; (*used)++;
    return true;
}
static void fc_pop(struct fc_vec* v, uint32_t* used, uint32_t* a, uint32_t* b) {
    uint64_t at = (uint64_t)--(*used) * 2; uint32_t* w = (uint32_t*)fc_page(v, at / 1024); *a = w[at % 1024]; *b = w[at % 1024 + 1];
}
static void fc_print_number(uint64_t n) {
    char digits[24]; int i = 0;
    do { digits[i++] = (char)('0' + n % 10); n /= 10; } while (n && i < 23);
    char out[24]; int k = 0; while (i) out[k++] = digits[--i]; out[k] = 0; vga_print(out);
}
static void fc_problem(const char* what, uint32_t a, uint32_t b) {
    fc.problems++;
    if (fc_messages++ >= 16) return;
    vga_print("[FAT32] check: "); vga_print(what); vga_print(" (directory cluster "); fc_print_number(a);
    vga_print(", slot "); fc_print_number(b); vga_print(fc_repair ? "): repaired\n" : ")\n");
}
/* Directory-slot write: the current cluster buffer (f57_clu at fc_cur_lba) or a read-modify-write of another sector. */
static void fc_patch(uint32_t lba, uint32_t off, const void* data, unsigned length) {
    if (lba >= fc_cur_lba && lba < fc_cur_lba + sectors_per_cluster) {
        uint8_t* sector = f57_clu + (lba - fc_cur_lba) * 512; memcpy(sector + off, data, length);
        if (fc_repair && volume_write(lba, 1, sector) < 0) fc_failed = true;
        return;
    }
    if (!fc_repair) return;
    if (volume_read(lba, 1, f57_sec) < 0) { fc_failed = true; return; }
    memcpy(f57_sec + off, data, length);
    if (volume_write(lba, 1, f57_sec) < 0) fc_failed = true;
}
static void fc_delete_slot(uint32_t lba, uint32_t off) { uint8_t mark = 0xE5; fc_patch(lba, off, &mark, 1); }
static void fc_put_le(uint32_t lba, uint32_t off, unsigned at, uint32_t value, unsigned bytes) {
    uint8_t raw[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    fc_patch(lba, off + at, raw, bytes);
}
static void fc_unclaim_from(uint32_t c, uint32_t limit) {   /* a chain this check claimed: back to unreachable */
    while (limit-- && f57_valid_cluster(c) && fc_bit(&fc_claim, c)) { fc_bit_put(&fc_claim, c, false); c = fc_get(c); }
}
/* Claim the chain from `first`, truncating it at the first link that leaves the volume, reaches a free/bad cluster
 * or one already claimed (cross-link or loop). 0: claimed (*count clusters); else why the first cluster is unusable:
 * 1 outside the volume, 2 free, 3 bad, 4 claimed by another chain, 5 another file's first cluster. */
static int fc_claim_chain(uint32_t first, uint32_t* count, uint32_t dir_cluster, uint32_t slot) {
    if (first < 2 || first >= cluster_limit) return 1;
    uint32_t v = fc_get(first);
    if (v == FAT32_FREE) return 2;
    if (v == FAT32_BAD) return 3;
    if (fc_bit(&fc_claim, first)) return fc_bit(&fc_head, first) ? 5 : 4;
    fc_bit_put(&fc_claim, first, true);
    uint32_t c = first, n = 1;
    for (;;) {
        v = fc_get(c);
        if (v >= 0x0FFFFFF8) break;
        bool cross = f57_valid_cluster(v) && fc_bit(&fc_claim, v);
        if (v == FAT32_FREE || v == FAT32_BAD || !f57_valid_cluster(v) || cross) {
            if (cross) { fc.cross++; fc_problem("chain cross-linked or looping; truncated", dir_cluster, slot); }
            else { fc.bad++; fc_problem("chain leaves the volume or reaches a free cluster; truncated", dir_cluster, slot); }
            fc_set(c, 0x0FFFFFFF); break;
        }
        fc_bit_put(&fc_claim, v, true); c = v; n++;
    }
    *count = n; return 0;
}
static bool fc_pending_name(const uint8_t* e) {
    if (e[0] != 'T' || e[8] != 'T' || e[9] != 'M' || e[10] != 'P') return false;
    for (int i = 1; i < 8; i++) if (e[i] < '0' || e[i] > '9') return false;
    return true;
}
/* One directory: its chain is claimed already. Subdirectories are pushed on fc_stack. */
static int fc_scan_dir(uint32_t first, uint32_t parent) {
    bool root = first == root_cluster, ended = false;
    uint32_t lfn_lba[24], lfn_off[24]; unsigned lfn_n = 0;
    uint8_t expected = 0, checksum = 0; bool sequence = false;   /* sequence: a long name started properly */
    uint32_t c = first, index = 0;
#define FC_ORPHANS() do { if (lfn_n) { fc.lfn++; fc_problem("orphaned long-name slots removed", first, index); \
        for (unsigned k_ = 0; k_ < lfn_n; k_++) fc_delete_slot(lfn_lba[k_], lfn_off[k_]); } lfn_n = 0; expected = 0; sequence = false; } while (0)
    for (uint32_t hops = dir_max_clusters(); hops-- && f57_valid_cluster(c); ) {
        uint32_t lba = cluster_to_lba(c);
        if (volume_read(lba, sectors_per_cluster, f57_clu) < 0) return -5;
        fc_cur_lba = lba;
        for (uint32_t i = 0; i < bytes_per_cluster / 32; i++, index++) {
            uint8_t* e = f57_clu + i * 32; uint32_t elba = lba + i / 16, eoff = (i % 16) * 32;
            uint32_t ec = ((uint32_t)e[21] << 24 | (uint32_t)e[20] << 16 | (uint32_t)e[27] << 8 | e[26]);
            uint32_t size = (uint32_t)e[28] | (uint32_t)e[29] << 8 | (uint32_t)e[30] << 16 | (uint32_t)e[31] << 24;
            if (ended) {   /* invisible to NexusOS and Windows, read by Linux: its clusters are never freed */
                if (e[0] && e[0] != 0xE5 && e[11] != ATTR_LFN && !(e[11] & ATTR_VOLUME_ID) && f57_valid_cluster(ec) &&
                    !fc_push(&fc_hidden, &fc_hidden_used, ec, first)) return -12;
                continue;
            }
            if (!e[0]) { FC_ORPHANS(); ended = true; continue; }
            if (e[0] == 0xE5) { FC_ORPHANS(); continue; }
            if (e[11] == ATTR_LFN) {
                uint8_t order = e[0] & 0x1F;
                if (e[0] & 0x40) { FC_ORPHANS(); if (order && order <= 20) { expected = order; checksum = e[13]; sequence = true; } }
                if (lfn_n == 24) FC_ORPHANS();
                lfn_lba[lfn_n] = elba; lfn_off[lfn_n] = eoff; lfn_n++;
                if (!sequence || !expected || order != expected || e[13] != checksum || e[12] || e[26] || e[27]) { sequence = false; expected = 0; continue; }
                expected--; continue;
            }
            if (e[11] & ATTR_VOLUME_ID) { FC_ORPHANS(); continue; }
            bool long_ok = lfn_n && sequence && !expected && checksum == lfn_checksum((const char*)e);
            if (lfn_n && !long_ok) {
                if (sequence && !expected && fc_pending_name(e)) {   /* fat32_begin_download never published: drop it */
                    fc.pending++; fc_problem("unpublished staged entry (interrupted symlink/download) removed", first, index);
                    for (unsigned k = 0; k < lfn_n; k++) fc_delete_slot(lfn_lba[k], lfn_off[k]);
                    fc_delete_slot(elba, eoff); lfn_n = 0; expected = 0; sequence = false;
                    continue;   /* its clusters are unreachable now: freed with the lost clusters */
                }
                FC_ORPHANS();
            }
            unsigned my_lfn = long_ok ? lfn_n : 0; uint32_t my_lba[24], my_off[24];
            for (unsigned k = 0; k < my_lfn; k++) { my_lba[k] = lfn_lba[k]; my_off[k] = lfn_off[k]; }
            lfn_n = 0; expected = 0; sequence = false;
            bool dot = !memcmp(e, ".          ", 11), dotdot = !memcmp(e, "..         ", 11);
            if (dot || dotdot) {
                if (root || index > 1 || !(e[11] & ATTR_DIRECTORY)) {
                    fc.dirfix++; fc_problem("misplaced '.' or '..' entry removed", first, index); fc_delete_slot(elba, eoff); continue;
                }
                uint32_t want = dot ? first : parent == root_cluster ? 0 : parent;
                if (ec != want) {
                    fc.dirfix++; fc_problem("'.' or '..' points elsewhere; fixed", first, index);
                    fc_put_le(elba, eoff, 20, want >> 16, 2); fc_put_le(elba, eoff, 26, want & 0xFFFF, 2);
                }
                continue;
            }
            if (e[11] & ATTR_DIRECTORY) {
                fc.dirs++;
                if (size) { fc.dirfix++; fc_problem("directory with a size; set to 0", first, index); fc_put_le(elba, eoff, 28, 0, 4); }
                uint32_t count = 0; int why = ec < 2 ? 2 : fc_claim_chain(ec, &count, first, index);
                if (why) {
                    fc.bad++; fc_problem("directory without usable clusters; entry removed", first, index);
                    for (unsigned k = 0; k < my_lfn; k++) fc_delete_slot(my_lba[k], my_off[k]);
                    fc_delete_slot(elba, eoff); continue;
                }
                if (!fc_push(&fc_stack, &fc_stack_used, ec, first)) return -12;
                continue;
            }
            fc.files++;
            uint32_t need = size / bytes_per_cluster + (size % bytes_per_cluster != 0);
            if (!ec) {
                if (size) { fc.size++; fc_problem("file with a size but no clusters; size set to 0", first, index); fc_put_le(elba, eoff, 28, 0, 4); }
                continue;
            }
            uint32_t count = 0; int why = fc_claim_chain(ec, &count, first, index);
            if (why == 5) {   /* the same file twice: an interrupted rename wrote the new name before erasing the old */
                fc.dup++; fc_problem("second entry of the same file (interrupted rename) removed", first, index);
                for (unsigned k = 0; k < my_lfn; k++) fc_delete_slot(my_lba[k], my_off[k]);
                fc_delete_slot(elba, eoff); continue;
            }
            if (why) {
                if (why == 4) fc.cross++; else fc.bad++;
                fc_problem("file starts on an unusable cluster; truncated to 0 bytes", first, index);
                fc_put_le(elba, eoff, 20, 0, 2); fc_put_le(elba, eoff, 26, 0, 2); fc_put_le(elba, eoff, 28, 0, 4); continue;
            }
            fc_bit_put(&fc_head, ec, true);
            if (count > need) {   /* the tail becomes unreachable (freed below unless another entry reaches it) */
                fc.size++; fc_problem("cluster chain longer than the file; truncated", first, index);
                if (!need) {
                    fc_bit_put(&fc_head, ec, false); fc_unclaim_from(ec, count);
                    fc_put_le(elba, eoff, 20, 0, 2); fc_put_le(elba, eoff, 26, 0, 2);
                } else {
                    uint32_t last = ec; for (uint32_t k = 1; k < need; k++) last = fc_get(last);
                    uint32_t tail = fc_get(last); fc_set(last, 0x0FFFFFFF); fc_unclaim_from(tail, count - need);
                }
            } else if (count < need) {
                fc.size++; fc_problem("file larger than its cluster chain; size shortened", first, index);
                fc_put_le(elba, eoff, 28, count * bytes_per_cluster, 4);
            }
        }
        uint32_t next = fc_get(c);
        if (next >= 0x0FFFFFF8) break;
        c = next;
    }
    FC_ORPHANS();
#undef FC_ORPHANS
    return fc_failed ? -5 : 0;
}
static uint32_t fc_invalid_entries(void) {
    uint32_t n = 0;
    for (uint32_t c = 2; c < cluster_limit; c++) {
        uint32_t v = fc_get(c);
        if (v && v != FAT32_BAD && v < 0x0FFFFFF8 && !f57_valid_cluster(v)) n++;
    }
    return n;
}
/* Load FAT copy `copy` (the entries of clusters 0..cluster_limit-1) into fc_fat; compare: true if equal (entries 0
 * and 1 -- media byte and state flags -- excluded). */
static int fc_load_fat(uint32_t copy, bool compare_only, bool* equal) {
    uint64_t sectors = ((uint64_t)cluster_limit * 4 + 511) / 512;
    if (sectors > bpb.fat_size_32) return -5;
    uint32_t base = fat_start_lba + copy * bpb.fat_size_32;
    if (equal) *equal = true;
    for (uint64_t s = 0; s < sectors; ) {
        uint32_t n = sectors - s > 128 ? 128 : (uint32_t)(sectors - s);
        if (volume_read_direct(base + (uint32_t)s, n, f57_clu) < 0) return -5;
        for (uint32_t k = 0; k < n; k++) {
            uint64_t sector = s + k; uint8_t* dst = fc_page(&fc_fat, sector / 8) + (sector % 8) * 512; const uint8_t* src = f57_clu + k * 512;
            if (compare_only) {
                if (sector == 0 ? memcmp(dst + 8, src + 8, 504) : memcmp(dst, src, 512)) { if (equal) *equal = false; }
            } else memcpy(dst, src, 512);
        }
        s += n;
    }
    return 0;
}
static int fc_write_fat(bool all) {   /* runs of up to 128 changed sectors, one command per run and FAT copy */
    uint64_t sectors = ((uint64_t)cluster_limit * 4 + 511) / 512;
    for (uint64_t s = 0; s < sectors; ) {
        if (!all && !fc_bit(&fc_dirty, s)) { s++; continue; }
        uint32_t run = 0;
        while (s + run < sectors && run < 128 && (all || fc_bit(&fc_dirty, s + run))) {
            memcpy(f57_clu + run * 512, fc_page(&fc_fat, (s + run) / 8) + ((s + run) % 8) * 512, 512); run++;
        }
        for (uint32_t copy = 0; copy < bpb.num_fats; copy++)
            if (volume_write(fat_start_lba + copy * bpb.fat_size_32 + (uint32_t)s, run, f57_clu) < 0) return -5;
        s += run;
    }
    return 0;
}
static int fc_flush(void) { return block_flush(volume_part->dev) < 0 ? -5 : 0; }
/* FAT[1] clean-shutdown (bit 27) and no-hard-error (bit 26) flags of every copy, written through. */
static int fat_state_flags(bool clean) {
    uint8_t* sector = state_sector;
    for (uint32_t copy = 0; copy < bpb.num_fats; copy++) {
        uint32_t lba = fat_start_lba + copy * bpb.fat_size_32;
        if (volume_read(lba, 1, sector) < 0) return -5;
        uint32_t state; memcpy(&state, sector + 4, 4);
        uint32_t next = clean ? state | FAT32_CLEAN | 0x04000000 : state & ~FAT32_CLEAN;
        if (next == state) continue;
        memcpy(sector + 4, &next, 4);
        if (VOLUME_DISK_WRITE(lba, 1, sector) < 0) { volume_cache_invalidate(lba, 1); return -5; }
        volume_cache_update(lba, 1, sector);
        if (copy == 0 && fat_cache_start == fat_start_lba) memcpy(fat_cache + 4, &next, 4);   /* keep the FAT window coherent */
    }
    return 0;
}
/* First write of a mounted session: mark the volume dirty before anything else reaches the disk. */
static int volume_mark_dirty(void) {
    volume_marked_dirty = true;
    if (fat_state_flags(false) < 0 || fc_flush() < 0) {
        vga_print("[FAT32] cannot mark the volume dirty; writes blocked\n"); writes_blocked = true; return -5;
    }
    return 0;
}
static int fsinfo_write(uint32_t free_count, uint32_t hint) {
    if (!bpb.fs_info || bpb.fs_info >= bpb.reserved_sectors) return 0;
    if (volume_read(bpb.fs_info, 1, f57_sec) < 0) return -5;
    uint32_t lead, mid; memcpy(&lead, f57_sec, 4); memcpy(&mid, f57_sec + 484, 4);
    if (lead != 0x41615252 || mid != 0x61417272) return 0;   /* no FSInfo: nothing to keep current */
    memcpy(f57_sec + 488, &free_count, 4); memcpy(f57_sec + 492, &hint, 4);
    return volume_write(bpb.fs_info, 1, f57_sec);
}
static int boot_dirty_flag(bool clear) {   /* Linux vfat: boot sector byte 0x41 bit 0 (FAT_STATE_DIRTY) */
    if (volume_read(0, 1, f57_sec) < 0) return -5;
    bool set = f57_sec[0x41] & 1;
    if (!clear || !set) return set;
    f57_sec[0x41] &= 0xFE;
    return volume_write(0, 1, f57_sec) < 0 ? -5 : 0;
}

/* 0 clean, 1 repaired, -1 problems left (check-only), -5 I/O error, -12 out of memory. */
static int fat32_volume_check(bool repair, bool dirty) {
    uint64_t started = native_clock_ns();
    memset(&fc, 0, sizeof(fc)); fc_repair = repair; fc_failed = false; fc_messages = 0; fc_stack_used = fc_hidden_used = 0;
    fc_cur_lba = 0xFFFFFFFFu;
    uint64_t fat_pages = ((uint64_t)cluster_limit * 4 + 4095) / 4096, bit_pages = ((uint64_t)cluster_limit + 32767) / 32768;
    uint64_t sector_bits = (((uint64_t)cluster_limit * 4 + 511) / 512 + 32767) / 32768;
    int result = -12;
    uint64_t memory_free_pages(void);
    if (fat_pages + 2 * bit_pages + sector_bits + 64 > memory_free_pages() / 2) goto out;   /* never squeeze the machine for it */
    if (!fc_vec_alloc(&fc_fat, fat_pages) || !fc_vec_alloc(&fc_claim, bit_pages) || !fc_vec_alloc(&fc_head, bit_pages) ||
        !fc_vec_alloc(&fc_dirty, sector_bits)) goto out;
    result = -5;
    bool equal = true;
    if (fc_load_fat(0, false, NULL) < 0) goto out;
    for (uint32_t copy = 1; copy < bpb.num_fats && equal; copy++) if (fc_load_fat(copy, true, &equal) < 0) goto out;
    if (!equal) {
        fc.fatdiff++; fc.problems++; vga_print("[FAT32] check: FAT copies differ\n");
        uint32_t invalid0 = fc_invalid_entries();
        if (invalid0) for (uint32_t copy = 1; copy < bpb.num_fats; copy++) {
            if (fc_load_fat(copy, false, NULL) < 0) goto out;
            if (fc_invalid_entries() < invalid0) break;   /* this copy is the intact one */
            if (fc_load_fat(0, false, NULL) < 0) goto out;
        }
    }
    for (uint32_t c = 2; c < cluster_limit; c++) {
        uint32_t v = fc_get(c);
        /* 1, clusters past the volume and the reserved 0x0FFFFFF0-6 all fail f57_valid_cluster */
        if (v && v != FAT32_BAD && v < 0x0FFFFFF8 && !f57_valid_cluster(v)) { fc.invalid++; fc.problems++; fc_set(c, 0x0FFFFFFF); }
    }
    if (fc.invalid) vga_print("[FAT32] check: FAT entries outside the volume set to end of chain\n");
    uint32_t root_count;
    if (fc_claim_chain(root_cluster, &root_count, root_cluster, 0)) { vga_print("[FAT32] check: the root directory chain is unusable\n"); result = -1; goto out; }
    if (!fc_push(&fc_stack, &fc_stack_used, root_cluster, 0)) { result = -12; goto out; }
    while (fc_stack_used) {
        uint32_t first, parent; fc_pop(&fc_stack, &fc_stack_used, &first, &parent);
        int r = fc_scan_dir(first, parent);
        if (r) { result = r; goto out; }
    }
    while (fc_hidden_used) {   /* after every visible entry: they only keep clusters nobody else uses */
        uint32_t x, dir; fc_pop(&fc_hidden, &fc_hidden_used, &x, &dir);
        if (fc_bit(&fc_claim, x) || !fc_get(x) || fc_get(x) == FAT32_BAD) continue;
        fc.hidden++;
        for (uint32_t limit = cluster_limit; limit-- && f57_valid_cluster(x) && !fc_bit(&fc_claim, x) && fc_get(x) && fc_get(x) != FAT32_BAD; x = fc_get(x))
            fc_bit_put(&fc_claim, x, true);
    }
    uint32_t free_count = 0, first_free = 0;
    for (uint32_t c = 2; c < cluster_limit; c++) {
        uint32_t v = fc_get(c);
        if (v && v != FAT32_BAD && !fc_bit(&fc_claim, c)) { fc.lost++; if (v >= 0x0FFFFFF8) fc.lost_chains++; fc_set(c, FAT32_FREE); v = 0; }
        if (fc_bit(&fc_claim, c)) fc.referenced++;
        if (!v) { free_count++; if (!first_free) first_free = c; }
    }
    if (fc.lost) {
        fc.problems++;
        vga_print("[FAT32] check: "); fc_print_number(fc.lost); vga_print(" lost clusters in "); fc_print_number(fc.lost_chains);
        vga_print(fc_repair ? " chains freed\n" : " chains\n");
    }
    fc.free = free_count;
    if (!repair) { result = fc.problems ? -1 : 0; goto out; }
    if (fc_failed) goto out;
    if (fc.problems) {
        if (fc_write_fat(fc.fatdiff != 0) < 0 || fc_flush() < 0) goto out;
        fat_cache_start = 0xFFFFFFFF; fat_dirty_mask = 0;
    }
    if (fc.problems || dirty) {
        if (fsinfo_write(free_count, first_free ? first_free : 0xFFFFFFFFu) < 0) goto out;
        if (fat_state_flags(true) < 0 || boot_dirty_flag(true) < 0 || fc_flush() < 0) goto out;
    }
    free_clusters = free_count; free_known = true; next_free = first_free ? first_free : 2;
    if (fc.problems) chain_epoch = chain_epoch == 0xFFFFFFFFu ? chain_epoch : chain_epoch + 1;
    result = fc.problems ? 1 : 0;
out:
    vga_print("[FAT32] check: "); fc_print_number(fc.files); vga_print(" files, "); fc_print_number(fc.dirs); vga_print(" directories, ");
    fc_print_number(fc.referenced); vga_print(" clusters in use, "); fc_print_number(fc.lost); vga_print(" lost, ");
    fc_print_number(fc.problems); vga_print(" problems; ");
    vga_print(result == 1 ? "repaired" : result == 0 ? "clean" : result == -1 ? "NOT repaired" : result == -12 ? "no memory" : "I/O error");
    vga_print(" in "); fc_print_number((native_clock_ns() - started) / 1000000); vga_print(" ms\n");
    fc_vec_free(&fc_fat); fc_vec_free(&fc_claim); fc_vec_free(&fc_head); fc_vec_free(&fc_dirty);
    fc_vec_free(&fc_stack); fc_vec_free(&fc_hidden);
    io_error = false; fat_cache_start = 0xFFFFFFFF; fat_dirty_mask = 0;
    return result;
}

/* fat32_init: 0 mount read-write, -1 read-only. Sets up dirty tracking for the session. */
static int fat32_boot_check(void) {
    bool dirty = false, pattern = true;
    for (uint32_t copy = 0; copy < bpb.num_fats; copy++) {
        if (volume_read(fat_start_lba + copy * bpb.fat_size_32, 1, sector_buf) < 0) return -1;
        uint32_t state; memcpy(&state, sector_buf + 4, 4);
        if ((state & 0x03FFFFFF) < 0x03FFFFF8) pattern = false;   /* not the usual EOC pattern: no flags kept here */
        else if (!(state & FAT32_CLEAN)) dirty = true;
    }
    int linux_dirty = boot_dirty_flag(false);
    if (linux_dirty < 0) return -1;
    if (linux_dirty) dirty = true;
    volume_dirty_tracking = pattern; volume_marked_dirty = false;
    char mode[16] = "auto", repair[16] = "preen";
    boot_value("fsck.mode", mode, sizeof(mode)); boot_value("fsck.repair", repair, sizeof(repair));
    bool force = !strcmp(mode, "force"), skip = !strcmp(mode, "skip"), check_only = !strcmp(repair, "no");
    if (skip || (!dirty && !force)) {
        if (dirty) { vga_print("[FAT32] unclean metadata; mounted read-only until filesystem repair\n"); return -1; }
        return 0;
    }
    vga_print(dirty ? "[FAT32] volume was not cleanly unmounted: checking\n" : "[FAT32] check forced (fsck.mode=force)\n");
    int result = fat32_volume_check(!check_only, dirty);
    if (result >= 0) return 0;   /* clean (it stays marked dirty after a check-only pass) or repaired */
    vga_print("[FAT32] unclean metadata; mounted read-only until filesystem repair\n");
    return -1;
}

static void fat32_entry_barrier(const char* where) {
    if (!block_io_busy()) return;
    static unsigned reported;
    if (reported < 16) { reported++; vga_print("[FAT32] "); vga_print(where); vga_print(" waited for another task's disk wait\n"); }
    block_io_barrier();
}

/* vfs.c disk_shutdown (orderly power-off/reboot), after the orphans were disposed: FAT sectors, FSInfo, clean flag;
 * no write reaches the volume afterwards. */
void fat32_unmount_clean(void) {
    if (!mounted || writes_blocked) return;
    io_error = false;
    if (fat_flush_dirty() < 0) { writes_blocked = true; return; }
    if (volume_marked_dirty) {
        uint32_t count = free_known ? free_clusters : 0xFFFFFFFFu;
        if (fsinfo_write(count, next_free >= 2 && next_free < cluster_limit ? next_free : 0xFFFFFFFFu) < 0 || fc_flush() < 0 ||
            fat_state_flags(true) < 0 || fc_flush() < 0) vga_print("[FAT32] clean unmount failed; the next boot checks the volume\n");
        else vga_print("[FAT32] volume cleanly unmounted\n");
    }
    writes_blocked = true;
}
