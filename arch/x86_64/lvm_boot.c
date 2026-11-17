/* LVM2 volume groups at boot -- what an initramfs does with "vgchange -ay" before mounting a root on LVM, and what
 * lvm2's autoactivation does for the other VGs before /etc/fstab (kernel.c: lvm_boot() right after cryptroot_boot(),
 * so LVM on LUKS works; lvm_boot_late() after /etc/crypttab).
 *
 * Physical volumes: the "LABELONE" label in sectors 0-3 (CRC, type "LVM2 001") of any partition, whole disk or
 * device-mapper device (an unlocked LUKS volume), its pv_header (uuid, data and metadata areas), the metadata area
 * header (CRC, magic " LVM2 x[5A%r0N*>", raw location; the text may wrap round the circular area) and the text
 * (CRC). The VG with the highest seqno among its PVs' copies wins. LVs: "striped" segments (linear when one stripe)
 * become linear/striped tables over the PVs' extents (pe_start + extent * extent_size); a "snapshot" LV pairs an origin
 * and a COW LV: <vg>-<origin>-real, <vg>-<cow>-cow, <vg>-<origin> (snapshot-origin) and <vg>-<cow> (snapshot, the
 * persistent store) as lvm2 builds them, with lvm2's dm names (<vg>-<lv>, '-' doubled) and uuids (LVM-<vg id><lv id>
 * [-real|-cow]), so lvm2 later recognises them as its own; /dev/<vg>/<lv> comes from blkdev_node.c. LVs on a missing
 * PV, other segment types (thin, raid, cache, mirror), hidden LVs other than snapshot segments (a thin pool's _tmeta and
 * _tdata, pmspare: lvm2 activates those itself) and LVs that are not active by default are left out.
 * Which VGs at boot: rd.lvm.vg=<vg>, rd.lvm.lv=<vg>/<lv> (dracut), the VG of root=/dev/mapper/<vg>-<lv> or
 * root=/dev/<vg>/<lv>, every VG when root= names a UUID/LABEL; lvm_boot_late() activates the rest. rd.lvm=0 or
 * lvm=0: none. The root LV becomes the root candidate (block_unlocked_root) like an unlocked LUKS root.
 * Hostile metadata (bad CRCs, sizes, nesting, extents outside a PV, too many stripes, non-contiguous segments) is
 * refused per VG or per LV with a "[64] LVM ..." message; nothing is trusted that was not checked. */
#include "native.h"
#include "boot_info.h"
#include "blockdev.h"
#include "cryptroot.h"
#include "storage_util.h"
#include "../../kernel/vga.h"
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */

int dm_kernel_create(const char* name, const char* uuid, const char* const* lines, unsigned count, bool readonly, struct block_device** out);
bool dm_name_exists(const char* name);
int block_spec_matches(struct block_partition* p, const char* spec);
bool dm_sysfs_attr(struct block_device* dev, unsigned index, char* text);   /* dm.c: 0 name */
#define LVM_INITIAL_CRC 0xf597a6cfu
#define MDA_HEADER 512u
#define TEXT_MAX (4u << 20)
#define MAX_PVS 32
#define MAX_LVS 64
#define MAX_SEGS 16
#define STRIPES 16
#define NODES_MAX 8192
#define DEPTH_MAX 24

