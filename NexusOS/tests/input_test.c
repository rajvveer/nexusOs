/* Exercise the production parser/queue/ioctls with simulated hardware ports. */
#include "../arch/x86_64/native.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>
#define PORT_H
static uint8_t replies[16], reply_status[16];
static unsigned reply_count, reply_read, commands, wakes, changes;
static bool busy;
static uint64_t ticks, inaccessible;
static uint8_t port_byte_in(uint16_t port) {
    if (port == 0x64) return busy ? 2 : reply_read < reply_count ? reply_status[reply_read] | 1 : 0;
    assert(port == 0x60 && reply_read < reply_count); return replies[reply_read++];
}
static void port_byte_out(uint16_t port, uint8_t byte) { (void)byte; assert(port == 0x60 || port == 0x64 || port == 0x21 || port == 0xA1); commands++; }
#include "../arch/x86_64/input.c"
#include "../arch/x86_64/device.c"
static uint32_t fsuid = 1000, display_refs;
static struct framebuffer_info test_display;
const struct framebuffer_info* framebuffer_get(void) { return &test_display; }
void framebuffer_retain(void) { display_refs++; }
void framebuffer_release(void) { assert(display_refs); display_refs--; }
bool native_random_available(void) { return false; }
bool native_random(void* data, uint64_t count) { (void)data; (void)count; return false; }
struct credentials* credentials_current(void) { return (void*)1; }
uint32_t credentials_kernel_id(struct credentials* credentials, bool group, unsigned which) { assert(credentials == (void*)1 && !group && which == 3); return fsuid; }
uint32_t credentials_visible_id(uint32_t id, bool group) { (void)group; return id; }
uint64_t native_clock_ns(void) { return ticks += 1000; }
uint64_t native_realtime_ns(void) { return native_clock_ns() + 1700000000000000000ULL; }
bool accessible(uint64_t address, uint64_t size, bool write) { (void)size; (void)write; return address > 4096 && address != inaccessible; }
int task_wait_channel(void* channel) { assert(channel == &inputs[0] || channel == &inputs[1]); return -4; }
void task_wake_channel(void* channel) { assert(channel == &inputs[0] || channel == &inputs[1]); wakes++; }
void event_source_changed(void* channel, uint32_t events) { assert(channel == &inputs[0] || channel == &inputs[1]); assert(events <= 1); changes++; }
void vga_print(const char* text) { (void)text; }
static struct input_event64 events[QUEUE_SIZE];
static int read_events(unsigned index, struct input_reader* reader) {
    return (int)input_read(index, reader, (uint64_t)events, sizeof(events), true) / (int)sizeof(events[0]);
}
static void exact(unsigned n, unsigned type, unsigned code, int value) { assert(events[n].type == type && events[n].code == code && events[n].value == value); }
int main(void) {
    busy = true; uint64_t start = ticks;
    assert(!send_port(0x64, 0xAD) && ticks - start >= 100000000 && !commands);
    busy = false; start = ticks; uint8_t byte;
    assert(!receive(false, &byte) && ticks - start >= 100000000);
    replies[0] = 0xFE; replies[1] = 0xFA; reply_count = 2;
    assert(command(false, 0xF4) && commands == 2 && reply_read == 2);
    commands = reply_read = 0; reply_count = 3; replies[0] = replies[1] = replies[2] = 0xFE;
    assert(!command(false, 0xF4) && commands == 3);
    inputs[0].present = inputs[1].present = true; realtime_offset = 1700000000000000000ULL;
    struct input_reader a, b; input_open(0, &a); input_open(0, &b);
    assert(!input_ready(0, &a) && input_read(0, &a, 1, 0, true) == 0 && input_read(0, &a, 1, 23, true) == -22);
    assert(input_read(0, &a, (uint64_t)events, 24, true) == -11 && input_read(0, &a, (uint64_t)events, 24, false) == -4);
    keyboard_byte(0x1E); keyboard_byte(0x1E); keyboard_byte(0x9E); keyboard_byte(0x9E);
    assert(input_ready(0, &a) && input_read(0, &a, 1, 24, true) == -14 && a.sequence == 0);
    assert(read_events(0, &a) == 6); exact(0,1,30,1); exact(2,1,30,2); exact(4,1,30,0); exact(5,0,0,0);
    assert(events[0].seconds >= 1700000000 && events[0].microseconds < 1000000);
    int clock = 1; assert(!input_ioctl(0, &b, 0x400445A0, (uint64_t)&clock));
    assert(read_events(0, &b) == 6 && events[0].seconds < 1700000000);
    keyboard_byte(0x1E); inaccessible = (uint64_t)&events[1]; unsigned changed = changes;
    assert(input_read(0, &a, (uint64_t)events, sizeof(events), true) == 24 && changes == changed + 1 && input_ready(0, &a));
    inaccessible = 0; assert(read_events(0, &a) == 1); exact(0,0,0,0);
    keyboard_byte(0x9E); assert(read_events(0, &a) == 2);
    const uint8_t extended[] = {0xE0,0x1D,0xE0,0x9D,0xE0,0x2A,0xE0,0x37,0xE0,0xB7,0xE0,0xAA,0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
    for (unsigned i = 0; i < sizeof(extended); i++) keyboard_byte(extended[i]);
    assert(read_events(0, &a) == 12); exact(0,1,97,1); exact(2,1,97,0); exact(4,1,99,1); exact(6,1,99,0); exact(8,1,119,1); exact(10,1,119,0);
    for (unsigned i = 0; i < 150; i++) { keyboard_byte(0x1E); keyboard_byte(0x9E); }
    keyboard_byte(0x30);
    assert(input_read(0, &a, (uint64_t)events, 24, true) == 24); exact(0,0,3,0);
    assert(input_read(0, &a, 1, 24, true) == -14 && a.report);
    uint8_t bits[96]; memset(bits, 0xFF, sizeof(bits));
    assert(input_ioctl(0, &a, 0x80604518, (uint64_t)bits) == 96 && (bits[6] & 1) && !(bits[3] & 64));
    assert(read_events(0, &a) == 1); exact(0,0,0,0); assert(!input_ready(0,&a));
    keyboard_byte(0); assert(read_events(0,&a) == 2); exact(0,0,3,0);
    assert(input_ioctl(0, &a, 0x80604518, (uint64_t)bits) == 96 && !(bits[6] & 1));
    mouse_type = 4; input_open(1, &a);
    mouse_byte(0); mouse_byte(0x29); mouse_byte(5); mouse_byte(253); mouse_byte(0x1F);
    assert(read_events(1,&a) == 6); exact(0,1,272,1); exact(1,1,275,1); exact(2,2,0,5); exact(3,2,1,3); exact(4,2,8,1);
    mouse_byte(0x18); mouse_byte(254); mouse_byte(4); mouse_byte(2);
    assert(read_events(1,&a) == 6); exact(0,1,272,0); exact(1,1,275,0); exact(2,2,0,-2); exact(3,2,1,-4); exact(4,2,8,-2);
    mouse_byte(0xC8); mouse_byte(44); mouse_byte(44); mouse_byte(0); assert(read_events(1,&a) == 1); exact(0,0,0,0);
    mouse_type = 0; mouse_byte(0x08); ticks += 100000001; mouse_byte(0x08); mouse_byte(1); mouse_byte(2);
    assert(read_events(1,&a) == 3); exact(0,2,0,1); exact(1,2,1,-2);
    assert(input_ioctl(1,&a,0x80604521,(uint64_t)bits) == 96 && (bits[34] & 7) == 7 && !(bits[34] & 24));
    assert(input_ioctl(1,&a,0x80604522,(uint64_t)bits) == 2 && bits[0] == 3 && !bits[1]);
    mouse_type = 4; assert(input_ioctl(1,&a,0x80604522,(uint64_t)bits) == 2 && bits[1] == 1);
    assert(input_ioctl(0,&a,0x80604521,(uint64_t)bits) == 96 && (bits[14] & 128) && !(bits[34] & 7));
    assert(input_ioctl(0,&a,0x80014506,(uint64_t)bits) == 1 && bits[0] == 'N');
    assert(input_ioctl(0,&a,0x80004506,1) == 0 && input_ioctl(0,&a,0x80044501,1) == -14);
    assert(input_ioctl(0,&a,0x80034501,(uint64_t)bits) == -25 && input_ioctl(0,&a,0x180044501ULL,(uint64_t)bits) == -25);
    clock = 2; assert(input_ioctl(0,&a,0x400445A0,(uint64_t)&clock) == -22);
    reply_read = 0; reply_count = 2; replies[0] = 0x1E; replies[1] = 0xAA; reply_status[0] = 0; reply_status[1] = 0x80;
    input_open(0,&a); input_irq(); assert(read_events(0,&a) == 4); exact(0,1,30,1); exact(2,0,3,0);
    assert(wakes > 300 && changes > 300);
    fsuid = 2000; assert(device_open_check(FRAMEBUFFER, 0) == -13 && device_open_check(KEYBOARD, 0) == -13);
    fsuid = 0; assert(!device_open_check(FRAMEBUFFER, 2) && !device_open_check(KEYBOARD, 0));
    fsuid = 1000; assert(!device_open_check(FRAMEBUFFER, 2) && !device_open_check(MOUSE, 0x800));
    assert(device_open_check(KEYBOARD, 2) == -13 && device_open_check(FRAMEBUFFER, 3) == -22 && device_open_check(INPUT_DIRECTORY, 2) == -21);
    assert(!device_open(FRAMEBUFFER, 2, &a) && display_refs == 1); device_close(FRAMEBUFFER, 2); assert(!display_refs);
    inputs[1].present = false; assert(device_open_check(MOUSE, 0) == -19);
    struct directory_item item; assert(device_directory(DIRECTORY, 7, &item) == 1 && !strcmp(item.name, "input") && item.type == 4 && !device_directory(DIRECTORY, 8, &item));
    assert(device_directory(INPUT_DIRECTORY, 2, &item) == 1 && !strcmp(item.name, "event0") && item.type == 2);
    assert(device_directory(INPUT_DIRECTORY, 3, &item) == 1 && !strcmp(item.name, "event1") && !device_directory(INPUT_DIRECTORY, 4, &item));
    struct linux_stat stat; device_stat(KEYBOARD, &stat); assert(stat.uid == 1000 && stat.mode == 0020400 && stat.rdev == 0xD40);
    puts("PASS native PS/2: deadlines/ACK retries, scan codes, buttons/wheel, IRQ errors, reader overflow recovery, clocks and ioctl validation");
}
