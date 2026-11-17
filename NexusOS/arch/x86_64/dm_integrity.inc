/* dm-integrity (Linux drivers/md/dm-integrity.c, target version 1.10.0), included by dm.c: every block carries a tag
 * (a checksum of its logical sector and data) in metadata areas interleaved with the data, so integritysetup
 * format/open/status/close/dump work and a changed block reads as an I/O error.
 *
 * On disk as Linux lays it out: the superblock ("integrt", versions 1-5) in the first 8 sectors at <offset>, then
 * the journal (sections of 8 entry sectors + the entries' data blocks; every journal sector ends with a commit id
 * 0x1111.. / 0x2222.. / 0x3333.. / 0x4444.. ^ (section << 32 ^ sector)), then areas of <metadata run> sectors of tags
 * followed by 2^log2_interleave_sectors data sectors (the metadata run padded to 128 KiB, or to 4 KiB with
 * fix_padding). A zeroed superblock is formatted at the first activation as Linux does (provided_data_sectors by the
 * same search, the journal written as empty), which is how integritysetup format works. Tags: crc32c (default) or
 * sha256 over [salt when fix_hmac] + le64 logical sector + the block, truncated/zero-padded to the tag size.
 * Modes: J (journal): each write is put in a journal section (entries with sector, the replaced last 8 bytes of each
 * sector and the tag; data blocks), flushed, written in place with its tags, flushed, and the section emptied again
 * (flushed) -- so after a power cut every block is either old or new with a matching tag; activation replays a
 * committed section (all entry sectors and the entries' data sectors carry the same commit id sequence) and empties
 * torn ones. D (direct): data and tags written in place, no journal (a power cut can leave a block whose tag does not
 * match: EIO, as on Linux). R (recovery): read-only, nothing checked or replayed. A tag mismatch logs
 * "[64] DM integrity <dev>: checksum failed at sector N" and fails the read (EIO; Linux EILSEQ); the status counts
 * mismatches. Not done: bitmap mode (B), keyed hashes (hmac), journal_crypt/journal_mac, meta_device, recalculate,
 * discards (allow_discards is accepted, no discard reaches the device), inline tags (version 6): refused (EINVAL). */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */
#define INTEGRITY_SB_SECTORS 8
#define INTEGRITY_JOURNAL_BLOCK 8          /* entry sectors per journal section */
#define INTEGRITY_SECTOR_DATA 504          /* bytes of a journal sector before its commit id */
#define INTEGRITY_PADDING 8                /* METADATA_PADDING_SECTORS */
#define INTEGRITY_CHUNK 64                 /* blocks per data/tag pass */
enum { IF_JOURNAL_MAC = 1, IF_RECALCULATING = 2, IF_DIRTY_BITMAP = 4, IF_FIXED_PADDING = 8, IF_FIXED_HMAC = 16, IF_INLINE = 32 };
uint32_t ext4_crc32c(uint32_t crc, const void* data, uint64_t length);   /* raw (no inversion): ext4_internal.h */
struct integrity {
    struct block_partition* dev; uint64_t start, device_sectors, provided, initial, metadata_run, mismatches;
    uint32_t tag_size, spb, log2_spb, log2_interleave, sections, section_sectors, entry_size, section_entries, flags, buffer_sectors, watermark, commit_time;
    char mode; bool sha, discard, readonly; uint8_t version, salt[16]; unsigned errors;
    unsigned cur; uint8_t* seqs;           /* per section: the commit id sequence of its last use (0..3) */
    uint8_t* meta; uint8_t* bounce; uint8_t* tags;   /* journal entry block (4 KiB), data bounce, tag sectors */
    uint8_t tagbuf[INTEGRITY_CHUNK * 32], jtags[INTEGRITY_CHUNK * 32];   /* tags of one pass (off the kernel stack) */
    volatile int busy;
};
static uint32_t integrity_block_size(const struct integrity* x) { return x->spb * 512; }
static void integrity_free(struct integrity* x) {
    if (!x) return;
    if (x->seqs) arena_free((uint64_t)x->seqs, x->sections);
    if (x->meta) arena_free((uint64_t)x->meta, INTEGRITY_JOURNAL_BLOCK * 512);
    if (x->bounce) arena_free((uint64_t)x->bounce, (uint64_t)INTEGRITY_CHUNK * x->spb * 512);
    if (x->tags) arena_free((uint64_t)x->tags, 8192);
    arena_free((uint64_t)x, sizeof(*x));
}
static void integrity_lock(struct integrity* x) {
    while (__atomic_exchange_n(&x->busy, 1, __ATOMIC_ACQUIRE)) {
        if (dm_may_wait()) kernel_io_sleep((void*)&x->busy, native_clock_ns() + 1000000ULL); else __asm__ volatile("pause");
    }
}
static void integrity_unlock(struct integrity* x) { __atomic_store_n(&x->busy, 0, __ATOMIC_RELEASE); task_wake_channel((void*)&x->busy); }

