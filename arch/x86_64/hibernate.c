/* Hibernation (suspend-to-disk): the image writer (pm.c s4: /sys/power/state "disk", reboot(2) SW_SUSPEND, the
 * critical-battery and hot-trip policies, Aurora's Hibernate) and the boot-time restore (kernel.c, right after the
 * disks are probed - before anything is mounted, written or started).
 *
 * Area (first found): a GPT partition of type NEXUS_HIBERNATE (C0B1A0B7-A548-4E4E-9F1D-4E5848494245) or an MBR
 * partition of type 0xA0 on any disk; else the file /nexus/hiber.img on the FAT32 root, preallocated by the user
 * (`dd if=/dev/zero of=/nexus/hiber.img bs=1M count=1024`; about 40% of RAM is plenty, the image is compressed). The
 * file's clusters are written in place - its directory entry and FAT chain never change - and it is located again at
 * boot by reading the FAT directly, before the file system is mounted. The image never uses swap.
 *
 * Image: header (4 KiB, CRC32) | PFN bitmap (CRC32) | blocks of up to 16 pages, each {magic, sequence, pages,
 * zero-page mask, payload bytes, CRC32} + the non-zero pages LZ77-compressed (stored when that does not help).
 * Saved: every page of the page pool in use (with the pool's own reference tables) and the usable RAM below 16 MiB
 * (kernel image, boot page tables, trampolines). Not saved: free pages and this writer's own buffers.
 *
 * Consistency: the snapshot is the CPU context pm_suspend_cpu saves after every non-storage driver suspended and the
 * other CPUs went offline; the image is then written from live memory by the boot CPU alone, with only the storage
 * driver, the timer and this writer running. What changes during the write (storage queues, clock counters, this
 * file's state) is reinitialised on restore: every driver's resume runs (storage included) and the clocks are
 * recomputed from the RTC as after S3.
 *
 * Restore (hibernate_resume): checks - magic/version/CRC, kernel identity (SHA-256 of text+rodata), machine (memory
 * map, framebuffer, CPUID signature, CPU count; FACS hardware signature), disk identity (SHA-256 of the root volume's
 * first 8 MiB with the area's own sectors read as zeros, and the area's location: a file system changed since the
 * image was written makes it stale). Any failed check discards the image (header invalidated); the boot continues.
 * Loading is one pass: a page whose frame is free in this (boot) kernel is claimed and filled in place, the others go
 * to spare frames free in both kernels and are copied at the end (a load error frees everything and boots on).
 * Before the jump the header is invalidated (a failed restore is never retried), every driver is quiesced and the
 * other CPUs are stopped; hibernate_restore_jump (pm_asm.asm) copies the spare frames on temporary page tables,
 * switches to the image's tables and enters pm_wake_long as an S3 wake does; pm.c then resumes as after S3. */
#include "native.h"
#include "power.h"
#include "blockdev.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define PAGE 4096ULL
#define LOW_LIMIT 0x1000000ULL                 /* below: RAM outside the page pool (kernel image), saved whole */
#define HEADER_BYTES 4096U
#define BLOCK_PAGES 16U
#define BLOCK_MAGIC 0x4B4C4248U                /* "HBLK" */
#define BLOCK_STORED 0x80000000U
#define STREAM_BYTES (2U << 20)                /* write/read staging */
#define FINGERPRINT_BYTES (8U << 20)
#define AREA_EXTENTS 512U
#define HIB_VERSION 2U                         /* 2: area kinds, swap areas */
#define HIB_VALID 0x56414C49U
#define EXCLUDED_MAX 8U
#define LZ_HASH_BITS 12U
static const char HIB_MAGIC[8] = {'N', 'X', 'H', 'I', 'B', 'E', 'R', '1'};
static const uint8_t HIB_GUID[16] = {0xB7, 0xA0, 0xB1, 0xC0, 0x48, 0xA5, 0x4E, 0x4E, 0x9F, 0x1D, 0x4E, 0x58, 0x48, 0x49, 0x42, 0x45};

extern uint8_t __text_start[], __rodata_end[], __bss_end[];
extern uint64_t boot_pml4[];
extern void pm_wake_long(void);
extern void hibernate_restore_jump(uint64_t list, uint64_t temp_cr3, uint64_t image_cr3, uint64_t entry) __attribute__((noreturn));
void pm_quiesce_all(void);
void pm_stop_other_cpus(void);

struct hib_header {
    char magic[8];
    uint32_t version, state;                   /* HIB_VALID while the image may be restored */
    uint8_t kernel[32], machine[32], disk[32];
    uint64_t facs;                             /* acpi_facs_hardware_signature() at hibernation */
    uint64_t pfn_limit, pages, stored_pages, blocks;
    uint64_t bitmap_offset, bitmap_bytes, data_offset, data_bytes;
    uint64_t created, raw_bytes, restore_pages;
    uint32_t bitmap_crc, cpus;
    uint32_t kind, table_bytes, table_crc;     /* area kind (AREA_*); swap areas: the extent table after the header */
    uint64_t swap_header;                      /* swap areas: sector of the swap header page (its tail points here) */
    uint32_t crc;                              /* CRC32 of this structure with crc = 0 */
};
struct hib_block { uint32_t magic, sequence; uint16_t pages, zero_mask; uint32_t bytes, crc, reserved; };
_Static_assert(sizeof(struct hib_block) == 24 && sizeof(struct hib_header) <= HEADER_BYTES, "hibernation image layout");

static void say(const char* s) { vga_print(s); }
static void say_number(uint64_t value) { char t[24]; number64(value, t); vga_print(t); }

