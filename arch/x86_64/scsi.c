/* SCSI disks over any transport: see scsi.h. The retry policy follows Linux's scsi_decide_disposition /
 * sd_done in spirit: UNIT ATTENTION and ABORTED COMMAND are retried, NOT READY waits while the unit is becoming
 * ready (and starts a stopped unit), MEDIUM/HARDWARE ERROR are retried once, ILLEGAL REQUEST and DATA PROTECT
 * fail at once (EOPNOTSUPP/EIO, EROFS), a missing medium is ENOMEDIUM; transport failures go through the
 * transport's reset (USB: Bulk-Only reset recovery, UAS pipe resets) at most three times before the unit is
 * taken offline. A logical block size of 1024/2048 bytes (optical media) is presented as 512-byte sectors,
 * read-only. */
#include "scsi.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static void log_disk(const struct scsi_disk* d, const char* text) {
    vga_print("[64] SCSI "); vga_print(d->block.linux_name[0] ? d->block.linux_name : d->kind); vga_print(" lun ");
    char n[12]; uint_to_str(d->lun, n); vga_print(n); vga_print(": "); vga_print(text); vga_print("\n");
}
static void log_sense(const struct scsi_disk* d, const uint8_t* cdb, const struct scsi_sense* s) {
    static const char hex[] = "0123456789abcdef";
    char text[64] = "sense key=0x0 asc=0x00 ascq=0x00 op=0x00";
    text[12] = hex[s->key & 15]; text[19] = hex[s->asc >> 4]; text[20] = hex[s->asc & 15];
    text[28] = hex[s->ascq >> 4]; text[29] = hex[s->ascq & 15]; text[35] = hex[cdb[0] >> 4]; text[36] = hex[cdb[0] & 15];
    log_disk(d, text);
}

bool scsi_decode_sense(const uint8_t* sense, unsigned length, struct scsi_sense* out) {
    memset(out, 0, sizeof(*out));
    if (length < 2) return false;
    unsigned code = sense[0] & 0x7F;
    if (code == 0x70 || code == 0x71) {           /* fixed format */
        if (length < 3) return false;
        out->key = sense[2] & 15;
        if (length >= 14) { out->asc = sense[12]; out->ascq = sense[13]; }
    } else if (code == 0x72 || code == 0x73) {    /* descriptor format */
        if (length < 4) return false;
        out->key = sense[1] & 15; out->asc = sense[2]; out->ascq = sense[3];
    } else return false;
    out->current = code == 0x70 || code == 0x72; out->valid = true;
    return true;
}

