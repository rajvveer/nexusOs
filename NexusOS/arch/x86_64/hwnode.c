/* Hot-plug character devices of the hardware classes (see hwnode.h). The table is small and changes only from the USB
 * worker or a driver's own ioctl (uinput), always under the kernel lock. A slot keeps its generation across reuse:
 * an fd stores DEVICE_HW | generation << 8 | slot, and an fd whose node was removed meanwhile gets ENODEV (poll: POLLERR
 * | POLLHUP) while its close still reaches the driver, which owns the per-fd state in input_reader. */
#include "hwnode.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

struct hwnode {
    char path[48];                    /* "/dev/input/event3" */
    const struct hwnode_ops* ops;
    hwnode_vectors_fn write_vectors;  /* optional: writev as one message (hwnode_set_write_vectors) */
    const char* subsystem;           /* sysfs class: "input", "sound", "video4linux", "misc", "bluetooth" */
    unsigned major, minor, cookie, mode, generation, opens;   /* opens: fds on it; a slot is not reused before they close */
    uint32_t uid;
    bool used;
};
static struct hwnode nodes[HWNODE_SLOTS];

static struct hwnode* node_of(unsigned device, bool live) {
    if (device < DEVICE_HW || device >= DEVICE_HW + 0x100000U) return NULL;
    unsigned slot = device & 0xFF, generation = (device >> 8) & 0xFFF;
    if (slot >= HWNODE_SLOTS) return NULL;
    struct hwnode* n = &nodes[slot];
    if (live && (!n->used || (n->generation & 0xFFF) != generation)) return NULL;
    return n;
}
static unsigned number_of(unsigned slot) { return DEVICE_HW | (nodes[slot].generation & 0xFFF) << 8 | slot; }

int hwnode_add(const char* path, unsigned major, unsigned minor, unsigned cookie, unsigned mode, uint32_t uid, const struct hwnode_ops* ops, const char* subsystem) {
    if (!path || strlen(path) >= sizeof(nodes[0].path) || !ops) return -1;
    for (unsigned i = 0; i < HWNODE_SLOTS; i++) if (nodes[i].used && !strcmp(nodes[i].path, path)) return -1;
    for (unsigned i = 0; i < HWNODE_SLOTS; i++) if (!nodes[i].used && !nodes[i].opens) {
        struct hwnode* n = &nodes[i]; unsigned generation = n->generation + 1;
        memset(n, 0, sizeof(*n)); strcpy(n->path, path);
        n->ops = ops; n->subsystem = subsystem; n->major = major; n->minor = minor; n->cookie = cookie; n->mode = mode; n->uid = uid;
        n->generation = generation; n->used = true;
        return (int)i;
    }
    vga_print("[64] HWNODE table full: "); vga_print(path); vga_print("\n");
    return -1;
}
void hwnode_remove(int slot) {
    if (slot < 0 || slot >= (int)HWNODE_SLOTS || !nodes[slot].used) return;
    nodes[slot].used = false;   /* generation stays: open fds now fail */
}
int hwnode_slot_cookie(int slot) { return slot >= 0 && slot < (int)HWNODE_SLOTS && nodes[slot].used ? (int)nodes[slot].cookie : -1; }

