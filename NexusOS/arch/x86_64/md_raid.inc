/* RAID5, RAID6 and RAID10 (Linux drivers/md/raid5.c, raid10.c) and the internal write-intent bitmap (md-bitmap.c),
 * included by md.c.
 *
 * RAID5/6: the layouts mdadm offers (layout 0-3: left/right asymmetric/symmetric, raid5_compute_sector), P = XOR of
 * the data chunks, Q = sum g^i * D_i over GF(2^8) (polynomial 0x11d, g = 2, i = the data disk's position after Q, as
 * Linux's syndrome order). I/O in pieces of up to PIECE sectors within one chunk: reads go to the chunk's member,
 * else the stripe row is read and the chunk reconstructed (RAID5: one missing member; RAID6: any two, P/Q/data);
 * writes read the whole row range, reconstruct what is missing, put the new data in, recompute P (and Q) and write
 * data and parity (reconstruct-write). Resync recomputes and writes the parity of every row (or of the bitmap's dirty
 * chunks) -- how Linux closes the RAID5 write hole after an unclean shutdown; a dirty degraded RAID5/6 array is not
 * started (Linux "cannot start dirty degraded array", md_mod.start_dirty_degraded=1 overrides). Recovery rebuilds a
 * member's chunks (data or parity) from the others.
 * RAID10: the near layout (layout 0x1NN: N copies on consecutive devices, raid10 __raid10_find_phys with far 1);
 * reads from the first valid copy (another after an error), writes to every copy; resync copies the first copy to the
 * others, recovery from another copy. Far and offset layouts are refused.
 * Bitmap (superblock feature bit 0, bitmap_offset at byte 96 relative to the superblock): the 256-byte bitmap
 * superblock ("bitm", version 3/4, uuid, events, events_cleared, sync_size, chunksize) followed by one bit per
 * chunk of the resync domain (RAID1/10: array sectors, RAID5/6: member sectors), on every member. A chunk's bit is
 * set (and the bitmap written and flushed) before its first write; bits are cleared when the array is idle and not
 * degraded (events_cleared = events), so they record what a missing member lacks. Resync after an unclean shutdown
 * covers only dirty chunks; a member re-added (ADD_NEW_DISK with MD_DISK_SYNC, mdadm --re-add) whose events are not
 * older than events_cleared takes its old role and recovers only the dirty chunks. A bitmap older than the array
 * (events) marks everything dirty, as Linux does. */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */
#define PIECE 64u                       /* sectors per RAID5/6/10 piece (32 KiB) */
static uint8_t gf_exp[512], gf_log[256]; static bool gf_ready;
static void gf_init(void) {
    if (gf_ready) return;
    unsigned v = 1;
    for (unsigned i = 0; i < 255; i++) { gf_exp[i] = gf_exp[i + 255] = (uint8_t)v; gf_log[v] = (uint8_t)i; v <<= 1; if (v & 0x100) v ^= 0x11d; }
    gf_exp[510] = gf_exp[0]; gf_exp[511] = gf_exp[1]; gf_ready = true;
}
static uint8_t gf_mul(uint8_t x, uint8_t y) { return x && y ? gf_exp[gf_log[x] + gf_log[y]] : 0; }
static uint8_t gf_pow2(unsigned e) { return gf_exp[e % 255]; }
static uint8_t gf_inv(uint8_t x) { return gf_exp[255 - gf_log[x]]; }
static void gf_mul_add(uint8_t* out, const uint8_t* in, uint8_t c, uint32_t bytes) {   /* out ^= c * in */
    if (!c) return;
    uint8_t t[256]; for (unsigned v = 0; v < 256; v++) t[v] = gf_mul(c, (uint8_t)v);
    for (uint32_t i = 0; i < bytes; i++) out[i] ^= t[in[i]];
}
static void xor_into(uint8_t* out, const uint8_t* in, uint32_t bytes) {
    uint64_t* o = (uint64_t*)out; const uint64_t* p = (const uint64_t*)in;
    for (uint32_t i = 0; i < bytes / 8; i++) o[i] ^= p[i];
}

