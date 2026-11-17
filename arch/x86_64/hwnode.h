/* Hot-plug character devices of the hardware classes (hwnode.c): USB sound cards 1..3 (/dev/snd/controlCn,
 * pcmCnD0p, pcmCnD0c), evdev /dev/input/eventN (N >= 2) and joydev /dev/input/jsN (evdev.c), /dev/uinput, V4L2
 * /dev/videoN (v4l2.c) and Bluetooth /dev/vhci (bluetooth.c). A class driver registers a node with its ops and its own
 * minor; FD_DEVICE numbers are DEVICE_HW + generation << 8 + slot, so an fd of a removed node (unplugged device) gets
 * ENODEV instead of reaching whatever later takes its slot. device.c delegates the whole range here. */
#ifndef NEXUS_HWNODE_H
#define NEXUS_HWNODE_H
#include "native.h"

#define DEVICE_HW 0x3000000U
#define HWNODE_SLOTS 96U

struct hwnode_ops {
    int (*open_check)(unsigned minor, uint64_t flags);                       /* optional: 0 or -errno */
    void (*open)(unsigned minor, uint64_t flags, struct input_reader* reader);
    void (*close)(unsigned minor, uint64_t flags, struct input_reader* reader);
    int64_t (*io)(unsigned minor, struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags);
    int64_t (*ioctl)(unsigned minor, struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags);
    uint32_t (*ready)(unsigned minor, const struct input_reader* reader);   /* poll mask */
    void* (*source)(unsigned minor, const struct input_reader* reader);     /* event_source_changed() channel */
    struct memory_file* (*mapping)(unsigned minor, uint64_t* offset, uint64_t length, struct input_reader* reader);
    void (*sysfs)(unsigned minor, char* text, unsigned capacity, const char* attribute);  /* optional sysfs attribute */
};
/* Optional for a node: a writev is one message (/dev/vhci: one H:4 packet), not one write per vector. */
typedef int64_t (*hwnode_vectors_fn)(unsigned minor, struct input_reader* reader, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags);
void hwnode_set_write_vectors(int slot, hwnode_vectors_fn fn);

/* Register /dev/<path> (char device major:minor, mode, owner uid); `cookie` (the driver's own index) is what the ops get. Returns the slot or
 * -1 when the table is full. Removing wakes pollers; open fds then get ENODEV (their close still reaches the ops). */
int hwnode_add(const char* path, unsigned major, unsigned minor, unsigned cookie, unsigned mode, uint32_t uid, const struct hwnode_ops* ops, const char* subsystem);
void hwnode_remove(int slot);
int hwnode_slot_cookie(int slot);

/* device.c entry points for numbers >= DEVICE_HW */
int hwnode_lookup(const char* path);
int hwnode_open_check(unsigned device, uint64_t flags);
void hwnode_open(unsigned device, uint64_t flags, struct input_reader* reader);
void hwnode_close(unsigned device, uint64_t flags, struct input_reader* reader);
int64_t hwnode_io(unsigned device, uint64_t buffer, uint64_t count, bool write, uint64_t flags, struct input_reader* reader);
bool hwnode_write_vectors(unsigned device, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags, struct input_reader* reader, int64_t* result);
int hwnode_ioctl(unsigned device, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags);
uint32_t hwnode_ready(unsigned device, const struct input_reader* reader);
void* hwnode_source(unsigned device, const struct input_reader* reader);
struct memory_file* hwnode_mapping(unsigned device, uint64_t* offset, uint64_t length, struct input_reader* reader);
void hwnode_stat(unsigned device, struct linux_stat* stat);
/* Children of /dev, /dev/input, /dev/snd that are hwnode entries: index-th entry directly below `directory`. */
int hwnode_directory_entry(const char* directory, uint64_t index, struct directory_item* item);
bool hwnode_any_below(const char* directory);
unsigned hwnode_count_below(const char* directory);
/* Dynamic sysfs objects (hwsysfs.c), see there: /sys/devices/virtual/<class>/<name>[/<child>], /sys/class/<class>/<name>,
 * /sys/bus/input/devices/<name>. read() fills `out` with an attribute's text and returns its length or -errno. */
#define HW_SYSFS_BASE 0x60000000U
typedef int (*hwsys_read)(void* context, const char* attribute, char* out, unsigned capacity);
int hwsys_add(const char* class_name, const char* name, int parent, const char* const* attributes, unsigned attribute_count, hwsys_read read, void* context);
void hwsys_remove(int id);
bool hw_sysfs_path(const char* path);
int hw_sysfs_resolve(const char* path, bool follow, uint32_t* node);
bool hw_sysfs_directory(uint32_t node);
bool hw_sysfs_link(uint32_t node);
int hw_sysfs_stat(uint32_t node, struct linux_stat* stat);
int64_t hw_sysfs_read(uint32_t node, uint64_t address, uint64_t count, uint64_t offset);
int hw_sysfs_entry(uint32_t node, uint64_t position, struct directory_item* item);
int hw_sysfs_readlink(uint32_t node, char* result, unsigned capacity);
int hw_sysfs_node_path(uint32_t node, char* result, unsigned capacity);
/* Scheduler loop (kernel.c native_io_poll): periodic work of the classes (cameras, force-feedback durations). */
void hwnode_poll(uint64_t now);
void hwnode_init(void);                                         /* boot: /dev/uinput, virtual camera (vcam), pad (vpad) */
#endif
