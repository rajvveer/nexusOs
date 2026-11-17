/* USB mass storage: Bulk-Only Transport (BOT, protocol 0x50) and USB Attached SCSI (UAS, protocol 0x62) as SCSI
 * transports (scsi.c) on the native xHCI stack.
 *
 * Binding (worker): the full configuration descriptor is parsed (every alternate setting: UAS is usually
 * alternate 1 behind a BOT alternate 0), UAS is preferred when its four pipes are described by Pipe Usage
 * descriptors and either the device runs below SuperSpeed (no streams; READ/WRITE READY IUs on the status
 * pipe) or the controller and device support streams (status/data pipes use stream = tag). The chosen pipes
 * are configured, SET_CONFIGURATION/SET_INTERFACE sent, LUNs found (GET MAX LUN / REPORT LUNS) and every
 * direct-access LUN becomes a block device sdX (blockdev.c block_hot_add: partitions, /dev, uevents).
 *
 * I/O runs in whatever context calls the block layer (kernel lock held, any CPU): transfers are queued and
 * waited for while usb_drive() processes events and runs the worker (so hot-plug proceeds meanwhile). Stalls,
 * timeouts and protocol errors are recovered by the worker (xHC endpoint reset + Set TR Dequeue, CLEAR_FEATURE
 * (ENDPOINT_HALT), Bulk-Only Mass Storage Reset): the I/O path requests it and drives the worker until done.
 * Surprise removal: the worker detaches the device (port change), waiters return ENODEV, the block devices are
 * hot-removed; memory lives until the last block device reference is gone. Removable-media LUNs are polled
 * (TEST UNIT READY every 2 s) for medium insertion/removal. */
#include "usb.h"
#include "scsi.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define MAX_STORAGE 8
#define MAX_LUNS 8
#define DMA_BYTES (64U * 1024U)
#define POLL_NS 2000000000ULL
enum { PROTOCOL_BOT = 0x50, PROTOCOL_UAS = 0x62 };
enum { PIPE_COMMAND = 1, PIPE_STATUS = 2, PIPE_DATA_IN = 3, PIPE_DATA_OUT = 4 };
enum { ROLE_IN, ROLE_OUT, ROLE_COMMAND, ROLE_STATUS, ROLES };
#define RECOVER_IN 1u
#define RECOVER_OUT 2u
#define RECOVER_COMMAND 4u
#define RECOVER_STATUS 8u
#define RECOVER_PIPES 15u
#define RECOVER_BOT_RESET 16u

struct ustor {
    struct usb_device* device;          /* NULL once detached */
    unsigned interface, alternate, protocol, max_lun, references;
    struct usb_bulk *in, *out, *command, *status;   /* BOT uses in/out */
    bool streams, gone, busy;
    uint8_t *dma, *small;                /* bounce buffer and the CBW/CSW/IU page (both DMA32) */
    uint64_t dma_limit, poll_due;
    uint32_t tag;
    volatile unsigned recover; volatile bool recovering; int recover_result;
    struct usb_bulk_transfer xfer[ROLES];
    struct scsi_disk* luns[MAX_LUNS];    /* registered or waiting for a medium */
    bool registered[MAX_LUNS];
    bool pending[MAX_LUNS];              /* probed, registration deferred (blockio.c block_io_defer) */
    unsigned lun_count;
    char serial[21];
};
static struct ustor* storages[MAX_STORAGE];
static unsigned pending;
bool boot_option(const char* word);
unsigned usbstorage_pending(void) { return pending; }

static uint32_t le32(const uint8_t* p) { return get_le32(p); }
static void log_storage(const struct ustor* st, const char* text) {
    if (st->device) { usb_log_device(st->device, text); return; }
    vga_print("[64] USB storage: "); vga_print(text); vga_print("\n");
}

/* ---- lifetime ---- */
static void ustor_free(struct ustor* st) {
    for (unsigned i = 0; i < MAX_STORAGE; i++) if (storages[i] == st) storages[i] = NULL;
    if (st->dma) arena_free((uint64_t)st->dma, DMA_BYTES);
    if (st->small) arena_free((uint64_t)st->small, 4096);
    arena_free((uint64_t)st, sizeof(*st));
}
static void st_retain(void* context) { ((struct ustor*)context)->references++; }
static void st_release(void* context) {
    struct ustor* st = context;
    if (!st->references || --st->references) return;
    ustor_free(st);
}
static bool st_gone(void* context) { return ((struct ustor*)context)->gone; }
static void spin_drive(uint64_t until) {
    while (native_clock_ns() < until) { usb_drive(); for (unsigned i = 0; i < 64; i++) __asm__ volatile("pause"); }
}
static void st_sleep(void* context, uint64_t ns) {
    (void)context;
    if (usb_in_worker()) usb_sleep(ns); else spin_drive(native_clock_ns() + ns);
}