/* ---- layout ---- */
static unsigned parity_count(const struct md* a) { return a->level == 6 ? 2 : a->level == 5 ? 1 : 0; }
static unsigned data_count(const struct md* a) { return a->raid_disks - parity_count(a); }
static void row_roles(const struct md* a, uint64_t stripe, unsigned* pd, unsigned* qd, unsigned* roles) {
    unsigned n = a->raid_disks, dd = data_count(a), r = (unsigned)(stripe % n), alg = a->layout;
    if (a->level == 5) {
        unsigned p = alg == 0 || alg == 2 ? dd - r : r; *pd = p; *qd = ~0u;
        for (unsigned j = 0; j < dd; j++) roles[j] = alg <= 1 ? (j >= p ? j + 1 : j) : (p + 1 + j) % n;
        return;
    }
    unsigned p = alg == 0 || alg == 2 ? n - 1 - r : r, q;
    if (alg <= 1) {
        q = p + 1;
        if (p == n - 1) { q = 0; for (unsigned j = 0; j < dd; j++) roles[j] = j + 1; }
        else for (unsigned j = 0; j < dd; j++) roles[j] = j >= p ? j + 2 : j;
    } else { q = (p + 1) % n; for (unsigned j = 0; j < dd; j++) roles[j] = (p + 2 + j) % n; }
    *pd = p; *qd = q;
}
static unsigned q_slot(const struct md* a, unsigned qd, unsigned role) { return (role + a->raid_disks - (qd + 1) % a->raid_disks) % a->raid_disks; }
static uint64_t sync_end(const struct md* a) { return a->level == 10 ? a->block.blocks : a->level >= 5 ? a->size / a->chunk * a->chunk : a->size; }

/* ---- bitmap ---- */
static bool bm_test(const struct md* a, uint64_t domain) {
    uint64_t b = domain / a->bm_chunk; return b >= a->bm_bits || (a->bm[256 + b / 8] >> (b % 8) & 1);
}
static uint64_t bm_sector(const struct member* x, const struct md* a) { return (uint64_t)((int64_t)x->sb_offset + a->bm_offset); }
static void bm_write_all(struct md* a) {
    if (!a->bitmap) return;
    put_le64(a->bm + 24, a->events); put_le64(a->bm + 32, a->bm_cleared);
    for (unsigned k = 0; k < a->members; k++) {
        struct member* x = &a->m[k];
        if (x->faulty || x->kicked || x->role < 0) continue;
        if (block_part_write(x->part, bm_sector(x, a), a->bm_bytes / 512, a->bm) < 0 || block_flush(x->part->dev) < 0) say_member(a, x, "bitmap write failed");
    }
}
static void bm_mark(struct md* a, uint64_t d0, uint64_t d1) {   /* before a write to domain [d0, d1) */
    if (!a->bitmap || d1 <= d0) return;
    bool fresh = false;
    for (uint64_t b = d0 / a->bm_chunk; b <= (d1 - 1) / a->bm_chunk && b < a->bm_bits; b++)
        if (!(a->bm[256 + b / 8] >> (b % 8) & 1)) { a->bm[256 + b / 8] |= (uint8_t)(1u << (b % 8)); fresh = true; }
    if (fresh) { a->bm_dirty = true; bm_write_all(a); }
}
static unsigned degraded(const struct md* a) { return a->raid_disks - working(a); }
static void bm_clear(struct md* a) {   /* idle, in sync, nobody missing: every chunk clean */
    if (!a->bitmap || !a->bm_dirty || degraded(a) || a->sync) return;
    for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k])) block_flush(a->m[k].part->dev);   /* the data first */
    memset(a->bm + 256, 0, a->bm_bytes - 256); a->bm_cleared = a->events; a->bm_dirty = false;
    bm_write_all(a);
}
static bool bm_load(struct md* a) {   /* run_array: the freshest member's bitmap */
    struct member* src = NULL;
    for (unsigned k = 0; k < a->members; k++) if (in_sync(a, &a->m[k]) && (!src || a->m[k].events > src->events)) src = &a->m[k];
    a->bitmap = false;
    if (!src || !(get_le32(src->sb + 8) & 1)) return true;
    a->bm_offset = (int32_t)get_le32(src->sb + 96);
    uint8_t* head = (void*)arena_alloc(4096); if (!head) return false;
    bool ok = block_part_read(src->part, bm_sector(src, a), 1, head) == 0;
    uint32_t version = get_le32(head + 4), chunk_bytes = get_le32(head + 52), nodes = get_le32(head + 68), reserved = get_le32(head + 64);
    uint64_t domain = sync_end(a);
    if (ok && (get_le32(head) != 0x6d746962u || (version != 3 && version != 4) || memcmp(head + 8, a->uuid, 16) || chunk_bytes < 512 ||
               (chunk_bytes & (chunk_bytes - 1)) || nodes > 1 || get_le64(head + 40) < domain)) ok = false;
    if (ok) {
        a->bm_chunk = chunk_bytes / 512; a->bm_bits = (domain + a->bm_chunk - 1) / a->bm_chunk;
        a->bm_bytes = (uint32_t)((256 + (a->bm_bits + 7) / 8 + 511) & ~511ULL);
        if (a->bm_bits > (8u << 20) || (reserved && a->bm_bytes / 512 > reserved)) ok = false;
    }
    if (ok && a->bm_offset > 0 && (uint64_t)a->bm_offset + a->bm_bytes / 512 > src->data_offset - src->sb_offset && src->data_offset > src->sb_offset) ok = false;   /* over the data */
    if (ok) { a->bm = (void*)arena_alloc(a->bm_bytes); ok = a->bm && block_part_read(src->part, bm_sector(src, a), a->bm_bytes / 512, a->bm) == 0; }
    arena_free((uint64_t)head, 4096);
    if (!ok) { if (a->bm) arena_free((uint64_t)a->bm, a->bm_bytes); a->bm = NULL; say(a, "bitmap superblock refused"); return false; }
    a->bitmap = true; a->bm_cleared = get_le64(a->bm + 32);
    uint64_t bm_events = get_le64(a->bm + 24);
    for (uint64_t b = a->bm_bits; b < (uint64_t)(a->bm_bytes - 256) * 8; b++) a->bm[256 + b / 8] &= (uint8_t)~(1u << (b % 8));
    a->bm_dirty = false;
    for (uint32_t i = 256; i < a->bm_bytes; i++) if (a->bm[i]) a->bm_dirty = true;
    if (bm_events < a->events || (get_le32(a->bm + 48) & 2)) {   /* out of date or stale: everything dirty */
        say(a, "bitmap out of date: every chunk dirty");
        for (uint64_t b = 0; b < a->bm_bits; b++) a->bm[256 + b / 8] |= (uint8_t)(1u << (b % 8));
        a->bm_dirty = true;
    }
    return true;
}
static void bm_free(struct md* a) { if (a->bm) arena_free((uint64_t)a->bm, a->bm_bytes); a->bm = NULL; a->bitmap = false; }

