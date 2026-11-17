/* Aurora Network: the Wi-Fi page ("Wi-Fi" segment of `aurora-app network`) and `aurora-app network --wifi ...`.
 *
 * Wi-Fi is wpa_supplicant (Debian's, arch/x86_64/wifi_install.py) over the kernel's nl80211; this file only talks to
 * its control socket (the protocol wpa_cli speaks): STATUS, SCAN, SCAN_RESULTS, LIST_NETWORKS, ADD/SET/SELECT/
 * REMOVE_NETWORK, SAVE_CONFIG. A system wpa_supplicant (control directory /run/wpa_supplicant) is used when one runs;
 * otherwise the session starts its own for the first wl* interface with the saved networks of
 * /home/nexus/.aurora/wpa_supplicant-IFACE.conf (mode 0600: it holds the passphrases; update_config=1 so "Connect"
 * and "Forget" persist), control directory /tmp/aurora-wpa. `aurora-app network --apply` (the boot step) starts it when
 * saved networks exist, so they reconnect at boot.
 * Security: WPA2-PSK ("psk"), WPA3-SAE ("sae_password" via psk, ieee80211w=2), WPA2/WPA3 transition, open networks.
 * CLI (also what tests/test_x64_wifi.py drives): status | scan | saved | connect SSID [PASSPHRASE] | forget SSID.
 * The privileged part (starting wpa_supplicant, the nl80211 configuration it does) runs with the desktop owner's
 * network rights (netlink.c netlink_admin); sec2's privilege broker may take it over. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../kernel/appui.h"

#define OWN_DIR "/tmp/aurora-wpa"
#define SYSTEM_DIR "/run/wpa_supplicant"
#define CONF_FMT "/home/nexus/.aurora/wpa_supplicant-%s.conf"
#define MAX_NETS 12

struct wifi_net { char ssid[33], bssid[18], security[16]; int signal, freq; bool saved, current; };
static struct wifi_net nets[MAX_NETS]; static int net_count, selected = -1;
static char iface[IFNAMSIZ], state[32], current_ssid[33], current_bssid[18], wifi_message[160], password[64];
static int current_signal; static bool password_focus;
static unsigned serial;

static bool find_iface(void) {
    struct if_nameindex* list = if_nameindex(); iface[0] = 0;
    for (struct if_nameindex* i = list; list && i->if_index; i++) if (!strncmp(i->if_name, "wl", 2)) { snprintf(iface, sizeof(iface), "%s", i->if_name); break; }
    if (list) if_freenameindex(list);
    return iface[0] != 0;
}
static void ctrl_path(char* out, size_t size, const char* dir) { snprintf(out, size, "%s/%s", dir, iface); }
/* Only a control directory that root or we own and nobody else can write: the passphrases go through it */
static bool dir_trusted(const char* dir) {
    struct stat st; if (lstat(dir, &st) || !S_ISDIR(st.st_mode)) return false;
    return (st.st_uid == getuid() || st.st_uid == 0) && !(st.st_mode & 022);
}
static bool ctrl_exists(const char* dir) { char p[108]; ctrl_path(p, sizeof(p), dir); struct stat st; return dir_trusted(dir) && !stat(p, &st) && S_ISSOCK(st.st_mode); }
/* One request on wpa_supplicant's control socket; false when it does not answer. Unsolicited "<N>" events are skipped. */
static bool wpa(const char* command, char* reply, size_t size) {
    reply[0] = 0;
    const char* dir = ctrl_exists(SYSTEM_DIR) ? SYSTEM_DIR : OWN_DIR;
    if (!dir_trusted(dir)) return false;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return false;
    struct sockaddr_un local = {.sun_family = AF_UNIX}, peer = {.sun_family = AF_UNIX};
    snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/aurora-wpa-client-%d-%u", (int)getpid(), serial++);
    unlink(local.sun_path);
    ctrl_path(peer.sun_path, sizeof(peer.sun_path), dir);
    bool ok = !bind(fd, (struct sockaddr*)&local, sizeof(local)) && !connect(fd, (struct sockaddr*)&peer, sizeof(peer)) &&
              send(fd, command, strlen(command), 0) == (ssize_t)strlen(command);
    while (ok) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 4000) <= 0) { ok = false; break; }
        ssize_t n = recv(fd, reply, size - 1, 0); if (n < 0) { ok = false; break; }
        reply[n] = 0; if (reply[0] != '<') break;
    }
    close(fd); unlink(local.sun_path);
    return ok;
}
static bool wpa_ok(const char* command) { char r[64]; return wpa(command, r, sizeof(r)) && !strncmp(r, "OK", 2); }
static void conf_path(char* out, size_t size) { snprintf(out, size, CONF_FMT, iface); }
static bool conf_has_networks(void) {
    char path[128]; conf_path(path, sizeof(path)); FILE* f = fopen(path, "re"); if (!f) return false;
    char l[256]; bool any = false; while (fgets(l, sizeof(l), f)) if (!strncmp(l, "network={", 9)) any = true;
    fclose(f); return any;
}
/* A wpa_supplicant for the interface: the system's, or ours (started here) */
static bool ensure_supplicant(void) {
    if (!iface[0] && !find_iface()) { snprintf(wifi_message, sizeof(wifi_message), "No Wi-Fi adapter"); return false; }
    char r[256]; if (wpa("PING", r, sizeof(r)) && !strncmp(r, "PONG", 4)) return true;
    if (access("/usr/sbin/wpa_supplicant", X_OK)) { snprintf(wifi_message, sizeof(wifi_message), "wpa_supplicant is not installed (wifi_install.py)"); return false; }
    char path[128]; conf_path(path, sizeof(path));
    mkdir("/home", 0755); mkdir("/home/nexus", 0755); mkdir("/home/nexus/.aurora", 0700); mkdir(OWN_DIR, 0700);   /* (exist on a desktop) */
    if (!dir_trusted(OWN_DIR)) { snprintf(wifi_message, sizeof(wifi_message), "%s is not private; not using it", OWN_DIR); return false; }
    if (access(path, F_OK)) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) { const char* head = "ctrl_interface=" OWN_DIR "\nupdate_config=1\nsae_pwe=2\n"; ssize_t w = write(fd, head, strlen(head)); (void)w; close(fd); }
    }
    pid_t child = fork();
    if (!child) {
        char* args[] = {"/usr/sbin/wpa_supplicant", "-B", "-q", "-i", iface, "-D", "nl80211", "-c", path, NULL};
        char* env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL};
        umask(077);                                  /* SAVE_CONFIG rewrites the file: keep it private */
        execve(args[0], args, env); _exit(127);
    }
    if (child < 0) return false;
    int status = 0; waitpid(child, &status, 0);
    for (int i = 0; i < 40; i++) { if (wpa("PING", r, sizeof(r)) && !strncmp(r, "PONG", 4)) return true; usleep(100000); }
    snprintf(wifi_message, sizeof(wifi_message), "wpa_supplicant did not start (status %d)", status);
    return false;
}
static void security_of(const char* flags, char* out, size_t size) {
    bool sae = strstr(flags, "SAE") != NULL, psk = strstr(flags, "PSK") != NULL, eap = strstr(flags, "EAP") != NULL;
    snprintf(out, size, "%s", sae && psk ? "WPA2/WPA3" : sae ? "WPA3" : psk ? "WPA2" : eap ? "Enterprise" : strstr(flags, "WEP") ? "WEP" : "Open");
}
void aurora_wifi_refresh(void) {
    net_count = 0; state[0] = current_ssid[0] = current_bssid[0] = 0; current_signal = 0;
    if (!iface[0] && !find_iface()) { snprintf(wifi_message, sizeof(wifi_message), "No Wi-Fi adapter"); return; }
    static char r[8192];
    if (!wpa("STATUS", r, sizeof(r))) { snprintf(state, sizeof(state), "not running"); return; }
    for (char* l = strtok(r, "\n"); l; l = strtok(NULL, "\n")) {
        if (!strncmp(l, "wpa_state=", 10)) snprintf(state, sizeof(state), "%s", l + 10);
        else if (!strncmp(l, "ssid=", 5)) snprintf(current_ssid, sizeof(current_ssid), "%s", l + 5);
        else if (!strncmp(l, "bssid=", 6)) snprintf(current_bssid, sizeof(current_bssid), "%s", l + 6);
    }
    if (wpa("SIGNAL_POLL", r, sizeof(r))) { char* s = strstr(r, "RSSI="); if (s) current_signal = atoi(s + 5); }
    static char saved[4096]; if (!wpa("LIST_NETWORKS", saved, sizeof(saved))) saved[0] = 0;
    if (wpa("SCAN_RESULTS", r, sizeof(r))) {
        char* save = NULL;
        for (char* l = strtok_r(r, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
            char bssid[18], flags[200], ssid[64] = ""; int freq, signal;
            if (sscanf(l, "%17s %d %d %199s %63[^\n]", bssid, &freq, &signal, flags, ssid) < 4 || !strcmp(bssid, "bssid")) continue;
            if (!ssid[0]) continue;                                                         /* hidden */
            int k = -1; for (int i = 0; i < net_count; i++) if (!strcmp(nets[i].ssid, ssid)) k = i;
            if (k >= 0 && nets[k].signal >= signal) continue;                               /* the strongest BSS per SSID */
            if (k < 0) { if (net_count >= MAX_NETS) continue; k = net_count++; }
            struct wifi_net* n = &nets[k]; memset(n, 0, sizeof(*n));
            snprintf(n->ssid, sizeof(n->ssid), "%s", ssid); snprintf(n->bssid, sizeof(n->bssid), "%s", bssid);
            n->signal = signal; n->freq = freq; security_of(flags, n->security, sizeof(n->security));
        }
    }
    for (int i = 0; i < net_count; i++) {
        char needle[40]; snprintf(needle, sizeof(needle), "\t%s\t", nets[i].ssid);
        nets[i].saved = strstr(saved, needle) != NULL;
        nets[i].current = !strcmp(state, "COMPLETED") && !strcmp(nets[i].ssid, current_ssid);
    }
}
static int saved_id(const char* ssid) {
    static char r[4096]; if (!wpa("LIST_NETWORKS", r, sizeof(r))) return -1;
    for (char* l = strtok(r, "\n"); l; l = strtok(NULL, "\n")) { int id; char s[64]; if (sscanf(l, "%d\t%63[^\t]", &id, s) == 2 && !strcmp(s, ssid)) return id; }
    return -1;
}
static bool quoted_ok(const char* s) { for (; *s; s++) if (*s == '"' || *s == '\n' || *s == '\\') return false; return true; }
/* Save (or update) a network and connect to it */
static bool wifi_connect(const char* ssid, const char* secret, const char* security) {
    if (!ensure_supplicant()) return false;
    size_t sl = strlen(ssid), pl = secret ? strlen(secret) : 0;
    if (!sl || sl > 32 || !quoted_ok(ssid) || (secret && !quoted_ok(secret))) { snprintf(wifi_message, sizeof(wifi_message), "Invalid network name or password"); return false; }
    bool open = security && !strcmp(security, "Open");
    if (!open && (pl < 8 || pl > 63)) { snprintf(wifi_message, sizeof(wifi_message), "The password must have 8 to 63 characters"); return false; }
    int id = saved_id(ssid); char r[64], c[200];
    if (id < 0) { if (!wpa("ADD_NETWORK", r, sizeof(r)) || (id = atoi(r)) < 0 || r[0] < '0' || r[0] > '9') { snprintf(wifi_message, sizeof(wifi_message), "wpa_supplicant refused a new network"); return false; } }
    bool ok = true;
    snprintf(c, sizeof(c), "SET_NETWORK %d ssid \"%s\"", id, ssid); ok &= wpa_ok(c);
    const char* mgmt = open ? "NONE" : security && !strcmp(security, "WPA3") ? "SAE" : security && !strcmp(security, "WPA2") ? "WPA-PSK" : "WPA-PSK SAE";
    snprintf(c, sizeof(c), "SET_NETWORK %d key_mgmt %s", id, mgmt); ok &= wpa_ok(c);
    if (!open) {
        snprintf(c, sizeof(c), "SET_NETWORK %d psk \"%s\"", id, secret); ok &= wpa_ok(c);
        snprintf(c, sizeof(c), "SET_NETWORK %d ieee80211w %d", id, !strcmp(mgmt, "SAE") ? 2 : !strcmp(mgmt, "WPA-PSK SAE") ? 1 : 0); ok &= wpa_ok(c);
    }
    snprintf(c, sizeof(c), "SELECT_NETWORK %d", id); ok &= wpa_ok(c);
    wpa_ok("ENABLE_NETWORK all no-connect");     /* SELECT disables the others: keep them saved for auto-join */
    ok &= wpa_ok("SAVE_CONFIG");
    snprintf(wifi_message, sizeof(wifi_message), ok ? "Connecting to %s..." : "Could not configure %s", ssid);
    return ok;
}
static bool wifi_forget(const char* ssid) {
    if (!ensure_supplicant()) return false;
    int id = saved_id(ssid); if (id < 0) { snprintf(wifi_message, sizeof(wifi_message), "%s is not saved", ssid); return false; }
    char c[64]; snprintf(c, sizeof(c), "REMOVE_NETWORK %d", id);
    bool ok = wpa_ok(c) && wpa_ok("SAVE_CONFIG");
    snprintf(wifi_message, sizeof(wifi_message), ok ? "Forgot %s" : "Could not forget %s", ssid);
    return ok;
}
/* Boot step (aurora-app network --apply): reconnect to saved networks */
void aurora_wifi_boot(void) { if (find_iface() && conf_has_networks()) ensure_supplicant(); }

/* ---- CLI ---- */
int aurora_wifi_cli(int argc, char** argv) {
    const char* cmd = argc ? argv[0] : "status";
    if (!find_iface()) { puts("wifi: no Wi-Fi adapter"); return 1; }
    if (!strcmp(cmd, "status")) {
        aurora_wifi_refresh();
        printf("wifi iface=%s state=%s ssid=%s bssid=%s signal=%d\n", iface, state[0] ? state : "-", current_ssid, current_bssid, current_signal);
        return 0;
    }
    if (!strcmp(cmd, "scan")) {
        if (!ensure_supplicant()) { printf("wifi: %s\n", wifi_message); return 1; }
        wpa_ok("SCAN"); sleep(3); aurora_wifi_refresh();
        for (int i = 0; i < net_count; i++) printf("%s\t%d dBm\t%s\t%s%s\n", nets[i].ssid, nets[i].signal, nets[i].security, nets[i].saved ? "saved" : "-", nets[i].current ? " connected" : "");
        return 0;
    }
    if (!strcmp(cmd, "saved")) {
        if (!ensure_supplicant()) { printf("wifi: %s\n", wifi_message); return 1; }
        static char r[4096]; if (!wpa("LIST_NETWORKS", r, sizeof(r))) return 1;
        for (char* l = strtok(r, "\n"); l; l = strtok(NULL, "\n")) { int id; char s[64]; if (sscanf(l, "%d\t%63[^\t]", &id, s) == 2) printf("saved %s\n", s); }
        return 0;
    }
    if (!strcmp(cmd, "connect") && argc >= 2) {
        if (!ensure_supplicant()) { printf("wifi: %s\n", wifi_message); return 1; }
        wpa_ok("SCAN"); sleep(2); aurora_wifi_refresh();
        const char* security = argc >= 3 ? "WPA2/WPA3" : "Open";
        for (int i = 0; i < net_count; i++) if (!strcmp(nets[i].ssid, argv[1])) security = nets[i].security;
        if (!wifi_connect(argv[1], argc >= 3 ? argv[2] : NULL, security)) { printf("wifi: %s\n", wifi_message); return 1; }
        for (int i = 0; i < 40; i++) { aurora_wifi_refresh(); if (!strcmp(state, "COMPLETED") && !strcmp(current_ssid, argv[1])) break; usleep(500000); }
        printf("wifi connect %s security=%s state=%s bssid=%s signal=%d\n", argv[1], security, state, current_bssid, current_signal);
        return !strcmp(state, "COMPLETED") ? 0 : 1;
    }
    if (!strcmp(cmd, "forget") && argc >= 2) { bool ok = wifi_forget(argv[1]); printf("wifi: %s\n", wifi_message); return ok ? 0 : 1; }
    fputs("usage: aurora-app network --wifi status|scan|saved|connect SSID [PASSPHRASE]|forget SSID\n", stderr);
    return 2;
}

/* ---- the page ---- */
#define WROW 26
static int list_y, list_x, list_w, field_y, buttons_y, connect_x, forget_x, scan_x;
void aurora_wifi_draw(int x, int y, int w, int cy) {
    int cx = x + 18;
    char line[160];
    if (!iface[0]) snprintf(line, sizeof(line), "No Wi-Fi adapter");
    else if (!strcmp(state, "COMPLETED")) snprintf(line, sizeof(line), "%s: connected to %s (%d dBm)", iface, current_ssid, current_signal);
    else snprintf(line, sizeof(line), "%s: %s", iface, state[0] ? state : "not running");
    appui_px_text(cx, cy, line, APPUI_F_BODY, !strcmp(state, "COMPLETED") ? APPUI_GREEN : APPUI_TEXT); cy += 30;
    list_x = cx - 8; list_w = w - 20; list_y = cy - y;
    appui_px_card(list_x, cy - 6, list_w, WROW * (net_count ? net_count : 1) + 12, 10, APPUI_PANEL);
    if (!net_count) appui_px_text(cx, cy, "No networks found (Scan)", APPUI_F_SMALL, APPUI_SUBTLE);
    for (int i = 0; i < net_count; i++) {
        int ry = cy + i * WROW;
        if (i == selected) appui_px_row(list_x + 4, ry - 4, list_w - 8, WROW - 2, true);
        appui_px_text(cx, ry, nets[i].ssid, APPUI_F_BODY, APPUI_TEXT);
        snprintf(line, sizeof(line), "%s  %d dBm%s%s", nets[i].security, nets[i].signal, nets[i].saved ? "  saved" : "", nets[i].current ? "  connected" : "");
        appui_px_text_r(x + w - 30, ry + 2, line, APPUI_F_SMALL, nets[i].current ? APPUI_GREEN : APPUI_SUBTLE);
    }
    cy += WROW * (net_count ? net_count : 1) + 20;
    appui_px_text(cx, cy, "Password", APPUI_F_SMALL, APPUI_SUBTLE); cy += 18;
    char masked[64]; size_t n = strlen(password); if (n > sizeof(masked) - 1) n = sizeof(masked) - 1; memset(masked, '*', n); masked[n] = 0;
    field_y = cy - y; appui_px_field(cx, cy, w - 40, 28, masked, password_focus); cy += 40;
    buttons_y = cy - y; scan_x = 18; forget_x = w - 260; connect_x = w - 140;
    appui_px_button(x + scan_x, cy, 100, 30, "Scan", false);
    appui_px_button(x + forget_x, cy, 110, 30, "Forget", false);
    appui_px_button(x + connect_x, cy, 110, 30, "Connect", true);
    cy += 44;
    if (wifi_message[0]) appui_px_text(cx, cy, wifi_message, APPUI_F_SMALL, strstr(wifi_message, "not") || strstr(wifi_message, "Could") || strstr(wifi_message, "must") ? APPUI_ORANGE : APPUI_GREEN);
}
bool aurora_wifi_click(int px, int py) {
    password_focus = false;
    if (net_count && py >= list_y - 4 && py < list_y + net_count * WROW - 4 && px >= list_x - 0) { selected = (py - list_y + 4) / WROW; password[0] = 0; return true; }
    if (py >= field_y && py < field_y + 28) { password_focus = true; return true; }
    if (py >= buttons_y && py < buttons_y + 30) {
        if (px >= scan_x && px < scan_x + 100) { if (ensure_supplicant()) { wpa_ok("SCAN"); snprintf(wifi_message, sizeof(wifi_message), "Scanning..."); } return true; }
        if (selected >= 0 && selected < net_count && px >= connect_x && px < connect_x + 110) { wifi_connect(nets[selected].ssid, password, nets[selected].security); password[0] = 0; return true; }
        if (selected >= 0 && selected < net_count && px >= forget_x && px < forget_x + 110) { wifi_forget(nets[selected].ssid); return true; }
    }
    return false;
}
bool aurora_wifi_key(char k) {
    if (!password_focus) return false;
    size_t n = strlen(password);
    if (k == '\n') { if (selected >= 0 && selected < net_count) wifi_connect(nets[selected].ssid, password, nets[selected].security); password[0] = 0; password_focus = false; }
    else if (k == '\b' || k == 127) { if (n) password[n - 1] = 0; }
    else if ((unsigned char)k >= 32 && (unsigned char)k < 127 && n + 1 < sizeof(password)) { password[n] = k; password[n + 1] = 0; }
    return true;
}