/* ---- layout (Linux calculate_journal_section_size / calculate_device_limits / get_data_sector) ---- */
static void section_size(struct integrity* x) {
    x->entry_size = (8 + 8 * x->spb + x->tag_size + 7) & ~7u;   /* roundup(offsetof(journal_entry, last_bytes[spb]) + tag, 8) */
    uint32_t per_sector = INTEGRITY_SECTOR_DATA / x->entry_size;
    x->section_entries = per_sector * INTEGRITY_JOURNAL_BLOCK;
    x->section_sectors = (x->section_entries << x->log2_spb) + INTEGRITY_JOURNAL_BLOCK;
}
static uint64_t area_of(const struct integrity* x, uint64_t sector, uint64_t* offset) { *offset = sector & ((1ULL << x->log2_interleave) - 1); return sector >> x->log2_interleave; }
static uint64_t data_sector(const struct integrity* x, uint64_t area, uint64_t offset) {
    return x->start + x->initial + (area << x->log2_interleave) + (area + 1) * x->metadata_run + offset;
}
static uint64_t meta_sector(const struct integrity* x, uint64_t area) { return x->start + x->initial + (area << x->log2_interleave) + area * x->metadata_run; }
static bool device_limits(struct integrity* x) {
    section_size(x);
    uint64_t initial = INTEGRITY_SB_SECTORS + (uint64_t)x->section_sectors * x->sections;
    if (initial + INTEGRITY_PADDING >= x->device_sectors || initial > 0xFFFFFFFFULL) return false;
    x->initial = initial;
    uint64_t padding = x->flags & IF_FIXED_PADDING ? INTEGRITY_PADDING * 512 : 512u << INTEGRITY_PADDING;
    uint64_t bytes = (uint64_t)x->tag_size << (x->log2_interleave - x->log2_spb);
    x->metadata_run = (bytes + padding - 1) / padding * padding / 512;
    if (!x->provided) return false;
    uint64_t offset, area = area_of(x, x->provided - 1, &offset), last = data_sector(x, area, offset);
    return last >= x->start && last < x->device_sectors;
}
static void provided_search(struct integrity* x) {   /* Linux get_provided_data_sectors */
    x->provided = 0; int top = 63; while (top >= 0 && !(x->device_sectors >> top & 1)) top--;
    for (int bit = top; bit >= 3; bit--) { uint64_t before = x->provided; x->provided |= 1ULL << bit; if (!device_limits(x)) x->provided = before; }
    if (x->provided) device_limits(x);
}

