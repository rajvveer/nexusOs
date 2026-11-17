/* Loop devices: /dev/loop-control and /dev/loopN (block 7:N), a file shown as a disk (Linux drivers/block/loop.c).
 *
 * /dev/loop-control: LOOP_CTL_ADD (create loopN), LOOP_CTL_REMOVE (delete an unbound, unopened loopN),
 * LOOP_CTL_GET_FREE (the lowest unbound loop, created when none is free). Devices exist from then on, size 0
 * until bound, like Linux's.
 * /dev/loopN ioctls: LOOP_SET_FD, LOOP_CONFIGURE (fd, block size, offset, size limit, flags, name in one call),
 * LOOP_CLR_FD (deferred to the last close while the device is in use: autoclear, as Linux), LOOP_SET_STATUS64 /
 * LOOP_GET_STATUS64 (and the old LOOP_SET_STATUS / LOOP_GET_STATUS), LOOP_CHANGE_FD (read-only devices),
 * LOOP_SET_CAPACITY, LOOP_SET_DIRECT_IO, LOOP_SET_BLOCK_SIZE. Flags: READ_ONLY (also when the file is not open
 * for writing), AUTOCLEAR (unbind at the last close or unmount), PARTSCAN (partition table read: loopNpM), DIRECT_IO.
 * Data goes through the backing file's page cache (shmem.c memory_file) at offset + sector * block size; reads past
 * the end of the file return zeros, the device never grows the file. Flush writes the file's dirty pages and flushes
 * the disks. sysfs: /sys/block/loopN/loop/{backing_file,offset,sizelimit,autoclear,partscan,dio}. Each bind or
 * unbind is a "change" uevent. All of it runs under the kernel lock. */
#include "native.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define LOOP_MAX 256
#define LO_FLAGS_READ_ONLY 1u
#define LO_FLAGS_AUTOCLEAR 4u
#define LO_FLAGS_PARTSCAN 8u
#define LO_FLAGS_DIRECT_IO 16u
#define LO_NAME_SIZE 64
#ifndef PAGE
#define PAGE 4096ULL
#endif
struct loop_info64 {
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t lo_file_name[LO_NAME_SIZE], lo_crypt_name[LO_NAME_SIZE], lo_encrypt_key[32];
    uint64_t lo_init[2];
};
struct loop_info {   /* the old interface (x86-64: __kernel_old_dev_t and unsigned long are 8 bytes) */
    int32_t lo_number; uint64_t lo_device; uint64_t lo_inode; uint64_t lo_rdevice; int32_t lo_offset;
    int32_t lo_encrypt_type, lo_encrypt_key_size, lo_flags; char lo_name[LO_NAME_SIZE]; uint8_t lo_encrypt_key[32];
    uint64_t lo_init[2]; char reserved[4];
};
struct loop_config { uint32_t fd, block_size; struct loop_info64 info; uint64_t reserved[8]; };
_Static_assert(sizeof(struct loop_info64) == 232, "loop_info64 ABI");
_Static_assert(sizeof(struct loop_config) == 304, "loop_config ABI");
_Static_assert(sizeof(struct loop_info) == 168, "loop_info ABI");

struct open_file;
struct open_file* kernel_file(uint64_t fd);
void fd_reference(struct open_file* f);
void fd_release(struct open_file* f);
int fd_description_stat(struct open_file* description, struct linux_stat* stat);
struct memory_file* kernel_file_cache(struct open_file* f, uint64_t* flags, const char** path);
uint64_t disk_file_max_size(const fat32_file_t* file);

struct loop {
    struct block_device block;        /* first: dev->driver == the loop */
    unsigned number;
    struct open_file* backing;        /* NULL: unbound */
    struct memory_file* cache;
    uint64_t offset, sizelimit, backing_dev, backing_ino;
    uint32_t flags;
    uint8_t file_name[LO_NAME_SIZE], crypt_name[LO_NAME_SIZE];
    char path[256];                   /* /sys/.../loop/backing_file */
    bool gone;
};
static struct loop* loops[LOOP_MAX];
static const struct block_extra_ops loop_extra;