static void say(const char* a, const char* b, const char* c) { vga_print("[64] LVM "); vga_print(a); if (b) vga_print(b); if (c) vga_print(c); vga_print("\n"); }
static uint32_t lvm_crc(uint32_t crc, const uint8_t* data, uint64_t size) {   /* lvm2 calc_crc: CRC-32 (reflected), no inversion */
    static const uint32_t table[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
                                       0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    for (uint64_t i = 0; i < size; i++) { crc ^= data[i]; crc = (crc >> 4) ^ table[crc & 0xf]; crc = (crc >> 4) ^ table[crc & 0xf]; }
    return crc;
}

/* ---- the metadata text: a small tree of sections and key = value ---- */
struct cfg_node { uint32_t key, klen, value, vlen; int32_t child, next; uint8_t section; };
struct cfg { const char* text; uint32_t length; struct cfg_node* nodes; unsigned count; };
static bool name_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '+' || c == '-'; }
static uint32_t skip(const struct cfg* c, uint32_t at) {
    for (;;) {
        while (at < c->length && (c->text[at] == ' ' || c->text[at] == '\t' || c->text[at] == '\n' || c->text[at] == '\r')) at++;
        if (at < c->length && c->text[at] == '#') { while (at < c->length && c->text[at] != '\n') at++; continue; }
        return at;
    }
}
static bool string_end(const struct cfg* c, uint32_t* at) {   /* past a "..." with \" and \\ escapes */
    uint32_t p = *at + 1;
    for (; p < c->length; p++) { if (c->text[p] == '\\') { p++; continue; } if (c->text[p] == '"') { *at = p + 1; return true; } }
    return false;
}
static bool value_end(const struct cfg* c, uint32_t* at) {   /* a number, a string or a [list] */
    uint32_t p = *at;
    if (p >= c->length) return false;
    if (c->text[p] == '"') return string_end(c, at);
    if (c->text[p] == '[') {
        p++;
        for (unsigned items = 0; ; items++) {
            p = skip(c, p); if (p >= c->length || items > 100000) return false;
            if (c->text[p] == ']') { *at = p + 1; return true; }
            if (c->text[p] == '"') { if (!string_end(c, &p)) return false; }
            else { uint32_t q = p; while (q < c->length && (name_char(c->text[q]))) q++; if (q == p) return false; p = q; }
            p = skip(c, p); if (p < c->length && c->text[p] == ',') p++;
        }
    }
    uint32_t q = p; while (q < c->length && name_char(c->text[q])) q++;
    if (q == p) return false;
    *at = q; return true;
}
static int parse_body(struct cfg* c, uint32_t* at, int32_t* first, unsigned depth, bool top) {
    int32_t last = -1;
    if (depth > DEPTH_MAX) return -22;
    for (;;) {
        uint32_t p = skip(c, *at);
        if (p >= c->length) { *at = p; return top ? 0 : -22; }
        if (c->text[p] == '}') { *at = p + 1; return top ? -22 : 0; }
        uint32_t key = p; while (p < c->length && name_char(c->text[p])) p++;
        if (p == key || c->count >= NODES_MAX) return -22;
        struct cfg_node* n = &c->nodes[c->count]; int32_t self = (int32_t)c->count++;
        n->key = key; n->klen = p - key; n->child = -1; n->next = -1; n->section = 0;
        p = skip(c, p);
        if (p < c->length && c->text[p] == '{') {
            n->section = 1; *at = p + 1;
            int e = parse_body(c, at, &c->nodes[self].child, depth + 1, false); if (e) return e;
        } else if (p < c->length && c->text[p] == '=') {
            p = skip(c, p + 1); uint32_t v = p;
            if (!value_end(c, &p)) return -22;
            c->nodes[self].value = v; c->nodes[self].vlen = p - v; *at = p;
        } else return -22;
        if (last < 0) *first = self; else c->nodes[last].next = self;
        last = self;
    }
}
static int32_t child(const struct cfg* c, int32_t section, const char* name) {
    unsigned n = strlen(name);
    for (int32_t i = section < 0 ? 0 : c->nodes[section].child; i >= 0; i = c->nodes[i].next)
        if (c->nodes[i].klen == n && !memcmp(c->text + c->nodes[i].key, name, n)) return i;
    return -1;
}
static bool number(const struct cfg* c, int32_t node, uint64_t* out) {
    if (node < 0 || c->nodes[node].section) return false;
    const char* p = c->text + c->nodes[node].value; uint32_t n = c->nodes[node].vlen; uint64_t v = 0;
    if (!n || n > 20) return false;
    for (uint32_t i = 0; i < n; i++) { if (p[i] < '0' || p[i] > '9') return false; uint64_t d = (uint64_t)(p[i] - '0'); if (v > (~0ULL - d) / 10) return false; v = v * 10 + d; }
    *out = v; return true;
}
static bool string_value(const struct cfg* c, const char* p, uint32_t n, char* out, unsigned capacity) {
    if (n < 2 || p[0] != '"' || p[n - 1] != '"') return false;
    unsigned k = 0;
    for (uint32_t i = 1; i + 1 < n; i++) { char ch = p[i]; if (ch == '\\' && i + 2 < n) ch = p[++i]; if (k + 1 >= capacity) return false; out[k++] = ch; }
    (void)c; out[k] = 0; return true;
}
static bool text_of(const struct cfg* c, int32_t node, char* out, unsigned capacity) {
    return node >= 0 && !c->nodes[node].section && string_value(c, c->text + c->nodes[node].value, c->nodes[node].vlen, out, capacity);
}
/* item `index` of a [list]: a string (kind 's') or a number (kind 'n') */
static bool list_item(const struct cfg* c, int32_t node, unsigned index, char* text, unsigned capacity, uint64_t* value) {
    if (node < 0 || c->nodes[node].section) return false;
    struct cfg sub = {c->text + c->nodes[node].value, c->nodes[node].vlen, NULL, 0};
    if (!sub.length || sub.text[0] != '[') return false;
    uint32_t p = 1;
    for (unsigned i = 0; ; i++) {
        p = skip(&sub, p); if (p >= sub.length || sub.text[p] == ']') return false;
        uint32_t start = p;
        if (sub.text[p] == '"') { if (!string_end(&sub, &p)) return false; } else { while (p < sub.length && name_char(sub.text[p])) p++; }
        if (i == index) {
            if (sub.text[start] == '"') return text && string_value(&sub, sub.text + start, p - start, text, capacity);
            struct cfg_node fake = {0, 0, start, p - start, -1, -1, 0}; struct cfg one = {sub.text, sub.length, &fake, 1};
            return value && number(&one, 0, value);
        }
        p = skip(&sub, p); if (p < sub.length && sub.text[p] == ',') p++;
    }
}
static bool list_has(const struct cfg* c, int32_t node, const char* word) {
    char item[64];
    for (unsigned i = 0; i < 64 && list_item(c, node, i, item, sizeof(item), NULL); i++) if (!strcmp(item, word)) return true;
    return false;
}