/* ---- tags ---- */
static void integrity_tag(const struct integrity* x, uint64_t sector, const uint8_t* data, uint8_t* out) {
    uint8_t le[8]; put_le64(le, sector);
    if (x->sha) {
        struct sha256 s; sha256_init(&s); uint8_t d[32];
        if (x->flags & IF_FIXED_HMAC) sha256_update(&s, x->salt, 16);
        sha256_update(&s, le, 8); sha256_update(&s, data, (uint64_t)x->spb * 512); sha256_final(&s, d);
        memset(out, 0, x->tag_size); memcpy(out, d, x->tag_size < 32 ? x->tag_size : 32);
        return;
    }
    uint32_t c = 0xFFFFFFFFu;
    if (x->flags & IF_FIXED_HMAC) c = ext4_crc32c(c, x->salt, 16);
    c = ext4_crc32c(ext4_crc32c(c, le, 8), data, (uint64_t)x->spb * 512) ^ 0xFFFFFFFFu;
    memset(out, 0, x->tag_size); uint8_t v[4]; put_le32(v, c); memcpy(out, v, x->tag_size < 4 ? x->tag_size : 4);
}
/* Tags of `blocks` blocks from `offset` (sectors, in one area): read (write=false) into or written from `tags`. */
static int tag_io(struct integrity* x, uint64_t area, uint64_t offset, uint32_t blocks, uint8_t* tags, bool write) {
    uint64_t first = (offset >> x->log2_spb) * x->tag_size, bytes = (uint64_t)blocks * x->tag_size;
    uint64_t sector = meta_sector(x, area) + first / 512; uint32_t skip = (uint32_t)(first % 512), count = (uint32_t)((skip + bytes + 511) / 512);
    if (count > 16) return -1;   /* INTEGRITY_CHUNK blocks x 32-byte tags: at most 5 sectors */
    if (block_part_read(x->dev, sector, count, x->tags) < 0) return -1;
    if (!write) { memcpy(tags, x->tags + skip, bytes); return 0; }
    memcpy(x->tags + skip, tags, bytes);
    return block_part_write(x->dev, sector, count, x->tags);
}
static int integrity_mismatch(struct integrity* x, uint64_t sector) {
    x->mismatches++;
    if (x->errors < 100) {
        char line[128] = "[64] DM integrity "; put_number(line, x->dev->major); strcat(line, ":"); put_number(line, x->dev->minor);
        strcat(line, ": checksum failed at sector "); put_number(line, sector); strcat(line, "\n"); vga_print(line);
        x->errors++;
    }
    return -1;
}
/* In place: data blocks and their tags (D mode, and J mode after the journal commit). */
static int write_in_place(struct integrity* x, uint64_t logical, uint32_t blocks, const uint8_t* data, const uint8_t* tags) {
    for (uint32_t done = 0; done < blocks; ) {
        uint64_t offset, area = area_of(x, logical + ((uint64_t)done << x->log2_spb), &offset);
        uint32_t room = (uint32_t)(((1ULL << x->log2_interleave) - offset) >> x->log2_spb), n = blocks - done < room ? blocks - done : room;
        if (block_part_write(x->dev, data_sector(x, area, offset), n << x->log2_spb, data + ((uint64_t)done << x->log2_spb) * 512) < 0) return -1;
        if (tag_io(x, area, offset, n, (uint8_t*)tags + (uint64_t)done * x->tag_size, true) < 0) return -1;
        done += n;
    }
    return 0;
}

/* ---- journal ---- */
static uint64_t commit_id(unsigned section, unsigned sector, unsigned seq) { return 0x1111111111111111ULL * (seq + 1) ^ ((uint64_t)section << 32 ^ sector); }
static uint64_t section_start(const struct integrity* x, unsigned s) { return x->start + INTEGRITY_SB_SECTORS + (uint64_t)s * x->section_sectors; }
static uint8_t* entry_at(struct integrity* x, unsigned n) { return x->meta + (n % INTEGRITY_JOURNAL_BLOCK) * 512 + (n / INTEGRITY_JOURNAL_BLOCK) * x->entry_size; }
static bool entry_unused(const uint8_t* e) { uint32_t hi = get_le32(e + 4); return hi == 0xFFFFFFFFu || hi == 0xFFFFFFFEu; }
static void empty_entries(struct integrity* x, unsigned section, unsigned seq) {   /* all entries unused, commit ids of `seq` */
    memset(x->meta, 0, INTEGRITY_JOURNAL_BLOCK * 512);
    for (unsigned n = 0; n < x->section_entries; n++) put_le32(entry_at(x, n) + 4, 0xFFFFFFFFu);
    for (unsigned j = 0; j < INTEGRITY_JOURNAL_BLOCK; j++) put_le64(x->meta + j * 512 + INTEGRITY_SECTOR_DATA, commit_id(section, j, seq));
}
static int journal_init(struct integrity* x) {   /* a just formatted device: every section empty, sequence 0 */
    uint8_t* zero = x->bounce; uint32_t per = INTEGRITY_CHUNK * x->spb;
    for (unsigned s = 0; s < x->sections; s++) {
        empty_entries(x, s, 0);
        if (block_part_write(x->dev, section_start(x, s), INTEGRITY_JOURNAL_BLOCK, x->meta) < 0) return -1;
        for (uint32_t j = INTEGRITY_JOURNAL_BLOCK; j < x->section_sectors; ) {
            uint32_t n = x->section_sectors - j < per ? x->section_sectors - j : per;
            memset(zero, 0, (uint64_t)n * 512);
            for (uint32_t k = 0; k < n; k++) put_le64(zero + k * 512 + INTEGRITY_SECTOR_DATA, commit_id(s, j + k, 0));
            if (block_part_write(x->dev, section_start(x, s) + j, n, zero) < 0) return -1;
            j += n;
        }
        x->seqs[s] = 0;
    }
    return block_flush(x->dev->dev) < 0 ? -1 : 0;
}
/* One journalled write of `blocks` (<= section entries, <= INTEGRITY_CHUNK, within one area): journal, flush, place,
 * flush, empty the section, flush. */
