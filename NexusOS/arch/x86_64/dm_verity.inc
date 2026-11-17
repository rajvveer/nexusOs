/* dm-verity (Linux drivers/md/dm-verity-target.c, target version 1.10.0), included by dm.c: a read-only device whose
 * every data block is checked against a SHA-256 hash tree before it is returned (veritysetup format/open/verify/status).
 *
 * Table: "<version 0|1> <data dev> <hash dev> <data block> <hash block> <data blocks> <hash start block> sha256 <root
 * digest> <salt|-> [<#opts> ignore_corruption|restart_on_corruption|panic_on_corruption ignore_zero_blocks
 * check_at_most_once]". Tree layout as Linux computes it: levels until one hash block covers everything, the top level
 * first at <hash start>; version 1 hashes salt||block and pads each digest to a power of two, version 0 hashes
 * block||salt. Verified hash blocks stay in a small per-device cache (re-read ones are verified again). A mismatch
 * (data or hash block) logs "data|metadata block N is corrupted" (up to 100 times), sets the status to "C" and:
 * EIO (default), passes the data through (ignore_corruption), resets the machine (restart_on_corruption) or panics
 * (panic_on_corruption). Not done: FEC (use_fec_from_device ...), root_hash_sig_key_desc (signed root hashes, refused
 * with EINVAL so nothing is mounted unverified), algorithms other than sha256.
 * Boot (verity_boot, kernel.c after LVM): systemd's roothash=<hex> with systemd.verity_root_data=<spec> and
 * systemd.verity_root_hash=<spec> (/dev path, UUID=, PARTUUID=, LABEL=), systemd.verity_root_options=<o,...>
 * (ignore-corruption, restart-on-corruption, panic-on-corruption, ignore-zero-blocks, check-at-most-once,
 * hash-offset=<bytes>): the veritysetup superblock on the hash device gives the rest; /dev/mapper/root is the root. */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */
#define VERITY_CACHE 16
#define VERITY_LEVELS 63
#define VERITY_MAX_ERRS 100
enum { V_EIO, V_LOGGING, V_RESTART, V_PANIC };
struct verity {
    struct block_partition* data; struct block_partition* hash;
    uint32_t version, data_block, hash_block, bits, hash_bits; uint64_t data_blocks, hash_start, hash_blocks;
    uint8_t root[32], salt[256], zero[32]; unsigned salt_size, mode, errors, levels; bool ignore_zero, failed;
    uint64_t level_block[VERITY_LEVELS];
    uint8_t* checked; uint64_t checked_bytes;   /* check_at_most_once: verified data blocks */
    struct { uint64_t block; bool valid; uint8_t* data; } cache[VERITY_CACHE];
    volatile int busy;
};
static uint32_t verity_block_size(const struct verity* v) { return v->data_block; }
static void verity_free(struct verity* v) {
    if (!v) return;
    for (unsigned i = 0; i < VERITY_CACHE; i++) if (v->cache[i].data) arena_free((uint64_t)v->cache[i].data, v->hash_block);
    if (v->checked) arena_free((uint64_t)v->checked, v->checked_bytes);
    arena_free((uint64_t)v, sizeof(*v));
}
static void verity_lock(struct verity* v) {
    while (__atomic_exchange_n(&v->busy, 1, __ATOMIC_ACQUIRE)) {
        if (dm_may_wait()) kernel_io_sleep((void*)&v->busy, native_clock_ns() + 1000000ULL); else __asm__ volatile("pause");
    }
}
static void verity_unlock(struct verity* v) { __atomic_store_n(&v->busy, 0, __ATOMIC_RELEASE); task_wake_channel((void*)&v->busy); }
static void verity_digest(const struct verity* v, const uint8_t* block, uint32_t size, uint8_t out[32]) {
    struct sha256 s; sha256_init(&s);
    if (v->version) sha256_update(&s, v->salt, v->salt_size);
    sha256_update(&s, block, size);
    if (!v->version) sha256_update(&s, v->salt, v->salt_size);
    sha256_final(&s, out);
}
static int verity_corrupt(struct verity* v, bool metadata, uint64_t block) {   /* Linux verity_handle_err: 0 = go on */
    v->failed = true;
    if (v->errors < VERITY_MAX_ERRS) {
        char line[128] = "[64] DM verity "; put_number(line, v->data->major); strcat(line, ":"); put_number(line, v->data->minor);
        strcat(line, metadata ? ": metadata block " : ": data block "); put_number(line, block); strcat(line, " is corrupted\n");
        vga_print(line);
        if (++v->errors == VERITY_MAX_ERRS) vga_print("[64] DM verity: reached maximum errors\n");
    }
    if (v->mode == V_LOGGING) return 0;
    if (v->mode == V_RESTART) { vga_print("[64] DM verity: dm-verity device corrupted, restarting\n"); acpi_reset(); }
    if (v->mode == V_PANIC) panic("dm-verity device corrupted");
    return -1;
}
/* A hash block, verified against `want` (from the level above or the root digest); NULL: I/O error or corruption. */
static const uint8_t* verity_hash_block(struct verity* v, uint64_t block, const uint8_t want[32]) {
    unsigned slot = (unsigned)(block % VERITY_CACHE);
    if (v->cache[slot].valid && v->cache[slot].block == block) return v->cache[slot].data;
    uint8_t* buffer = v->cache[slot].data; v->cache[slot].valid = false;
    if (block_part_read(v->hash, block * (v->hash_block / 512), v->hash_block / 512, buffer) < 0) return NULL;
    uint8_t got[32]; verity_digest(v, buffer, v->hash_block, got);
    if (memcmp(got, want, 32)) return verity_corrupt(v, true, block) ? NULL : buffer;   /* logging mode: used, not cached */
    v->cache[slot].block = block; v->cache[slot].valid = true;
    return buffer;
}
static int verity_block(struct verity* v, uint64_t block, uint8_t* data) {
    if (v->checked && (v->checked[block / 8] >> (block % 8) & 1)) return 0;
    uint8_t want[32]; memcpy(want, v->root, 32);
    for (int level = (int)v->levels - 1; level >= 0; level--) {
        uint64_t position = block >> ((unsigned)level * v->bits);
        const uint8_t* h = verity_hash_block(v, v->level_block[level] + (position >> v->bits), want);
        if (!h) return -1;
        uint64_t index = position & ((1ULL << v->bits) - 1);
        memcpy(want, h + (v->version ? index << (v->hash_bits - v->bits) : index * 32), 32);
    }
    if (v->ignore_zero && !memcmp(want, v->zero, 32)) { memset(data, 0, v->data_block); return 0; }   /* ignore_zero_blocks */
    uint8_t got[32]; verity_digest(v, data, v->data_block, got);
    if (memcmp(got, want, 32)) return verity_corrupt(v, false, block);
    if (v->checked) v->checked[block / 8] |= (uint8_t)(1u << (block % 8));
    return 0;
}
static int verity_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {
    struct verity* v = g->verity; uint32_t per = v->data_block / 512;
    if (write || relative % per || count % per) return -1;
    if (block_part_read(v->data, relative, count, data) < 0) return -1;
    verity_lock(v);
    int e = 0;
    for (uint32_t i = 0; i < count && !e; i += per) e = verity_block(v, (relative + i) / per, data + (uint64_t)i * 512);
    verity_unlock(v);
    return e;
}
static int hex_bytes(const char* text, uint8_t* out, unsigned capacity) {   /* exact hex; returns the byte count */
    unsigned n = strlen(text);
    if (n % 2 || n / 2 > capacity) return -1;
    for (unsigned i = 0; i < n; i++) if (hex_value(text[i]) < 0) return -1;
    for (unsigned i = 0; i < n / 2; i++) out[i] = (uint8_t)(hex_value(text[2 * i]) << 4 | hex_value(text[2 * i + 1]));
    return (int)(n / 2);
}
static unsigned log2_exact(uint64_t v) { unsigned n = 0; while ((1ULL << n) < v) n++; return (1ULL << n) == v ? n : 0; }
static int parse_verity(struct target* g, char* params, bool readonly) {
    if (!readonly) return -22;   /* Linux: "Device must be readonly" */
    char* c = params;
    char *version = word(&c), *data = word(&c), *hash = word(&c), *data_block = word(&c), *hash_block = word(&c), *blocks = word(&c),
         *start = word(&c), *algorithm = word(&c), *digest = word(&c), *salt = word(&c);
    if (!salt) return -22;
    struct verity* v = (void*)arena_alloc(sizeof(*v)); if (!v) return -12;
    memset(v, 0, sizeof(*v)); g->verity = v;
    uint64_t n[5]; const char* end; const char* texts[5] = {version, data_block, hash_block, blocks, start};
    for (unsigned i = 0; i < 5; i++) if (!parse_u64(texts[i], &n[i], &end) || *end) return -22;
    if (n[0] > 1 || n[1] < 512 || n[1] > 4096 || !log2_exact(n[1]) || n[2] < 512 || n[2] > 4096 || !log2_exact(n[2])) return -22;
    v->version = (uint32_t)n[0]; v->data_block = (uint32_t)n[1]; v->hash_block = (uint32_t)n[2]; v->data_blocks = n[3]; v->hash_start = n[4];
    if (v->data_blocks > (~0ULL >> 13) || v->hash_start > (~0ULL >> 13)) return -22;   /* in sectors without overflow */
    if (strcmp(algorithm, "sha256")) return -22;
    if (hex_bytes(digest, v->root, 32) != 32) return -22;
    if (strcmp(salt, "-")) { int s = hex_bytes(salt, v->salt, sizeof(v->salt)); if (s < 0) return -22; v->salt_size = (unsigned)s; }
    char* count_text = word(&c);
    if (count_text) {
        uint64_t count; if (!parse_u64(count_text, &count, &end) || *end || count > 8) return -22;
        bool at_most_once = false;
        for (uint64_t i = 0; i < count; i++) {
            char* o = word(&c); if (!o) return -22;
            if (!strcmp(o, "ignore_corruption") && v->mode == V_EIO) v->mode = V_LOGGING;
            else if (!strcmp(o, "restart_on_corruption") && v->mode == V_EIO) v->mode = V_RESTART;
            else if (!strcmp(o, "panic_on_corruption") && v->mode == V_EIO) v->mode = V_PANIC;
            else if (!strcmp(o, "ignore_zero_blocks")) v->ignore_zero = true;
            else if (!strcmp(o, "check_at_most_once")) at_most_once = true;
            else return -22;   /* FEC, signatures, conflicting modes */
        }
        if (at_most_once) {
            v->checked_bytes = (v->data_blocks + 7) / 8; if (v->checked_bytes > (64u << 20)) return -12;
            v->checked = (void*)arena_alloc(v->checked_bytes); if (!v->checked) { v->checked_bytes = 0; return -12; }
            memset(v->checked, 0, v->checked_bytes);
        }
    }
    if (word(&c)) return -22;
    g->under = device_of(data); g->more[0] = device_of(hash); g->extra = 1;
    if (!g->under || !g->more[0]) return -6;
    v->data = g->under; v->hash = g->more[0];
    if (v->data_block < v->data->dev->block_size || v->hash_block < v->hash->dev->block_size) return -22;
    if (g->length > v->data_blocks * (v->data_block / 512) || g->length % (v->data_block / 512)) return -22;   /* "Data device is too small" */
    v->hash_bits = log2_exact(v->hash_block); v->bits = v->hash_bits - 5;   /* 32-byte digests per hash block */
    while (v->bits * v->levels < 64 && v->data_blocks && (v->data_blocks - 1) >> (v->bits * v->levels)) v->levels++;
    if (v->levels > VERITY_LEVELS) return -22;
    uint64_t position = v->hash_start;
    for (int i = (int)v->levels - 1; i >= 0; i--) {
        v->level_block[i] = position;
        uint64_t shift = (uint64_t)(i + 1) * v->bits;
        uint64_t s = shift >= 64 ? 1 : (v->data_blocks + (1ULL << shift) - 1) >> shift;
        if (position + s < position) return -22;
        position += s;
    }
    v->hash_blocks = position;
    uint64_t hash_sectors = block_part_sectors(v->hash);
    if (hash_sectors && v->hash_blocks > hash_sectors / (v->hash_block / 512)) {   /* "Hash device is too small" */
        vga_print("[64] DM verity: hash device too small\n"); return -22;
    }
    for (unsigned i = 0; i < VERITY_CACHE; i++) if (!(v->cache[i].data = (void*)arena_alloc(v->hash_block))) return -12;
    if (v->ignore_zero) {
        uint8_t* zero = v->cache[0].data; memset(zero, 0, v->data_block > v->hash_block ? v->hash_block : v->data_block);
        struct sha256 s; sha256_init(&s);   /* the digest of a zero data block (it may be larger than a cache buffer) */
        if (v->version) sha256_update(&s, v->salt, v->salt_size);
        for (uint32_t done = 0; done < v->data_block; done += v->hash_block < v->data_block ? v->hash_block : v->data_block)
            sha256_update(&s, zero, v->hash_block < v->data_block ? v->hash_block : v->data_block);
        if (!v->version) sha256_update(&s, v->salt, v->salt_size);
        sha256_final(&s, v->zero);
    }
    return 0;
}
static void verity_status(const struct target* g, bool table, char* text) {
    const struct verity* v = g->verity;
    if (!table) { strcat(text, v->failed ? "C" : "V"); return; }
    put_number(text, v->version); strcat(text, " ");
    put_number(text, v->data->major); strcat(text, ":"); put_number(text, v->data->minor); strcat(text, " ");
    put_number(text, v->hash->major); strcat(text, ":"); put_number(text, v->hash->minor); strcat(text, " ");
    put_number(text, v->data_block); strcat(text, " "); put_number(text, v->hash_block); strcat(text, " ");
    put_number(text, v->data_blocks); strcat(text, " "); put_number(text, v->hash_start); strcat(text, " sha256 ");
    for (unsigned i = 0; i < 32; i++) append_hex(text, v->root[i], 2);
    strcat(text, " ");
    if (!v->salt_size) strcat(text, "-"); else for (unsigned i = 0; i < v->salt_size; i++) append_hex(text, v->salt[i], 2);
    unsigned args = (v->mode != V_EIO) + v->ignore_zero + (v->checked != NULL);
    if (!args) return;
    strcat(text, " "); put_number(text, args);
    static const char* const modes[4] = {"", " ignore_corruption", " restart_on_corruption", " panic_on_corruption"};
    strcat(text, modes[v->mode]);
    if (v->ignore_zero) strcat(text, " ignore_zero_blocks");
    if (v->checked) strcat(text, " check_at_most_once");
}