/* ---- transfers ---- */
static int wait_transfer(struct ustor* st, struct usb_bulk_transfer* t, uint64_t deadline) {
    while (!t->done) {
        if (st->gone) return -19;
        if (native_clock_ns() >= deadline) return -110;
        if (usb_in_worker()) usb_sleep(0);
        else { usb_drive(); for (unsigned i = 0; i < 16; i++) __asm__ volatile("pause"); }
    }
    switch (t->code) {
    case 1: case 13: return (int)t->actual;     /* success, short packet */
    case 6: return -32;                         /* stall */
    case 26: case 27: return -125;              /* stopped (cancelled by a reset) */
    default: return -71;                        /* babble, transaction error, ... */
    }
}
static int submit(struct ustor* st, struct usb_bulk* pipe, unsigned stream, struct usb_bulk_transfer* t, void* buffer, uint32_t length) {
    if (st->gone) return -19;
    return xhci_bulk_submit(pipe, stream, virt_to_phys(buffer), length, t);
}
static int bulk(struct ustor* st, struct usb_bulk* pipe, unsigned stream, struct usb_bulk_transfer* t, void* buffer, uint32_t length, uint64_t deadline) {
    int r = submit(st, pipe, stream, t, buffer, length);
    return r ? r : wait_transfer(st, t, deadline);
}

/* ---- recovery (commands and control transfers: worker only) ---- */
static int do_recover(struct ustor* st, unsigned what) {
    struct usb_device* device = st->device;
    if (!device || st->gone) return -19;
    int result = 0;
    if (what & RECOVER_BOT_RESET) {
        int r = usb_control(device, 0x21, 0xFF, 0, (uint16_t)st->interface, NULL, 0);   /* Bulk-Only Mass Storage Reset */
        if (r < 0 && r != -32) result = r;
        what |= RECOVER_IN | RECOVER_OUT;
        log_storage(st, "Bulk-Only reset recovery");
    }
    struct usb_bulk* pipes[4] = {st->in, st->out, st->command, st->status};
    for (unsigned i = 0; i < 4; i++) {
        if (!(what & (1U << i)) || !pipes[i] || st->gone) continue;
        if (!xhci_bulk_reset(pipes[i])) { if (!result) result = -5; continue; }
        int r = usb_control(device, 0x02, 1, 0, pipes[i]->address, NULL, 0);           /* CLEAR_FEATURE(ENDPOINT_HALT) */
        if (r < 0 && !result) result = r;
    }
    return st->gone ? -19 : result;
}
static int recover(struct ustor* st, unsigned what) {
    if (st->gone) return -19;
    if (usb_in_worker()) return do_recover(st, what);
    st->recover |= what; st->recovering = true; usb_worker_kick();
    uint64_t deadline = native_clock_ns() + 20000000000ULL;
    while (st->recovering && !st->gone) {
        if (native_clock_ns() >= deadline) { log_storage(st, "endpoint recovery timed out"); return -110; }
        usb_drive(); for (unsigned i = 0; i < 64; i++) __asm__ volatile("pause");
    }
    return st->gone ? -19 : st->recover_result;
}
static int lock(struct ustor* st) {
    while (st->busy) {
        if (st->gone) return -19;
        if (usb_in_worker()) usb_sleep(0); else { usb_drive(); __asm__ volatile("pause"); }
    }
    if (st->gone) return -19;
    st->busy = true; return 0;
}
static void unlock(struct ustor* st) { st->busy = false; }