#define NO_START 1u
#define QUIET 2u
static int run(struct scsi_disk* d, struct scsi_command* c, unsigned flags) {
    const struct scsi_transport* t = d->transport;
    unsigned attempts = 0, not_ready = 0, transport_errors = 0;
    for (;;) {
        if (t->gone(d->context)) return -19;
        if (d->offline) return -5;
        c->transferred = 0; c->sense_length = 0;
        if (!c->timeout_ns) c->timeout_ns = 30000000000ULL;
        int r = t->execute(d->context, d->lun, c);
        if (r == SCSI_GOOD) return 0;
        if (r < 0) {
            if (r == -19 || t->gone(d->context)) return -19;
            if (++transport_errors > 3) { d->offline = true; log_disk(d, "transport keeps failing: unit offline"); return -5; }
            d->resets++;
            if (!(flags & QUIET)) log_disk(d, r == -110 ? "command timed out: transport reset" : "transport error: reset");
            int e = t->reset(d->context, d->lun);
            if (e == -19) return -19;
            continue;
        }
        if (r == SCSI_BUSY) { if (++attempts > 30) return -5; t->sleep(d->context, 100000000ULL); continue; }
        struct scsi_sense s;
        if (!scsi_decode_sense(c->sense, c->sense_length, &s)) { if (++attempts > 3) return -5; continue; }
        if (!(flags & QUIET) && s.key != 6 && !(s.key == 2 && s.asc == 0x04) && !(s.key == 5 && (s.asc == 0x20 || s.asc == 0x24))) log_sense(d, c->cdb, &s);
        switch (s.key) {
        case 0x0: if (++attempts > 3) return -5; continue;                     /* NO SENSE */
        case 0x1: return 0;                                                    /* RECOVERED ERROR */
        case 0x6: if (++attempts > 8) return -5; continue;                     /* UNIT ATTENTION */
        case 0x2:                                                              /* NOT READY */
            if (s.asc == 0x3A) return -123;                                    /* medium not present */
            if (s.asc == 0x04 && s.ascq == 0x02 && !(flags & NO_START)) {      /* initializing command required */
                struct scsi_command start = {.cdb = {0x1B, 0, 0, 0, 0x01, 0}, .cdb_length = 6, .timeout_ns = 30000000000ULL};
                if (++not_ready > 3) return -5;
                int e = run(d, &start, NO_START | QUIET); if (e == -19) return e;
                continue;
            }
            if (s.asc == 0x04 && s.ascq != 0x03) {                             /* becoming ready / in progress */
                if (++not_ready > 100) return -5;
                t->sleep(d->context, 100000000ULL); continue;
            }
            return -5;
        case 0x3: case 0x4: if (++attempts > 1) return -5; continue;          /* MEDIUM / HARDWARE ERROR */
        case 0x5: return s.asc == 0x20 || s.asc == 0x24 || s.asc == 0x25 ? -95 : -5; /* ILLEGAL REQUEST */
        case 0x7: d->write_protect = true; return -30;                         /* DATA PROTECT */
        case 0xB: if (++attempts > 5) return -5; continue;                     /* ABORTED COMMAND */
        default: return -5;
        }
    }
}
static int simple(struct scsi_disk* d, const uint8_t* cdb, unsigned cdb_length, void* data, uint32_t length, bool in, uint32_t* transferred, unsigned flags) {
    struct scsi_command c; memset(&c, 0, sizeof(c));
    memcpy(c.cdb, cdb, cdb_length); c.cdb_length = cdb_length; c.data = data; c.length = length; c.in = in;
    c.timeout_ns = 10000000000ULL;
    int r = run(d, &c, flags);
    if (transferred) *transferred = c.transferred;
    return r;
}
static void trimmed(char* out, const uint8_t* in, unsigned length) {
    unsigned n = 0;
    for (unsigned i = 0; i < length; i++) out[n++] = in[i] >= 32 && in[i] < 127 ? (char)in[i] : ' ';
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}
int scsi_disk_ready(struct scsi_disk* d) {
    static const uint8_t tur[6] = {0x00};
    return simple(d, tur, 6, NULL, 0, false, NULL, QUIET);
}
static void mode_pages(struct scsi_disk* d, const uint8_t* page, const uint8_t* end) {
    while (page + 2 <= end) {
        unsigned code = page[0] & 0x3F, length = page[1];
        if (page + 2 + length > end) break;
        if (code == 0x08 && length >= 1) { d->write_cache = (page[2] & 0x04) != 0; d->cache_known = true; }
        page += 2 + length;
    }
}
static void mode_sense(struct scsi_disk* d) {
    uint8_t data[192] __attribute__((aligned(16))); uint32_t got = 0;
    uint8_t six[6] = {0x1A, 0x00, 0x3F, 0x00, 192, 0x00};
    memset(data, 0, sizeof(data));
    int r = simple(d, six, 6, data, 192, true, &got, QUIET);
    if (!r && got >= 4) {
        d->write_protect = (data[2] & 0x80) != 0;
        unsigned total = data[0] + 1u; if (total > got) total = got;
        if (4u + data[3] <= total) mode_pages(d, data + 4 + data[3], data + total);
        return;
    }
    if (r == -19) return;
    uint8_t ten[10] = {0x5A, 0x00, 0x3F, 0, 0, 0, 0, 0, 192, 0};
    memset(data, 0, sizeof(data));
    r = simple(d, ten, 10, data, 192, true, &got, QUIET);
    if (!r && got >= 8) {
        d->write_protect = (data[3] & 0x80) != 0;
        unsigned total = ((unsigned)data[0] << 8 | data[1]) + 2u, descriptors = (unsigned)data[6] << 8 | data[7];
        if (total > got) total = got;
        if (8u + descriptors <= total) mode_pages(d, data + 8 + descriptors, data + total);
    }
}
static int capacity(struct scsi_disk* d) {
    uint8_t data[32] __attribute__((aligned(16))); uint32_t got = 0;
    uint8_t ten[10] = {0x25};
    memset(data, 0, sizeof(data));
    int r = simple(d, ten, 10, data, 8, true, &got, 0);
    if (r) return r;
    if (got < 8) return -5;
    uint64_t last = be32(data); uint32_t bs = be32(data + 4);
    d->physical_exponent = 0; d->lbpme = false;
    if (last == 0xFFFFFFFFULL || d->version >= 5) {
        uint8_t sixteen[16] = {0x9E, 0x10}; put32(sixteen + 10, 32);
        memset(data, 0, sizeof(data));
        int r16 = simple(d, sixteen, 16, data, 32, true, &got, QUIET);
        if (!r16 && got >= 12) {
            last = be64(data); bs = be32(data + 8);
            if (got >= 15) { d->physical_exponent = data[13] & 15; d->lbpme = (data[14] & 0x80) != 0; }
        } else if (last == 0xFFFFFFFFULL) return r16 ? r16 : -5;
    }
    if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096) { log_disk(d, "unsupported logical block size"); return -5; }
    if (last == ~0ULL) return -5;
    d->blocks = last + 1; d->block_size = bs;
    d->use16 = d->blocks > 0xFFFFFFFFULL;
    return 0;
}
static int emulated(struct scsi_disk* d) { return (int)(d->block_size == 1024 || d->block_size == 2048 ? d->block_size / 512 : 1); }
static const struct block_extra_ops scsi_extra;
int scsi_disk_probe(struct scsi_disk* d) {
    uint8_t data[96] __attribute__((aligned(16))); uint32_t got = 0;
    uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    memset(data, 0, sizeof(data));
    int r = simple(d, inquiry, 6, data, 36, true, &got, 0);
    if (r) return r;
    if (got < 5) return -5;
    unsigned qualifier = data[0] >> 5, type = data[0] & 31;
    if (qualifier == 3 || type == 0x1F) return -6;
    d->peripheral = (uint8_t)type; d->removable = (data[1] & 0x80) != 0; d->version = data[2] & 7;
    if (got >= 36) { trimmed(d->vendor, data + 8, 8); trimmed(d->product, data + 16, 16); trimmed(d->revision, data + 32, 4); }
    if (type != 0 && type != 0x0E && type != 5 && type != 7) return -95;
    /* TEST UNIT READY until ready: UNIT ATTENTION after power-on/reset and "becoming ready" are retried by run(). */
    r = scsi_disk_ready(d);
    if (r == -123) { d->media = false; return -123; }
    if (r == -19) return r;
    if (r) { r = scsi_disk_ready(d); if (r) return r == -123 ? -123 : r; }
    d->media = true;
    if ((r = capacity(d))) return r;
    if (type == 0 || type == 0x0E) mode_sense(d); else d->write_protect = true;
    if (!d->cache_known) d->write_cache = true;   /* unknown: send SYNCHRONIZE CACHE (ILLEGAL REQUEST disables it) */
    struct block_device* b = &d->block;
    unsigned factor = (unsigned)emulated(d);
    b->kind = d->kind; b->driver = d; b->ops = &scsi_disk_ops; b->extra = &scsi_extra;
    b->block_size = factor > 1 ? 512 : d->block_size;
    b->blocks = d->blocks * factor;
    uint32_t per = d->transport->max_transfer / d->block_size; if (!per) per = 1;
    b->max_blocks = per * factor;
    b->physical_block_size = d->block_size << d->physical_exponent;
    b->removable = d->removable; b->read_only = d->write_protect || factor > 1 || type == 5;
    b->rotational = false;
    strcpy(b->vendor, d->vendor);
    char model[41]; unsigned n = 0;
    for (const char* s = d->vendor; *s && n < 40; s++) model[n++] = *s;
    if (n && n < 40) model[n++] = ' ';
    for (const char* s = d->product; *s && n < 40; s++) model[n++] = *s;
    model[n] = 0; strcpy(b->model, model);
    if (d->lbpme) b->discard_granularity = (uint64_t)d->block_size << d->physical_exponent;
    return 0;
}
static int transfer(struct scsi_disk* d, uint64_t lba, uint32_t blocks, uint8_t* buffer, bool write) {
    uint8_t cdb[16]; memset(cdb, 0, sizeof(cdb)); unsigned length;
    if (d->use16 || lba + blocks > 0xFFFFFFFFULL || blocks > 0xFFFF) {
        cdb[0] = write ? 0x8A : 0x88; put64(cdb + 2, lba); put32(cdb + 10, blocks); length = 16;
    } else {
        cdb[0] = write ? 0x2A : 0x28; put32(cdb + 2, (uint32_t)lba); put16(cdb + 7, blocks); length = 10;
    }
    struct scsi_command c; memset(&c, 0, sizeof(c));
    memcpy(c.cdb, cdb, length); c.cdb_length = length; c.data = buffer; c.length = blocks * d->block_size; c.in = !write;
    c.timeout_ns = 30000000000ULL;
    int r = run(d, &c, 0);
    if (!r && c.transferred != c.length) { log_disk(d, "short transfer"); r = -5; }
    return r;
}
int scsi_disk_io(struct scsi_disk* d, uint64_t lba, uint32_t count, void* data, bool write) {
    if (!count) return 0;
    if (write && d->block.read_only) return -30;
    struct block_device* b = &d->block;
    unsigned factor = (unsigned)emulated(d);
    uint8_t* buffer = data; int r = 0;
    uint64_t begin = block_ticks();
    block_get(b);
    d->transport->retain(d->context);
    uint32_t per = d->transport->max_transfer / d->block_size; if (!per) per = 1;
    if (factor > 1) {
        /* 512-byte view of 1024/2048-byte blocks (read-only): whole device blocks go straight to the caller,
         * partial ones through a block-sized buffer. */
        uint8_t* bounce = (void*)arena_alloc(d->block_size);
        if (!bounce) r = -12;
        while (!r && count) {
            uint64_t device = lba / factor; unsigned skip = (unsigned)(lba % factor);
            if (!skip && count >= factor) {
                uint32_t whole = count / factor; if (whole > per) whole = per;
                r = transfer(d, device, whole, buffer, false);
                if (!r) { lba += (uint64_t)whole * factor; count -= whole * factor; buffer += (uint64_t)whole * d->block_size; b->stats.read_cmds++; b->stats.read_sectors += whole * factor; }
            } else {
                r = transfer(d, device, 1, bounce, false);
                unsigned take = factor - skip; if (take > count) take = count;
                if (!r) { memcpy(buffer, bounce + skip * 512, take * 512); lba += take; count -= take; buffer += take * 512; b->stats.read_cmds++; b->stats.read_sectors += take; }
            }
        }
        if (bounce) arena_free((uint64_t)bounce, d->block_size);
    } else {
        while (!r && count) {
            uint32_t n = count < per ? count : per;
            r = transfer(d, lba, n, buffer, write);
            if (r) break;
            uint64_t sectors = (uint64_t)n * (d->block_size / 512);
            if (write) { b->stats.write_cmds++; b->stats.write_sectors += sectors; } else { b->stats.read_cmds++; b->stats.read_sectors += sectors; }
            lba += n; count -= n; buffer += (uint64_t)n * d->block_size;
        }
    }
    if (r) b->stats.errors++;
    b->stats.busy_ticks += block_ticks() - begin;
    d->transport->release(d->context);
    block_put(b);   /* may release the disk (device left meanwhile): nothing touches it afterwards */
    return r;
}
int scsi_disk_flush(struct scsi_disk* d) {
    if (d->no_sync || d->block.read_only || !d->write_cache) return 0;
    static const uint8_t sync[10] = {0x35};
    struct scsi_command c; memset(&c, 0, sizeof(c)); memcpy(c.cdb, sync, 10); c.cdb_length = 10; c.timeout_ns = 60000000000ULL;
    block_get(&d->block); d->transport->retain(d->context);
    int r = run(d, &c, QUIET);
    if (r == -95) { d->no_sync = true; r = 0; }
    if (!r) d->block.stats.flushes++;
    d->transport->release(d->context);
    block_put(&d->block);
    return r;
}
int scsi_disk_prevent(struct scsi_disk* d, bool prevent) {
    uint8_t cdb[6] = {0x1E, 0, 0, 0, prevent ? 1 : 0, 0};
    int r = simple(d, cdb, 6, NULL, 0, false, NULL, QUIET);
    if (!r) d->prevented = prevent;
    return r == -95 ? 0 : r;
}
int scsi_disk_eject(struct scsi_disk* d) {
    int r = scsi_disk_flush(d);
    if (r == -19) return r;
    scsi_disk_prevent(d, false);
    uint8_t stop[6] = {0x1B, 0, 0, 0, d->removable ? 0x02 : 0x00, 0};   /* stop (+ load/eject for removable media) */
    r = simple(d, stop, 6, NULL, 0, false, NULL, QUIET);
    if (r == -95) r = 0;
    /* A fixed-medium drive (a USB stick) leaves like udisks' power-off: the transport removes it from /dev.
     * A removable-medium drive keeps its unit; medium polling notices the ejected medium. */
    if (!r && !d->removable) d->ejected = true;
    return r;
}
/* REPORT LUNS: single-level LUN numbers (peripheral and flat addressing); 0 entries on failure. */
int scsi_report_luns(const struct scsi_transport* t, void* context, uint8_t* luns, unsigned capacity) {
    struct scsi_disk probe; memset(&probe, 0, sizeof(probe));
    probe.transport = t; probe.context = context; strcpy(probe.kind, "LUNS");
    uint8_t data[8 + 8 * 16] __attribute__((aligned(16))); uint32_t got = 0;
    uint8_t cdb[12] = {0xA0, 0, 0}; put32(cdb + 6, sizeof(data));
    memset(data, 0, sizeof(data));
    if (simple(&probe, cdb, 12, data, sizeof(data), true, &got, QUIET) || got < 8) return 0;
    unsigned listed = be32(data) / 8, count = 0;
    for (unsigned i = 0; i < listed && 8 + 8 * (i + 1) <= got && count < capacity; i++) {
        const uint8_t* e = data + 8 + 8 * i; unsigned method = e[0] >> 6, lun = (e[0] & 0x3F) << 8 | e[1];
        if ((method == 0 || method == 1) && lun < 256) luns[count++] = (uint8_t)lun;
    }
    return (int)count;
}
static int discard(struct block_device* b, uint64_t lba, uint64_t count) {
    struct scsi_disk* d = b->driver;
    if (!d->lbpme || b->read_only) return -95;
    while (count) {
        uint32_t n = count > 0x400000 ? 0x400000 : (uint32_t)count;
        uint8_t parameters[24] __attribute__((aligned(8))); memset(parameters, 0, sizeof(parameters));
        put16(parameters, 22); put16(parameters + 2, 16); put64(parameters + 8, lba); put32(parameters + 16, n);
        uint8_t cdb[10] = {0x42}; put16(cdb + 7, 24);
        int r = simple(d, cdb, 10, parameters, 24, false, NULL, 0);
        if (r) return r;
        lba += n; count -= n;
    }
    return 0;
}
static int eject(struct block_device* b) {
    struct scsi_disk* d = b->driver; int r = scsi_disk_eject(d);
    if (!r && d->ejected && d->transport->changed) d->transport->changed(d->context);
    return r;
}
static int sd_read(struct block_device* b, uint64_t lba, uint32_t count, void* buffer) { return scsi_disk_io(b->driver, lba, count, buffer, false) ? -1 : 0; }
static int sd_write(struct block_device* b, uint64_t lba, uint32_t count, const void* buffer) { return scsi_disk_io(b->driver, lba, count, (void*)buffer, true) ? -1 : 0; }
static int sd_flush(struct block_device* b) { return scsi_disk_flush(b->driver) ? -1 : 0; }
const struct block_ops scsi_disk_ops = {sd_read, sd_write, sd_flush};
static void release(struct block_device* b) {
    struct scsi_disk* d = b->driver;
    const struct scsi_transport* t = d->transport; void* context = d->context;
    arena_free((uint64_t)d, sizeof(*d));
    t->release(context);   /* the disk's reference on its transport */
}
static const struct block_extra_ops scsi_extra = {.eject = eject, .discard = discard, .release = release};