static int journal_write(struct integrity* x, uint64_t logical, uint32_t blocks, const uint8_t* data) {
    unsigned s = x->cur, seq = (x->seqs[s] + 1) & 3; uint64_t base = section_start(x, s); uint8_t* tags = x->jtags;
    empty_entries(x, s, seq);
    uint8_t* journal = x->bounce;
    for (uint32_t e = 0; e < blocks; e++) {
        const uint8_t* block = data + ((uint64_t)e << x->log2_spb) * 512; uint8_t* entry = entry_at(x, e);
        uint64_t sector = logical + ((uint64_t)e << x->log2_spb);
        integrity_tag(x, sector, block, tags + e * x->tag_size);
        put_le64(entry, sector); memcpy(entry + 8 + 8 * x->spb, tags + e * x->tag_size, x->tag_size);
        for (uint32_t k = 0; k < x->spb; k++) {   /* each data sector's last 8 bytes go to the entry, a commit id replaces them */
            uint8_t* j = journal + ((uint64_t)e * x->spb + k) * 512;
            memcpy(j, block + k * 512, 512); memcpy(entry + 8 + 8 * k, j + INTEGRITY_SECTOR_DATA, 8);
            put_le64(j + INTEGRITY_SECTOR_DATA, commit_id(s, INTEGRITY_JOURNAL_BLOCK + e * x->spb + k, seq));
        }
    }
    if (block_part_write(x->dev, base + INTEGRITY_JOURNAL_BLOCK, blocks << x->log2_spb, journal) < 0 || block_flush(x->dev->dev) < 0 ||
        block_part_write(x->dev, base, INTEGRITY_JOURNAL_BLOCK, x->meta) < 0 || block_flush(x->dev->dev) < 0) return -1;
    x->seqs[s] = (uint8_t)seq;
    if (write_in_place(x, logical, blocks, data, tags) < 0 || block_flush(x->dev->dev) < 0) return -1;
    empty_entries(x, s, seq);
    if (block_part_write(x->dev, base, INTEGRITY_JOURNAL_BLOCK, x->meta) < 0 || block_flush(x->dev->dev) < 0) return -1;
    x->cur = (s + 1) % x->sections;
    return 0;
}
/* Activation: committed sections are written in place again (idempotent), torn ones emptied. */
static int journal_replay(struct integrity* x) {
    unsigned replayed = 0, torn = 0, blocks = 0;
    for (unsigned s = 0; s < x->sections; s++) {
        if (block_part_read(x->dev, section_start(x, s), INTEGRITY_JOURNAL_BLOCK, x->meta) < 0) return -1;
        int seq = -1; bool uniform = true, valid = true;
        for (unsigned j = 0; j < INTEGRITY_JOURNAL_BLOCK; j++) {
            uint64_t id = get_le64(x->meta + j * 512 + INTEGRITY_SECTOR_DATA); int k = -1;
            for (unsigned q = 0; q < 4; q++) if (id == commit_id(s, j, q)) k = (int)q;
            if (k < 0) valid = false; else if (seq < 0) seq = k; else if (k != seq) uniform = false;
        }
        if (!valid) {   /* not a journal (hostile or never formatted by this layout): emptied */
            vga_print("[64] DM integrity: invalid journal commit ids, section emptied\n");
            x->seqs[s] = 0; empty_entries(x, s, 0);
            if (block_part_write(x->dev, section_start(x, s), INTEGRITY_JOURNAL_BLOCK, x->meta) < 0) return -1;
            continue;
        }
        x->seqs[s] = (uint8_t)seq;
        unsigned used = 0; for (unsigned n = 0; n < x->section_entries; n++) if (!entry_unused(entry_at(x, n))) used++;
        if (!used) continue;
        bool committed = uniform;
        uint8_t* data = x->bounce; uint8_t tag[32];
        for (unsigned n = 0; n < x->section_entries && committed; n++) {   /* every entry: its data carries the same sequence */
            uint8_t* e = entry_at(x, n); if (entry_unused(e)) continue;
            uint64_t sector = get_le64(e);
            if (sector & (x->spb - 1) || sector + x->spb > x->provided) { committed = false; break; }
            if (block_part_read(x->dev, section_start(x, s) + INTEGRITY_JOURNAL_BLOCK + ((uint64_t)n << x->log2_spb), x->spb, data) < 0) return -1;
            for (uint32_t k = 0; k < x->spb; k++) {
                if (get_le64(data + k * 512 + INTEGRITY_SECTOR_DATA) != commit_id(s, INTEGRITY_JOURNAL_BLOCK + n * x->spb + k, (unsigned)seq)) committed = false;
                memcpy(data + k * 512 + INTEGRITY_SECTOR_DATA, e + 8 + 8 * k, 8);
            }
            integrity_tag(x, sector, data, tag);
            if (memcmp(tag, e + 8 + 8 * x->spb, x->tag_size)) committed = false;
            if (committed && write_in_place(x, sector, 1, data, tag) < 0) return -1;
            if (committed) blocks++;
        }
        if (committed) replayed++; else torn++;
        if (block_flush(x->dev->dev) < 0) return -1;
        empty_entries(x, s, (unsigned)seq);
        if (block_part_write(x->dev, section_start(x, s), INTEGRITY_JOURNAL_BLOCK, x->meta) < 0 || block_flush(x->dev->dev) < 0) return -1;
    }
    if (replayed || torn) {
        char line[128] = "[64] DM integrity: journal replayed "; put_number(line, replayed); strcat(line, " section(s), ");
        put_number(line, blocks); strcat(line, " block(s); "); put_number(line, torn); strcat(line, " torn section(s) dropped\n"); vga_print(line);
    }
    return 0;
}

