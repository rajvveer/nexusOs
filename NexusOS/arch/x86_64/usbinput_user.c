/* /dev/input probe for tests/test_x64_usbnet.py: prints the evdev capabilities of the keyboard (event0) and
 * pointer (event1) nodes, then every event read from them, on the serial console. Exits 0 when Escape is
 * released, 1 after 300 s without it. Installed as /sbin/init on the test's private disk copy. */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static void capabilities(int fd, const char* node) {
    unsigned char types[4] = {0}, keys[KEY_MAX / 8 + 1] = {0}, relative[2] = {0}, absolute[8] = {0};
    char name[64] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 || ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) { printf("[64] USBINPUT %s ioctl failed errno=%d\n", node, errno); return; }
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative)), relative);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absolute)), absolute);
    unsigned key_count = 0;
    for (unsigned i = 0; i <= KEY_MAX; i++) key_count += (keys[i / 8] >> (i % 8)) & 1;
    printf("[64] USBINPUT CAPS %s name=\"%s\" ev=0x%02x keys=%u btn_left=%d rel=0x%02x%02x abs=0x%02x\n", node, name, types[0], key_count,
           (keys[BTN_LEFT / 8] >> (BTN_LEFT % 8)) & 1, relative[1], relative[0], absolute[0]);
    if (types[0] & (1 << EV_ABS)) {
        struct input_absinfo x, y;
        if (!ioctl(fd, EVIOCGABS(ABS_X), &x) && !ioctl(fd, EVIOCGABS(ABS_Y), &y))
            printf("[64] USBINPUT ABS %s x=%d..%d y=%d..%d\n", node, x.minimum, x.maximum, y.minimum, y.maximum);
    }
}
int main(void) {
    struct pollfd fds[2];
    const char* nodes[2] = {"/dev/input/event0", "/dev/input/event1"};
    for (int i = 0; i < 2; i++) {
        fds[i].fd = open(nodes[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC); fds[i].events = POLLIN;
        if (fds[i].fd < 0) { printf("[64] USBINPUT open %s failed errno=%d\n", nodes[i], errno); return 2; }
        int clock = CLOCK_MONOTONIC;
        if (ioctl(fds[i].fd, EVIOCSCLOCKID, &clock)) { printf("[64] USBINPUT clock ioctl failed\n"); return 2; }
        capabilities(fds[i].fd, i ? "pointer" : "keyboard");
    }
    printf("[64] USBINPUT READY\n"); fflush(stdout);
    struct timespec start, now; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec > 300) { printf("[64] USBINPUT TIMEOUT\n"); return 1; }
        if (poll(fds, 2, 1000) < 0 && errno != EINTR) { printf("[64] USBINPUT poll failed errno=%d\n", errno); return 3; }
        for (int i = 0; i < 2; i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event events[64]; ssize_t size;
            while ((size = read(fds[i].fd, events, sizeof(events))) > 0) {
                for (size_t k = 0; k < (size_t)size / sizeof(*events); k++) {
                    struct input_event* e = &events[k];
                    if (e->type == EV_SYN && e->code == SYN_REPORT) continue;
                    printf("[64] USBINPUT %s type=%u code=%u value=%d\n", i ? "pointer" : "keyboard", e->type, e->code, e->value);
                    if (!i && e->type == EV_KEY && e->code == KEY_ESC && e->value == 0) { printf("[64] USBINPUT DONE\n"); fflush(stdout); return 0; }
                }
                fflush(stdout);
            }
            if (size < 0 && errno != EAGAIN) { printf("[64] USBINPUT read failed errno=%d\n", errno); return 4; }
        }
    }
}