/* ---- members by role ---- */
static struct member* role_member(struct md* a, unsigned role) {
    for (unsigned k = 0; k < a->members; k++) if (usable(a, &a->m[k]) && a->m[k].role == (int)role) return &a->m[k];
    return NULL;
}
/* x holds valid data at member sectors [s, s + len) (domain position d for a bitmap-limited recovery) */
static bool member_valid(const struct md* a, const struct member* x, uint64_t s, uint32_t len, uint64_t d) {
    if (!x || !usable(a, x)) return false;
    if (x->recovery_offset == NO_SECTOR || s + len <= x->recovery_offset) return true;
    return x->bm_recovery && a->bitmap && !bm_test(a, d);
}

/* ---- RAID5/6 rows ---- */
static unsigned row_load(struct md* a, uint64_t ms, uint32_t len, int exclude, unsigned* missing) {   /* every role's [ms, ms+len) */
    unsigned nm = 0;
    for (unsigned r = 0; r < a->raid_disks; r++) {
        struct member* x = (int)r == exclude ? NULL : role_member(a, r);
        if (member_valid(a, x, ms, len, ms)) {
            if (member_io(x, ms, len, a->work[r], false) == 0) continue;
            fail_member(a, x, "read error: faulty");
        }
        missing[nm++] = r; memset(a->work[r], 0, (uint64_t)len * 512);
    }
    return nm;
}
static void gen_parity(struct md* a, const unsigned* roles, unsigned pd, unsigned qd, uint32_t bytes, bool want_p, bool want_q) {
    unsigned dd = data_count(a);
    if (want_p) { memset(a->work[pd], 0, bytes); for (unsigned j = 0; j < dd; j++) xor_into(a->work[pd], a->work[roles[j]], bytes); }
    if (want_q && a->level == 6) { memset(a->work[qd], 0, bytes); for (unsigned j = 0; j < dd; j++) gf_mul_add(a->work[qd], a->work[roles[j]], gf_pow2(q_slot(a, qd, roles[j])), bytes); }
}
static int reconstruct(struct md* a, const unsigned* roles, unsigned pd, unsigned qd, uint32_t bytes, const unsigned* missing, unsigned nm) {
    if (nm > parity_count(a)) return -1;
    if (!nm) return 0;
    if (a->level == 5) {   /* XOR of all members is zero */
        uint8_t* out = a->work[missing[0]]; memset(out, 0, bytes);
        for (unsigned r = 0; r < a->raid_disks; r++) if (r != missing[0]) xor_into(out, a->work[r], bytes);
        return 0;
    }
    bool mp = false, mq = false; unsigned dx[2], nd = 0;
    for (unsigned i = 0; i < nm; i++) { if (missing[i] == pd) mp = true; else if (missing[i] == qd) mq = true; else dx[nd++] = missing[i]; }
    uint8_t* t = a->work[a->raid_disks];   /* scratch */
    if (nd == 1 && !mp) {   /* D = P ^ the other data */
        memcpy(a->work[dx[0]], a->work[pd], bytes);
        for (unsigned j = 0; j < data_count(a); j++) if (roles[j] != dx[0]) xor_into(a->work[dx[0]], a->work[roles[j]], bytes);
    } else if (nd == 1) {   /* P lost too: D = (Q ^ sum of the others) / g^slot */
        memcpy(t, a->work[qd], bytes);
        for (unsigned j = 0; j < data_count(a); j++) if (roles[j] != dx[0]) gf_mul_add(t, a->work[roles[j]], gf_pow2(q_slot(a, qd, roles[j])), bytes);
        memset(a->work[dx[0]], 0, bytes); gf_mul_add(a->work[dx[0]], t, gf_inv(gf_pow2(q_slot(a, qd, dx[0]))), bytes);
    } else if (nd == 2) {   /* two data chunks: Pxy = Dx ^ Dy, Qxy = g^x Dx ^ g^y Dy -> Dx = (Qxy ^ g^y Pxy) / (g^x ^ g^y) */
        uint8_t* p = a->work[a->raid_disks + 1];
        memcpy(p, a->work[pd], bytes); memcpy(t, a->work[qd], bytes);
        for (unsigned j = 0; j < data_count(a); j++) if (roles[j] != dx[0] && roles[j] != dx[1]) {
            xor_into(p, a->work[roles[j]], bytes); gf_mul_add(t, a->work[roles[j]], gf_pow2(q_slot(a, qd, roles[j])), bytes);
        }
        uint8_t gx = gf_pow2(q_slot(a, qd, dx[0])), gy = gf_pow2(q_slot(a, qd, dx[1]));
        gf_mul_add(t, p, gy, bytes);   /* t = Qxy ^ g^y Pxy */
        memset(a->work[dx[0]], 0, bytes); gf_mul_add(a->work[dx[0]], t, gf_inv(gx ^ gy), bytes);
        memcpy(a->work[dx[1]], p, bytes); xor_into(a->work[dx[1]], a->work[dx[0]], bytes);
    }
    gen_parity(a, roles, pd, qd, bytes, mp, mq);
    return 0;
}
static int raid56_piece(struct md* a, uint64_t sector, uint32_t len, uint8_t* data, bool write) {   /* within one chunk */
    uint64_t chunk_no = sector / a->chunk; uint32_t off = (uint32_t)(sector % a->chunk); unsigned dd = data_count(a);
    uint64_t stripe = chunk_no / dd; unsigned j = (unsigned)(chunk_no % dd), pd, qd, roles[MD_MEMBERS], missing[MD_MEMBERS];
    row_roles(a, stripe, &pd, &qd, roles);
    uint64_t ms = stripe * a->chunk + off; uint32_t bytes = len * 512;
    struct member* x = role_member(a, roles[j]);
    if (!write) {
        if (member_valid(a, x, ms, len, ms)) { if (member_io(x, ms, len, data, false) == 0) return 0; fail_member(a, x, "read error: faulty"); }
        unsigned nm = row_load(a, ms, len, -1, missing);
        if (reconstruct(a, roles, pd, qd, bytes, missing, nm) < 0) return -1;
        memcpy(data, a->work[roles[j]], bytes);
        return 0;
    }
    bm_mark(a, ms, ms + len);
    unsigned nm = row_load(a, ms, len, -1, missing);
    if (reconstruct(a, roles, pd, qd, bytes, missing, nm) < 0) return -1;
    memcpy(a->work[roles[j]], data, bytes);
    gen_parity(a, roles, pd, qd, bytes, true, true);
    unsigned targets[3] = {roles[j], pd, qd};
    for (unsigned t = 0; t < 1 + parity_count(a); t++) {
        struct member* y = role_member(a, targets[t]);
        if (y && member_io(y, ms, len, a->work[targets[t]], true) < 0) fail_member(a, y, "write error: faulty");
    }
    return degraded(a) > parity_count(a) ? -1 : 0;
}