static int integrity_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {
    struct integrity* x = g->integ;
    if (relative % x->spb || count % x->spb) return -1;
    if (write && (x->mode == 'R' || x->readonly)) return -1;
    integrity_lock(x);
    int e = 0; uint32_t blocks = count >> x->log2_spb;
    for (uint32_t done = 0; done < blocks && !e; ) {
        uint64_t logical = relative + ((uint64_t)done << x->log2_spb), offset, area = area_of(x, logical, &offset);
        uint32_t room = (uint32_t)(((1ULL << x->log2_interleave) - offset) >> x->log2_spb), n = blocks - done;
        if (n > room) n = room;
        if (n > INTEGRITY_CHUNK) n = INTEGRITY_CHUNK;
        if (write && x->mode == 'J' && n > x->section_entries) n = x->section_entries;
        uint8_t* block = data + ((uint64_t)done << x->log2_spb) * 512; uint8_t* tags = x->tagbuf; uint8_t want[32];
        if (write && x->mode == 'J') e = journal_write(x, logical, n, block);
        else if (write) {
            for (uint32_t i = 0; i < n; i++) integrity_tag(x, logical + ((uint64_t)i << x->log2_spb), block + ((uint64_t)i << x->log2_spb) * 512, tags + i * x->tag_size);
            e = write_in_place(x, logical, n, block, tags);
        } else {
            e = block_part_read(x->dev, data_sector(x, area, offset), n << x->log2_spb, block) < 0 ? -1 : 0;
            if (!e && x->mode != 'R') e = tag_io(x, area, offset, n, tags, false);
            for (uint32_t i = 0; i < n && !e && x->mode != 'R'; i++) {
                uint64_t sector = logical + ((uint64_t)i << x->log2_spb);
                integrity_tag(x, sector, block + ((uint64_t)i << x->log2_spb) * 512, want);
                if (memcmp(want, tags + i * x->tag_size, x->tag_size)) e = integrity_mismatch(x, sector);
            }
        }
        done += n;
    }
    integrity_unlock(x);
    return e;
}

