/* nexus-xtest: a dependency-free X11 client (core protocol over the X socket) that draws a known pattern and logs
 * the input it receives, for checking an X server (Xwayland inside Aurora) end to end.
 *
 * Connects to $DISPLAY (":N" -> /tmp/.X11-unix/XN), creates a 400x300 window at 100,100 of the root window with a
 * dark-cyan background and four 100x100 squares (red, green, blue, white) along y=50, maps it and prints
 *   [64] XTEST READY window=0x.. root=WxH x=100 y=100
 * then one line per event: KEY press|release keycode=K keysym=0x.. state=S, BUTTON press|release button=B x=X y=Y,
 * MOTION x=X y=Y (at most every 8th), FOCUS in|out, EXPOSE. It exits on the keysym 'q' or when the server goes away. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int x;   /* connection */
static uint32_t id_base, id_mask, root, root_visual; static uint16_t root_w, root_h; static uint8_t min_key, max_key;
static uint32_t* keysyms; static unsigned per_key;
static void fail(const char* what) { printf("[64] XTEST FAIL %s errno=%d\n", what, errno); fflush(stdout); exit(1); }
static void send_all(const void* data, size_t size) {
    const uint8_t* p = data;
    while (size) { ssize_t n = write(x, p, size); if (n <= 0) { if (n < 0 && errno == EINTR) continue; fail("write"); } p += n; size -= (size_t)n; }
}
static void recv_all(void* data, size_t size) {
    uint8_t* p = data;
    while (size) { ssize_t n = read(x, p, size); if (n <= 0) { if (n < 0 && errno == EINTR) continue; printf("[64] XTEST SERVER GONE\n"); fflush(stdout); exit(n ? 1 : 0); } p += n; size -= (size_t)n; }
}
static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int connect_display(void) {
    const char* d = getenv("DISPLAY"); if (!d || d[0] != ':') fail("DISPLAY must be :N");
    int number = atoi(d + 1); struct sockaddr_un a = {.sun_family = AF_UNIX};
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (s < 0) fail("socket");
    snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/.X11-unix/X%d", number);
    if (connect(s, (struct sockaddr*)&a, sizeof(a))) {          /* Linux servers also listen in the abstract namespace */
        memset(a.sun_path, 0, sizeof(a.sun_path)); snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "/tmp/.X11-unix/X%d", number);
        if (connect(s, (struct sockaddr*)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(a.sun_path + 1)))) fail("connect");
    }
    return s;
}
/* MIT-MAGIC-COOKIE-1 for this display from $XAUTHORITY (entries: family, address, number, name, data; 16-bit BE lengths). */
static bool cookie(uint8_t out[16]) {
    const char* path = getenv("XAUTHORITY"); const char* d = getenv("DISPLAY"); if (!path || !d) return false;
    FILE* f = fopen(path, "rb"); if (!f) return false;
    uint8_t buffer[4096]; size_t size = fread(buffer, 1, sizeof(buffer), f), at = 0; fclose(f);
    while (at + 2 <= size) {
        const uint8_t* field[4]; unsigned length[4]; at += 2;
        for (int i = 0; i < 4; i++) {
            if (at + 2 > size) return false;
            length[i] = (unsigned)buffer[at] << 8 | buffer[at + 1]; at += 2;
            if (at + length[i] > size) return false;
            field[i] = buffer + at; at += length[i];
        }
        bool number = length[1] == strlen(d + 1) && !memcmp(field[1], d + 1, length[1]);
        if (number && length[2] == 18 && !memcmp(field[2], "MIT-MAGIC-COOKIE-1", 18) && length[3] == 16) { memcpy(out, field[3], 16); return true; }
    }
    return false;
}
static void setup(void) {
    uint8_t request[48] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0}; size_t size = 12; uint8_t secret[16];
    if (cookie(secret)) {   /* name "MIT-MAGIC-COOKIE-1" (18, padded to 20) + 16 bytes of data */
        put16(request + 6, 18); put16(request + 8, 16);
        memcpy(request + 12, "MIT-MAGIC-COOKIE-1", 18); memcpy(request + 32, secret, 16); size = 48;
    }
    send_all(request, size);
    uint8_t head[8]; recv_all(head, 8);
    size_t extra = (size_t)get16(head + 6) * 4; uint8_t* data = malloc(extra); if (!data) fail("malloc"); recv_all(data, extra);
    if (head[0] != 1) { printf("[64] XTEST FAIL setup refused: %.*s\n", head[1], (char*)data); exit(1); }
    id_base = get32(data + 4); id_mask = get32(data + 8);
    uint16_t vendor = get16(data + 16); uint8_t formats = data[21]; min_key = data[26]; max_key = data[27];
    const uint8_t* screen = data + 32 + ((vendor + 3) & ~3u) + 8u * formats;
    root = get32(screen); root_w = get16(screen + 20); root_h = get16(screen + 22); root_visual = get32(screen + 32);
    free(data);
}
static uint32_t next_id(void) { static uint32_t n; return id_base | ((++n) & id_mask); }
static void read_reply(uint8_t first[32]) {   /* skips events until the reply; returns its first 32 bytes */
    for (;;) {
        recv_all(first, 32);
        if (first[0] == 1) return;
        if (first[0] == 0) { printf("[64] XTEST FAIL X error code=%u major=%u\n", first[1], first[10]); fflush(stdout); exit(1); }
    }
}
static void keyboard_mapping(void) {
    uint8_t r[8] = {101, 0, 2, 0, min_key, (uint8_t)(max_key - min_key + 1), 0, 0}; send_all(r, 8);
    uint8_t first[32]; read_reply(first); per_key = first[1];
    size_t words = get32(first + 4); keysyms = malloc(words * 4 + 4); if (!keysyms) fail("malloc"); recv_all(keysyms, words * 4);
}
static uint32_t keysym_of(uint8_t keycode) { return keycode >= min_key && keycode <= max_key && per_key ? keysyms[(keycode - min_key) * per_key] : 0; }
static void fill(uint32_t window, uint32_t gc, uint32_t color, int16_t rx, int16_t ry, uint16_t w, uint16_t h) {
    uint8_t change[16] = {56, 0, 4, 0}; put32(change + 4, gc); put32(change + 8, 4); put32(change + 12, color); send_all(change, 16);
    uint8_t rect[20] = {70, 0, 5, 0}; put32(rect + 4, window); put32(rect + 8, gc);
    put16(rect + 12, (uint16_t)rx); put16(rect + 14, (uint16_t)ry); put16(rect + 16, w); put16(rect + 18, h); send_all(rect, 20);
}
static void draw(uint32_t window, uint32_t gc) {
    fill(window, gc, 0x008080, 0, 0, 400, 300);
    fill(window, gc, 0xFF0000, 0, 50, 100, 100); fill(window, gc, 0x00FF00, 100, 50, 100, 100);
    fill(window, gc, 0x0000FF, 200, 50, 100, 100); fill(window, gc, 0xFFFFFF, 300, 50, 100, 100);
}
int main(void) {
    x = connect_display(); setup(); keyboard_mapping();
    uint32_t window = next_id(), gc = next_id();
    uint8_t create[40] = {1, 0, 10, 0}; put32(create + 4, window); put32(create + 8, root);
    put16(create + 12, 100); put16(create + 14, 100); put16(create + 16, 400); put16(create + 18, 300); put16(create + 20, 0);
    put16(create + 22, 1); put32(create + 24, 0); put32(create + 28, 0x2 | 0x800); put32(create + 32, 0x008080);
    put32(create + 36, 0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x8000 | 0x20000 | 0x200000); send_all(create, 40);
    uint8_t gcreq[20] = {55, 0, 5, 0}; put32(gcreq + 4, gc); put32(gcreq + 8, window); put32(gcreq + 12, 4); put32(gcreq + 16, 0); send_all(gcreq, 20);
    uint8_t map[8] = {8, 0, 2, 0}; put32(map + 4, window); send_all(map, 8);
    unsigned motions = 0; bool ready = false;
    for (;;) {
        uint8_t e[32]; recv_all(e, 32);
        uint8_t code = e[0] & 0x7F;
        if (code == 0) { printf("[64] XTEST X error code=%u major=%u\n", e[1], e[10]); fflush(stdout); continue; }
        if (code == 1) { size_t more = (size_t)get32(e + 4) * 4; uint8_t skip[256]; while (more) { size_t n = more > sizeof(skip) ? sizeof(skip) : more; recv_all(skip, n); more -= n; } continue; }
        if (code == 12 || code == 19) {   /* Expose, MapNotify */
            draw(window, gc);
            if (!ready) {   /* no window manager: give the window the input focus once it is viewable */
                uint8_t focus[12] = {42, 2, 3, 0}; put32(focus + 4, window); put32(focus + 8, 0); send_all(focus, 12);
                ready = true; printf("[64] XTEST READY window=0x%x root=%ux%u x=100 y=100\n", window, root_w, root_h);
            }
            else if (code == 12) printf("[64] XTEST EXPOSE\n");
            fflush(stdout); continue;
        }
        if (code == 2 || code == 3) {
            uint32_t sym = keysym_of(e[1]);
            printf("[64] XTEST KEY %s keycode=%u keysym=0x%x state=0x%x\n", code == 2 ? "press" : "release", e[1], sym, get16(e + 28)); fflush(stdout);
            if (code == 3 && sym == 'q') { printf("[64] XTEST QUIT\n"); fflush(stdout); return 0; }
        } else if (code == 4 || code == 5) {
            printf("[64] XTEST BUTTON %s button=%u x=%d y=%d\n", code == 4 ? "press" : "release", e[1], (int16_t)get16(e + 24), (int16_t)get16(e + 26)); fflush(stdout);
        } else if (code == 6) {
            if (!(motions++ % 8)) { printf("[64] XTEST MOTION x=%d y=%d\n", (int16_t)get16(e + 24), (int16_t)get16(e + 26)); fflush(stdout); }
        } else if (code == 9 || code == 10) { printf("[64] XTEST FOCUS %s\n", code == 9 ? "in" : "out"); fflush(stdout); }
    }
}