/* ---- RAID10 (near) ---- */
static void r10_copy(const struct md* a, uint64_t chunk_no, unsigned k, unsigned* dev, uint64_t* stripe) {
    uint64_t c = chunk_no * a->near + k; *dev = (unsigned)(c % a->raid_disks); *stripe = c / a->raid_disks;
}
static int raid10_piece(struct md* a, uint64_t sector, uint32_t len, uint8_t* data, bool write) {
    uint64_t chunk_no = sector / a->chunk; uint32_t off = (uint32_t)(sector % a->chunk); unsigned done = 0;
    if (write) bm_mark(a, sector, sector + len);
    for (unsigned k = 0; k < a->near; k++) {
        unsigned dev; uint64_t stripe; r10_copy(a, chunk_no, k, &dev, &stripe);
        uint64_t ms = stripe * a->chunk + off; struct member* x = role_member(a, dev);
        if (!write) {
            if (!member_valid(a, x, ms, len, sector)) continue;
            if (member_io(x, ms, len, data, false) == 0) return 0;
            fail_member(a, x, "read error: faulty"); continue;
        }
        if (!x) continue;
        if (member_io(x, ms, len, data, true) == 0) done++; else fail_member(a, x, "write error: faulty");
    }
    return write && done ? 0 : -1;
}
static int raid_io(struct md* a, uint64_t sector, uint32_t count, uint8_t* data, bool write) {   /* levels 5, 6, 10 */
    while (count) {
        uint32_t within = (uint32_t)(sector % a->chunk), n = a->chunk - within;
        if (n > count) n = count;
        if (n > PIECE) n = PIECE;
        if ((a->level == 10 ? raid10_piece(a, sector, n, data, write) : raid56_piece(a, sector, n, data, write)) < 0) return -1;
        sector += n; count -= n; data += (uint64_t)n * 512;
    }
    return 0;
}
/* One sync piece at domain position pos (member sectors for 5/6, array sectors for 10): resync (target < 0) or the
 * recovery of member `target`. Returns the sectors covered, 0 on failure. */
