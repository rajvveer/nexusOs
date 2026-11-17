/* Aurora menu-bar network status (desktop.elf): an icon left of the search item that shows whether the machine is
 * online (a running interface with an address and a default route), grey when offline, plus a "VPN" badge while a
 * WireGuard or TUN interface is up. Clicking it opens the Network settings (aurora-app network, desktop action 47).
 * The desktop also runs `aurora-app network --apply` once at startup so the saved settings (static address, DNS,
 * firewall) take effect at every boot. State is sampled at most every 2 s from /proc/net and interface ioctls; a
 * change asks the desktop to redraw the menu bar. Wired by one-line hooks in desktop.c and kernel/menubar.c. */
#define _GNU_SOURCE
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../../kernel/macui.h"
#include "aurora_netstatus.h"

static struct { bool online, vpn; char address[48]; } state, shown;
static unsigned long long sampled_ms;
static unsigned long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000; }

static void sample(void) {
    state.online = state.vpn = false; state.address[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (s < 0) return;
    bool default4 = false, default6 = false;
    FILE* f = fopen("/proc/net/route", "r");
    if (f) { char l[256]; while (fgets(l, sizeof(l), f)) { char ifn[32]; unsigned dst; if (sscanf(l, "%31s %x", ifn, &dst) == 2 && !dst && strcmp(ifn, "Iface")) default4 = true; } fclose(f); }
    f = fopen("/proc/net/ipv6_route", "r");
    if (f) { char l[512]; while (fgets(l, sizeof(l), f)) if (!strncmp(l, "00000000000000000000000000000000 00", 35) && !strstr(l, " lo\n")) default6 = true; fclose(f); }
    f = fopen("/proc/net/dev", "r");
    if (f) {
        char l[512];
        while (fgets(l, sizeof(l), f)) {
            char* colon = strchr(l, ':'); if (!colon) continue; *colon = 0;
            char* name = l; while (*name == ' ') name++;
            if (!strcmp(name, "lo")) continue;
            struct ifreq q; memset(&q, 0, sizeof(q)); snprintf(q.ifr_name, sizeof(q.ifr_name), "%s", name);
            if (ioctl(s, SIOCGIFFLAGS, &q) || !(q.ifr_flags & IFF_UP)) continue;
            bool tunnel = !strncmp(name, "wg", 2) || !strncmp(name, "tun", 3) || !strncmp(name, "tap", 3);
            if (tunnel) { state.vpn = true; continue; }
            if (!(q.ifr_flags & IFF_RUNNING)) continue;
            if (!ioctl(s, SIOCGIFADDR, &q) && !state.address[0]) {
                unsigned char* a = (unsigned char*)&((struct sockaddr_in*)&q.ifr_addr)->sin_addr;
                snprintf(state.address, sizeof(state.address), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
            }
            if (default4 || default6) state.online = true;
        }
        fclose(f);
    }
    close(s);
}
bool aurora_netstatus_poll(void) {
    unsigned long long now = now_ms(); if (now - sampled_ms < 2000) return false;
    sampled_ms = now; sample();
    bool changed = state.online != shown.online || state.vpn != shown.vpn;
    shown = state; return changed;
}
/* Draw the item ending at `right` (vertically centred on cy); returns its left edge. */
int aurora_netstatus_draw(int right, int cy) {
    if (!sampled_ms) { sampled_ms = now_ms(); sample(); shown = state; }
    int x = right - 22;
    unsigned color = shown.online ? 0xFFFFFF : UI_RGB(120, 120, 126);
    /* three ascending bars (a wired/any-network glyph) */
    for (int i = 0; i < 3; i++) ui_rrect(x + 2 + i * 6, cy + 5 - (i + 1) * 4, 4, (i + 1) * 4, 1, color, shown.online || i == 0 ? 255 : 150);
    if (!shown.online) ui_line(x + 1, cy + 6, x + 18, cy - 8, 2, UI_RGB(255, 69, 58), 255);
    if (shown.vpn) {
        int w = ui_text_width("VPN", UF_SMALL) + 8; x -= w + 4;
        ui_rrect(x, cy - 7, w, 15, 4, UI_RGB(10, 132, 255), 255);
        ui_text(x + 4, cy - 6, "VPN", UF_SMALL, 0xFFFFFF);
    }
    return x;
}
/* One boot-time run of the saved network settings (fire and forget; the desktop reaps it). */
void aurora_netstatus_boot(void) {
    if (access("/bin/aurora-app", X_OK)) return;
    pid_t child = fork();
    if (!child) { execl("/bin/aurora-app", "aurora-app", "network", "--apply", (char*)NULL); _exit(127); }
    if (child > 0) { printf("[64] AURORA NETWORK APPLY pid=%ld\n", (long)child); fflush(stdout); }
}
