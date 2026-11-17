/* The Aurora Network app's Wi-Fi logic (arch/x86_64/aurora_wifi.c) without the desktop: the same control-socket client,
 * saved-network handling and boot reconnection, linked with no-op drawing calls so tests/test_x64_wifi.py can run it on a
 * headless disk. Usage: aurora_wifi_cli status|scan|saved|connect SSID [PASSPHRASE]|forget SSID|--boot */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
int aurora_wifi_cli(int argc, char** argv);
void aurora_wifi_boot(void);
void appui_px_card(int x, int y, int w, int h, int r, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; }
int appui_px_text(int x, int y, const char* s, int f, uint32_t c) { (void)x; (void)y; (void)s; (void)f; (void)c; return 0; }
void appui_px_text_r(int x, int y, const char* s, int f, uint32_t c) { (void)x; (void)y; (void)s; (void)f; (void)c; }
void appui_px_row(int x, int y, int w, int h, bool s) { (void)x; (void)y; (void)w; (void)h; (void)s; }
void appui_px_field(int x, int y, int w, int h, const char* t, bool c) { (void)x; (void)y; (void)w; (void)h; (void)t; (void)c; }
void appui_px_button(int x, int y, int w, int h, const char* l, bool p) { (void)x; (void)y; (void)w; (void)h; (void)l; (void)p; }
int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--boot")) { aurora_wifi_boot(); return 0; }
    return aurora_wifi_cli(argc - 1, argv + 1);
}