int hwnode_lookup(const char* path) {
    for (unsigned i = 0; i < HWNODE_SLOTS; i++) if (nodes[i].used && !strcmp(nodes[i].path, path)) return (int)number_of(i);
    return -1;
}
int hwnode_open_check(unsigned device, uint64_t flags) {
    struct hwnode* n = node_of(device, true);
    if (!n) return -19;
    if ((flags & 3) == 3 || (flags & 0x10000)) return (flags & 0x10000) ? -20 : -22;   /* O_ACCMODE 3, O_DIRECTORY */
    uint32_t uid = credentials_kernel_id(credentials_current(), false, 3);
    if (uid && uid != n->uid) {                        /* mode bits: owner rw for the console user, others by mode */
        bool write = (flags & 3) != 0, read = (flags & 3) != 1;
        if ((read && !(n->mode & 04)) || (write && !(n->mode & 02))) return -13;
    }
    return n->ops->open_check ? n->ops->open_check(n->cookie, flags) : 0;
}
void hwnode_open(unsigned device, uint64_t flags, struct input_reader* reader) {
    struct hwnode* n = node_of(device, true);
    memset(reader, 0, sizeof(*reader));
    if (!n) return;
    n->opens++;
    if (n->ops->open) n->ops->open(n->cookie, flags, reader);
}
void hwnode_close(unsigned device, uint64_t flags, struct input_reader* reader) {
    struct hwnode* n = node_of(device, false);
    /* A removed node's driver still owns the fd state: close reaches it with the old minor (drivers keep it valid). */
    if (!n || !n->ops || (n->generation & 0xFFF) != ((device >> 8) & 0xFFF)) return;
    if (n->opens) n->opens--;
    if (n->ops->close) n->ops->close(n->cookie, flags, reader);
}
int64_t hwnode_io(unsigned device, uint64_t buffer, uint64_t count, bool write, uint64_t flags, struct input_reader* reader) {
    struct hwnode* n = node_of(device, true);
    if (!n) return -19;
    return n->ops->io ? n->ops->io(n->cookie, reader, buffer, count, write, flags) : -22;
}
/* writev on a node with message semantics: true when the node took it (result set), false = per-vector writes. */
bool hwnode_write_vectors(unsigned device, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags, struct input_reader* reader, int64_t* result) {
    struct hwnode* n = node_of(device, true);
    if (!n || !n->write_vectors) return false;
    *result = n->write_vectors(n->cookie, reader, vectors, count, total, flags); return true;
}
void hwnode_set_write_vectors(int slot, hwnode_vectors_fn fn) { if (slot >= 0 && slot < (int)HWNODE_SLOTS && nodes[slot].used) nodes[slot].write_vectors = fn; }
int hwnode_ioctl(unsigned device, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags) {
    struct hwnode* n = node_of(device, true);
    if (!n) return -19;
    return n->ops->ioctl ? (int)n->ops->ioctl(n->cookie, reader, command, argument, flags) : -25;
}
uint32_t hwnode_ready(unsigned device, const struct input_reader* reader) {
    struct hwnode* n = node_of(device, true);
    if (!n) return 8U | 16U;                           /* POLLERR | POLLHUP */
    return n->ops->ready ? n->ops->ready(n->cookie, reader) : 5U;
}
void* hwnode_source(unsigned device, const struct input_reader* reader) {
    struct hwnode* n = node_of(device, false);
    if (!n || !n->ops || !n->ops->source) return NULL;
    return n->ops->source(n->cookie, reader);
}
struct memory_file* hwnode_mapping(unsigned device, uint64_t* offset, uint64_t length, struct input_reader* reader) {
    struct hwnode* n = node_of(device, true);
    return n && n->ops->mapping ? n->ops->mapping(n->cookie, offset, length, reader) : NULL;
}
void hwnode_stat(unsigned device, struct linux_stat* stat) {
    struct hwnode* n = node_of(device, false);
    memset(stat, 0, sizeof(*stat)); stat->dev = 9; stat->ino = 0x30000 + (device & 0xFF); stat->nlink = 1; stat->block_size = 4096;
    if (!n) { stat->mode = 0020000; return; }
    stat->mode = 0020000 | (n->mode & 0777); stat->rdev = storage_linux_dev(n->major, n->minor);
    stat->uid = credentials_visible_id(n->uid, false); stat->gid = credentials_visible_id(0, true);
}
static bool directly_below(const char* path, const char* directory) {
    unsigned length = strlen(directory);
    return !strncmp(path, directory, length) && path[length] == '/' && path[length + 1] && !strstr(path + length + 1, "/");
}
bool hwnode_any_below(const char* directory) {
    for (unsigned i = 0; i < HWNODE_SLOTS; i++) if (nodes[i].used && directly_below(nodes[i].path, directory)) return true;
    return false;
}
unsigned hwnode_count_below(const char* directory) {
    unsigned count = 0;
    for (unsigned i = 0; i < HWNODE_SLOTS; i++) count += nodes[i].used && directly_below(nodes[i].path, directory);
    return count;
}
int hwnode_directory_entry(const char* directory, uint64_t index, struct directory_item* item) {
    uint64_t seen = 0;
    for (unsigned i = 0; i < HWNODE_SLOTS; i++) {
        if (!nodes[i].used || !directly_below(nodes[i].path, directory)) continue;
        if (seen++ != index) continue;
        strcpy(item->name, nodes[i].path + strlen(directory) + 1); item->inode = 0x30000 + i; item->type = 2;
        return 1;
    }
    return 0;
}
void v4l2_poll(uint64_t now);
void evdev_poll(uint64_t now);
void vpad_poll(uint64_t now);
void bluetooth_poll(uint64_t now);
void hwnode_poll(uint64_t now) { v4l2_poll(now); evdev_poll(now); vpad_poll(now); bluetooth_poll(now); }
uint64_t v4l2_next_event(uint64_t now); uint64_t evdev_next_event(uint64_t now); uint64_t vpad_next_event(uint64_t now); uint64_t bluetooth_next_event(uint64_t now);
/* Tickless boot CPU (power2's tick.c): when hwnode_poll has work next - camera frames, force-feedback ends, vpad steps;
 * ~0 when nothing is active, so an idle machine stays idle. (usbclass_poll has no timed work: UVC, UAC and Xbox pads
 * run from USB completions on the worker; USB audio's mixer timing is the sound engine's, see sound_next_event.) */
uint64_t hwnode_next_event(uint64_t now) {
    uint64_t a = v4l2_next_event(now), b = evdev_next_event(now), c = vpad_next_event(now), d = bluetooth_next_event(now);
    uint64_t m = a < b ? (a < c ? a : c) : (b < c ? b : c);
    return m < d ? m : d;
}
void tick_register_poller(const char* name, uint64_t (*next)(uint64_t now)) __attribute__((weak));
void uinput_init(void);
void vcam_init(void);
void vpad_init(void);
void bluetooth_init(void);
void hwnode_init(void) {
    uinput_init(); vcam_init(); vpad_init(); bluetooth_init();
    if (tick_register_poller) tick_register_poller("hwnode", hwnode_next_event);
}