/* ---- data: the page cache, from kernel buffers ---- */
static int64_t cache_io(struct memory_file* file, uint8_t* data, uint64_t count, uint64_t offset, bool write) {
    if (write) {
        if (file->seals & 24) return -1;
        if (file->disk_backed && (offset > disk_file_max_size(&file->disk) || count > disk_file_max_size(&file->disk) - offset)) return -27;
    } else {
        if (offset >= file->size) return 0;
        if (count > file->size - offset) count = file->size - offset;
    }
    uint64_t done = 0;
    while (done < count) {
        uint64_t at = offset + done, part = PAGE - (at & (PAGE - 1)); if (part > count - done) part = count - done;
        uint64_t frame = memory_file_page(file, at / PAGE, write || file->disk_backed);
        if ((write || file->disk_backed) && !frame) return done ? (int64_t)done : file->disk_backed ? -5 : -12;
        if (write) {
            memcpy((uint8_t*)phys_to_virt(frame) + (at & (PAGE - 1)), data + done, part);
            if (at + part > file->size) file->size = at + part;
            memory_file_dirty(file, at / PAGE);
        } else if (frame) memcpy(data + done, (uint8_t*)phys_to_virt(frame) + (at & (PAGE - 1)), part);
        else memset(data + done, 0, part);
        done += part;
    }
    return (int64_t)done;
}
static int loop_rw(struct block_device* b, uint64_t lba, uint32_t count, void* buffer, bool write) {
    struct loop* lo = b->driver;
    if (!lo->backing || !lo->cache) return -1;
    uint64_t bytes = (uint64_t)count * b->block_size, at = lo->offset + lba * b->block_size;
    if (lba >= b->blocks || count > b->blocks - lba) return -1;
    if (write) {
        if (b->read_only) return -1;
        int64_t n = cache_io(lo->cache, buffer, bytes, at, true);
        if (n != (int64_t)bytes) return -1;
        /* no write-back from here: it could re-enter this device through a filesystem on top of it */
        return 0;
    }
    int64_t n = cache_io(lo->cache, buffer, bytes, at, false);
    if (n < 0) return -1;
    if ((uint64_t)n < bytes) memset((uint8_t*)buffer + n, 0, bytes - (uint64_t)n);   /* past the end of the file */
    return 0;
}
static int loop_read(struct block_device* b, uint64_t lba, uint32_t count, void* buffer) { return loop_rw(b, lba, count, buffer, false); }
static int loop_write(struct block_device* b, uint64_t lba, uint32_t count, const void* buffer) { return loop_rw(b, lba, count, (void*)buffer, true); }
static int loop_flush(struct block_device* b) {
    struct loop* lo = b->driver;
    if (!lo->cache) return 0;
    if (memory_file_sync(lo->cache, 0, ~0ULL)) return -1;
    return 0;
}
static const struct block_ops loop_ops = {loop_read, loop_write, loop_flush};