/* ---- Bulk-Only Transport ---- */
static int bot_command(struct ustor* st, unsigned lun, struct scsi_command* c, bool sense_request) {
    uint64_t deadline = native_clock_ns() + c->timeout_ns;
    uint8_t *cbw = st->small, *csw = st->small + 64;
    uint32_t tag = ++st->tag;
    memset(cbw, 0, 32);
    put_le32(cbw, 0x43425355); put_le32(cbw + 4, tag); put_le32(cbw + 8, c->length);
    cbw[12] = c->length && c->in ? 0x80 : 0; cbw[13] = (uint8_t)(lun & 15); cbw[14] = (uint8_t)c->cdb_length;
    memcpy(cbw + 15, c->cdb, c->cdb_length);
    int r = bulk(st, st->out, 0, &st->xfer[ROLE_OUT], cbw, 31, deadline);
    if (r != 31) { if (r == -19) return r; recover(st, RECOVER_BOT_RESET); return r < 0 ? r : -71; }
    uint32_t actual = 0;
    if (c->length) {
        struct usb_bulk* pipe = c->in ? st->in : st->out; unsigned role = c->in ? ROLE_IN : ROLE_OUT;
        bool direct = block_dma_direct(c->data, c->length, st->dma_limit);
        while (actual < c->length) {
            uint32_t n = c->length - actual; if (n > DMA_BYTES) n = DMA_BYTES;
            uint8_t* target = direct ? (uint8_t*)c->data + actual : st->dma;
            if (!direct && !c->in) memcpy(st->dma, (uint8_t*)c->data + actual, n);
            r = bulk(st, pipe, 0, &st->xfer[role], target, n, deadline);
            if (r < 0) break;
            if (!direct && c->in) memcpy((uint8_t*)c->data + actual, st->dma, (uint32_t)r);
            actual += (uint32_t)r;
            if ((uint32_t)r < n) break;            /* short packet: the device ended the data stage */
        }
        if (r == -32) {                            /* stalled data stage: clear it, the CSW tells the outcome */
            int e = recover(st, c->in ? RECOVER_IN : RECOVER_OUT); if (e == -19) return e;
        } else if (r < 0) { if (r == -19) return r; recover(st, RECOVER_BOT_RESET); return r; }
    }
    r = bulk(st, st->in, 0, &st->xfer[ROLE_IN], csw, 13, deadline);
    if (r == -32) { int e = recover(st, RECOVER_IN); if (e == -19) return e; r = bulk(st, st->in, 0, &st->xfer[ROLE_IN], csw, 13, deadline); }
    if (r < 0) { if (r == -19) return r; recover(st, RECOVER_BOT_RESET); return r; }
    if (r != 13 || le32(csw) != 0x53425355 || le32(csw + 4) != tag || csw[12] > 2) {
        log_storage(st, "invalid command status wrapper"); recover(st, RECOVER_BOT_RESET); return -71;
    }
    if (csw[12] == 2) { log_storage(st, "phase error"); recover(st, RECOVER_BOT_RESET); return -71; }
    uint32_t residue = le32(csw + 8);
    if (residue <= c->length && c->length - residue < actual) actual = c->length - residue;
    c->transferred = actual;
    if (csw[12] == 0) return SCSI_GOOD;
    if (sense_request) return -71;                 /* REQUEST SENSE itself failed */
    struct scsi_command sense; memset(&sense, 0, sizeof(sense));
    sense.cdb[0] = 0x03; sense.cdb[4] = 18; sense.cdb_length = 6; sense.data = st->small + 512; sense.length = 18; sense.in = true;
    sense.timeout_ns = 10000000000ULL;
    memset(st->small + 512, 0, 18);
    r = bot_command(st, lun, &sense, true);
    if (r == -19) return r;
    c->sense_length = r == SCSI_GOOD ? sense.transferred : 0;
    if (c->sense_length > SCSI_SENSE_MAX) c->sense_length = SCSI_SENSE_MAX;
    memcpy(c->sense, st->small + 512, c->sense_length);
    return SCSI_CHECK;
}
static int bot_execute(void* context, unsigned lun, struct scsi_command* c) {
    struct ustor* st = context;
    int r = lock(st); if (r) return r;
    r = bot_command(st, lun, c, false);
    unlock(st); return r;
}
static int bot_reset(void* context, unsigned lun) {
    (void)lun; struct ustor* st = context;
    int r = lock(st); if (r) return r;
    r = recover(st, RECOVER_BOT_RESET);
    unlock(st); return r;
}