/* ---- SHA-256 (FIPS 180-4) ---- */
struct sha { uint32_t h[8]; uint64_t length; uint8_t block[64]; unsigned used; };
static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }
static void sha_block(struct sha* s, const uint8_t* p) {
    uint32_t w[64], v[8];
    for (unsigned i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (unsigned i = 0; i < 8; i++) v[i] = s->h[i];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha_k[i] + w[i];
        uint32_t t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (unsigned i = 0; i < 8; i++) s->h[i] += v[i];
}
static void sha_init(struct sha* s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv)); s->length = 0; s->used = 0;
}
static void sha_update(struct sha* s, const void* data, uint64_t n) {
    const uint8_t* p = data; s->length += n;
    while (n) {
        if (!s->used && n >= 64) { sha_block(s, p); p += 64; n -= 64; continue; }
        unsigned take = 64 - s->used; if (take > n) take = (unsigned)n;
        memcpy(s->block + s->used, p, take); s->used += take; p += take; n -= take;
        if (s->used == 64) { sha_block(s, s->block); s->used = 0; }
    }
}
static void sha_final(struct sha* s, uint8_t out[32]) {
    uint64_t bits = s->length * 8; uint8_t pad[72] = {0x80}; unsigned n = s->used < 56 ? 56 - s->used : 120 - s->used;
    for (unsigned i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, pad, n + 8);
    for (unsigned i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}

/* ---- LZ77: sequences [token: literal count << 4 | (match length - 4)] [count extensions] [literals] [offset, 16 bit]
 * [length extensions]; a nibble of 15 continues in bytes (255 = more). The last sequence may end after its literals. */
static uint32_t lz_read32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint8_t* lz_extend(uint8_t* out, uint8_t* end, uint32_t value) {
    while (value >= 255) { if (out >= end) return NULL; *out++ = 255; value -= 255; }
    if (out >= end) return NULL;
    *out++ = (uint8_t)value; return out;
}
static uint32_t lz_compress(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t capacity, uint32_t* table) {
    for (unsigned i = 0; i < 1U << LZ_HASH_BITS; i++) table[i] = 0xFFFFFFFFU;
    uint8_t* o = out, *end = out + capacity; uint32_t anchor = 0, pos = 0;
    while (pos + 12 <= n) {   /* the last 11 bytes stay literal */
        uint32_t sequence = lz_read32(in + pos), h = (sequence * 2654435761U) >> (32 - LZ_HASH_BITS), candidate = table[h];
        table[h] = pos;
        if (candidate == 0xFFFFFFFFU || pos - candidate > 65535 || lz_read32(in + candidate) != sequence) { pos++; continue; }
        uint32_t length = 4; while (pos + length < n && in[candidate + length] == in[pos + length]) length++;
        uint32_t literals = pos - anchor;
        if ((uint64_t)(end - o) < 1ULL + literals + literals / 255 + 4 + length / 255) return 0;
        uint8_t* token = o++;
        *token = (uint8_t)((literals >= 15 ? 15 : literals) << 4 | (length - 4 >= 15 ? 15 : length - 4));
        if (literals >= 15 && !(o = lz_extend(o, end, literals - 15))) return 0;
        memcpy(o, in + anchor, literals); o += literals;
        uint32_t offset = pos - candidate; *o++ = (uint8_t)offset; *o++ = (uint8_t)(offset >> 8);
        if (length - 4 >= 15 && !(o = lz_extend(o, end, length - 4 - 15))) return 0;
        pos += length; anchor = pos;
    }
    uint32_t literals = n - anchor;
    if (literals) {
        if ((uint64_t)(end - o) < 2ULL + literals + literals / 255) return 0;
        *o++ = (uint8_t)((literals >= 15 ? 15 : literals) << 4);
        if (literals >= 15 && !(o = lz_extend(o, end, literals - 15))) return 0;
        memcpy(o, in + anchor, literals); o += literals;
    }
    return (uint32_t)(o - out);
}
static bool lz_decompress(const uint8_t* in, uint32_t size, uint8_t* out, uint32_t n) {
    const uint8_t* i = in, *end = in + size; uint32_t o = 0;
    while (o < n) {
        if (i >= end) return false;
        uint8_t token = *i++; uint32_t literals = token >> 4;
        if (literals == 15) { uint8_t b; do { if (i >= end) return false; b = *i++; literals += b; } while (b == 255); }
        if (literals > n - o || literals > (uint32_t)(end - i)) return false;
        memcpy(out + o, i, literals); i += literals; o += literals;
        if (o == n) break;
        if (end - i < 2) return false;
        uint32_t offset = i[0] | (uint32_t)i[1] << 8, length = (token & 15U) + 4; i += 2;
        if ((token & 15U) == 15) { uint8_t b; do { if (i >= end) return false; b = *i++; length += b; } while (b == 255); }
        if (!offset || offset > o || length > n - o) return false;
        const uint8_t* from = out + o - offset;
        if (offset >= length) memcpy(out + o, from, length); else for (uint32_t k = 0; k < length; k++) out[o + k] = from[k];
        o += length;
    }
    return i == end;
}

/* ---- the area ---- */
struct extent { uint64_t sector, count; };   /* 512-byte sectors of area.part */
enum { AREA_PARTITION, AREA_FILE, AREA_SWAP };
static struct {
    const struct block_partition* part; struct extent extents[AREA_EXTENTS]; unsigned count; uint64_t sectors; bool file;
    unsigned kind; int swap_type; uint64_t swap_header;   /* AREA_SWAP: swap.c's area, its header page's sector */
} area;
static uint8_t* scratch;                     /* one page: FAT sectors */
static bool scratch_page(void) { if (!scratch) scratch = (uint8_t*)page_alloc(); return scratch != NULL; }
static int area_io(uint64_t offset, void* buffer, uint64_t bytes, bool write) {   /* offset, bytes: whole sectors */
    uint64_t sector = offset / 512, count = bytes / 512; uint8_t* p = buffer;
    if ((offset | bytes) & 511 || sector + count > area.sectors || sector + count < sector) return -28;
    for (unsigned e = 0; e < area.count && count; e++) {
        if (sector >= area.extents[e].count) { sector -= area.extents[e].count; continue; }
        while (count && sector < area.extents[e].count) {
            uint64_t take = area.extents[e].count - sector; if (take > count) take = count; if (take > 2048) take = 2048;
            int error = write ? block_part_write(area.part, area.extents[e].sector + sector, (uint32_t)take, p)
                              : block_part_read(area.part, area.extents[e].sector + sector, (uint32_t)take, p);
            if (error) return -5;
            p += take * 512; sector += take; count -= take;
        }
        sector = 0;
    }
    return count ? -28 : 0;
}
/* FAT32 root: /nexus/hiber.img by its 8.3 names, read straight from the volume (works before the mount). */
static struct { uint32_t bps, spc, reserved, fats, fat_size, root, clusters; uint64_t data; } fat;
static uint64_t fat_cached = ~0ULL;
static bool fat_entry(uint32_t cluster, uint32_t* next) {
    uint64_t byte = (uint64_t)cluster * 4, sector = (uint64_t)fat.reserved * (fat.bps / 512) + byte / 512;
    if (sector != fat_cached) { if (block_part_read(area.part, sector, 1, scratch)) return false; fat_cached = sector; }
    *next = *(uint32_t*)(scratch + byte % 512) & 0x0FFFFFFF; return true;
}
static uint64_t cluster_sector(uint32_t cluster) { return fat.data + (uint64_t)(cluster - 2) * fat.spc * (fat.bps / 512); }
static bool directory_find(uint32_t cluster, const char name[11], bool directory, uint32_t* first, uint32_t* size) {
    for (unsigned guard = 0; cluster >= 2 && cluster < fat.clusters + 2 && guard < 65536; guard++) {
        for (uint32_t s = 0; s < fat.spc * (fat.bps / 512); s++) {
            uint8_t sector[512];
            if (block_part_read(area.part, cluster_sector(cluster) + s, 1, sector)) return false;
            for (unsigned at = 0; at < 512; at += 32) {
                const uint8_t* e = sector + at;
                if (!e[0]) return false;
                if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
                if (memcmp(e, name, 11) || !!(e[11] & 0x10) != directory) continue;
                *first = (uint32_t)(e[20] | e[21] << 8) << 16 | (uint32_t)(e[26] | e[27] << 8);
                *size = (uint32_t)e[28] | (uint32_t)e[29] << 8 | (uint32_t)e[30] << 16 | (uint32_t)e[31] << 24;
                return true;
            }
        }
        uint32_t next;
        if (!fat_entry(cluster, &next)) return false;
        cluster = next;
    }
    return false;
}
static bool file_area(const struct block_partition* root) {
    uint8_t boot[512];
    area.part = root; fat_cached = ~0ULL;
    if (!scratch_page() || block_part_read(root, 0, 1, boot) || boot[510] != 0x55 || boot[511] != 0xAA) return false;
    fat.bps = boot[11] | boot[12] << 8; fat.spc = boot[13]; fat.reserved = boot[14] | boot[15] << 8; fat.fats = boot[16];
    fat.fat_size = (uint32_t)boot[36] | (uint32_t)boot[37] << 8 | (uint32_t)boot[38] << 16 | (uint32_t)boot[39] << 24;
    fat.root = (uint32_t)boot[44] | (uint32_t)boot[45] << 8 | (uint32_t)boot[46] << 16 | (uint32_t)boot[47] << 24;
    uint32_t total = (uint32_t)boot[32] | (uint32_t)boot[33] << 8 | (uint32_t)boot[34] << 16 | (uint32_t)boot[35] << 24;
    if ((fat.bps != 512 && fat.bps != 1024 && fat.bps != 2048 && fat.bps != 4096) || !fat.spc || (fat.spc & (fat.spc - 1)) || !fat.fats ||
        !fat.fat_size || boot[17] || boot[18] || !total) return false;   /* FAT32 only (no fixed root directory) */
    uint64_t data = (uint64_t)fat.reserved + (uint64_t)fat.fats * fat.fat_size;
    if (data >= total) return false;
    fat.clusters = (uint32_t)((total - data) / fat.spc); fat.data = data * (fat.bps / 512);
    uint32_t nexus, size, first;
    if (!directory_find(fat.root, "NEXUS      ", true, &nexus, &size) || !directory_find(nexus, "HIBER   IMG", false, &first, &size)) return false;
    uint64_t cluster_sectors = (uint64_t)fat.spc * (fat.bps / 512), wanted = size / (cluster_sectors * 512);
    area.count = 0; area.sectors = 0;
    for (uint32_t c = first, n = 0; n < wanted; n++) {
        if (c < 2 || c >= fat.clusters + 2) return false;
        uint64_t s = cluster_sector(c);
        if (area.count && area.extents[area.count - 1].sector + area.extents[area.count - 1].count == s) area.extents[area.count - 1].count += cluster_sectors;
        else if (area.count == AREA_EXTENTS) { say("[64] PM: /nexus/hiber.img is too fragmented\n"); return false; }
        else area.extents[area.count++] = (struct extent){s, cluster_sectors};
        area.sectors += cluster_sectors;
        if (n + 1 < wanted && !fat_entry(c, &c)) return false;
    }
    area.file = true;
    return area.sectors >= 64ULL * 1024 * 2;   /* at least 64 MiB */
}
static uint64_t available_at;   /* hibernate_available's cache (tentative: defined with it below) */
static bool dedicated_area(void) {
    area.count = 0; area.sectors = 0; area.file = false; area.part = NULL; area.kind = AREA_PARTITION; area.swap_type = -1;
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        if (!p || !((p->scheme == BLOCK_SCHEME_GPT && !memcmp(p->type_guid, HIB_GUID, 16)) || (p->scheme == BLOCK_SCHEME_MBR && p->mbr_type == 0xA0))) continue;
        area.part = p; area.extents[0] = (struct extent){0, block_part_sectors(p)}; area.count = 1; area.sectors = block_part_sectors(p);
        return area.sectors >= 64ULL * 1024 * 2;
    }
    const struct block_partition* root = block_root();
    if (root && file_area(root)) { area.kind = AREA_FILE; return true; }
    return false;
}