/* ---- devices ---- */
static struct loop* create(unsigned number) {
    if (number >= LOOP_MAX || loops[number]) return NULL;
    struct loop* lo = (void*)arena_alloc(sizeof(*lo)); if (!lo) return NULL;
    memset(lo, 0, sizeof(*lo)); lo->number = number;
    struct block_device* b = &lo->block;
    block_assign_name(b, "loop", number, 7);
    strcpy(b->name, b->linux_name); strcpy(b->model, "Loop device");
    b->kind = "loop"; b->block_size = 512; b->blocks = 0; b->max_blocks = 2048; b->ops = &loop_ops; b->driver = lo;
    b->extra = &loop_extra; b->rotational = false;
    loops[number] = lo;
    if (block_hot_add(b) < 0) { loops[number] = NULL; arena_free((uint64_t)lo, sizeof(*lo)); return NULL; }
    return lo;
}
static struct loop* loop_of(struct block_device* dev) { return dev && dev->ops == &loop_ops ? dev->driver : NULL; }
static uint64_t backing_size(struct loop* lo) { return lo->cache ? lo->cache->size : 0; }
static int resize(struct loop* lo) {
    uint64_t size = backing_size(lo), bytes = size > lo->offset ? size - lo->offset : 0;
    if (lo->sizelimit && lo->sizelimit < bytes) bytes = lo->sizelimit;
    lo->block.blocks = bytes / lo->block.block_size;
    return 0;
}
static void announce(struct loop* lo) {
    struct block_device* b = &lo->block;
    b->generation++;
    if ((lo->flags & LO_FLAGS_PARTSCAN) || !lo->backing) { if (block_rescan(b) == 0) return; }   /* partitions + "change" */
    if (block_event_hook) block_event_hook(b, NULL, "change");
}
static void unbind(struct loop* lo) {
    if (!lo->backing) return;
    if (lo->cache) memory_file_sync(lo->cache, 0, ~0ULL);
    struct open_file* f = lo->backing;
    lo->backing = NULL; lo->cache = NULL; lo->flags = 0; lo->offset = lo->sizelimit = 0; lo->backing_dev = lo->backing_ino = 0;
    memset(lo->file_name, 0, sizeof(lo->file_name)); memset(lo->crypt_name, 0, sizeof(lo->crypt_name)); lo->path[0] = 0;
    lo->block.blocks = 0; lo->block.read_only = false; lo->block.block_size = 512;
    fd_release(f);
    vga_print("[64] LOOP "); vga_print(lo->block.linux_name); vga_print(" detached\n");
    announce(lo);
}
static bool in_use(struct loop* lo) {   /* opened (beyond the caller) or mounted: references beside the driver's and caller's */
    return lo->block.references > 2;
}
static int bind(struct loop* lo, uint32_t fd, uint32_t block_size, const struct loop_info64* info) {
    if (lo->backing) return -16;
    struct open_file* f = kernel_file(fd); if (!f) return -9;
    uint64_t flags = 0; const char* path = NULL;
    struct memory_file* cache = kernel_file_cache(f, &flags, &path);   /* regular and memory files: no loops of block devices */
    if (!cache) return -22;
    if (block_size && (block_size < 512 || block_size > 4096 || (block_size & (block_size - 1)))) return -22;
    if (info && (info->lo_encrypt_type || info->lo_encrypt_key_size)) return -22;   /* cryptoloop is gone from Linux too */
    struct linux_stat st; memset(&st, 0, sizeof(st)); fd_description_stat(f, &st);
    fd_reference(f);
    lo->backing = f; lo->cache = cache; lo->backing_dev = st.dev; lo->backing_ino = st.ino;
    lo->block.block_size = block_size ? block_size : 512;
    lo->offset = info ? info->lo_offset : 0; lo->sizelimit = info ? info->lo_sizelimit : 0;
    lo->flags = info ? info->lo_flags & (LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR | LO_FLAGS_PARTSCAN | LO_FLAGS_DIRECT_IO) : 0;
    if ((flags & 3) == 0) lo->flags |= LO_FLAGS_READ_ONLY;
    lo->block.read_only = (lo->flags & LO_FLAGS_READ_ONLY) != 0;
    if (info) { memcpy(lo->file_name, info->lo_file_name, LO_NAME_SIZE); lo->file_name[LO_NAME_SIZE - 1] = 0; memcpy(lo->crypt_name, info->lo_crypt_name, LO_NAME_SIZE); lo->crypt_name[LO_NAME_SIZE - 1] = 0; }
    if (path) { strncpy(lo->path, path, sizeof(lo->path) - 1); lo->path[sizeof(lo->path) - 1] = 0; }
    if (!lo->file_name[0] && path) { strncpy((char*)lo->file_name, path, LO_NAME_SIZE - 1); }
    resize(lo);
    vga_print("[64] LOOP "); vga_print(lo->block.linux_name); vga_print(" -> "); vga_print(lo->path);
    char n[24] = ""; append_u64(n, lo->block.blocks); vga_print(" blocks="); vga_print(n); vga_print(lo->block.read_only ? " ro\n" : " rw\n");
    announce(lo);
    return 0;
}