/* ---- USB Attached SCSI ---- */
static int uas_status(struct ustor* st, const uint8_t* iu, int length, uint16_t tag, struct scsi_command* c) {
    if (length >= 4 && ((uint16_t)(iu[2] << 8 | iu[3])) != tag) { log_storage(st, "UAS status for another tag"); return -71; }
    if (iu[0] == 0x03 && length >= 16) {                       /* SENSE IU */
        unsigned status = iu[6], size = (unsigned)iu[14] << 8 | iu[15];
        if (size > (unsigned)length - 16) size = (unsigned)length - 16;
        if (size > SCSI_SENSE_MAX) size = SCSI_SENSE_MAX;
        memcpy(c->sense, iu + 16, size); c->sense_length = size;
        return status == 0 ? SCSI_GOOD : status == 2 ? SCSI_CHECK : status == 8 || status == 0x28 ? SCSI_BUSY : -71;
    }
    if (iu[0] == 0x04 && length >= 8) { log_storage(st, "UAS response IU (task management failure)"); return -71; }
    log_storage(st, "UAS unexpected information unit");
    return -71;
}
static int uas_command(struct ustor* st, unsigned lun, struct scsi_command* c) {
    uint64_t deadline = native_clock_ns() + c->timeout_ns;
    const uint16_t tag = 1;
    uint8_t *iu = st->small, *status = st->small + 256;
    if (c->length > DMA_BYTES || c->cdb_length > 16) return -22;
    bool data = c->length != 0;
    struct usb_bulk* pipe = c->in ? st->in : st->out; unsigned role = c->in ? ROLE_IN : ROLE_OUT;
    bool direct = data && block_dma_direct(c->data, c->length, st->dma_limit);
    uint8_t* buffer = direct ? c->data : st->dma;
    if (data && !direct && !c->in) memcpy(st->dma, c->data, c->length);
    memset(iu, 0, 32); iu[0] = 0x01; iu[2] = (uint8_t)(tag >> 8); iu[3] = (uint8_t)tag; iu[9] = (uint8_t)lun;
    memcpy(iu + 16, c->cdb, c->cdb_length);
    memset(status, 0, 256);
    unsigned stream = st->streams ? tag : 0;
    int r, length = 0; uint32_t actual = 0;
    if (st->streams) {
        if ((r = submit(st, st->status, stream, &st->xfer[ROLE_STATUS], status, 256))) goto failed;
        if (data && (r = submit(st, pipe, stream, &st->xfer[role], buffer, c->length))) goto failed;
        r = bulk(st, st->command, 0, &st->xfer[ROLE_COMMAND], iu, 32, deadline);
        if (r != 32) { if (r >= 0) r = -71; goto failed; }
        if ((length = wait_transfer(st, &st->xfer[ROLE_STATUS], deadline)) < 0) { r = length; goto failed; }
        int verdict = uas_status(st, status, length, tag, c);
        if (data) {
            if (verdict == SCSI_GOOD || st->xfer[role].done) {
                r = wait_transfer(st, &st->xfer[role], deadline);
                if (r < 0) goto failed;
                actual = (uint32_t)r;
            } else { int e = recover(st, c->in ? RECOVER_IN : RECOVER_OUT); if (e == -19) return e; }   /* cancel the data TD */
        }
        if (data && !direct && c->in) memcpy(c->data, st->dma, actual);
        c->transferred = actual;
        if (verdict < 0) recover(st, RECOVER_PIPES);
        return verdict;
    }
    if ((r = submit(st, st->status, 0, &st->xfer[ROLE_STATUS], status, 256))) goto failed;
    r = bulk(st, st->command, 0, &st->xfer[ROLE_COMMAND], iu, 32, deadline);
    if (r != 32) { if (r >= 0) r = -71; goto failed; }
    for (unsigned rounds = 0; ; rounds++) {
        if ((length = wait_transfer(st, &st->xfer[ROLE_STATUS], deadline)) < 0) { r = length; goto failed; }
        if (length >= 4 && (status[0] == 0x06 || status[0] == 0x07) && rounds < 4) {   /* READ READY / WRITE READY */
            if (!data || (status[0] == 0x06) != c->in) { log_storage(st, "UAS ready IU without a matching data stage"); r = -71; goto failed; }
            r = bulk(st, pipe, 0, &st->xfer[role], buffer, c->length, deadline);
            if (r < 0) goto failed;
            actual = (uint32_t)r;
            memset(status, 0, 256);
            if ((r = submit(st, st->status, 0, &st->xfer[ROLE_STATUS], status, 256))) goto failed;
            continue;
        }
        break;
    }
    int verdict = uas_status(st, status, length, tag, c);
    if (data && !direct && c->in) memcpy(c->data, st->dma, actual);
    c->transferred = actual;
    if (verdict < 0) recover(st, RECOVER_PIPES);
    return verdict;
failed:
    if (r == -19 || st->gone) return -19;
    recover(st, RECOVER_PIPES);
    return r < 0 ? r : -71;
}
static int uas_execute(void* context, unsigned lun, struct scsi_command* c) {
    struct ustor* st = context;
    int r = lock(st); if (r) return r;
    r = uas_command(st, lun, c);
    unlock(st); return r;
}
static int uas_reset(void* context, unsigned lun) {
    (void)lun; struct ustor* st = context;
    int r = lock(st); if (r) return r;
    r = recover(st, RECOVER_PIPES);
    unlock(st); return r;
}
static void st_changed(void* context) { (void)context; usb_worker_kick(); }   /* an ejected unit: usbstorage_service removes it */
static const struct scsi_transport bot_transport = {bot_execute, bot_reset, st_gone, st_sleep, st_retain, st_release, DMA_BYTES, st_changed};
static const struct scsi_transport uas_transport = {uas_execute, uas_reset, st_gone, st_sleep, st_retain, st_release, DMA_BYTES, st_changed};

