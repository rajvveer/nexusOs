/* Basic native character devices; random bytes use the same checked source as getrandom. */
#include "native.h"
#include "../../kernel/string.h"
#include "gpu.h"
#include "hwnode.h"
#include "sound_hw.h"

enum { DIRECTORY, NULL_DEVICE, ZERO_DEVICE, RANDOM_DEVICE, URANDOM_DEVICE, FRAMEBUFFER, INPUT_DIRECTORY, KEYBOARD, MOUSE,
       SOUND_DIRECTORY, SOUND_CONTROL, SOUND_PCM, DRI_DIRECTORY, DRI_CARD, DRI_RENDER, DMABUF, SYNC_FILE, PTS_DIRECTORY, SHM_DIRECTORY, RTC_DEVICE, NET_DIRECTORY,
       SOUND_CAPTURE, DEVICE_COUNT };
static const char* paths[] = {"/dev", "/dev/null", "/dev/zero", "/dev/random", "/dev/urandom", "/dev/fb0", "/dev/input", "/dev/input/event0", "/dev/input/event1",
                              "/dev/snd", "/dev/snd/controlC0", "/dev/snd/pcmC0D0p", "/dev/dri", "/dev/dri/card0", "/dev/dri/renderD128",
                              "/dmabuf:", "anon_inode:sync_file", "/dev/pts", "/dev/shm", "/dev/rtc0", "/dev/net", "/dev/snd/pcmC0D0c"};
static const unsigned numbers[] = {0, 0x103, 0x105, 0x108, 0x109, 0x1D00, 0, 0xD40, 0xD41, 0, 0x7400, 0x7410, 0, 0xE200, 0xE280, 0, 0, 0, 0, 0xFD00, 0, 0x7418};
static bool random_device(unsigned device) { return device == RANDOM_DEVICE || device == URANDOM_DEVICE; }
static bool input_device(unsigned device) { return device == KEYBOARD || device == MOUSE; }
static bool sound_device(unsigned device) { return device == SOUND_CONTROL || device == SOUND_PCM || device == SOUND_CAPTURE; }
static unsigned sound_node(unsigned device) { return device == SOUND_CAPTURE ? 2 : device - SOUND_CONTROL; }   /* sound.c: 0 control, 1 playback, 2 capture */
static bool drm_device(unsigned device) { return device >= DRI_CARD && device <= SYNC_FILE; }
int device_index(unsigned node) { return DRI_CARD + (int)node; }
int device_node(unsigned device) { return drm_device(device) ? (int)(device - DRI_CARD) : -1; }
static bool directory(unsigned device) { return device == DIRECTORY || device == INPUT_DIRECTORY || device == SOUND_DIRECTORY || device == DRI_DIRECTORY || device == PTS_DIRECTORY || device == SHM_DIRECTORY || device == NET_DIRECTORY; }
static bool sound_visible(unsigned device) {   /* card 0 nodes (a USB card can be card 0 without a PCI one) */
    if (device == SOUND_DIRECTORY) { for (unsigned i = 0; i < SOUND_CARDS; i++) if (sound_card_present(i)) return true; return hwnode_any_below("/dev/snd"); }
    return sound_node_available(device == SOUND_CAPTURE ? 2 : device - SOUND_CONTROL);
}
static bool visible(unsigned device) { return device < SOUND_DIRECTORY || (device <= SOUND_PCM && sound_visible(device)) || ((device == DRI_DIRECTORY || device == DRI_CARD) && drm_available()) || (device == DRI_RENDER && drm_render_available()) ||
                                                device == PTS_DIRECTORY || device == SHM_DIRECTORY || device == RTC_DEVICE || device == NET_DIRECTORY ||
                                                (device == SOUND_CAPTURE && sound_visible(device)); }