/* ---- swap areas: the image in free slots of an active swap partition or file (swap.c) ----
 * Where: resume= (/dev/NAME, UUID= or LABEL= of the mkswap header, PARTUUID= of a GPT partition, MAJOR:MINOR) and, for
 * a swap file, resume_offset= (its header page, in 4 KiB pages of that partition) - the kernel options, or what was
 * written to /sys/power/resume{,_offset}; without resume= only a partition of swap type (MBR 0x82, GPT Linux swap),
 * which the next boot finds by itself. Layout: the image header, the extent table (where the image's slots lie), the
 * bitmap, the blocks. The swap header page keeps what mkswap wrote; its last 40 bytes (Linux's swsusp_header place)
 * get the signature "NXSUSPEND", the original "SWAPSPACE2" and the first extent - written last, after the image is
 * complete and flushed; any boot that finds it (restoring or discarding) puts "SWAPSPACE2" back first. */
int swap_hibernate_pick(const struct block_partition* part, uint64_t header_page, uint64_t* header_sector, uint64_t* free_pages, const struct block_partition** area_part);
uint64_t swap_hibernate_reserve(unsigned type, uint64_t pages, int (*run)(void* context, uint64_t sector, uint64_t sectors), void* context);
void swap_hibernate_release(unsigned type);
uint64_t reclaim_hibernate(uint64_t want_free);   /* reclaim.c */
uint64_t pm_image_size(void);                     /* power.c: /sys/power/image_size */
uint64_t memory_free_pages(void);                 /* memory.c */
struct swap_tail { uint32_t crc; uint64_t first, first_sectors; char orig[10], sig[10]; } __attribute__((packed));
_Static_assert(sizeof(struct swap_tail) == 40, "swap header tail");
static const char SWAP_SIGNATURE[10] = {'S', 'W', 'A', 'P', 'S', 'P', 'A', 'C', 'E', '2'};
static const char IMAGE_SIGNATURE[10] = {'N', 'X', 'S', 'U', 'S', 'P', 'E', 'N', 'D', 0};
static const uint8_t LINUX_SWAP_GUID[16] = {0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43, 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F};
static const struct block_partition* resume_part; static uint64_t resume_page; static bool resume_known;
static char resume_spec[96];
static int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }
static bool parse_uuid(const char* text, uint8_t out[16]) {   /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, bytes in text order */
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (text[n] == '-') n++;
        int hi = hex_value(text[n]), lo = hi < 0 ? -1 : hex_value(text[n + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo); n += 2;
    }
    return !text[n];
}
static bool swap_header_names(const struct block_partition* p, const char* spec) {   /* UUID= / LABEL= of the mkswap header */
    if (!scratch_page() || block_part_read(p, 0, 8, scratch)) return false;
    bool swap = !memcmp(scratch + PAGE - 10, SWAP_SIGNATURE, 10) || !memcmp(scratch + PAGE - 10, IMAGE_SIGNATURE, 10);
    if (!swap) return false;
    if (!strncmp(spec, "LABEL=", 6)) return scratch[1052] && strlen(spec + 6) <= 16 && !strncmp((const char*)scratch + 1052, spec + 6, 16);
    uint8_t uuid[16]; return parse_uuid(spec + 5, uuid) && !memcmp(scratch + 1036, uuid, 16);
}
/* UUID= / LABEL= of a filesystem (a swap file on it: resume=UUID=<fs> resume_offset=N): ext4 s_uuid / s_volume_name,
 * FAT32 volume serial (XXXX-XXXX) / volume label. */
static bool fs_names(const struct block_partition* p, const char* spec) {
    if (!scratch_page() || block_part_read(p, 0, 8, scratch)) return false;
    bool label = !strncmp(spec, "LABEL=", 6); const char* want = spec + (label ? 6 : 5);
    if (scratch[1024 + 0x38] == 0x53 && scratch[1024 + 0x39] == 0xEF) {   /* ext2/3/4 superblock magic */
        if (label) return want[0] && strlen(want) <= 16 && !strncmp((const char*)scratch + 1024 + 0x78, want, 16);
        uint8_t uuid[16]; return parse_uuid(want, uuid) && !memcmp(scratch + 1024 + 0x68, uuid, 16);
    }
    if (scratch[510] != 0x55 || scratch[511] != 0xAA || memcmp(scratch + 0x52, "FAT32   ", 8)) return false;
    if (label) { unsigned n = (unsigned)strlen(want); if (!n || n > 11) return false;
                 for (unsigned i = 0; i < 11; i++) if (scratch[0x47 + i] != (i < n ? (uint8_t)want[i] : ' ')) return false; return true; }
    uint32_t serial = (uint32_t)scratch[0x43] | (uint32_t)scratch[0x44] << 8 | (uint32_t)scratch[0x45] << 16 | (uint32_t)scratch[0x46] << 24;
    if (strlen(want) != 9 || want[4] != '-') return false;
    for (unsigned i = 0, shift = 28; i < 9; i++) {
        if (i == 4) continue;
        if (hex_value(want[i]) != (int)((serial >> shift) & 15)) return false;
        shift -= 4;
    }
    return true;
}
/* stor2's encrypted root (cryptroot.c, blockdev.h): the opened LUKS volume's whole-device partition, and its
 * /dev/mapper name (dm.c). Weak: absent without the cryptroot code. */