/* ---- physical volumes ---- */
struct pv { struct block_partition* part; char uuid[33]; uint64_t mda_offset[2], mda_size[2]; unsigned mdas; };
struct vg_copy { char* text; uint32_t length; uint64_t seqno; char id[40]; };
static struct pv pvs[MAX_PVS]; static unsigned pv_count;
static bool already(const struct block_partition* p) { for (unsigned i = 0; i < pv_count; i++) if (pvs[i].part == p) return true; return false; }
static void probe_pv(struct block_partition* part, uint8_t* sector) {
    if (pv_count == MAX_PVS || already(part)) return;
    uint64_t sectors = block_part_sectors(part);
    for (unsigned s = 0; s < 4 && s < sectors; s++) {
        if (block_part_read(part, s, 1, sector) < 0) return;
        if (memcmp(sector, "LABELONE", 8) || get_le64(sector + 8) != s || memcmp(sector + 24, "LVM2 001", 8)) continue;
        if (lvm_crc(LVM_INITIAL_CRC, sector + 20, 512 - 20) != get_le32(sector + 16)) { say("bad PV label checksum on ", part->linux_name, NULL); return; }
        uint32_t offset = get_le32(sector + 20);
        if (offset < 32 || offset > 512 - 32 - 8 - 32) return;
        struct pv* v = &pvs[pv_count]; memset(v, 0, sizeof(*v)); v->part = part;
        memcpy(v->uuid, sector + offset, 32); v->uuid[32] = 0;
        uint32_t at = offset + 40;   /* data areas, then metadata areas, each a {offset, size} list ending in zeros */
        while (at + 16 <= 512 && (get_le64(sector + at) || get_le64(sector + at + 8))) at += 16;
        at += 16;
        while (at + 16 <= 512 && (get_le64(sector + at) || get_le64(sector + at + 8))) {
            if (v->mdas < 2) { v->mda_offset[v->mdas] = get_le64(sector + at); v->mda_size[v->mdas] = get_le64(sector + at + 8); v->mdas++; }
            at += 16;
        }
        pv_count++; return;
    }
}
static char* read_text(const struct pv* v, unsigned m, uint32_t* length) {   /* the current metadata text of one area */
    uint64_t start = v->mda_offset[m], size = v->mda_size[m], sectors = block_part_sectors(v->part) * 512;
    if (start % 512 || size < 2 * MDA_HEADER || size > (1ULL << 30) || start + size > sectors) return NULL;
    uint8_t header[512];
    if (block_part_read(v->part, start / 512, 1, header) < 0) return NULL;
    if (lvm_crc(LVM_INITIAL_CRC, header + 4, 512 - 4) != get_le32(header) || memcmp(header + 4, " LVM2 x[5A%r0N*>", 16) ||
        get_le32(header + 20) != 1 || get_le64(header + 24) != start || get_le64(header + 32) != size) { say("bad metadata area header on ", v->part->linux_name, NULL); return NULL; }
    uint64_t offset = get_le64(header + 40), bytes = get_le64(header + 48); uint32_t checksum = get_le32(header + 56), flags = get_le32(header + 60);
    if (flags & 1 || !bytes) return NULL;   /* an ignored or empty area */
    if (offset < MDA_HEADER || offset >= size || bytes > size - MDA_HEADER || bytes > TEXT_MAX) { say("bad metadata location on ", v->part->linux_name, NULL); return NULL; }
    char* text = (void*)arena_alloc(bytes + 512); if (!text) return NULL;
    uint64_t first = offset + bytes <= size ? bytes : size - offset;   /* the circular buffer wraps to MDA_HEADER */
    uint8_t* bounce = (void*)arena_alloc(65536); if (!bounce) { arena_free((uint64_t)text, bytes + 512); return NULL; }
    bool ok = true;
    for (unsigned part = 0; part < 2 && ok; part++) {
        uint64_t from = part ? MDA_HEADER : offset, count = part ? bytes - first : first, done = 0, out = part ? first : 0;
        while (done < count && ok) {   /* sector-aligned reads through a bounce buffer */
            uint64_t at = start + from + done, sector = at / 512, skip_bytes = at % 512, want = count - done;
            uint64_t span = (skip_bytes + want + 511) / 512; if (span > 128) span = 128;
            if (block_part_read(v->part, sector, (uint32_t)span, bounce) < 0) { ok = false; break; }
            uint64_t got = span * 512 - skip_bytes; if (got > want) got = want;
            memcpy(text + out + done, bounce + skip_bytes, got); done += got;
        }
    }
    arena_free((uint64_t)bounce, 65536);
    if (!ok || lvm_crc(LVM_INITIAL_CRC, (const uint8_t*)text, bytes) != checksum) {
        if (ok) say("metadata checksum mismatch on ", v->part->linux_name, NULL);
        arena_free((uint64_t)text, bytes + 512); return NULL;
    }
    text[bytes] = 0; *length = (uint32_t)bytes; return text;
}