/* ---- ioctls on /dev/loopN (blkdev_node.c passes unknown block ioctls here) ---- */
static int copy_in(void* to, uint64_t from, uint64_t size) { if (!accessible(from, size, false)) return -14; return copy_from_user(to, (void*)from, size) ? -14 : 0; }
static int copy_out(uint64_t to, const void* from, uint64_t size) { if (!accessible(to, size, true)) return -14; return copy_to_user((void*)to, from, size) ? -14 : 0; }
static void status64(struct loop* lo, struct loop_info64* info) {
    memset(info, 0, sizeof(*info));
    info->lo_device = lo->backing_dev; info->lo_inode = lo->backing_ino; info->lo_rdevice = lo->backing_dev;
    info->lo_offset = lo->offset; info->lo_sizelimit = lo->sizelimit; info->lo_number = lo->number; info->lo_flags = lo->flags;
    memcpy(info->lo_file_name, lo->file_name, LO_NAME_SIZE); memcpy(info->lo_crypt_name, lo->crypt_name, LO_NAME_SIZE);
}
static int set_status(struct loop* lo, const struct loop_info64* info) {
    if (!lo->backing) return -6;
    if (info->lo_encrypt_type || info->lo_encrypt_key_size) return -22;
    bool geometry = info->lo_offset != lo->offset || info->lo_sizelimit != lo->sizelimit;
    if (geometry && in_use(lo)) return -16;
    if (info->lo_offset != lo->offset && (info->lo_offset & 511)) return -22;
    lo->offset = info->lo_offset; lo->sizelimit = info->lo_sizelimit;
    /* settable: AUTOCLEAR and PARTSCAN (Linux LOOP_SET_STATUS_SETTABLE_FLAGS); PARTSCAN cannot be cleared */
    uint32_t settable = LO_FLAGS_AUTOCLEAR | LO_FLAGS_PARTSCAN;
    bool scan = !(lo->flags & LO_FLAGS_PARTSCAN) && (info->lo_flags & LO_FLAGS_PARTSCAN);
    lo->flags = (lo->flags & ~LO_FLAGS_AUTOCLEAR) | (info->lo_flags & settable) | (lo->flags & LO_FLAGS_PARTSCAN);
    memcpy(lo->file_name, info->lo_file_name, LO_NAME_SIZE); lo->file_name[LO_NAME_SIZE - 1] = 0;
    memcpy(lo->crypt_name, info->lo_crypt_name, LO_NAME_SIZE); lo->crypt_name[LO_NAME_SIZE - 1] = 0;
    if (geometry) resize(lo);
    if (geometry || scan) announce(lo);
    return 0;
}
static int loop_ioctl(struct block_device* dev, uint64_t command, uint64_t argument) {
    struct loop* lo = loop_of(dev); if (!lo) return -25;
    switch (command) {
    case 0x4C00:   /* LOOP_SET_FD */
        if (argument > 0x7FFFFFFF) return -9;
        return bind(lo, (uint32_t)argument, 0, NULL);
    case 0x4C0A: { /* LOOP_CONFIGURE */
        struct loop_config config; int e = copy_in(&config, argument, sizeof(config)); if (e) return e;
        for (unsigned i = 0; i < 8; i++) if (config.reserved[i]) return -22;
        return bind(lo, config.fd, config.block_size, &config.info);
    }
    case 0x4C01:   /* LOOP_CLR_FD */
        if (!lo->backing) return -6;
        if (in_use(lo)) { lo->flags |= LO_FLAGS_AUTOCLEAR; return 0; }   /* unbinds at the last close */
        unbind(lo); return 0;
    case 0x4C05: { /* LOOP_GET_STATUS64 */
        if (!lo->backing) return -6;
        struct loop_info64 info; status64(lo, &info); return copy_out(argument, &info, sizeof(info));
    }
    case 0x4C04: { /* LOOP_SET_STATUS64 */
        struct loop_info64 info; int e = copy_in(&info, argument, sizeof(info)); if (e) return e;
        return set_status(lo, &info);
    }
    case 0x4C03: { /* LOOP_GET_STATUS (old) */
        if (!lo->backing) return -6;
        struct loop_info old; memset(&old, 0, sizeof(old));
        old.lo_number = (int32_t)lo->number; old.lo_device = lo->backing_dev; old.lo_inode = lo->backing_ino;
        old.lo_rdevice = lo->backing_dev; old.lo_offset = lo->offset > 0x7FFFFFFF ? -1 : (int32_t)lo->offset; old.lo_flags = (int32_t)lo->flags;
        memcpy(old.lo_name, lo->file_name, LO_NAME_SIZE);
        if (lo->offset > 0x7FFFFFFF) return -75;   /* EOVERFLOW */
        return copy_out(argument, &old, sizeof(old));
    }
    case 0x4C02: { /* LOOP_SET_STATUS (old) */
        struct loop_info old; int e = copy_in(&old, argument, sizeof(old)); if (e) return e;
        if (old.lo_offset < 0) return -22;
        struct loop_info64 info; status64(lo, &info);
        info.lo_offset = (uint64_t)old.lo_offset; info.lo_flags = (uint32_t)old.lo_flags; info.lo_encrypt_type = (uint32_t)old.lo_encrypt_type;
        info.lo_encrypt_key_size = (uint32_t)old.lo_encrypt_key_size; memcpy(info.lo_file_name, old.lo_name, LO_NAME_SIZE);
        return set_status(lo, &info);
    }
    case 0x4C06: { /* LOOP_CHANGE_FD: read-only devices only, same size */
        if (!lo->backing) return -6;
        if (!(lo->flags & LO_FLAGS_READ_ONLY)) return -22;
        struct open_file* f = kernel_file(argument); if (!f) return -9;
        uint64_t flags; const char* path; struct memory_file* cache = kernel_file_cache(f, &flags, &path);
        if (!cache || cache->size != backing_size(lo)) return -22;
        fd_reference(f); struct open_file* old = lo->backing; lo->backing = f; lo->cache = cache;
        if (path) { strncpy(lo->path, path, sizeof(lo->path) - 1); }
        fd_release(old); announce(lo); return 0;
    }
    case 0x4C07:   /* LOOP_SET_CAPACITY */
        if (!lo->backing) return -6;
        resize(lo); announce(lo); return 0;
    case 0x4C08:   /* LOOP_SET_DIRECT_IO: the page cache is used either way */
        if (!lo->backing) return -6;
        if (argument) lo->flags |= LO_FLAGS_DIRECT_IO; else lo->flags &= ~LO_FLAGS_DIRECT_IO;
        return 0;
    case 0x4C09:   /* LOOP_SET_BLOCK_SIZE */
        if (argument < 512 || argument > 4096 || (argument & (argument - 1))) return -22;
        if (lo->backing && in_use(lo)) return -16;
        lo->block.block_size = (uint32_t)argument; if (lo->backing) { resize(lo); announce(lo); }
        return 0;
    }
    return -25;
}
static void loop_idle(struct block_device* dev) {
    struct loop* lo = loop_of(dev);
    if (lo && lo->backing && (lo->flags & LO_FLAGS_AUTOCLEAR) && !dev->holders) unbind(lo);
}
static void loop_release(struct block_device* dev) {
    struct loop* lo = loop_of(dev); if (!lo) return;
    if (lo->backing) { struct open_file* f = lo->backing; lo->backing = NULL; fd_release(f); }
    arena_free((uint64_t)lo, sizeof(*lo));
}
static const struct block_extra_ops loop_extra = {.release = loop_release, .ioctl = loop_ioctl, .idle = loop_idle};