extern const struct block_partition* block_unlocked_root __attribute__((weak));
const char* dm_device_name(const struct block_device* dev) __attribute__((weak));
static const struct block_partition* unlocked_root(void) { return &block_unlocked_root ? block_unlocked_root : NULL; }
static const struct block_partition* partition_spec(const char* spec) {
    uint8_t guid[16]; bool partuuid = !strncmp(spec, "PARTUUID=", 9) && parse_uuid(spec + 9, guid);
    if (partuuid) {   /* GPT unique GUIDs are stored mixed-endian */
        uint8_t t; t = guid[0]; guid[0] = guid[3]; guid[3] = t; t = guid[1]; guid[1] = guid[2]; guid[2] = t;
        t = guid[4]; guid[4] = guid[5]; guid[5] = t; t = guid[6]; guid[6] = guid[7]; guid[7] = t;
    }
    unsigned major = 0, minor = 0; const char* colon = NULL; bool numbers = spec[0] >= '0' && spec[0] <= '9';
    for (const char* c = spec; numbers && *c; c++) { if (*c == ':' && !colon) colon = c; else if (*c < '0' || *c > '9') numbers = false; }
    if (numbers && colon) { for (const char* c = spec; c < colon; c++) major = major * 10 + (unsigned)(*c - '0'); for (const char* c = colon + 1; *c; c++) minor = minor * 10 + (unsigned)(*c - '0'); }
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i); if (!p) continue;
        if (!strncmp(spec, "/dev/", 5) && (!strcmp(spec + 5, p->linux_name) || !strcmp(spec + 5, p->label))) return p;
        if (partuuid && p->scheme == BLOCK_SCHEME_GPT && !memcmp(p->unique_guid, guid, 16)) return p;
        if (numbers && colon && p->major == major && p->minor == minor) return p;
        if ((!strncmp(spec, "UUID=", 5) || !strncmp(spec, "LABEL=", 6)) && (swap_header_names(p, spec) || fs_names(p, spec))) return p;
    }
    const struct block_partition* dm = unlocked_root();   /* resume=/dev/mapper/NAME, or UUID=/LABEL= inside it */
    if (dm && !strncmp(spec, "/dev/mapper/", 12) && dm_device_name && dm_device_name(dm->dev) && !strcmp(spec + 12, dm_device_name(dm->dev))) return dm;
    if (dm && (!strncmp(spec, "UUID=", 5) || !strncmp(spec, "LABEL=", 6)) && (swap_header_names(dm, spec) || fs_names(dm, spec))) return dm;
    return NULL;
}
static void resume_config(void) {   /* the boot options, once (a /sys/power/resume write replaces them) */
    if (resume_known) return;
    resume_known = true;
    if (boot_value("resume", resume_spec, sizeof(resume_spec))) resume_part = partition_spec(resume_spec);
    char text[24]; resume_page = 0;
    if (boot_value("resume_offset", text, sizeof(text))) for (const char* c = text; *c >= '0' && *c <= '9' && resume_page < (1ULL << 40); c++) resume_page = resume_page * 10 + (uint64_t)(*c - '0');
}
/* /sys/power/resume and resume_offset (power.c): what the next hibernation writes to; Linux's MAJOR:MINOR form. */
int hibernate_resume_set(const char* spec) {
    resume_config();
    char value[96]; unsigned n = 0; while (spec[n] && spec[n] != '\n' && n + 1 < sizeof(value)) { value[n] = spec[n]; n++; } value[n] = 0;
    if (!strcmp(value, "0:0")) { resume_part = NULL; resume_spec[0] = 0; available_at = 0; return 0; }
    const struct block_partition* p = partition_spec(value); if (!p) return -19;
    resume_part = p; memcpy(resume_spec, value, n + 1); available_at = 0; return 0;
}
int hibernate_resume_offset_set(uint64_t page) { resume_config(); resume_page = page; available_at = 0; return 0; }
void hibernate_resume_get(unsigned* major, unsigned* minor, uint64_t* page) {
    resume_config(); *major = resume_part ? resume_part->major : 0; *minor = resume_part ? resume_part->minor : 0; *page = resume_page;
}
static int add_run(void* context, uint64_t sector, uint64_t sectors) {
    (void)context;
    if (area.count && area.extents[area.count - 1].sector + area.extents[area.count - 1].count == sector) { area.extents[area.count - 1].count += sectors; area.sectors += sectors; return 0; }
    if (area.count == AREA_EXTENTS) return -28;
    area.extents[area.count++] = (struct extent){sector, sectors}; area.sectors += sectors; return 0;
}
static bool swap_area(void) {   /* an active swap area for the next image (no slot taken yet) */
    uint64_t header, free_pages; const struct block_partition* p;
    resume_config();
    int type = swap_hibernate_pick(resume_part, resume_page, &header, &free_pages, &p);
    if (type < 0) return false;
    area.count = 0; area.sectors = 0; area.file = false; area.part = p; area.kind = AREA_SWAP; area.swap_type = type; area.swap_header = header;
    return free_pages >= 16ULL * 1024 * 1024 / PAGE;   /* room for at least 16 MiB (images compress; a bigger one fails with ENOSPC) */
}
/* The swap header page's tail: sig/orig/first extent; write: true replaces it, false checks it (ok: our image there). */
static bool swap_tail_io(const struct block_partition* p, uint64_t header_sector, struct swap_tail* tail, bool write) {
    if (!scratch_page() || block_part_read(p, header_sector, 8, scratch)) return false;
    if (write) { memcpy(scratch + PAGE - sizeof(*tail), tail, sizeof(*tail)); return !block_part_write(p, header_sector, 8, scratch) && !block_flush_all(); }
    memcpy(tail, scratch + PAGE - sizeof(*tail), sizeof(*tail));
    uint32_t crc = tail->crc; tail->crc = 0; bool ok = !memcmp(tail->sig, IMAGE_SIGNATURE, 10) && block_crc32(tail, sizeof(*tail)) == crc; tail->crc = crc;
    return ok;
}
static bool swap_mark(void) {   /* the image is complete: the signature points at it */
    struct swap_tail tail = {0}; memcpy(tail.orig, SWAP_SIGNATURE, 10); memcpy(tail.sig, IMAGE_SIGNATURE, 10);
    tail.first = area.extents[0].sector; tail.first_sectors = area.extents[0].count; tail.crc = 0; tail.crc = block_crc32(&tail, sizeof(tail));
    if (!scratch_page() || block_part_read(area.part, area.swap_header, 8, scratch) || memcmp(scratch + PAGE - 10, SWAP_SIGNATURE, 10)) return false;
    return swap_tail_io(area.part, area.swap_header, &tail, true);
}
static void swap_unmark(void) {   /* "SWAPSPACE2" again: swapon works, no image is found */
    if (!scratch_page() || block_part_read(area.part, area.swap_header, 8, scratch)) { say("[64] PM: could not read the swap header\n"); return; }
    if (memcmp(scratch + PAGE - 10, IMAGE_SIGNATURE, 10)) return;
    memcpy(scratch + PAGE - 10, SWAP_SIGNATURE, 10); memset(scratch + PAGE - 40, 0, 30);
    if (block_part_write(area.part, area.swap_header, 8, scratch) || block_flush_all()) say("[64] PM: could not restore the swap signature\n");
}
/* Boot: an image in a swap area - resume= (and resume_offset=), else every partition of swap type. Reads its header
 * into h and sets the area from the extent table. */
static bool swap_image_find(struct hib_header* h) {
    resume_config();
    for (unsigned i = 0; i <= block_partition_count(); i++) {
        const struct block_partition* p = resume_part ? resume_part : i < block_partition_count() ? block_partition_get(i) : unlocked_root();
        if (!p) { if (resume_part) break; continue; }
        bool typed = (p->scheme == BLOCK_SCHEME_MBR && p->mbr_type == 0x82) || (p->scheme == BLOCK_SCHEME_GPT && !memcmp(p->type_guid, LINUX_SWAP_GUID, 16));
        if (!resume_part && !typed && p != unlocked_root()) continue;   /* a dm device has no partition type: its header page decides */
        uint64_t header = resume_part ? resume_page * 8 : 0; struct swap_tail tail;
        if (swap_tail_io(p, header, &tail, false) && tail.first_sectors >= HEADER_BYTES / 512 && tail.first + tail.first_sectors <= block_part_sectors(p)) {
            area.part = p; area.kind = AREA_SWAP; area.swap_type = -1; area.swap_header = header; area.file = false;
            area.extents[0] = (struct extent){tail.first, tail.first_sectors}; area.count = 1; area.sectors = tail.first_sectors;
            if (!area_io(0, h, HEADER_BYTES, false) && !memcmp(h->magic, HIB_MAGIC, 8) && h->kind == AREA_SWAP && h->table_bytes &&
                h->table_bytes <= ((AREA_EXTENTS * sizeof(struct extent) + PAGE - 1) & ~(PAGE - 1)) && HEADER_BYTES + h->table_bytes <= tail.first_sectors * 512) {
                uint8_t* table = (uint8_t*)arena_alloc(h->table_bytes);
                if (table && !area_io(HEADER_BYTES, table, h->table_bytes, false) && block_crc32(table, h->table_bytes) == h->table_crc) {
                    const struct extent* e = (const struct extent*)table; unsigned count = 0; uint64_t sectors = 0; bool sane = true;
                    while (count < AREA_EXTENTS && count * sizeof(struct extent) < h->table_bytes && e[count].count) {
                        if (e[count].sector + e[count].count > block_part_sectors(p) || e[count].sector + e[count].count < e[count].sector) sane = false;
                        sectors += e[count].count; count++;
                    }
                    if (sane && count && e[0].sector == tail.first && e[0].count == tail.first_sectors) {
                        memcpy(area.extents, e, count * sizeof(struct extent)); area.count = count; area.sectors = sectors;
                        arena_free((uint64_t)table, h->table_bytes); return true;
                    }
                }
                if (table) arena_free((uint64_t)table, h->table_bytes);
            }
            say("[64] PM: the swap area's hibernation signature leads to no valid image; signature removed\n");
            area.count = 0; swap_unmark();
        }
        if (resume_part) break;
    }
    return false;
}
static bool find_area(void) {
    resume_config();
    if (resume_part && swap_area()) return true;   /* resume= names an active swap area */
    if (dedicated_area()) return true;
    return swap_area();
}
static uint64_t available_at;
static bool available;
bool hibernate_available(void) {   /* /sys/power/state lists "disk"; rechecked at most every 2 s once disks exist */
    uint64_t now = native_clock_ns();
    if (!block_device_count()) return false;
    if (!available_at || now - available_at > 2000000000ULL) { available = find_area(); available_at = now ? now : 1; }
    return available;
}

