/* ============================================================================
 * NexusOS — HTTP Server — Phase 30
 * ============================================================================
 * Simple TCP-based HTTP/1.1 server on port 8080.
 * Serves a built-in HTML status page with system info.
 * Uses tcp_listen and non-blocking polling.
 * ============================================================================ */

#include "httpd.h"
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "rtc.h"
#include "memory.h"
#include "vga.h"
#include "string.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Server state */
static bool httpd_running = false;
static int  httpd_conn_idx = -1;  /* Which TCB index we're using */

/* --------------------------------------------------------------------------
 * httpd_init
 * -------------------------------------------------------------------------- */
void httpd_init(void) {
    httpd_running = false;
    httpd_conn_idx = -1;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("HTTP server initialized (port 8080)\n");
}

/* --------------------------------------------------------------------------
 * build_status_page: Generate HTML status page into buf
 * Returns length written.
 * -------------------------------------------------------------------------- */
static int build_status_page(char* buf, int max) {
    int pos = 0;
    char tmp[16];

    #define APPEND(s) do { \
        const char* _s = (s); \
        while (*_s && pos < max - 1) buf[pos++] = *_s++; \
    } while(0)

    APPEND("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n");
    APPEND("Connection: close\r\nServer: NexusOS/3.0\r\n\r\n");

    APPEND("<!DOCTYPE html><html><head><title>NexusOS</title>");
    APPEND("<style>body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;");
    APPEND("max-width:600px;margin:40px auto;padding:20px}");
    APPEND("h1{color:#0ff}h2{color:#8bf}");
    APPEND(".s{background:#16213e;padding:8px;margin:6px 0;border-radius:4px}");
    APPEND(".g{color:#0f0}.d{color:#888}");
    APPEND("</style></head><body>");

    APPEND("<h1>NexusOS v3.0</h1>");
    APPEND("<p>The Hybrid Operating System</p><h2>Status</h2>");

    /* Uptime */
    APPEND("<div class='s'><span class='d'>Uptime: </span>");
    uint32_t secs = system_ticks / 18;
    uint32_t mins = secs / 60; secs %= 60;
    int_to_str(mins, tmp); APPEND(tmp); APPEND("m ");
    int_to_str(secs, tmp); APPEND(tmp); APPEND("s</div>");

    /* Time */
    rtc_time_t t; rtc_read(&t);
    char tb[9]; rtc_format_time(&t, tb);
    APPEND("<div class='s'><span class='d'>Time: </span>");
    APPEND(tb); APPEND("</div>");

    /* Memory */
    APPEND("<div class='s'><span class='d'>Memory: </span>");
    int_to_str(pmm_get_free_pages() * 4, tmp); APPEND(tmp);
    APPEND(" KB free / ");
    int_to_str(pmm_get_total_pages() * 4, tmp); APPEND(tmp);
    APPEND(" KB</div>");

    /* Network */
    APPEND("<h2>Network</h2>");
    const net_config_t* cfg = ip_get_config();
    char ip_str[16];
    APPEND("<div class='s'><span class='d'>IP: </span>");
    ip_to_string(cfg->ip, ip_str); APPEND(ip_str); APPEND("</div>");
    APPEND("<div class='s'><span class='d'>GW: </span>");
    ip_to_string(cfg->gateway, ip_str); APPEND(ip_str); APPEND("</div>");

    APPEND("<p class='g'>Server on port 8080</p></body></html>");
    #undef APPEND

    buf[pos] = '\0';
    return pos;
}

/* --------------------------------------------------------------------------
 * httpd_start
 * -------------------------------------------------------------------------- */
void httpd_start(void) {
    if (httpd_running) return;

    tcp_conn_t* conn = tcp_listen(8080);
    if (!conn) {
        vga_print_color("  HTTPD: No free TCP slots\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }

    httpd_running = true;

    /* Find which index this connection is */
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    httpd_conn_idx = -1;
    for (int i = 0; i < count; i++) {
        if (&all[i] == conn) { httpd_conn_idx = i; break; }
    }

    vga_print_color("  HTTPD: Listening on port 8080\n",
                    VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * httpd_stop
 * -------------------------------------------------------------------------- */
void httpd_stop(void) {
    if (!httpd_running) return;

    if (httpd_conn_idx >= 0) {
        int count;
        const tcp_conn_t* all = tcp_get_connections(&count);
        if (httpd_conn_idx < count && all[httpd_conn_idx].active) {
            tcp_close((tcp_conn_t*)&all[httpd_conn_idx]);
        }
    }

    httpd_running = false;
    httpd_conn_idx = -1;
    vga_print_color("  HTTPD: Stopped\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * httpd_poll: Check for incoming data and respond
 * -------------------------------------------------------------------------- */
void httpd_poll(void) {
    if (!httpd_running || httpd_conn_idx < 0) return;

    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    if (httpd_conn_idx >= count) return;

    tcp_conn_t* conn = (tcp_conn_t*)&all[httpd_conn_idx];
    if (!conn->active) {
        /* Connection was cleaned up, re-listen */
        tcp_conn_t* nc = tcp_listen(8080);
        if (nc) {
            for (int i = 0; i < count; i++) {
                if (&all[i] == nc) { httpd_conn_idx = i; break; }
            }
        } else {
            httpd_running = false;
            httpd_conn_idx = -1;
        }
        return;
    }

    /* If connected and have request data, send response */
    if (conn->connected && conn->rx_len > 0) {
        char page[1536];
        int plen = build_status_page(page, sizeof(page));
        tcp_send(conn, page, plen);
        tcp_close(conn);
        /* Connection will transition to FIN/TIME_WAIT, then become inactive.
         * On next poll when it's inactive, we'll re-listen. */
    }
}

/* --------------------------------------------------------------------------
 * httpd_is_running
 * -------------------------------------------------------------------------- */
bool httpd_is_running(void) {
    return httpd_running;
}