/* ---- /dev/loop-control ---- */
static int control_open(uint64_t flags, struct input_reader* reader) { (void)flags; (void)reader; return 0; }
static int control_ioctl(struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)reader; (void)flags;
    if (!credentials_capable(credentials_current(), NULL, 21)) return -1;   /* CAP_SYS_ADMIN, as Linux's loop_control_ioctl */
    switch (command) {
    case 0x4C80:   /* LOOP_CTL_ADD */
        if (argument >= LOOP_MAX) return -22;
        if (loops[argument]) return -17;
        return create((unsigned)argument) ? (int)argument : -12;
    case 0x4C81: { /* LOOP_CTL_REMOVE */
        if (argument >= LOOP_MAX) return -22;
        struct loop* lo = loops[argument]; if (!lo) return -6;
        if (lo->backing || lo->block.references > 1 || lo->block.holders) return -16;
        loops[argument] = NULL; block_hot_remove(&lo->block);   /* freed with its last reference (loop_release) */
        return (int)argument;
    }
    case 0x4C82:   /* LOOP_CTL_GET_FREE */
        for (unsigned i = 0; i < LOOP_MAX; i++) if (loops[i] && !loops[i]->backing && loops[i]->block.references == 1) return (int)i;
        for (unsigned i = 0; i < LOOP_MAX; i++) if (!loops[i]) return create(i) ? (int)i : -12;
        return -28;
    }
    return -25;
}
static const struct storage_char_ops control_ops = {.open = control_open, .ioctl = control_ioctl};
void loop_init(void) { storage_loop_control_ops = &control_ops; }

/* sysfs /sys/block/loopN/loop/<attr> (blkdev_node.c loop_attrs order) */
bool loop_sysfs_attr(struct block_device* dev, unsigned index, char* text) {
    struct loop* lo = loop_of(dev);
    if (!lo || !lo->backing) return false;
    switch (index) {
    case 0: strcat(text, lo->path); return true;
    case 1: append_u64(text, lo->offset); return true;
    case 2: append_u64(text, lo->sizelimit); return true;
    case 3: strcat(text, lo->flags & LO_FLAGS_AUTOCLEAR ? "1" : "0"); return true;
    case 4: strcat(text, lo->flags & LO_FLAGS_PARTSCAN ? "1" : "0"); return true;
    case 5: strcat(text, lo->flags & LO_FLAGS_DIRECT_IO ? "1" : "0"); return true;
    }
    return false;
}