/* ---- one volume group ---- */
struct segment { uint64_t start, count, stripe_size; unsigned stripes; int pv[STRIPES]; uint64_t pv_extent[STRIPES]; };
struct lv { char name[128], id[40]; struct segment seg[MAX_SEGS]; unsigned segs; bool usable, snapshot, hidden; char origin[128], cow[128]; uint64_t chunk; int origin_of, cow_of; };
struct vg { char name[128], id[40]; uint64_t extent; int pv_map[MAX_PVS]; uint64_t pv_start[MAX_PVS], pv_count_extents[MAX_PVS]; unsigned pvn; char pv_names[MAX_PVS][16];
            struct lv lv[MAX_LVS]; unsigned lvs; };
static void plain_id(const char* id, char* out) { unsigned k = 0; for (; *id && k < 32; id++) if (*id != '-') out[k++] = *id; out[k] = 0; }
static void dm_name(const char* vg, const char* lv, const char* layer, char* out, unsigned capacity) {   /* lvm2: '-' doubled */
    unsigned k = 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        const char* p = pass ? lv : vg;
        if (pass && k + 1 < capacity) out[k++] = '-';
        for (; *p && k + 2 < capacity; p++) { if (*p == '-') out[k++] = '-'; out[k++] = *p; }
    }
    out[k] = 0;
    if (layer) { unsigned n = strlen(layer); if (k + n + 2 < capacity) { out[k++] = '-'; memcpy(out + k, layer, n + 1); } }
}
static int load_vg(struct cfg* c, struct vg* g) {
    int32_t top = -1;
    for (int32_t i = 0; i >= 0 && (unsigned)i < c->count; i = c->nodes[i].next) if (c->nodes[i].section) { top = i; break; }
    if (top < 0 || c->nodes[top].klen >= sizeof(g->name)) return -22;
    memset(g, 0, sizeof(*g)); memcpy(g->name, c->text + c->nodes[top].key, c->nodes[top].klen);
    if (!text_of(c, child(c, top, "id"), g->id, sizeof(g->id)) || !number(c, child(c, top, "extent_size"), &g->extent) || !g->extent || g->extent > (1ULL << 32)) return -22;
    int32_t list = child(c, top, "physical_volumes"); if (list < 0) return -22;
    for (int32_t p = c->nodes[list].child; p >= 0; p = c->nodes[p].next) {
        if (!c->nodes[p].section || g->pvn == MAX_PVS || c->nodes[p].klen >= 16) return -22;
        char id[40], plain[33]; uint64_t start, count;
        if (!text_of(c, child(c, p, "id"), id, sizeof(id)) || !number(c, child(c, p, "pe_start"), &start) || !number(c, child(c, p, "pe_count"), &count)) return -22;
        unsigned k = g->pvn++; memcpy(g->pv_names[k], c->text + c->nodes[p].key, c->nodes[p].klen); g->pv_names[k][c->nodes[p].klen] = 0;
        g->pv_start[k] = start; g->pv_count_extents[k] = count; g->pv_map[k] = -1;
        plain_id(id, plain);
        for (unsigned i = 0; i < pv_count; i++) if (!strcmp(pvs[i].uuid, plain)) {
            uint64_t room = block_part_sectors(pvs[i].part);
            if (count && (start >= room || count > (room - start) / g->extent)) { say("PV extents beyond the device: ", pvs[i].part->linux_name, NULL); return -22; }
            g->pv_map[k] = (int)i;
        }
    }
    int32_t lvs = child(c, top, "logical_volumes");
    for (int32_t l = lvs < 0 ? -1 : c->nodes[lvs].child; l >= 0; l = c->nodes[l].next) {
        if (!c->nodes[l].section || g->lvs == MAX_LVS || c->nodes[l].klen >= 128) return -22;
        struct lv* v = &g->lv[g->lvs++]; memset(v, 0, sizeof(*v)); v->origin_of = v->cow_of = -1;
        memcpy(v->name, c->text + c->nodes[l].key, c->nodes[l].klen);
        if (!text_of(c, child(c, l, "id"), v->id, sizeof(v->id))) return -22;
        uint64_t segs; if (!number(c, child(c, l, "segment_count"), &segs) || !segs || segs > MAX_SEGS) { say("LV skipped (segments): ", v->name, NULL); continue; }
        if (list_has(c, child(c, l, "flags"), "ACTIVATION_SKIP")) { say("LV not activated by default (activation skip): ", v->name, NULL); continue; }
        v->usable = true;
        uint64_t next = 0;
        for (unsigned s = 1; s <= segs && v->usable; s++) {
            char key[16] = "segment"; append_u64(key, s);
            int32_t sg = child(c, l, key); char type[32];
            struct segment* x = &v->seg[v->segs];
            if (sg < 0 || !text_of(c, child(c, sg, "type"), type, sizeof(type)) || !number(c, child(c, sg, "start_extent"), &x->start) ||
                !number(c, child(c, sg, "extent_count"), &x->count) || x->start != next || !x->count) { v->usable = false; say("LV skipped (segment layout): ", v->name, NULL); break; }
            if (!strcmp(type, "snapshot")) {
                if (segs != 1 || !text_of(c, child(c, sg, "origin"), v->origin, sizeof(v->origin)) || !text_of(c, child(c, sg, "cow_store"), v->cow, sizeof(v->cow)) ||
                    !number(c, child(c, sg, "chunk_size"), &v->chunk) || v->chunk < 8 || v->chunk > 1024 || (v->chunk & (v->chunk - 1))) { v->usable = false; say("snapshot skipped: ", v->name, NULL); }
                v->snapshot = true; break;
            }
            uint64_t stripes;
            if (strcmp(type, "striped") || !number(c, child(c, sg, "stripe_count"), &stripes) || !stripes || stripes > STRIPES || x->count % stripes) {
                v->usable = false; say("LV skipped (segment type not supported): ", v->name, NULL); break;
            }
            x->stripes = (unsigned)stripes;
            if (stripes > 1 && (!number(c, child(c, sg, "stripe_size"), &x->stripe_size) || x->stripe_size < 8 || (x->stripe_size & (x->stripe_size - 1)) ||
                                (x->count / stripes * g->extent) % x->stripe_size)) { v->usable = false; say("LV skipped (stripe size): ", v->name, NULL); break; }
            int32_t list_node = child(c, sg, "stripes");
            for (unsigned k = 0; k < stripes && v->usable; k++) {
                char pv[16]; uint64_t extent; int found = -1;
                if (!list_item(c, list_node, 2 * k, pv, sizeof(pv), NULL) || !list_item(c, list_node, 2 * k + 1, NULL, 0, &extent)) { v->usable = false; break; }
                for (unsigned i = 0; i < g->pvn; i++) if (!strcmp(g->pv_names[i], pv)) found = (int)i;
                if (found < 0 || g->pv_map[found] < 0) { v->usable = false; say("LV skipped (a PV is missing): ", v->name, NULL); break; }
                if (extent > g->pv_count_extents[found] || x->count / stripes > g->pv_count_extents[found] - extent) { v->usable = false; say("LV skipped (extents beyond a PV): ", v->name, NULL); break; }
                x->pv[k] = found; x->pv_extent[k] = extent;
            }
            if (v->usable) { v->segs++; next = x->start + x->count; }
        }
        if (v->usable && !v->snapshot && next > (~0ULL / 2) / g->extent) v->usable = false;
        if (!list_has(c, child(c, l, "status"), "VISIBLE")) v->hidden = true;   /* a pool's _tmeta/_tdata, pmspare, raid images, a snapshot's COW store */
    }
    for (unsigned i = 0; i < g->lvs; i++) if (g->lv[i].snapshot && g->lv[i].usable) {   /* origin and COW LVs of each snapshot */
        struct lv* s = &g->lv[i]; int o = -1, w = -1;
        for (unsigned k = 0; k < g->lvs; k++) { if (!strcmp(g->lv[k].name, s->origin)) o = (int)k; if (!strcmp(g->lv[k].name, s->cow)) w = (int)k; }
        if (o < 0 || w < 0 || g->lv[o].snapshot || g->lv[w].snapshot || !g->lv[o].usable || !g->lv[w].usable || g->lv[w].cow_of >= 0) { s->usable = false; say("snapshot skipped (origin or COW): ", s->name, NULL); continue; }
        g->lv[w].cow_of = (int)i; g->lv[o].origin_of = (int)i;
    }
    for (unsigned i = 0; i < g->lvs; i++)   /* hidden LVs other than snapshot segments and their COW stores: lvm2's own */
        if (g->lv[i].hidden && !g->lv[i].snapshot && g->lv[i].cow_of < 0) g->lv[i].usable = false;
    return 0;
}
static uint64_t lv_sectors(const struct vg* g, const struct lv* v) { uint64_t n = 0; for (unsigned s = 0; s < v->segs; s++) n += v->seg[s].count * g->extent; return n; }
static int create_lv(const struct vg* g, const struct lv* v, const char* layer, struct block_device** out) {   /* its segments as linear/striped */
    char name[260], uuid[129], vid[33], lid[33], lines[MAX_SEGS][512]; const char* list[MAX_SEGS];
    dm_name(g->name, v->name, layer, name, sizeof(name));
    if (strlen(name) >= 128) return -22;
    plain_id(g->id, vid); plain_id(v->id, lid);
    strcpy(uuid, "LVM-"); strcat(uuid, vid); strcat(uuid, lid); if (layer) { strcat(uuid, "-"); strcat(uuid, layer); }
    for (unsigned s = 0; s < v->segs; s++) {
        const struct segment* x = &v->seg[s]; char* p = lines[s]; p[0] = 0;
        append_u64(p, x->start * g->extent); strcat(p, " "); append_u64(p, x->count * g->extent);
        strcat(p, x->stripes == 1 ? " linear" : " striped ");
        if (x->stripes > 1) { append_u64(p, x->stripes); strcat(p, " "); append_u64(p, x->stripe_size); }
        for (unsigned k = 0; k < x->stripes; k++) {
            const struct block_partition* part = pvs[g->pv_map[x->pv[k]]].part;
            strcat(p, " "); append_u64(p, part->major); strcat(p, ":"); append_u64(p, part->minor); strcat(p, " ");
            append_u64(p, g->pv_start[x->pv[k]] + x->pv_extent[k] * g->extent);
        }
        list[s] = p;
    }
    if (dm_name_exists(name)) { say("already active: ", name, NULL); return -17; }
    int e = dm_kernel_create(name, uuid, list, v->segs, false, out);
    if (e) say("could not activate ", name, NULL);
    return e;
}
static int create_single(const char* name, const char* uuid, uint64_t sectors, const char* type_params, struct block_device** out) {
    char line[512] = "0 "; append_u64(line, sectors); strcat(line, " "); strcat(line, type_params);
    const char* list[1] = {line};
    int e = dm_kernel_create(name, uuid, list, 1, false, out);
    if (e) say("could not activate ", name, NULL);
    return e;
}
static void dev_text(struct block_device* d, char* out) { const struct block_partition* w = block_whole(d); append_u64(out, w->major); strcat(out, ":"); append_u64(out, w->minor); }
/* every usable LV of the VG; returns how many came up */
static unsigned activate(struct vg* g) {
    unsigned up = 0; char vid[33]; plain_id(g->id, vid);
    for (unsigned i = 0; i < g->lvs; i++) {
        struct lv* v = &g->lv[i]; struct block_device* d = NULL;
        if (!v->usable || v->snapshot || v->cow_of >= 0) continue;
        if (v->origin_of < 0) { if (!create_lv(g, v, NULL, &d)) up++; continue; }
        /* an origin with a snapshot: <lv>-real under <lv> (snapshot-origin), <cow>-cow under <cow> (snapshot) */
        struct lv* s = &g->lv[v->origin_of]; struct lv* w = NULL;
        for (unsigned k = 0; k < g->lvs; k++) if (g->lv[k].cow_of == v->origin_of) w = &g->lv[k];
        struct block_device *real = NULL, *cow = NULL;
        if (create_lv(g, v, "real", &real)) continue;
        char name[260], uuid[129], lid[33], params[256];
        dm_name(g->name, v->name, NULL, name, sizeof(name)); plain_id(v->id, lid);
        strcpy(uuid, "LVM-"); strcat(uuid, vid); strcat(uuid, lid);
        strcpy(params, "snapshot-origin "); dev_text(real, params);
        if (!create_single(name, uuid, lv_sectors(g, v), params, &d)) up++;
        if (!w || create_lv(g, w, "cow", &cow)) continue;
        dm_name(g->name, w->name, NULL, name, sizeof(name)); plain_id(w->id, lid);
        strcpy(uuid, "LVM-"); strcat(uuid, vid); strcat(uuid, lid);
        strcpy(params, "snapshot "); dev_text(real, params); strcat(params, " "); dev_text(cow, params); strcat(params, " P "); append_u64(params, s->chunk);
        if (!create_single(name, uuid, lv_sectors(g, v), params, &d)) up++;
    }
    return up;
}