/* ---- boot: a verity root from systemd's roothash= options (kernel.c, after LVM: verity may sit on an LV) ---- */
int block_spec_matches(struct block_partition* p, const char* spec);
bool boot_value(const char* key, char* value, unsigned size);   /* boot_info.h */
static struct block_partition* spec_partition(const char* spec) {
    if (!strncmp(spec, "/dev/", 5)) { struct block_partition* p = device_of(spec); if (p) return p; }
    for (unsigned i = 0; i < block_partition_count(); i++) {
        struct block_partition* p = (struct block_partition*)block_partition_get(i);
        if (p && !p->dev->removed && block_spec_matches(p, spec) == 1) return p;
    }
    for (unsigned i = 0; i < block_device_count(); i++) {
        struct block_device* d = block_device_get(i); struct block_partition* w = d && !d->removed ? block_whole(d) : NULL;
        if (w && block_spec_matches(w, spec) == 1) return w;
    }
    return NULL;
}
static bool option_in(const char* list, const char* name) {
    unsigned n = strlen(name);
    for (const char* p = list; *p; ) {
        const char* e = p; while (*e && *e != ',') e++;
        if ((unsigned)(e - p) == n && !strncmp(p, name, n)) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}
bool verity_boot(void) {
    char root[160], data_spec[128], hash_spec[128], options[256] = "";
    if (!boot_value("roothash", root, sizeof(root))) return false;
    uint8_t digest[32];
    if (hex_bytes(root, digest, 32) != 32) { vga_print("[64] VERITY: roothash= is not a SHA-256 digest in hex\n"); return false; }
    if (!boot_value("systemd.verity_root_data", data_spec, sizeof(data_spec)) || !boot_value("systemd.verity_root_hash", hash_spec, sizeof(hash_spec))) {
        vga_print("[64] VERITY: roothash= needs systemd.verity_root_data= and systemd.verity_root_hash=\n"); return false;
    }
    boot_value("systemd.verity_root_options", options, sizeof(options));
    struct block_partition* data = spec_partition(data_spec); struct block_partition* hash = spec_partition(hash_spec);
    if (!data || !hash) { vga_print("[64] VERITY: data or hash device not found\n"); return false; }
    uint64_t offset = 0;
    for (const char* p = options; *p; ) {   /* hash-offset=<bytes> */
        if (!strncmp(p, "hash-offset=", 12)) { const char* end; if (!parse_u64(p + 12, &offset, &end) || (offset & 511)) { vga_print("[64] VERITY: bad hash-offset\n"); return false; } }
        while (*p && *p != ',') p++;
        if (*p) p++;
    }
    uint8_t* sb = (void*)arena_alloc(4096); if (!sb) return false;
    bool ok = block_part_read(hash, offset / 512, 1, sb) == 0 && !memcmp(sb, "verity\0\0", 8) && get_le32(sb + 8) == 1;
    uint32_t type = get_le32(sb + 12), data_block = get_le32(sb + 64), hash_block = get_le32(sb + 68); uint64_t blocks = get_le64(sb + 72);
    unsigned salt_size = (unsigned)sb[80] | (unsigned)sb[81] << 8;
    char algorithm[33]; memcpy(algorithm, sb + 32, 32); algorithm[32] = 0;
    if (ok && (type > 1 || strcmp(algorithm, "sha256") || salt_size > 256 || !hash_block || hash_block > 4096 || !blocks)) ok = false;
    char line[1024] = "", uuid[64] = "CRYPT-VERITY-";
    if (ok) {
        put_number(line, 0); strcat(line, " "); put_number(line, blocks * (data_block / 512)); strcat(line, " verity "); put_number(line, type); strcat(line, " ");
        put_number(line, data->major); strcat(line, ":"); put_number(line, data->minor); strcat(line, " ");
        put_number(line, hash->major); strcat(line, ":"); put_number(line, hash->minor); strcat(line, " ");
        put_number(line, data_block); strcat(line, " "); put_number(line, hash_block); strcat(line, " "); put_number(line, blocks); strcat(line, " ");
        put_number(line, (offset + 512 + hash_block - 1) / hash_block); strcat(line, " sha256 ");   /* libcryptsetup VERITY_hash_offset_block */
        for (unsigned i = 0; i < 32; i++) append_hex(line, digest[i], 2);
        strcat(line, " ");
        if (!salt_size) strcat(line, "-"); else for (unsigned i = 0; i < salt_size; i++) append_hex(line, sb[88 + i], 2);
        static const char* const names[5] = {"ignore-corruption", "restart-on-corruption", "panic-on-corruption", "ignore-zero-blocks", "check-at-most-once"};
        static const char* const table[5] = {" ignore_corruption", " restart_on_corruption", " panic_on_corruption", " ignore_zero_blocks", " check_at_most_once"};
        char extra[128] = ""; unsigned count = 0;
        for (unsigned i = 0; i < 5; i++) if (option_in(options, names[i])) { strcat(extra, table[i]); count++; }
        if (count) { strcat(line, " "); put_number(line, count); strcat(line, extra); }
        for (unsigned i = 0; i < 16; i++) append_hex(uuid, sb[16 + i], 2);   /* libcryptsetup dm_prepare_uuid: no dashes */
        strcat(uuid, "-root");
    }
    arena_free((uint64_t)sb, 4096);
    if (!ok) { vga_print("[64] VERITY: no usable verity superblock on the hash device\n"); return false; }
    const char* lines[1] = {line}; struct block_device* dev;
    int e = dm_kernel_create("root", uuid, lines, 1, true, &dev);
    if (e) { vga_print("[64] VERITY: /dev/mapper/root refused\n"); return false; }
    char value[96];
    if (boot_value("root", value, sizeof(value)) && strcmp(value, "/dev/mapper/root")) { vga_print("[64] VERITY: /dev/mapper/root ready (root= names another device)\n"); return false; }
    block_unlocked_root = block_whole(dev);
    vga_print("[64] VERITY: /dev/mapper/root verified against roothash= (read-only)\n");
    return true;
}
#pragma clang attribute pop