bool device_framebuffer(unsigned device) { return device == FRAMEBUFFER && framebuffer_get(); }
struct memory_file* device_mapping(unsigned device, uint64_t* offset, uint64_t length, struct input_reader* reader) {
    if (device >= DEVICE_HW) return hwnode_mapping(device, offset, length, reader);
    if (drm_device(device)) return drm_mapping(device - DRI_CARD, offset, length, reader);
    return sound_device(device) ? sound_mapping(sound_node(device), *offset, length, reader) : NULL;
}
int device_lookup(const char* path) {
    if (!strcmp(path, "/dev/ptmx")) return DEVICE_PTMX;
    if (!strcmp(path, "/dev/tty")) return DEVICE_TTY;
    if (!strncmp(path, "/dev/pts/", 9)) return pty_lookup(path + 9);
    if (!strncmp(path, "/dev/shm/", 9)) return shm_lookup(path + 9);
    for (unsigned i = 0; i < DEVICE_COUNT; i++) if (visible(i) && !strcmp(path, paths[i])) return i;
    int hw = hwnode_lookup(path); if (hw >= 0) return hw;   /* hot-plug class devices (hwnode.c) */
    return storage_lookup(path);   /* disks, partitions, loop/mapper/fuse nodes (blkdev_node.c) */
}
int device_open_check(unsigned device, uint64_t flags) {
    if (device >= DEVICE_HW) return hwnode_open_check(device, flags);
    if (device >= DEVICE_STORAGE) return storage_open_check(device, flags);
    if ((flags & 3) == 3 || (flags & ~(3ULL | 0x40ULL | 0x80ULL | 0x100ULL | 0x200ULL | 0x400ULL | 0x800ULL |
                                      0x101000ULL | 0x80000ULL | 0x10000ULL | 0x20000ULL | 0x8000ULL))) return -22;
    if ((flags & 0xC0) == 0xC0) return -17;
    if (directory(device)) return flags & (3 | 0x200) ? -21 : 0;
    if (flags & 0x10000) return -20;
    /* shortcut: entropy injection needs a kernel mixing pool; hardware RNG nodes stay read-only. */
    if (random_device(device) && (flags & 3)) return -13;
    if (drm_device(device)) return device == DRI_CARD || device == DRI_RENDER ? drm_open_check() : -6;
    if (device == RTC_DEVICE) return rtc_device_open_check(flags);   /* power.c */
    if (sound_device(device)) {
        if (!seat_device_access()) return -13;   /* root, pid 1 or the seat's console user (seat.c) */
        return sound_open_check(sound_node(device), flags);
    }
    if (device == FRAMEBUFFER || input_device(device)) {
        if (device == FRAMEBUFFER ? !framebuffer_get() : !input_available(device - KEYBOARD)) return -19;
        if (!seat_device_access()) return -13;
        if (input_device(device) && (flags & 3)) return -13;
    }
    return 0;
}
uint64_t device_open(unsigned device, uint64_t flags, struct input_reader* reader) {
    if (device >= DEVICE_HW) { hwnode_open(device, flags, reader); return 0; }
    if (device >= DEVICE_STORAGE) { storage_open(device, flags, reader); return 0; }
    if (device == FRAMEBUFFER && (flags & 3)) framebuffer_retain();
    if (input_device(device)) input_open(device - KEYBOARD, reader);
    if (input_device(device) || device == FRAMEBUFFER) reader->seat = seat_open_token();   /* revoked on a session switch */
    if (sound_device(device)) sound_open(sound_node(device), reader);
    if (drm_device(device) && drm_open(device - DRI_CARD, flags, reader)) reader->sequence = 0;
    if (device == RTC_DEVICE) rtc_device_open(reader);
    return 0;
}
void device_close(unsigned device, uint64_t flags, struct input_reader* reader) {
    if (device >= DEVICE_HW) { hwnode_close(device, flags, reader); return; }
    if (device >= DEVICE_STORAGE) { storage_close(device, flags, reader); return; }
    if (device == FRAMEBUFFER && (flags & 3)) framebuffer_release();
    if (sound_device(device)) sound_close(sound_node(device), reader);
    if (drm_device(device)) drm_close(device - DRI_CARD, reader);
}
int64_t device_io(unsigned device, uint64_t buffer, uint64_t count, bool write, uint64_t* position, uint64_t flags, struct input_reader* reader) {
    if (device >= DEVICE_HW) return hwnode_io(device, buffer, count, write, flags, reader);
    if (device >= DEVICE_STORAGE) return storage_io(device, buffer, count, write, position, flags, reader);
    if (directory(device)) return -21;
    if ((input_device(device) || device == FRAMEBUFFER) && seat_revoked(reader->seat)) return -19;
    if (input_device(device)) return write ? -9 : input_read(device - KEYBOARD, reader, buffer, count, (flags & 0x800) != 0);
    if (sound_device(device)) return sound_io(sound_node(device), buffer, count, write, flags, reader);
    if (drm_device(device)) return drm_io(device - DRI_CARD, buffer, count, write, flags, reader);
    if (device == RTC_DEVICE) return write ? -9 : rtc_device_read(buffer, count, (flags & 0x800) != 0, reader);
    if (count > 0x7FFFF000) count = 0x7FFFF000;
    if (!count || (!write && device == NULL_DEVICE)) return 0;
    if (device == FRAMEBUFFER) {
        const struct framebuffer_info* fb = framebuffer_get(); if (!fb) return -19;
        if (*position >= fb->size) return write ? -28 : 0;
        if (count > fb->size - *position) count = fb->size - *position;
        if (!accessible(buffer, count, !write)) return -14;
        volatile uint8_t* video = (volatile uint8_t*)mmio_map(fb->physical, fb->size, MMIO_WRITE_COMBINED); /* the console's mapping */
        if (!video) return -19;
        video += *position; uint8_t* data = (void*)buffer;
        if (write ? copy_from_user((void*)video, data, count) : copy_to_user(data, (const void*)video, count)) return -14;
        *position += count; return count;
    }
    if (write) return device == NULL_DEVICE || device == ZERO_DEVICE ? (int64_t)count : -9;
    if (!accessible(buffer, count, true)) return -14;
    if (device == ZERO_DEVICE) { if (clear_user(buffer, count)) return -14; }
    else if (native_random_user(buffer, count) < 0) return -11;
    return count;
}
int64_t device_seek(unsigned device, uint64_t* position, int64_t offset, uint64_t whence, struct input_reader* reader) {
    if (device >= DEVICE_HW) return -29;
    if (device >= DEVICE_STORAGE) return storage_seek(device, position, offset, whence);
    if (whence > 2) return -22;
    if (drm_device(device)) return drm_seek(device - DRI_CARD, position, offset, whence, reader);
    if (device == NULL_DEVICE || device == ZERO_DEVICE) { *position = 0; return 0; }
    if ((!directory(device) && device != FRAMEBUFFER) || (directory(device) && whence == 2)) return -29;
    int64_t base = whence == 1 ? (int64_t)*position : whence == 2 ? framebuffer_get()->size : 0, result;
    if (__builtin_add_overflow(base, offset, &result) || result < 0) return -22;
    *position = result; return result;
}
struct fb_fixed {
    char id[16]; uint64_t memory; uint32_t size, type, type_aux, visual;
    uint16_t xpan, ypan, ywrap; uint32_t stride; uint64_t mmio;
    uint32_t mmio_size, accel; uint16_t capabilities, reserved[2];
};
struct fb_field { uint32_t offset, length, msb_right; };
struct fb_variable {
    uint32_t width, height, virtual_width, virtual_height, x, y, bpp, grayscale;
    struct fb_field red, green, blue, alpha;
    uint32_t nonstd, activate, height_mm, width_mm, accel, pixelclock;
    uint32_t left, right, upper, lower, hsync, vsync, sync, mode, rotate, colorspace, reserved[4];
};
_Static_assert(sizeof(struct fb_fixed) == 80 && sizeof(struct fb_variable) == 160, "x86-64 framebuffer ABI");
int device_ioctl(unsigned device, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags) {
    if (device >= DEVICE_HW) return hwnode_ioctl(device, command, argument, reader, flags);
    if (device >= DEVICE_STORAGE) return storage_ioctl(device, command, argument, reader, flags);
    if (input_device(device)) return seat_revoked(reader->seat) ? -19 : input_ioctl(device - KEYBOARD, reader, command, argument);
    if (sound_device(device)) return (int)sound_ioctl(sound_node(device), command, argument, flags, reader);
    if (drm_device(device)) return drm_ioctl(device - DRI_CARD, command, argument, reader, flags);
    if (device == RTC_DEVICE) return rtc_device_ioctl(command, argument);
    if (device != FRAMEBUFFER) return -25;
    const struct framebuffer_info* fb = framebuffer_get(); if (!fb) return -19;
    if (command == 0x4602) {
        struct fb_fixed info = {0}; strcpy(info.id, "Nexus VBE");
        info.memory = fb->physical; info.size = fb->size; info.visual = 2; info.stride = fb->stride;
        if (!accessible(argument, sizeof(info), true)) return -14;
        return copy_to_user((void*)argument, &info, sizeof(info)) ? -14 : 0;
    }
    if (command == 0x4600) {
        struct fb_variable info = {0}; info.width = info.virtual_width = fb->width; info.height = info.virtual_height = fb->height;
        info.bpp = 32; info.height_mm = info.width_mm = ~0U;
        bool bgr = fb->format == 2;                                       /* NEXUS_FB_XBGR8888: bytes R,G,B,X */
        info.red = (struct fb_field){bgr ? 0 : 16, 8, 0}; info.green = (struct fb_field){8, 8, 0}; info.blue = (struct fb_field){bgr ? 16 : 0, 8, 0};
        if (!accessible(argument, sizeof(info), true)) return -14;
        return copy_to_user((void*)argument, &info, sizeof(info)) ? -14 : 0;
    }
    /* shortcut: fixed firmware mode; add mode setting, panning and vblank with a native GPU driver. */
    return -25;
}
void* device_event_source(unsigned device, const struct input_reader* reader) {
    if (device >= DEVICE_HW) return hwnode_source(device, reader);
    if (device >= DEVICE_STORAGE) return storage_event_source(device, reader);
    if (drm_device(device)) return drm_event_source(device - DRI_CARD, reader);
    if (device == RTC_DEVICE) return rtc_device_source();
    return input_device(device) ? input_source(device - KEYBOARD) : sound_device(device) ? sound_source(sound_node(device)) : NULL;
}
uint32_t device_ready(unsigned device, const struct input_reader* reader) {
    if (device >= DEVICE_HW) return hwnode_ready(device, reader);
    if (device >= DEVICE_STORAGE) return storage_ready(device, reader);
    if (sound_device(device)) return sound_ready(sound_node(device), reader);
    if (drm_device(device)) return drm_ready(device - DRI_CARD, reader);
    if (device == RTC_DEVICE) return rtc_device_ready(reader);
    return input_device(device) ? (seat_revoked(reader->seat) ? 1U : input_ready(device - KEYBOARD, reader)) : random_device(device) ? (native_random_available() ? 1U : 0) : 5U;
}
bool device_pollable(unsigned device) { if (device >= DEVICE_HW) return true; if (device >= DEVICE_STORAGE) return storage_pollable(device); return input_device(device) || random_device(device) || sound_device(device) || drm_device(device) || device == RTC_DEVICE; }
void device_stat(unsigned device, struct linux_stat* stat) {
    if (device >= DEVICE_HW) { hwnode_stat(device, stat); return; }
    if (device >= DEVICE_STORAGE) { storage_stat(device, stat); return; }
    if (device >= DEVICE_SHM) { shm_device_stat(device, stat); return; }
    if (device >= DEVICE_PTMX) { pty_device_stat(device, stat); return; }
    memset(stat, 0, sizeof(*stat)); stat->dev = 9; stat->ino = device + 1; stat->nlink = directory(device) ? 2 : 1;
    if (device == PTS_DIRECTORY || device == SHM_DIRECTORY) stat->dev = device == PTS_DIRECTORY ? 11 : 10;   /* devpts / tmpfs mount roots (mountinfo 0:11, 0:10) */
    stat->mode = device == SHM_DIRECTORY ? 0041777 : directory(device) ? 0040555 : device == FRAMEBUFFER ? 0020600 : input_device(device) ? 0020400 : random_device(device) ? 0020444 :
                 sound_device(device) ? 0020660 : 0020666;
    stat->rdev = numbers[device]; stat->block_size = 4096;
    if (drm_device(device)) stat->mode = device == DRI_CARD || device == DRI_RENDER ? 0020666 : 0600;
    if (device == RTC_DEVICE) stat->mode = 0020600;
    stat->uid = credentials_visible_id(device == FRAMEBUFFER || input_device(device) || sound_device(device) ? seat_console_uid() : 0, false); stat->gid = credentials_visible_id(0, true);
}
void device_description_stat(unsigned device, const struct input_reader* reader, struct linux_stat* stat) {
    device_stat(device, stat);
    if (drm_device(device)) drm_stat(device - DRI_CARD, reader, stat);
}
/* Children of a directory node are the visible paths directly below it, in table order. */
int device_directory(unsigned device, uint64_t position, struct directory_item* item) {
    if (device >= DEVICE_HW) return -20;
    if (device >= DEVICE_STORAGE) return storage_directory(device, position, item);
    if (!directory(device)) return -20;
    memset(item, 0, sizeof(*item)); item->next = position + 1;
    if (position < 2) { strcpy(item->name, position ? ".." : "."); item->inode = position ? 1 : device + 1; item->type = 4; return 1; }
    if (device == PTS_DIRECTORY) return pty_directory(position - 2, item);
    if (device == SHM_DIRECTORY) return shm_directory(position - 2, item);
    uint64_t index = 2; size_t prefix = strlen(paths[device]);
    for (unsigned child = 0; child < DEVICE_COUNT; child++) {
        const char* name = paths[child] + prefix;
        if (!visible(child) || strncmp(paths[child], paths[device], prefix) || name[0] != '/' || strstr(name + 1, "/")) continue;
        if (index++ != position) continue;
        strcpy(item->name, name + 1); item->inode = child + 1; item->type = directory(child) ? 4 : 2; return 1;
    }
    if (device == DIRECTORY && position - index < 2) { /* terminal nodes served by pty.c */
        strcpy(item->name, position == index ? "ptmx" : "tty"); item->inode = position == index ? DEVICE_PTMX : DEVICE_TTY; item->type = 2; return 1;
    }
    if (device == DIRECTORY) index += 2;
    if (hwnode_directory_entry(paths[device], position - index, item)) return 1;   /* /dev/videoN, uinput, input/eventN, snd/controlC1.. */
    if (device == DIRECTORY) return storage_root_entry(position - index - hwnode_count_below("/dev"), item);   /* disks, loop/mapper/fuse */
    return 0;
}
int device_readlink(unsigned device, char* target, unsigned capacity) { return device >= DEVICE_HW ? -22 : device >= DEVICE_STORAGE ? storage_readlink(device, target, capacity) : -22; }
int device_fsync(unsigned device) { return device >= DEVICE_HW ? 0 : device >= DEVICE_STORAGE ? storage_fsync(device) : 0; }
