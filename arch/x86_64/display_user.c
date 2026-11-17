#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] DISPLAY FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static void waited(pid_t child, int expected) { int status; CHECK(waitpid(child, &status, 0) == child && status == expected); }
static void phase(const char* name) { printf("[64] INPUT %s\n", name); fflush(stdout); }
static void event_is(struct input_event* event, unsigned type, unsigned code, int value) {
    CHECK(event->type == type && event->code == code && event->value == value);
}
static void key_event(int fd, unsigned code, int value) {
    struct input_event events[2]; struct pollfd wait = {fd, POLLIN, 0};
    CHECK(poll(&wait, 1, 10000) == 1 && (wait.revents & POLLIN));
    CHECK(read(fd, events, sizeof(events)) == sizeof(events)); event_is(events, EV_KEY, code, value); event_is(events + 1, EV_SYN, SYN_REPORT, 0);
}
static bool held(int fd, unsigned code) {
    unsigned char bits[(KEY_MAX + 8) / 8] = {0}; CHECK(ioctl(fd, EVIOCGKEY(sizeof(bits)), bits) == sizeof(bits));
    return (bits[code / 8] & (1U << (code % 8))) != 0;
}
static volatile sig_atomic_t interrupted;
static void handler(int number) { interrupted = number; }
static void mouse_events(int fd, int x, int y, int wheel, unsigned button, int value) {
    int dx = 0, dy = 0, dz = 0; bool clicked = false;
    for (unsigned n = 0; n < 64; n++) {
        struct pollfd wait = {fd, POLLIN, 0}; struct input_event event;
        CHECK(poll(&wait, 1, 10000) == 1 && read(fd, &event, sizeof(event)) == sizeof(event));
        if (event.type == EV_REL) {
            if (event.code == REL_X) dx += event.value;
            else if (event.code == REL_Y) dy += event.value;
            else if (event.code == REL_WHEEL) dz += event.value;
            else CHECK(0);
        } else if (event.type == EV_KEY) { CHECK(event.code == button && event.value == value); clicked = true; }
        else { event_is(&event, EV_SYN, SYN_REPORT, 0); if (dx == x && dy == y && dz == wheel && clicked) return; }
    }
    CHECK(0);
}
static void input_tests(volatile uint32_t* fb, unsigned stride, unsigned width, unsigned height) {
    _Static_assert(sizeof(struct input_event) == 24, "system input ABI");
    int keyboard = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int mouse = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    int copy = open("/dev/input/event0", O_RDONLY | O_NONBLOCK), alias = dup(keyboard);
    CHECK(keyboard >= 0 && mouse >= 0 && copy >= 0 && alias >= 0);
    CHECK(open("/dev/input/event0", O_RDWR) == -1 && errno == EACCES);
    struct stat st; CHECK(!fstat(keyboard, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 13 && minor(st.st_rdev) == 64);
    int version, clock = CLOCK_MONOTONIC; struct input_id id; char name[80] = {0}; unsigned char bits[96] = {0};
    CHECK(!ioctl(keyboard, EVIOCGVERSION, &version) && version == EV_VERSION && !ioctl(mouse, EVIOCGID, &id) && id.bustype == BUS_I8042);
    CHECK(ioctl(keyboard, EVIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, "Nexus PS/2 Keyboard"));
    CHECK(ioctl(mouse, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) == sizeof(bits) && (bits[BTN_LEFT / 8] & 7) == 7);
    CHECK(ioctl(mouse, EVIOCGBIT(EV_REL, sizeof(bits)), bits) == 2 && (bits[0] & 3) == 3 && (bits[1] & 1));
    CHECK(!ioctl(keyboard, EVIOCSCLOCKID, &clock)); clock = CLOCK_PROCESS_CPUTIME_ID;
    CHECK(ioctl(keyboard, EVIOCSCLOCKID, &clock) == -1 && errno == EINVAL);
    CHECK(ioctl(keyboard, EVIOCGVERSION, (void*)1) == -1 && errno == EFAULT);
    CHECK(ioctl(keyboard, EVIOCGRAB, 1) == -1 && errno == ENOTTY);
    CHECK(lseek(keyboard, 0, SEEK_SET) == -1 && errno == ESPIPE);
    struct input_event events[2]; CHECK(read(keyboard, events, 23) == -1 && errno == EINVAL);
    CHECK(read(keyboard, events, sizeof(events)) == -1 && errno == EAGAIN);
    int epoll = epoll_create1(EPOLL_CLOEXEC); CHECK(epoll >= 0);
    struct epoll_event interest = {.events = EPOLLIN | EPOLLET, .data.u64 = 17}, ready;
    CHECK(!epoll_ctl(epoll, EPOLL_CTL_ADD, keyboard, &interest) && epoll_wait(epoll, &ready, 1, 0) == 0);
    phase("READY");
    CHECK(epoll_wait(epoll, &ready, 1, 10000) == 1 && (ready.events & EPOLLIN) && ready.data.u64 == 17);
    CHECK(read(keyboard, (void*)1, 24) == -1 && errno == EFAULT);
    CHECK(read(alias, events, sizeof(events)) == sizeof(events)); event_is(events, EV_KEY, KEY_A, 1);
    CHECK(events[0].time.tv_sec < 1700000000 && held(keyboard, KEY_A));
    CHECK(read(keyboard, events, sizeof(events)) == -1 && errno == EAGAIN && epoll_wait(epoll, &ready, 1, 0) == 0);
    CHECK(read(copy, events, sizeof(events)) == sizeof(events) && events[0].time.tv_sec >= 1700000000); event_is(events, EV_KEY, KEY_A, 1);
    for (unsigned y = 0; y < 16; y++) for (unsigned x = 0; x < 16; x++) fb[y * stride + x] = 0xFFFFFF;
    struct sigaction action = {.sa_handler = handler}; CHECK(!sigaction(SIGUSR1, &action, NULL));
    CHECK(!fcntl(keyboard, F_SETFL, 0)); pid_t parent = getpid(), child = fork(); CHECK(child >= 0);
    if (!child) { usleep(60000); CHECK(!kill(parent, SIGUSR1)); _exit(0); }
    CHECK(read(alias, events, sizeof(events)) == -1 && errno == EINTR && interrupted == SIGUSR1); waited(child, 0);
    phase("RELEASE READY"); key_event(keyboard, KEY_A, 0); CHECK(!held(keyboard, KEY_A));
    CHECK(!fcntl(keyboard, F_SETFL, O_NONBLOCK));
    phase("EXTENDED READY");
    const unsigned extended[] = {KEY_RIGHTCTRL, KEY_SYSRQ, KEY_PAUSE};
    for (unsigned i = 0; i < sizeof(extended) / sizeof(extended[0]); i++) { key_event(keyboard, extended[i], 1); key_event(keyboard, extended[i], 0); }
    phase("MOUSE READY"); mouse_events(mouse, 37, -19, 1, BTN_LEFT, 1); CHECK(held(mouse, BTN_LEFT));
    for (unsigned y = height / 2 - 19; y < height / 2 - 3; y++) for (unsigned x = width / 2 + 37; x < width / 2 + 53; x++) fb[y * stride + x] = 0xFF00FF;
    phase("MOUSE PRESS PASSED"); mouse_events(mouse, -5, 8, -1, BTN_LEFT, 0); CHECK(!held(mouse, BTN_LEFT));
    phase("OVERFLOW READY"); mouse_events(mouse, 0, 0, 0, BTN_RIGHT, 1);
    CHECK(read(keyboard, events, 24) == 24); event_is(events, EV_SYN, SYN_DROPPED, 0);
    CHECK(held(keyboard, KEY_B) && !held(keyboard, KEY_A));
    CHECK(read(keyboard, events, sizeof(events)) == 24); event_is(events, EV_SYN, SYN_REPORT, 0);
    CHECK(read(copy, events, sizeof(events)) == sizeof(events)); event_is(events, EV_SYN, SYN_DROPPED, 0); event_is(events + 1, EV_SYN, SYN_REPORT, 0);
    CHECK(read(alias, events, sizeof(events)) == -1 && errno == EAGAIN);
    phase("OVERFLOW RECOVERED"); key_event(keyboard, KEY_B, 0); mouse_events(mouse, 0, 0, 0, BTN_RIGHT, 0);
    CHECK(!held(keyboard, KEY_B) && !held(mouse, BTN_RIGHT));
    CHECK(!close(alias) && !close(copy) && !close(keyboard) && epoll_wait(epoll, &ready, 1, 0) == 0 && !close(epoll) && !close(mouse));
    phase("ALL PASSED: real IRQ input, blocking/signals, poll/epoll, independent opens, shared dup/fork and overflow recovery");
}
int main(void) {
    _Static_assert(sizeof(struct fb_fix_screeninfo) == 80 && sizeof(struct fb_var_screeninfo) == 160, "system framebuffer ABI");
    int ro = open("/dev/fb0", O_RDONLY | O_CLOEXEC); CHECK(ro >= 0);
    struct stat st; CHECK(!fstat(ro, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 29 && minor(st.st_rdev) == 0);
    struct fb_fix_screeninfo fix; struct fb_var_screeninfo var;
    CHECK(!ioctl(ro, FBIOGET_FSCREENINFO, &fix) && !ioctl(ro, FBIOGET_VSCREENINFO, &var));
    CHECK(var.xres >= 640 && var.yres >= 480 && var.bits_per_pixel == 32 && var.xres == var.xres_virtual && var.yres == var.yres_virtual);
    CHECK(fix.smem_len == fix.line_length * var.yres && !(fix.smem_len & 4095) && fix.line_length >= var.xres * 4);
    CHECK(fix.type == FB_TYPE_PACKED_PIXELS && fix.visual == FB_VISUAL_TRUECOLOR && !fix.mmio_len && !fix.accel);
    CHECK(var.red.offset == 16 && var.green.offset == 8 && !var.blue.offset && var.red.length == 8 && var.green.length == 8 && var.blue.length == 8);
    CHECK(ioctl(ro, FBIOGET_FSCREENINFO, (void*)1) == -1 && errno == EFAULT);
    CHECK(ioctl(ro, FBIOPUT_VSCREENINFO, &var) == -1 && errno == ENOTTY);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED && errno == EACCES);
    volatile uint32_t* readonly = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0); CHECK(readonly != MAP_FAILED);
    CHECK(mprotect((void*)readonly, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    pid_t child = fork(); CHECK(child >= 0); if (!child) { readonly[0] = 0; _exit(98); } waited(child, SIGSEGV);
    CHECK(!munmap((void*)readonly, 4096) && !close(ro));
    int fd = open("/dev/fb0", O_RDWR); CHECK(fd >= 0);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, fix.smem_len) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap(NULL, 8192, PROT_READ, MAP_SHARED, fd, fix.smem_len - 4096) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x7FFFFFFFFFFFF000LL) == MAP_FAILED && errno == EINVAL);
    volatile uint32_t* fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(fb != MAP_FAILED);
    volatile uint32_t* alias = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096); CHECK(alias != MAP_FAILED);
    CHECK(mprotect((void*)fb, fix.smem_len, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
    fb[1024] = 0x123456; CHECK(alias[0] == 0x123456);
    child = fork(); CHECK(child >= 0);
    if (!child) { alias[0] = 0xABCDEF; CHECK(!munmap((void*)fb, fix.smem_len) && !close(fd)); _exit(0); }
    waited(child, 0); CHECK(fb[1024] == 0xABCDEF);
    uint32_t pixel = 0x234567, readback = 0;
    CHECK(lseek(fd, 4096, SEEK_SET) == 4096 && write(fd, &pixel, 4) == 4 && alias[0] == pixel);
    CHECK(lseek(fd, -4, SEEK_CUR) == 4096 && read(fd, &readback, 4) == 4 && readback == pixel);
    CHECK(lseek(fd, -2, SEEK_END) == fix.smem_len - 2 && write(fd, &pixel, 4) == 2);
    CHECK(write(fd, &pixel, 4) == -1 && errno == ENOSPC && read(fd, &pixel, 4) == 0);
    CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, (void*)1, 4) == -1 && errno == EFAULT && lseek(fd, 0, SEEK_CUR) == 0);
    CHECK(!close(fd));
    CHECK(!mprotect((void*)alias, 4096, PROT_NONE) && !mprotect((void*)alias, 4096, PROT_READ | PROT_WRITE));
    CHECK(alias[0] == 0x234567 && !munmap((void*)alias, 4096));
    CHECK(!mprotect((char*)fb + 4096, 4096, PROT_READ) && !mprotect((char*)fb + 4096, 4096, PROT_READ | PROT_WRITE));
    CHECK(!madvise((char*)fb + 8192, 4096, MADV_DONTDUMP) && !madvise((char*)fb + 8192, 4096, MADV_DODUMP));
    CHECK(madvise((void*)fb, 4096, MADV_DONTNEED) == -1 && errno == EINVAL);
    child = fork(); CHECK(child >= 0); if (!child) { ((void (*)(void))fb)(); _exit(98); } waited(child, SIGSEGV);
    for (unsigned y = 0; y < var.yres; y++) for (unsigned x = 0; x < var.xres; x++)
        fb[y * (fix.line_length / 4) + x] = y < var.yres / 2 ? (x < var.xres / 2 ? 0x18C784 : 0x2878DD) : (x < var.xres / 2 ? 0xE59B35 : 0x101522);
    printf("[64] DISPLAY FRAME READY %ux%u\n", var.xres, var.yres); fflush(stdout);
    input_tests(fb, fix.line_length / 4, var.xres, var.yres);
    CHECK(mmap((void*)fb, 4096, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == (void*)fb);
    CHECK(fb[0] == 0); fb[0] = 55;
    CHECK(!munmap((void*)fb, fix.smem_len));
    child = fork(); CHECK(child >= 0); if (!child) { volatile uint32_t p = fb[1024]; (void)p; _exit(98); } waited(child, SIGSEGV);
    puts("[64] DISPLAY ALL PASSED: actual framebuffer, ABI, bounds, shared fork, offset aliases, NX, permissions and reclamation");
    return 0;
}