/* ---- binding ---- */
bool usbstorage_match(const struct usb_device* device) {
    for (unsigned i = 0; i < device->interface_count; i++) if (device->interfaces[i].class_code == 8) return true;
    return false;
}
struct endpoint_desc { uint8_t address, attributes, burst, streams, pipe; uint16_t max_packet; };
struct candidate { uint8_t number, alternate, subclass, protocol, count; struct endpoint_desc eps[8]; };
static unsigned parse(const uint8_t* data, unsigned length, struct candidate* out, unsigned capacity) {
    unsigned count = 0; struct candidate* current = NULL; struct endpoint_desc* last = NULL;
    for (unsigned at = 0; at + 2 <= length;) {
        unsigned size = data[at], type = data[at + 1];
        if (size < 2 || at + size > length) break;
        const uint8_t* d = data + at;
        if (type == 4 && size >= 9) {
            current = NULL; last = NULL;
            if (d[5] == 8 && count < capacity) {
                current = &out[count++]; memset(current, 0, sizeof(*current));
                current->number = d[2]; current->alternate = d[3]; current->subclass = d[6]; current->protocol = d[7];
            }
        } else if (type == 5 && size >= 7 && current) {
            last = NULL;
            if (current->count < 8 && (d[3] & 3) == 2) {
                last = &current->eps[current->count++]; memset(last, 0, sizeof(*last));
                last->address = d[2]; last->attributes = d[3]; last->max_packet = (uint16_t)(d[4] | d[5] << 8);
            }
        } else if (type == 0x30 && size >= 4 && last) { last->burst = d[2]; last->streams = d[3] & 31; }
        else if (type == 0x24 && size >= 3 && last) last->pipe = d[2];
        at += size;
    }
    return count;
}
static const struct endpoint_desc* find_ep(const struct candidate* c, bool in, unsigned pipe) {
    for (unsigned i = 0; i < c->count; i++) {
        const struct endpoint_desc* e = &c->eps[i];
        if (((e->address & 0x80) != 0) == in && (!pipe || e->pipe == pipe)) return e;
    }
    return NULL;
}
static struct usb_bulk* setup_pipe(struct usb_device* device, const struct endpoint_desc* e, bool streams) {
    if (device->bulk_count >= USB_MAX_BULK) return NULL;
    struct usb_bulk* b = &device->bulk[device->bulk_count];
    memset(b, 0, sizeof(*b));
    b->device = device; b->address = e->address; b->in = (e->address & 0x80) != 0;
    b->dci = (uint8_t)((e->address & 15) * 2 + (b->in ? 1 : 0));
    b->max_packet = e->max_packet & 0x7FF;
    b->burst = device->speed >= USB_SPEED_SUPER ? (e->burst > 15 ? 15 : e->burst) : 0;
    if (!b->dci || !xhci_bulk_init(b, streams)) return NULL;
    device->bulk_count++;
    return b;
}
static void serial_string(struct usb_device* device, uint8_t* scratch, char* out) {
    out[0] = 0;
    uint8_t descriptor[18];
    if (usb_control(device, 0x80, 6, 0x0100, 0, descriptor, 18) < 18 || !descriptor[16]) return;
    int n = usb_control(device, 0x80, 6, (uint16_t)(0x0300 | descriptor[16]), 0x0409, scratch, 255);
    if (n < 4 || scratch[1] != 3) return;
    unsigned length = 0;
    for (int i = 2; i + 1 < n && i < scratch[0] && length < 20; i += 2) {
        unsigned unit = (unsigned)scratch[i] | (unsigned)scratch[i + 1] << 8;
        if (unit > 32 && unit < 127) out[length++] = (char)unit;
    }
    out[length] = 0;
}
static bool register_lun(struct ustor* st, unsigned slot) {
    struct scsi_disk* d = st->luns[slot];
    strcpy(d->serial, st->serial);
    strcpy(d->block.serial, st->serial);
    if (block_hot_add(&d->block) < 0) { log_storage(st, "block device table full"); return false; }
    st->registered[slot] = true;
    if (d->removable) scsi_disk_prevent(d, false);
    return true;
}
static struct scsi_disk* new_disk(struct ustor* st, unsigned lun) {
    struct scsi_disk* d = (void*)arena_alloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->transport = st->protocol == PROTOCOL_UAS ? &uas_transport : &bot_transport; d->context = st; d->lun = lun;
    strcpy(d->kind, st->protocol == PROTOCOL_UAS ? "UAS" : "USB");
    st->references++;   /* the disk's */
    return d;
}
static void drop_disk(struct ustor* st, struct scsi_disk* d) { arena_free((uint64_t)d, sizeof(*d)); st_release(st); }
static void probe_luns(struct ustor* st) {
    uint8_t list[MAX_LUNS]; unsigned count = 0;
    if (st->protocol == PROTOCOL_UAS) {
        int n = scsi_report_luns(&uas_transport, st, list, MAX_LUNS);
        count = n > 0 ? (unsigned)n : 0;
        if (!count) { list[0] = 0; count = 1; }
    } else for (unsigned i = 0; i <= st->max_lun && i < MAX_LUNS; i++) list[count++] = (uint8_t)i;
    for (unsigned i = 0; i < count && !st->gone; i++) {
        struct scsi_disk* d = new_disk(st, list[i]);
        if (!d) { log_storage(st, "no memory for a logical unit"); break; }
        int r = scsi_disk_probe(d);
        if (r == 0) {
            st->luns[st->lun_count] = d;
            if (block_io_defer()) { st->pending[st->lun_count++] = true; usb_worker_kick(); }   /* blockio.c: registered later */
            else if (register_lun(st, st->lun_count)) st->lun_count++;
            else { st->luns[st->lun_count] = NULL; drop_disk(st, d); }
        } else if (r == -123 && d->removable) {                    /* card reader without a card: polled */
            st->luns[st->lun_count++] = d;
            log_storage(st, "logical unit without medium (polled)");
        } else {
            if (r != -6 && r != -19) log_storage(st, r == -95 ? "logical unit is not a direct-access device" : "logical unit probe failed");
            drop_disk(st, d);
        }
    }
}
bool usbstorage_attach(struct usb_device* device) {
    unsigned index = 0; while (index < MAX_STORAGE && storages[index]) index++;
    if (index == MAX_STORAGE) { usb_log_device(device, "too many mass-storage devices"); return false; }
    struct ustor* st = (void*)arena_alloc(sizeof(*st));
    if (!st) return false;
    memset(st, 0, sizeof(*st));
    st->dma = (void*)dma32_alloc(DMA_BYTES); st->small = (void*)dma32_alloc(4096);
    if (!st->dma || !st->small) { ustor_free(st); usb_log_device(device, "no DMA memory for mass storage"); return false; }
    st->references = 1; st->device = device; st->dma_limit = xhci_dma_limit(device->hc);
    /* The whole configuration (all alternate settings): usb.c kept only alternate 0. */
    uint8_t header[9];
    int n = usb_control(device, 0x80, 6, 0x0200, 0, header, 9);
    unsigned total = n >= 9 ? (unsigned)(header[2] | header[3] << 8) : 0;
    if (total > 4096) total = 4096;
    if (total < 9 || (n = usb_control(device, 0x80, 6, 0x0200, 0, st->small, (uint16_t)total)) < 9) { ustor_free(st); usb_log_device(device, "configuration descriptor unreadable"); return false; }
    struct candidate candidates[6];
    unsigned count = parse(st->small, (unsigned)n, candidates, 6);
    const struct candidate *bot = NULL, *uas = NULL;
    for (unsigned i = 0; i < count; i++) {
        const struct candidate* c = &candidates[i];
        if (c->protocol == PROTOCOL_UAS && find_ep(c, false, PIPE_COMMAND) && find_ep(c, true, PIPE_STATUS) && find_ep(c, true, PIPE_DATA_IN) &&
            find_ep(c, false, PIPE_DATA_OUT) && !uas) uas = c;
        if (c->protocol == PROTOCOL_BOT && (c->subclass == 6 || c->subclass == 2 || c->subclass == 5) && find_ep(c, true, 0) && find_ep(c, false, 0) && !bot) bot = c;
    }
    bool super = device->speed >= USB_SPEED_SUPER;
    if (uas && super) {   /* SuperSpeed UAS needs streams on the controller and at least 4 per pipe on the device */
        const struct endpoint_desc* e[3] = {find_ep(uas, true, PIPE_STATUS), find_ep(uas, true, PIPE_DATA_IN), find_ep(uas, false, PIPE_DATA_OUT)};
        if (!xhci_streams_supported(device->hc) || e[0]->streams < 2 || e[1]->streams < 2 || e[2]->streams < 2) uas = NULL;
    }
    if (boot_option("usb-storage.no_uas")) uas = NULL;
    const struct candidate* chosen = uas ? uas : bot;
    if (!chosen) { ustor_free(st); usb_log_device(device, "mass storage without a supported transport (BOT/UAS, SCSI)"); return false; }
    st->protocol = chosen->protocol; st->interface = chosen->number; st->alternate = chosen->alternate;
    st->streams = uas && super;
    device->bulk_count = 0;
    bool ok;
    if (uas) {
        st->command = setup_pipe(device, find_ep(uas, false, PIPE_COMMAND), false);
        st->status = setup_pipe(device, find_ep(uas, true, PIPE_STATUS), st->streams);
        st->in = setup_pipe(device, find_ep(uas, true, PIPE_DATA_IN), st->streams);
        st->out = setup_pipe(device, find_ep(uas, false, PIPE_DATA_OUT), st->streams);
        ok = st->command && st->status && st->in && st->out;
    } else {
        st->in = setup_pipe(device, find_ep(bot, true, 0), false);
        st->out = setup_pipe(device, find_ep(bot, false, 0), false);
        ok = st->in && st->out;
    }
    device->class_state[0] = st; storages[index] = st;
    if (!ok || !xhci_bulk_configure(device)) { log_storage(st, "Configure Endpoint failed (bulk pipes)"); goto failed; }
    if (usb_control(device, 0x00, 9, device->configuration, 0, NULL, 0) < 0) { log_storage(st, "SET_CONFIGURATION failed"); goto failed; }
    if (st->alternate && usb_control(device, 0x01, 11, st->alternate, (uint16_t)st->interface, NULL, 0) < 0) { log_storage(st, "SET_INTERFACE failed"); goto failed; }
    if (st->protocol == PROTOCOL_BOT) {
        uint8_t max = 0;
        int r = usb_control(device, 0xA1, 0xFE, 0, (uint16_t)st->interface, &max, 1);
        st->max_lun = r == 1 && max < MAX_LUNS ? max : 0;
    }
    serial_string(device, st->small, st->serial);
    log_storage(st, st->protocol == PROTOCOL_UAS ? (st->streams ? "mass storage UAS (streams)" : "mass storage UAS") : "mass storage Bulk-Only");
    pending++;
    probe_luns(st);
    pending--;
    st->poll_due = native_clock_ns() + POLL_NS;
    return true;
failed:
    device->class_state[0] = NULL; storages[index] = NULL;
    st->gone = true; st->device = NULL;
    st_release(st);
    return false;
}
void usbstorage_detach(struct usb_device* device) {
    for (unsigned b = 0; b < device->bulk_count && b < USB_MAX_BULK; b++)
        for (unsigned s = 0; s < USB_BULK_STREAMS; s++) {
            struct usb_bulk_ring* ring = &device->bulk[b].rings[s];
            if (ring->active) { ring->active->code = 26; ring->active->done = true; ring->active = NULL; }
        }
    struct ustor* st = device->class_state[0];
    if (!st) return;
    st->gone = true; st->device = NULL; device->class_state[0] = NULL;
    for (unsigned i = 0; i < MAX_STORAGE; i++) if (storages[i] == st) storages[i] = NULL;
    for (unsigned i = 0; i < st->lun_count; i++) {
        struct scsi_disk* d = st->luns[i]; bool registered = st->registered[i];
        st->luns[i] = NULL; st->registered[i] = false;
        if (!d) continue;
        if (registered) block_hot_remove(&d->block); else drop_disk(st, d);
    }
    st_release(st);   /* the binding's reference; DMA buffers go with the last block device */
}
/* Worker: requested endpoint recovery, then medium polling of removable logical units. */
bool usbstorage_service(void) {
    bool work = false;
    for (unsigned i = 0; i < MAX_STORAGE; i++) {
        struct ustor* st = storages[i];
        if (!st || !st->recovering) continue;
        unsigned what = st->recover; st->recover = 0;
        st->recover_result = do_recover(st, what);
        st->recovering = false; work = true;
    }
    for (unsigned i = 0; i < MAX_STORAGE; i++) {   /* ejected fixed-medium units leave (power-off) */
        struct ustor* st = storages[i];
        if (!st || st->gone || st->busy) continue;
        for (unsigned l = 0; l < st->lun_count; l++) {
            struct scsi_disk* d = st->luns[l];
            if (!d || !d->ejected || !st->registered[l]) continue;
            log_storage(st, "stopped by eject: removed until it is plugged in again");
            st->luns[l] = NULL; st->registered[l] = false;
            block_hot_remove(&d->block);
            work = true;
        }
    }
    for (unsigned i = 0; i < MAX_STORAGE; i++) {   /* units probed while a disk wait owned the block path (blockio.c) */
        struct ustor* st = storages[i];
        if (!st || st->gone || st->busy) continue;
        for (unsigned l = 0; l < st->lun_count; l++) {
            if (!st->pending[l] || !st->luns[l] || st->registered[l]) continue;
            if (block_io_defer()) { usb_worker_kick(); continue; }
            st->pending[l] = false; work = true;
            if (!register_lun(st, l)) { struct scsi_disk* d = st->luns[l]; st->luns[l] = NULL; drop_disk(st, d); }
        }
    }
    uint64_t now = native_clock_ns();
    for (unsigned i = 0; i < MAX_STORAGE; i++) {
        struct ustor* st = storages[i];
        if (!st || st->gone || st->busy || now < st->poll_due) continue;
        st->poll_due = now + POLL_NS;
        for (unsigned l = 0; l < st->lun_count && !st->gone; l++) {
            struct scsi_disk* d = st->luns[l];
            if (!d || !d->removable) continue;
            st_retain(st);
            int r = scsi_disk_ready(d);
            if (st->gone) { st_release(st); break; }
            if (st->registered[l] && r == -123) {            /* medium removed */
                log_storage(st, "medium removed");
                unsigned lun = d->lun;
                st->luns[l] = NULL; st->registered[l] = false;
                block_hot_remove(&d->block);                   /* d may be released here */
                struct scsi_disk* fresh = new_disk(st, lun);   /* a fresh unit waits for the next medium */
                if (fresh) { fresh->removable = true; st->luns[l] = fresh; }
                work = true;
            } else if (!st->registered[l] && r == 0 && !block_io_defer()) {   /* medium inserted (blockio.c: not while a disk wait owns the path) */
                if (!scsi_disk_probe(d) && register_lun(st, l)) log_storage(st, "medium inserted");
                work = true;
            }
            st_release(st);
        }
    }
    return work;
}
/* usb_poll: wake the worker for medium polling. */
void usbstorage_tick(uint64_t now) {
    for (unsigned i = 0; i < MAX_STORAGE; i++) {
        struct ustor* st = storages[i];
        if (!st || st->gone || now < st->poll_due) continue;
        for (unsigned l = 0; l < st->lun_count; l++) if (st->luns[l] && st->luns[l]->removable) { usb_worker_kick(); return; }
    }
}
uint64_t usbstorage_next_event(void) {   /* tick.c via usb.c: the next media-change poll of a removable LUN (~0: none) */
    uint64_t due = ~0ULL;
    for (unsigned i = 0; i < MAX_STORAGE; i++) {
        const struct ustor* st = storages[i];
        if (!st || st->gone || st->poll_due >= due) continue;
        for (unsigned l = 0; l < st->lun_count; l++) if (st->luns[l] && st->luns[l]->removable) { due = st->poll_due; break; }
    }
    return due;
}