static uint32_t raid_sync_piece(struct md* a, uint64_t pos, int target) {
    uint32_t within = (uint32_t)(pos % a->chunk), n = a->chunk - within; uint64_t end = sync_end(a);
    if (n > PIECE) n = PIECE;
    if (n > end - pos) n = (uint32_t)(end - pos);
    uint32_t bytes = n * 512;
    if (a->level == 10) {
        uint64_t chunk_no = pos / a->chunk; uint8_t* buf = a->work[0]; int src = -1;
        for (unsigned k = 0; k < a->near && src < 0; k++) {
            unsigned dev; uint64_t stripe; r10_copy(a, chunk_no, k, &dev, &stripe);
            struct member* x = role_member(a, dev);
            if (x && (target < 0 || x != &a->m[target]) && member_valid(a, x, stripe * a->chunk + within, n, pos) && member_io(x, stripe * a->chunk + within, n, buf, false) == 0) src = (int)k;
        }
        if (src < 0) return 0;
        for (unsigned k = 0; k < a->near; k++) {
            unsigned dev; uint64_t stripe; r10_copy(a, chunk_no, k, &dev, &stripe);
            struct member* x = role_member(a, dev);
            if ((int)k == src || !x || (target >= 0 && x != &a->m[target])) continue;
            if (member_io(x, stripe * a->chunk + within, n, buf, true) < 0) fail_member(a, x, "write error during sync: faulty");
        }
        return n;
    }
    uint64_t stripe = pos / a->chunk; unsigned pd, qd, roles[MD_MEMBERS], missing[MD_MEMBERS];
    row_roles(a, stripe, &pd, &qd, roles);
    int exclude = target >= 0 ? a->m[target].role : -1;
    unsigned nm = row_load(a, pos, n, exclude, missing);
    if (target < 0) {   /* resync: the parity of the row from its data */
        if (nm) return 0;
        gen_parity(a, roles, pd, qd, bytes, true, true);
        for (unsigned t = 0; t < parity_count(a); t++) {
            unsigned role = t ? qd : pd; struct member* y = role_member(a, role);
            if (y && member_io(y, pos, n, a->work[role], true) < 0) fail_member(a, y, "write error during sync: faulty");
        }
        return n;
    }
    if (reconstruct(a, roles, pd, qd, bytes, missing, nm) < 0) return 0;
    if (member_io(&a->m[target], pos, n, a->work[exclude], true) < 0) { fail_member(a, &a->m[target], "write error during recovery: faulty"); return 0; }
    return n;
}
#pragma clang attribute pop