/* ---- identities ---- */
static void kernel_identity(uint8_t out[32]) { struct sha s; sha_init(&s); sha_update(&s, __text_start, (uint64_t)(__rodata_end - __text_start)); sha_final(&s, out); }
static void cpuid(uint32_t leaf, uint32_t r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0)); }
static void machine_identity(uint8_t out[32]) {
    struct sha s; sha_init(&s); uint32_t r[4], max; unsigned ranges = boot_info.memory_count < NEXUS_BOOT_MEMORY_MAX ? boot_info.memory_count : NEXUS_BOOT_MEMORY_MAX;
    sha_update(&s, &ranges, sizeof(ranges)); sha_update(&s, boot_info.memory, sizeof(boot_info.memory[0]) * ranges);
    sha_update(&s, &boot_info.framebuffer, sizeof(boot_info.framebuffer));
    cpuid(0, r); max = r[0]; sha_update(&s, r + 1, 12);
    cpuid(1, r); r[1] &= 0x0000FFFFU; r[2] &= ~(1U << 27); sha_update(&s, r, 16);   /* APIC id and OSXSAVE vary */
    if (max >= 7) { cpuid(7, r); sha_update(&s, r + 1, 12); }
    cpuid(0x80000001U, r); sha_update(&s, r + 2, 8);
    sha_update(&s, &cpu_count, sizeof(cpu_count));
    sha_final(&s, out);
}
/* The root volume's first 8 MiB (boot sector, FSInfo, the FAT, or the ext4 superblock and group tables) with the
 * area's sectors read as zeros, then where the area lies. */
static bool in_area(const struct block_partition* part, uint64_t sector) {
    if (part != area.part) return false;
    if (area.kind == AREA_SWAP && sector >= area.swap_header && sector < area.swap_header + 8) return true;   /* its tail changes */
    for (unsigned e = 0; e < area.count; e++) if (sector >= area.extents[e].sector && sector < area.extents[e].sector + area.extents[e].count) return true;
    return false;
}
static int disk_identity(uint8_t* buffer, uint64_t bytes, uint8_t out[32]) {
    const struct block_partition* root = block_root(); if (!root) root = area.part;
    struct sha s; sha_init(&s);
    uint64_t total = block_part_sectors(root), limit = FINGERPRINT_BYTES / 512; if (limit > total) limit = total;
    for (uint64_t at = 0; at < limit;) {
        uint64_t take = bytes / 512; if (take > limit - at) take = limit - at;
        if (block_part_read(root, at, (uint32_t)take, buffer)) return -5;
        for (uint64_t k = 0; k < take; k++) if (in_area(root, at + k)) memset(buffer + k * 512, 0, 512);
        sha_update(&s, buffer, take * 512); at += take;
    }
    sha_update(&s, &area.count, sizeof(area.count)); sha_update(&s, area.extents, sizeof(area.extents[0]) * area.count);
    sha_update(&s, area.part->label, sizeof(area.part->label));
    sha_final(&s, out); return 0;
}

/* ---- the writer ---- */
struct region { uint64_t address, bytes; };
static struct {
    uint8_t* bitmap; uint64_t bitmap_bytes, pfn_limit;
    uint8_t* stream; uint8_t* block; uint32_t* table; struct hib_header* header;
    struct region excluded[EXCLUDED_MAX]; unsigned excluded_count;
    uint8_t disk[32]; bool header_valid;
} w;
static uint64_t restore_point_pages;   /* this boot's page-pool use where hibernate_resume ran (feasibility) */
static void* writer_alloc(uint64_t bytes) {
    uint64_t address = arena_alloc(bytes);
    if (address && w.excluded_count < EXCLUDED_MAX) w.excluded[w.excluded_count++] = (struct region){virt_to_phys((void*)address), (bytes + PAGE - 1) & ~(PAGE - 1)};
    return (void*)address;
}
void hibernate_release(void) {
    for (unsigned i = 0; i < w.excluded_count; i++) arena_free((uint64_t)phys_to_virt(w.excluded[i].address), w.excluded[i].bytes);
    w.excluded_count = 0; w.bitmap = w.stream = w.block = NULL; w.table = NULL; w.header = NULL;
    if (area.kind == AREA_SWAP && area.swap_type >= 0) swap_hibernate_release((unsigned)area.swap_type);   /* the image's slots */
}
static uint64_t pool_limit(void) {   /* PFNs the bitmap covers */
    uint64_t top = LOW_LIMIT, begin, end; const uint32_t* references;
    for (unsigned i = 0, n = 1; i < n; i++) { n = memory_zone_view(i, &begin, &end, &references); if (n && end > top) top = end; }
    return top / PAGE;
}
static bool excluded(uint64_t physical) {
    for (unsigned i = 0; i < w.excluded_count; i++) if (physical >= w.excluded[i].address && physical < w.excluded[i].address + w.excluded[i].bytes) return true;
    return false;
}
/* A physical page as the kernel sees it: its direct-map address, else its kernel-image address, else NULL (not mapped:
 * the boot stack and kernel stack guard pages, whose contents do not matter). The upper half is shared by every space. */
static const void* mapped(uint64_t va) {
    const uint64_t* table = boot_pml4;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t entry = table[(va >> shift) & 511];
        if (!(entry & 1)) return NULL;
        if (shift == 12 || (shift < 39 && (entry & 0x80))) return (const void*)va;
        table = phys_to_virt(entry & 0x000FFFFFFFFFF000ULL);
    }
    return NULL;
}
static const void* page_view(uint64_t physical) {
    const void* view = mapped(DIRECT_MAP_BASE + physical);
    if (!view && physical < (1ULL << 30)) view = mapped(KERNEL_VMA + physical);
    return view;
}
/* The page set: usable RAM below 16 MiB, the kernel image (the UEFI loader reports it as reserved) and the AP/wake
 * trampoline page, and the pool pages in use; minus this writer's buffers. Returns the count. */