/* ---- boot ---- */
static bool decode(const char* dm, char* vg, char* lv) {   /* "<vg>-<lv>" with '-' doubled */
    unsigned v = 0, l = 0; bool second = false;
    for (const char* p = dm; *p; p++) {
        char ch = *p;
        if (ch == '-' && p[1] == '-') p++;
        else if (ch == '-' && !second) { second = true; continue; }
        if (second ? l + 1 >= 128 : v + 1 >= 128) return false;
        if (second) lv[l++] = ch; else vg[v++] = ch;
    }
    vg[v] = lv[l] = 0; return second && v && l;
}
static bool root_lv(char* vg, char* lv) {   /* root=/dev/mapper/<vg>-<lv> or /dev/<vg>/<lv> */
    char root[160];
    if (!boot_value("root", root, sizeof(root))) return false;
    if (!strncmp(root, "/dev/mapper/", 12)) return decode(root + 12, vg, lv);
    if (strncmp(root, "/dev/", 5) || !strncmp(root, "/dev/disk/", 10)) return false;
    const char* slash = s_strchr(root + 5, '/'); if (!slash || s_strchr(slash + 1, '/') || slash - (root + 5) >= 128 || strlen(slash + 1) >= 128) return false;
    memcpy(vg, root + 5, (unsigned)(slash - (root + 5))); vg[slash - (root + 5)] = 0; strcpy(lv, slash + 1); return true;
}
static bool wanted(const char* vg, bool late) {
    if (late) return true;
    char value[160], rv[128], rl[128];
    if (boot_value("rd.lvm.vg", value, sizeof(value)) && !strcmp(value, vg)) return true;
    if (boot_value("rd.lvm.lv", value, sizeof(value))) { const char* slash = s_strchr(value, '/'); if (slash && (unsigned)(slash - value) == strlen(vg) && !strncmp(value, vg, strlen(vg))) return true; }
    if (root_lv(rv, rl)) return !strcmp(rv, vg);
    return boot_value("root", value, sizeof(value)) && (!strncmp(value, "UUID=", 5) || !strncmp(value, "LABEL=", 6) || !strncmp(value, "PARTUUID=", 9));
}
static struct block_partition* candidate(unsigned index) {
    unsigned parts = block_partition_count();
    if (index < parts) return (struct block_partition*)block_partition_get(index);
    struct block_device* d = block_device_get(index - parts);
    return d && !d->removed ? block_whole(d) : NULL;
}
static uint32_t text_length(const char* text, uint32_t bytes) { uint32_t n = 0; while (n < bytes && text[n]) n++; return n; }   /* lvm2 stores the NUL */
struct vg_found { char* text; uint32_t bytes, length; uint64_t seq; char id[40]; };
static unsigned run(bool late) {
    if (boot_option("rd.lvm=0") || boot_option("lvm=0")) return 0;
    uint8_t* sector = (void*)arena_alloc(512); if (!sector) return 0;
    pv_count = 0;
    unsigned total = block_partition_count() + block_device_count();
    for (unsigned i = 0; i < total; i++) { struct block_partition* p = candidate(i); if (p && !p->dev->removed && block_part_sectors(p) >= 8 && !p->holders) probe_pv(p, sector); }
    arena_free((uint64_t)sector, 512);
    if (!pv_count) return 0;
    struct cfg_node* nodes = (void*)arena_alloc(NODES_MAX * sizeof(struct cfg_node));
    struct vg* g = (void*)arena_alloc(sizeof(struct vg));
    struct vg_found* found = (void*)arena_alloc(sizeof(struct vg_found) * MAX_PVS);
    unsigned up = 0, vgs = 0;
    for (unsigned j = 0; nodes && g && found && j < pv_count; j++) for (unsigned m = 0; m < pvs[j].mdas; m++) {   /* every copy once */
        uint32_t bytes; char* text = read_text(&pvs[j], m, &bytes); if (!text) continue;
        uint32_t length = text_length(text, bytes);
        struct cfg c = {text, length, nodes, 0}; uint32_t at = 0; int32_t first = -1; char id[40] = ""; uint64_t seq = 0; bool good = false;
        if (!parse_body(&c, &at, &first, 0, true) && c.count) {
            int32_t top = -1; for (int32_t k = 0; k >= 0; k = c.nodes[k].next) if (c.nodes[k].section) { top = k; break; }
            good = top >= 0 && text_of(&c, child(&c, top, "id"), id, sizeof(id)) && number(&c, child(&c, top, "seqno"), &seq);
        }
        if (!good) { say("metadata text refused on ", pvs[j].part->linux_name, NULL); arena_free((uint64_t)text, bytes + 512); continue; }
        unsigned k = 0; while (k < vgs && strcmp(found[k].id, id)) k++;
        if (k < vgs && found[k].seq >= seq) { arena_free((uint64_t)text, bytes + 512); continue; }   /* an older copy */
        if (k < vgs) arena_free((uint64_t)found[k].text, found[k].bytes + 512);
        else if (vgs == MAX_PVS) { arena_free((uint64_t)text, bytes + 512); continue; }
        else k = vgs++;
        found[k].text = text; found[k].bytes = bytes; found[k].length = length; found[k].seq = seq; strcpy(found[k].id, id);
    }
    for (unsigned k = 0; k < vgs; k++) {   /* each VG: its newest copy */
        struct cfg c = {found[k].text, found[k].length, nodes, 0}; uint32_t at = 0; int32_t first = -1;
        if (parse_body(&c, &at, &first, 0, true) || load_vg(&c, g)) say("volume group refused (metadata)", NULL, NULL);
        else if (wanted(g->name, late)) {
            unsigned n = activate(g); up += n;
            char count[24] = ""; append_u64(count, n);
            say("volume group ", g->name, late ? " activated (late)" : " activated"); say("logical volumes active: ", count, NULL);
        }
        arena_free((uint64_t)found[k].text, found[k].bytes + 512);
    }
    if (found) arena_free((uint64_t)found, sizeof(struct vg_found) * MAX_PVS);
    if (nodes) arena_free((uint64_t)nodes, NODES_MAX * sizeof(struct cfg_node));
    if (g) arena_free((uint64_t)g, sizeof(struct vg));
    return up;
}
/* kernel.c, before the root is looked for: the root's VG; true when the root is now an LV */
bool lvm_boot(void) {
    if (!run(false)) return false;
    char vg[128], lv[128], name[260], spec[160];
    for (unsigned i = 0; i < block_device_count(); i++) {   /* the root LV: by name, else by root=UUID=/LABEL= */
        struct block_device* d = block_device_get(i); if (!d || d->removed) continue;
        struct block_partition* w = block_whole(d); bool hit = false;
        if (root_lv(vg, lv)) { dm_name(vg, lv, NULL, name, sizeof(name)); char text[160] = ""; hit = dm_sysfs_attr(d, 0, text) && !strcmp(text, name); }
        else if (boot_value("root", spec, sizeof(spec))) hit = block_spec_matches(w, spec) == 1;
        if (hit) { block_unlocked_root = w; say("root on ", d->linux_name, NULL); return true; }
    }
    return false;
}
void lvm_boot_late(void) { run(true); }   /* kernel.c, after /etc/crypttab: every other VG */
#pragma clang attribute pop