/* ---- table: <dev> <offset> <tag size> <mode> [<#opts> <opt>...] ---- */
static int parse_integrity(struct target* g, char* params, bool readonly) {
    char* c = params; char *device = word(&c), *offset = word(&c), *tag = word(&c), *mode = word(&c);
    if (!mode) return -22;
    struct integrity* x = (void*)arena_alloc(sizeof(*x)); if (!x) return -12;
    memset(x, 0, sizeof(*x)); g->integ = x; x->readonly = readonly;
    uint64_t start, tag_size; const char* end;
    if (!parse_u64(offset, &start, &end) || *end || !parse_u64(tag, &tag_size, &end) || *end || tag_size > 32) return -22;
    if (strlen(mode) != 1 || (mode[0] != 'J' && mode[0] != 'D' && mode[0] != 'R')) return -22;   /* B (bitmap): not done */
    x->mode = mode[0]; x->start = start; x->tag_size = (uint32_t)tag_size;
    uint64_t journal_sectors = 0, interleave = 0, block = 512; bool fix_padding = false, fix_hmac = false, hash = false;
    x->buffer_sectors = 128; x->watermark = 50; x->commit_time = 10000;
    char* count_text = word(&c);
    if (count_text) {
        uint64_t count; if (!parse_u64(count_text, &count, &end) || *end || count > 32) return -22;
        for (uint64_t i = 0; i < count; i++) {
            char* o = word(&c); if (!o) return -22;
            uint64_t v = 0; const char* colon = s_strchr(o, ':');
            bool number = colon && parse_u64(colon + 1, &v, &end) && !*end;
            if (!strncmp(o, "journal_sectors:", 16) && number) journal_sectors = v;
            else if (!strncmp(o, "interleave_sectors:", 19) && number) interleave = v;
            else if (!strncmp(o, "buffer_sectors:", 15) && number && v) x->buffer_sectors = (uint32_t)v;
            else if (!strncmp(o, "journal_watermark:", 18) && number && v <= 100) x->watermark = (uint32_t)v;
            else if (!strncmp(o, "commit_time:", 12) && number) x->commit_time = (uint32_t)v;
            else if (!strncmp(o, "block_size:", 11) && number) block = v;
            else if (!strncmp(o, "sectors_per_bit:", 16) || !strncmp(o, "bitmap_flush_interval:", 22)) continue;   /* bitmap mode only */
            else if (!strcmp(o, "internal_hash:crc32c")) hash = true;
            else if (!strcmp(o, "internal_hash:sha256")) { hash = true; x->sha = true; }
            else if (!strcmp(o, "fix_padding")) fix_padding = true;
            else if (!strcmp(o, "fix_hmac")) fix_hmac = true;
            else if (!strcmp(o, "allow_discards")) x->discard = true;
            else return -22;   /* journal_crypt, journal_mac, meta_device, recalculate, keyed or other hashes */
        }
    }
    if (word(&c)) return -22;
    if (!hash) return -22;   /* external tags (dm-crypt AEAD over integrity): not done */
    if (block < 512 || block > 4096 || (block & (block - 1))) return -22;
    x->spb = (uint32_t)(block / 512); x->log2_spb = log2_exact(x->spb);
    if (!x->tag_size) x->tag_size = x->sha ? 32 : 4;
    g->under = device_of(device); g->offset = start;
    if (!g->under) return -6;
    x->dev = g->under; x->device_sectors = block_part_sectors(x->dev);
    if (block < x->dev->dev->block_size || x->start % x->spb) return -22;
    if (!(x->meta = (void*)arena_alloc(INTEGRITY_JOURNAL_BLOCK * 512)) || !(x->bounce = (void*)arena_alloc((uint64_t)INTEGRITY_CHUNK * x->spb * 512)) ||
        !(x->tags = (void*)arena_alloc(8192))) return -12;
    uint8_t* sb = x->bounce;
    if (block_part_read(x->dev, x->start, INTEGRITY_SB_SECTORS, sb) < 0) return -5;
    bool formatted = false;
    if (memcmp(sb, "integrt\0", 8)) {
        for (unsigned i = 0; i < INTEGRITY_SB_SECTORS * 512; i++) if (sb[i]) { vga_print("[64] DM integrity: the device is not initialized\n"); return -22; }
        if (readonly || x->mode == 'R') return -22;
        /* Linux initialize_superblock: journal size, interleave, provided data sectors */
        if (!journal_sectors) journal_sectors = x->device_sectors >> 7 < 131072 ? x->device_sectors >> 7 : 131072;
        if (fix_padding) x->flags |= IF_FIXED_PADDING;
        if (fix_hmac) { x->flags |= IF_FIXED_HMAC; if (!native_random(x->salt, 16)) return -5; }
        section_size(x);
        x->sections = (uint32_t)(journal_sectors / x->section_sectors); if (!x->sections) x->sections = 1;
        unsigned l = 0; uint64_t want = interleave ? interleave : 32768; while (l < 63 && (2ULL << l) <= want) l++;
        x->log2_interleave = l < 3 ? 3 : l > 31 ? 31 : l;
        provided_search(x);
        if (!x->provided) return -22;
        x->version = x->flags & IF_FIXED_HMAC ? 5 : x->flags & IF_FIXED_PADDING ? 4 : 1;
        memset(sb, 0, INTEGRITY_SB_SECTORS * 512); memcpy(sb, "integrt\0", 8); sb[8] = x->version; sb[9] = (uint8_t)x->log2_interleave;
        sb[10] = (uint8_t)x->tag_size; sb[11] = (uint8_t)(x->tag_size >> 8); put_le32(sb + 12, x->sections); put_le64(sb + 16, x->provided);
        put_le32(sb + 24, x->flags); sb[28] = (uint8_t)x->log2_spb; memcpy(sb + 48, x->salt, 16);
        if (block_part_write(x->dev, x->start, INTEGRITY_SB_SECTORS, sb) < 0) return -5;
        formatted = true;
    } else {
        x->version = sb[8]; x->log2_interleave = sb[9]; x->sections = get_le32(sb + 12); x->provided = get_le64(sb + 16); x->flags = get_le32(sb + 24);
        uint32_t stored_tag = (uint32_t)sb[10] | (uint32_t)sb[11] << 8; memcpy(x->salt, sb + 48, 16);
        if (x->version < 1 || x->version > 5 || stored_tag != x->tag_size || sb[28] != x->log2_spb || x->log2_interleave < 3 || x->log2_interleave > 31 ||
            !x->sections || (x->flags & (IF_JOURNAL_MAC | IF_RECALCULATING | IF_DIRTY_BITMAP | IF_INLINE)) || (x->flags & ~0x3Fu) ||
            (x->provided & (x->spb - 1)) || !device_limits(x)) {
            vga_print("[64] DM integrity: superblock refused (version, tag size, block size, layout or features)\n"); return -22;
        }
    }
    if (g->length > x->provided || g->length % x->spb) { vga_print("[64] DM integrity: not enough provided sectors for the mapping\n"); return -22; }
    if (!(x->seqs = (void*)arena_alloc(x->sections))) return -12;
    memset(x->seqs, 0, x->sections);
    if (formatted) { if (journal_init(x) < 0) return -5; }
    else if (x->mode == 'J' && !readonly && journal_replay(x) < 0) return -5;
    return 0;
}
static void integrity_status(const struct target* g, bool table, char* text) {
    const struct integrity* x = g->integ;
    if (!table) { put_number(text, x->mismatches); strcat(text, " "); put_number(text, x->provided); strcat(text, " -"); return; }
    unsigned args = 3 + (x->spb != 1) + x->discard + 2 * (x->mode == 'J') + 1 + ((x->flags & IF_FIXED_PADDING) != 0) + ((x->flags & IF_FIXED_HMAC) != 0);
    put_number(text, x->dev->major); strcat(text, ":"); put_number(text, x->dev->minor); strcat(text, " "); put_number(text, x->start); strcat(text, " ");
    put_number(text, x->tag_size); strcat(text, x->mode == 'J' ? " J " : x->mode == 'D' ? " D " : " R "); put_number(text, args);
    if (x->spb != 1) { strcat(text, " block_size:"); put_number(text, x->spb * 512); }
    if (x->discard) strcat(text, " allow_discards");
    strcat(text, " journal_sectors:"); put_number(text, x->initial - INTEGRITY_SB_SECTORS);
    strcat(text, " interleave_sectors:"); put_number(text, 1ULL << x->log2_interleave);
    strcat(text, " buffer_sectors:"); put_number(text, x->buffer_sectors);
    if (x->mode == 'J') { strcat(text, " journal_watermark:"); put_number(text, x->watermark); strcat(text, " commit_time:"); put_number(text, x->commit_time); }
    if (x->flags & IF_FIXED_PADDING) strcat(text, " fix_padding");
    if (x->flags & IF_FIXED_HMAC) strcat(text, " fix_hmac");
    strcat(text, x->sha ? " internal_hash:sha256" : " internal_hash:crc32c");
}
#pragma clang attribute pop