static uint64_t mark_low(uint64_t begin, uint64_t end) {
    uint64_t pages = 0;
    if (begin < PAGE) begin = PAGE;
    if (end > LOW_LIMIT) end = LOW_LIMIT;
    begin = (begin + PAGE - 1) & ~(PAGE - 1);
    for (uint64_t p = begin; p + PAGE <= end; p += PAGE)
        if (!excluded(p) && !(w.bitmap[p / PAGE / 8] & (1U << (p / PAGE % 8)))) { w.bitmap[p / PAGE / 8] |= (uint8_t)(1U << (p / PAGE % 8)); pages++; }
    return pages;
}
static uint64_t build_bitmap(void) {
    memset(w.bitmap, 0, w.bitmap_bytes); uint64_t pages = 0;
    for (unsigned i = 0; i < boot_info.memory_count && i < NEXUS_BOOT_MEMORY_MAX; i++)
        pages += mark_low(boot_info.memory[i].base, boot_info.memory[i].base + boot_info.memory[i].size);
    pages += mark_low((uint64_t)__text_start - KERNEL_VMA, (uint64_t)__bss_end - KERNEL_VMA + PAGE - 1);
    pages += mark_low(0x7000, 0x8000);
    uint64_t begin, end; const uint32_t* references;
    for (unsigned z = 0, n = 1; z < n; z++) {
        n = memory_zone_view(z, &begin, &end, &references); if (!n) break;
        for (uint64_t i = 0; begin + i * PAGE < end; i++) {
            uint64_t p = begin + i * PAGE;
            if (!references[i] || excluded(p) || p / PAGE >= w.pfn_limit) continue;
            w.bitmap[p / PAGE / 8] |= (uint8_t)(1U << (p / PAGE % 8)); pages++;
        }
    }
    return pages;
}
/* Before the snapshot (tasks frozen, interrupts on): the area, buffers, the disk identity, feasibility. */
int hibernate_prepare(void) {
    hibernate_release(); w.header_valid = false;
    if (!find_area()) return -19;
    {   /* stor2: the image holds every open dm-crypt key, so it goes only into the encrypted root's mapping (an area on
         * block_unlocked_root's device: encrypted at rest, and restorable since kernel.c unlocks it before
         * hibernate_resume_unlocked); other dm devices do not exist yet when the image would be read */
        bool dm_crypt_in_use(void); const char* dm_device_name(const struct block_device* dev);
        bool encrypted_root = block_unlocked_root && area.part->dev == block_unlocked_root->dev;
        if (dm_device_name(area.part->dev) && !encrypted_root) { say("[64] PM: hibernation refused: the hibernation area is on a device-mapper volume other than the encrypted root\n"); return -1; }
        if (dm_crypt_in_use() && !encrypted_root) { say("[64] PM: hibernation refused: an encrypted volume is open and the hibernation area is not encrypted\n"); return -1; }
    }
    w.pfn_limit = pool_limit(); w.bitmap_bytes = ((w.pfn_limit + 7) / 8 + PAGE - 1) & ~(PAGE - 1);
    w.header = writer_alloc(HEADER_BYTES); w.bitmap = writer_alloc(w.bitmap_bytes); w.stream = writer_alloc(STREAM_BYTES);
    w.block = writer_alloc(BLOCK_PAGES * PAGE); w.table = writer_alloc((1U << LZ_HASH_BITS) * 4);
    if (!w.header || !w.bitmap || !w.stream || !w.block || !w.table || w.excluded_count != 5) { hibernate_release(); return -12; }
    uint64_t pages = build_bitmap(), managed = memory_total_pages(), boot = restore_point_pages ? restore_point_pages : 16384;
    uint64_t room = managed + LOW_LIMIT / PAGE, need = pages + boot + boot / 4 + 8192, target = pm_image_size() / PAGE;
    if (need > room || pages > target) {   /* free memory first (page cache, swap): what /sys/power/image_size asks, and */
        uint64_t excess = need > room ? need - room + 4096 : 0, before = memory_free_pages();   /* what the restore needs */
        if (pages > target && pages - target > excess) excess = pages - target;
        say("[64] PM: freeing "); say_number(excess); say(" pages for the image\n");
        uint64_t after = reclaim_hibernate(before + excess);
        pages = build_bitmap();
        say("[64] PM: "); say_number(after > before ? after - before : 0); say(" pages freed; "); say_number(pages); say(" to save\n");
    }
    if (area.kind == AREA_SWAP) {
        /* slots for the worst case (nothing compresses): the pages, block headers, bitmap, header and extent table;
         * the largest run goes first (it holds the header and the table the next boot reads before the others) */
        uint64_t want = pages + pages / 512 + w.bitmap_bytes / PAGE + 1 + (AREA_EXTENTS * sizeof(struct extent) + PAGE - 1) / PAGE + 8;
        area.count = 0; area.sectors = 0;
        uint64_t got = swap_hibernate_reserve((unsigned)area.swap_type, want, add_run, NULL);
        if (!got) { say("[64] PM: the swap area has no room for the hibernation image (or is too fragmented)\n"); hibernate_release(); return -28; }
        unsigned largest = 0; for (unsigned i = 1; i < area.count; i++) if (area.extents[i].count > area.extents[largest].count) largest = i;
        struct extent first = area.extents[largest]; area.extents[largest] = area.extents[0]; area.extents[0] = first;
        uint64_t table = (area.count * sizeof(struct extent) + PAGE - 1) & ~(PAGE - 1);
        if (area.extents[0].count * 512 < HEADER_BYTES + table) { say("[64] PM: the swap area's free space is too fragmented for the image\n"); hibernate_release(); return -28; }
        if (got < want) { say("[64] PM: swap has room for "); say_number(got); say(" of "); say_number(want); say(" pages; the compressed image must fit\n"); }
    }
    if (disk_identity(w.stream, STREAM_BYTES, w.disk)) { hibernate_release(); return -5; }
    /* Restoring needs room for the image's pages, the restoring kernel's own and the spare frames it copies through. */
    if (pages + boot + boot / 4 + 8192 > managed + LOW_LIMIT / PAGE) {
        say("[64] PM: hibernation needs "); say_number(pages); say(" pages saved + "); say_number(boot); say(" for the restoring kernel; only ");
        say_number(managed); say(" managed: close applications first\n");
        hibernate_release(); return -12;
    }
    return 0;
}
static int stream_flush(uint64_t* offset, uint32_t* used, bool all) {
    uint32_t bytes = all ? (*used + 511) & ~511U : *used & ~511U;
    if (!bytes) return 0;
    if (all) memset(w.stream + *used, 0, bytes - *used);
    int error = area_io(*offset, w.stream, bytes, true); if (error) return error;
    *offset += bytes;
    if (!all) acpi_os_memmove(w.stream, w.stream + bytes, *used - bytes);
    *used = all ? 0 : *used - bytes;
    return 0;
}
/* The written image read back: the bitmap's CRC, then every block's magic, sequence, size and CRC, the page count. */
static int verify_image(const struct hib_header* h) {
    if (area_io(h->bitmap_offset, w.stream, w.bitmap_bytes < STREAM_BYTES ? w.bitmap_bytes : STREAM_BYTES, false)) return -5;
    if (w.bitmap_bytes <= STREAM_BYTES && block_crc32(w.stream, w.bitmap_bytes) != h->bitmap_crc) return -5;
    uint64_t offset = h->data_offset, end = h->data_offset + h->data_bytes, pages = 0, blocks = 0; uint32_t have = 0, at = 0;
    while (pages < h->pages) {
        if (have - at < sizeof(struct hib_block) + BLOCK_PAGES * PAGE && offset < end) {
            acpi_os_memmove(w.stream, w.stream + at, have - at); have -= at; at = 0;
            uint64_t take = STREAM_BYTES - have; if (take > end - offset) take = end - offset; take &= ~511ULL;
            if (take && area_io(offset, w.stream + have, take, false)) return -5;
            offset += take; have += (uint32_t)take;
        }
        if (have - at < sizeof(struct hib_block)) return -5;
        struct hib_block* b = (struct hib_block*)(w.stream + at); uint32_t bytes = b->bytes & ~BLOCK_STORED, check = b->crc;
        if (b->magic != BLOCK_MAGIC || b->sequence != blocks || !b->pages || b->pages > BLOCK_PAGES || bytes > BLOCK_PAGES * PAGE || have - at - sizeof(*b) < bytes) return -5;
        b->crc = 0; uint32_t crc = block_crc32(b, sizeof(*b) + bytes); b->crc = check;
        if (crc != check) { say("[64] PM: hibernation image block "); say_number(blocks); say(" read back with a different checksum\n"); return -5; }
        pages += b->pages; blocks++; at += (uint32_t)sizeof(*b) + bytes;
    }
    return pages == h->pages && blocks == h->blocks ? 0 : -5;
}
/* After the snapshot: the CPU context is saved, other CPUs are offline, non-storage drivers suspended. */
int hibernate_write(void) {
    /* Lockdown (lockdown.c): the image is CRC-checked, not authenticated, so under integrity lockdown a saved image
     * could be tampered with and restored into the running kernel. Refused (Linux LOCKDOWN_HIBERNATION). */
    if (kernel_lockdown_denied("hibernation", false)) return -1;
    uint64_t start = native_clock_ns();
    struct hib_header* h = w.header; memset(h, 0, HEADER_BYTES);
    memcpy(h->magic, HIB_MAGIC, 8); h->version = HIB_VERSION; h->pfn_limit = w.pfn_limit; h->cpus = cpu_count;
    kernel_identity(h->kernel); machine_identity(h->machine); memcpy(h->disk, w.disk, 32);
    h->facs = acpi_facs_hardware_signature(); h->created = native_realtime_ns() / 1000000000ULL; h->restore_pages = restore_point_pages;
    h->pages = build_bitmap();
    h->kind = area.kind; h->swap_header = area.kind == AREA_SWAP ? area.swap_header : 0;
    h->table_bytes = area.kind == AREA_SWAP ? (uint32_t)((area.count * sizeof(struct extent) + PAGE - 1) & ~(PAGE - 1)) : 0;
    h->bitmap_offset = HEADER_BYTES + h->table_bytes; h->bitmap_bytes = w.bitmap_bytes; h->bitmap_crc = block_crc32(w.bitmap, w.bitmap_bytes);
    h->data_offset = h->bitmap_offset + w.bitmap_bytes;
    if (h->data_offset / 512 >= area.sectors) return -28;
    int error = area_io(0, h, HEADER_BYTES, true);   /* state 0: invalid until the end */
    if (!error && h->table_bytes) {   /* swap: where every slot of the image lies (the stream buffer stages it) */
        memset(w.stream, 0, h->table_bytes); memcpy(w.stream, area.extents, area.count * sizeof(struct extent));
        h->table_crc = block_crc32(w.stream, h->table_bytes); error = area_io(HEADER_BYTES, w.stream, h->table_bytes, true);
    }
    if (error || (error = area_io(h->bitmap_offset, w.bitmap, w.bitmap_bytes, true))) return error;
    uint64_t offset = h->data_offset, raw = 0, done = 0, report = h->pages / 10, pfn = 0; uint32_t used = 0, sequence = 0;
    while (done < h->pages) {
        uint32_t count = 0, stored = 0; uint16_t zero = 0;
        while (count < BLOCK_PAGES && done + count < h->pages) {
            while (pfn < w.pfn_limit && !(w.bitmap[pfn / 8] & (1U << (pfn % 8)))) pfn++;
            if (pfn >= w.pfn_limit) break;
            const uint64_t* page = page_view(pfn++ * PAGE); bool empty = true;   /* unmapped (a stack guard page): saved as zeros */
            for (unsigned k = 0; page && k < PAGE / 8; k++) if (page[k]) { empty = false; break; }
            if (empty) zero |= (uint16_t)(1U << count); else memcpy(w.block + (uint64_t)stored++ * PAGE, page, PAGE);
            count++;
        }
        if (!count) return -5;
        if (used + sizeof(struct hib_block) + BLOCK_PAGES * PAGE + 64 > STREAM_BYTES && (error = stream_flush(&offset, &used, false))) return error;
        struct hib_block* b = (struct hib_block*)(w.stream + used);
        uint8_t* payload = (uint8_t*)(b + 1); uint32_t raw_bytes = stored * (uint32_t)PAGE, bytes = 0;
        if (raw_bytes) bytes = lz_compress(w.block, raw_bytes, payload, raw_bytes - 1, w.table);
        if (raw_bytes && !bytes) { memcpy(payload, w.block, raw_bytes); bytes = raw_bytes | BLOCK_STORED; }
        *b = (struct hib_block){BLOCK_MAGIC, sequence++, (uint16_t)count, zero, bytes, 0, 0};
        b->crc = block_crc32(b, sizeof(*b) + (bytes & ~BLOCK_STORED));
        used += (uint32_t)sizeof(*b) + (bytes & ~BLOCK_STORED); raw += raw_bytes; done += count; h->stored_pages += stored; h->blocks++;
        if (offset + used > area.sectors * 512) return -28;
        if (h->pages >= 10 && done >= report) { say("[64] PM: hibernation image "); say_number(done * 100 / h->pages); say("%\n"); report += h->pages / 10; }
    }
    if ((error = stream_flush(&offset, &used, true))) return error;
    h->data_bytes = offset - h->data_offset; h->raw_bytes = raw;
    if (block_flush_all()) return -5;
    /* Read the whole image back before it may be restored: header-less blocks that do not check out (a device that
     * lost writes, a bad sector) fail the hibernation here, while the system can still run on, not at the next boot. */
    if ((error = verify_image(h))) { say("[64] PM: hibernation image verification failed: the image is not used\n"); return error; }
    h->state = HIB_VALID; h->crc = 0; h->crc = block_crc32(h, sizeof(*h));
    if ((error = area_io(0, h, HEADER_BYTES, true))) return error;
    if (block_flush_all()) return -5;
    w.header_valid = true;
    if (area.kind == AREA_SWAP && !swap_mark()) { say("[64] PM: the swap header could not be marked\n"); hibernate_invalidate(); return -5; }   /* last */
    say("[64] PM: hibernation image written: "); say_number(h->pages); say(" pages ("); say_number(h->stored_pages); say(" non-zero, ");
    say_number(raw >> 20); say(" MiB) -> "); say_number(h->data_bytes >> 20); say(" MiB in "); say_number((native_clock_ns() - start) / 1000000); say(" ms\n");
    return 0;
}
/* Once the system ran on past the image (failed power-off, or a completed restore) it must never be restored. */
void hibernate_invalidate(void) {
    if (!w.header_valid || !w.header) return;
    if (area.kind == AREA_SWAP) swap_unmark();   /* first: no boot finds the image any more */
    memset(w.header, 0, HEADER_BYTES);
    if (area_io(0, w.header, HEADER_BYTES, true) || block_flush_all()) say("[64] PM: could not invalidate the hibernation image\n");
    w.header_valid = false;
}

/* ---- restore ---- */
struct copy_page { uint64_t next, count, pairs[255][2]; };   /* physical: next list page, (destination, source) */
_Static_assert(sizeof(struct copy_page) == PAGE, "copy list page");
static struct {
    uint8_t* bitmap; uint8_t* claimed; uint64_t bitmap_bytes, pfn_limit;
    uint8_t* stream; uint8_t* block;
    struct copy_page* first; struct copy_page* list; uint64_t staged, placed;
} r;
static void restore_release(void) {   /* a failed load: everything this boot claimed or allocated goes back */
    if (r.claimed) for (uint64_t pfn = 0; pfn < r.pfn_limit; pfn++) if (r.claimed[pfn / 8] & (1U << (pfn % 8))) page_free(pfn * PAGE);
    for (struct copy_page* c = r.first; c;) {
        for (uint64_t i = 0; i < c->count; i++) page_free(c->pairs[i][1]);
        struct copy_page* next = c->next ? phys_to_virt(c->next) : NULL; page_free((uint64_t)c); c = next;
    }
    if (r.bitmap) arena_free((uint64_t)r.bitmap, r.bitmap_bytes);
    if (r.claimed) arena_free((uint64_t)r.claimed, r.bitmap_bytes);
    if (r.stream) arena_free((uint64_t)r.stream, STREAM_BYTES);
    if (r.block) arena_free((uint64_t)r.block, BLOCK_PAGES * PAGE);
    memset(&r, 0, sizeof(r));
}
static void discard(struct hib_header* h, const char* why) {
    say("[64] PM: hibernation image discarded: "); say(why); say("\n");
    if (area.kind == AREA_SWAP) swap_unmark();   /* the swap area is usable again (swapon finds SWAPSPACE2) */
    memset(h, 0, HEADER_BYTES);
    if (area_io(0, h, HEADER_BYTES, true) || block_flush_all()) say("[64] PM: could not invalidate the hibernation image\n");
}
/* Where image page `pfn` goes now: its own frame (claimed) or a spare frame recorded for the final copy. */
static uint8_t* target(uint64_t pfn) {
    if (r.claimed[pfn / 8] & (1U << (pfn % 8))) { r.placed++; return phys_to_virt(pfn * PAGE); }
    if (!r.list || r.list->count == 255) {
        struct copy_page* c = (struct copy_page*)page_alloc(); if (!c) return NULL;
        if (r.list) r.list->next = virt_to_phys(c); else r.first = c;
        r.list = c;
    }
    uint64_t spare = frame_alloc(); if (!spare) return NULL;
    r.list->pairs[r.list->count][0] = pfn * PAGE; r.list->pairs[r.list->count][1] = spare; r.list->count++; r.staged++;
    return phys_to_virt(spare);
}
static bool load(const struct hib_header* h) {
    uint64_t offset = h->data_offset, end = h->data_offset + h->data_bytes, pfn = 0, done = 0; uint32_t have = 0, at = 0, sequence = 0;
    while (done < h->pages) {
        if (have - at < sizeof(struct hib_block) + BLOCK_PAGES * PAGE && offset < end) {   /* refill */
            acpi_os_memmove(r.stream, r.stream + at, have - at); have -= at; at = 0;
            uint64_t take = STREAM_BYTES - have; if (take > end - offset) take = end - offset; take &= ~511ULL;
            if (take && area_io(offset, r.stream + have, take, false)) { say("[64] PM: hibernation image read error\n"); return false; }
            offset += take; have += (uint32_t)take;
        }
        if (have - at < sizeof(struct hib_block)) return false;
        struct hib_block* b = (struct hib_block*)(r.stream + at);
        uint32_t bytes = b->bytes & ~BLOCK_STORED, check = b->crc;
        if (b->magic != BLOCK_MAGIC || b->sequence != sequence || !b->pages || b->pages > BLOCK_PAGES || bytes > BLOCK_PAGES * PAGE ||
            have - at - sizeof(*b) < bytes) { say("[64] PM: hibernation image block "); say_number(sequence); say(" malformed\n"); return false; }
        b->crc = 0;
        if (block_crc32(b, sizeof(*b) + bytes) != check) { say("[64] PM: hibernation image block "); say_number(sequence); say(" checksum mismatch\n"); return false; }
        unsigned nonzero = 0; for (unsigned k = 0; k < b->pages; k++) if (!(b->zero_mask & (1U << k))) nonzero++;
        const uint8_t* payload = (const uint8_t*)(b + 1);
        if (b->bytes & BLOCK_STORED) { if (bytes != nonzero * PAGE) return false; memcpy(r.block, payload, bytes); }
        else if ((nonzero && !lz_decompress(payload, bytes, r.block, nonzero * (uint32_t)PAGE)) || (!nonzero && bytes)) {
            say("[64] PM: hibernation image block "); say_number(sequence); say(" does not decompress\n"); return false;
        }
        for (unsigned k = 0, n = 0; k < b->pages; k++) {
            while (pfn < r.pfn_limit && !(r.bitmap[pfn / 8] & (1U << (pfn % 8)))) pfn++;
            if (pfn >= r.pfn_limit) return false;
            uint8_t* to = target(pfn++); if (!to) { say("[64] PM: no memory left to restore the hibernation image\n"); return false; }
            if (b->zero_mask & (1U << k)) memset(to, 0, PAGE); else memcpy(to, r.block + (uint64_t)n++ * PAGE, PAGE);
        }
        done += b->pages; at += (uint32_t)sizeof(*b) + bytes; sequence++;
    }
    return done == h->pages && sequence == h->blocks;
}
/* Temporary tables for the final copy: the kernel image at KERNEL_VMA and the direct map, 2 MiB pages. */
static uint64_t temporary_tables(void) {
    uint64_t pml4 = frame_alloc(), top = r.pfn_limit * PAGE; if (top < (4ULL << 30)) top = 4ULL << 30;
    uint64_t pdpt_k = frame_alloc(), pd_k = frame_alloc();
    if (!pml4 || !pdpt_k || !pd_k) return 0;
    uint64_t* root = phys_to_virt(pml4);
    root[511] = pdpt_k | 3; ((uint64_t*)phys_to_virt(pdpt_k))[510] = pd_k | 3;
    for (unsigned i = 0; i < 512; i++) ((uint64_t*)phys_to_virt(pd_k))[i] = (uint64_t)i << 21 | 0x83;
    for (uint64_t p = 0; p < top; p += 1ULL << 21) {
        uint64_t va = DIRECT_MAP_BASE + p; unsigned i4 = (unsigned)(va >> 39) & 511, i3 = (unsigned)(va >> 30) & 511, i2 = (unsigned)(va >> 21) & 511;
        if (!(root[i4] & 1)) { uint64_t t = frame_alloc(); if (!t) return 0; root[i4] = t | 3; }
        uint64_t* pdpt = phys_to_virt(root[i4] & 0x000FFFFFFFFFF000ULL);
        if (!(pdpt[i3] & 1)) { uint64_t t = frame_alloc(); if (!t) return 0; pdpt[i3] = t | 3; }
        ((uint64_t*)phys_to_virt(pdpt[i3] & 0x000FFFFFFFFFF000ULL))[i2] = p | 0x83 | 1ULL << 63;
    }
    return pml4;
}
/* stor2: kernel.c, after cryptroot.c unlocked an encrypted root (the Linux initramfs order: unlock, then resume):
 * its mapping now exists, so an area inside it (/nexus/hiber.img on that root) is found and restored like above. */
void hibernate_resume(void);
void hibernate_resume_unlocked(void) { say("[64] PM: looking for a hibernation image on the unlocked root\n"); hibernate_resume(); }
/* kernel.c, after block_probe: restore a valid image (never returns) or discard a stale one and go on. */
void hibernate_resume(void) {
    static unsigned calls; if (calls++) resume_known = false;   /* again after an unlock: /dev/mapper/NAME (or a UUID in it) exists now */
    restore_point_pages = memory_used_pages();
    struct hib_header* h = (struct hib_header*)arena_alloc(HEADER_BYTES);
    if (!h) return;
    /* a dedicated area (its header), else an image in a swap area (resume=, or partitions of swap type) */
    bool found = dedicated_area() && !area_io(0, h, HEADER_BYTES, false) && !memcmp(h->magic, HIB_MAGIC, 8);
    if (!found) found = swap_image_find(h);
    available_at = 0;   /* /sys/power/state looks again (swap areas come up later) */
    if (!found) { arena_free((uint64_t)h, HEADER_BYTES); return; }
    say("[64] PM: hibernation image found on "); say(area.file ? "/nexus/hiber.img" : area.part->label);
    if (area.kind == AREA_SWAP) { say(" (swap, "); say_number(area.count); say(" extent(s))"); }
    say("\n");
    if (boot_option("noresume")) { discard(h, "noresume"); arena_free((uint64_t)h, HEADER_BYTES); return; }
    if (h->state != HIB_VALID) { say("[64] PM: no valid hibernation image\n"); arena_free((uint64_t)h, HEADER_BYTES); return; }
    uint8_t value[32]; uint32_t crc = h->crc; h->crc = 0; const char* why = NULL;
    if (block_crc32(h, sizeof(*h)) != crc || h->version != HIB_VERSION) why = "header checksum or version";
    else if (kernel_identity(value), memcmp(value, h->kernel, 32)) why = "written by a different kernel";
    else if (machine_identity(value), memcmp(value, h->machine, 32)) why = "memory map, display or processors changed";
    else if (h->facs && acpi_facs_hardware_signature() && h->facs != acpi_facs_hardware_signature()) why = "the firmware reports changed hardware (FACS)";
    else if (h->kind != area.kind || (area.kind == AREA_SWAP) != (h->table_bytes != 0) || (area.kind == AREA_SWAP && h->swap_header != area.swap_header)) why = "the image belongs to another area";
    else if (h->pfn_limit != pool_limit() || !h->pages || h->bitmap_offset != HEADER_BYTES + h->table_bytes || h->data_offset != h->bitmap_offset + h->bitmap_bytes ||
             h->bitmap_bytes != ((h->pfn_limit + 7) / 8 + PAGE - 1) / PAGE * PAGE || h->data_offset / 512 > area.sectors ||
             h->data_bytes / 512 > area.sectors - h->data_offset / 512) why = "inconsistent layout";   /* no wrap-around */
    if (!why) {
        uint8_t* buffer = (uint8_t*)arena_alloc(1U << 20);
        if (!buffer) why = "no memory";
        else { if (disk_identity(buffer, 1U << 20, value) || memcmp(value, h->disk, 32)) why = "the disk changed after it was written"; arena_free((uint64_t)buffer, 1U << 20); }
    }
    if (why) { discard(h, why); arena_free((uint64_t)h, HEADER_BYTES); return; }
    uint64_t start = native_clock_ns();
    r.pfn_limit = h->pfn_limit; r.bitmap_bytes = h->bitmap_bytes;
    r.bitmap = (uint8_t*)arena_alloc(r.bitmap_bytes); r.claimed = (uint8_t*)arena_alloc(r.bitmap_bytes);
    if (!r.bitmap || !r.claimed || area_io(h->bitmap_offset, r.bitmap, r.bitmap_bytes, false) || block_crc32(r.bitmap, r.bitmap_bytes) != h->bitmap_crc) {
        restore_release(); discard(h, "bitmap unreadable or corrupt"); arena_free((uint64_t)h, HEADER_BYTES); return;
    }
    /* Claim every image frame this kernel has free first: the spare frames allocated afterwards are free in both. */
    uint64_t counted = 0;
    for (uint64_t pfn = 0; pfn < r.pfn_limit; pfn++) if (r.bitmap[pfn / 8] & (1U << (pfn % 8))) {
        counted++;
        if (pfn * PAGE >= LOW_LIMIT && memory_page_claim(pfn * PAGE)) r.claimed[pfn / 8] |= (uint8_t)(1U << (pfn % 8));
    }
    r.stream = (uint8_t*)arena_alloc(STREAM_BYTES); r.block = (uint8_t*)arena_alloc(BLOCK_PAGES * PAGE);
    if (counted != h->pages || !r.stream || !r.block || !load(h)) {
        restore_release(); discard(h, counted != h->pages ? "page count mismatch" : "unreadable, corrupt or too large for this memory");
        arena_free((uint64_t)h, HEADER_BYTES); return;
    }
    uint64_t tables = temporary_tables();
    if (!tables) { restore_release(); discard(h, "no memory for the restore tables"); arena_free((uint64_t)h, HEADER_BYTES); return; }
    say("[64] PM: hibernation image loaded: "); say_number(h->pages); say(" pages ("); say_number(r.placed); say(" in place, "); say_number(r.staged);
    say(" through spare frames) in "); say_number((native_clock_ns() - start) / 1000000); say(" ms; restoring\n");
    discard(h, "restoring (an image is used once)");
    pm_quiesce_all();
    pm_stop_other_cpus();
    serial_drain();
    __asm__ volatile("cli" ::: "memory");
    hibernate_restore_jump(r.first ? virt_to_phys(r.first) : 0, tables, virt_to_phys(boot_pml4), (uint64_t)pm_wake_long);
}
