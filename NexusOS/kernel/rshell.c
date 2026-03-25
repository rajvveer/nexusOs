/* ============================================================================
 * NexusOS — Remote Shell — Phase 30
 * ============================================================================
 * Telnet-style remote shell on TCP port 2323.
 * Accepts one connection at a time, provides basic commands.
 * Non-blocking via rshell_poll().
 * ============================================================================ */

#include "rshell.h"
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "rtc.h"
#include "memory.h"
#include "heap.h"
#include "vga.h"
#include "string.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* State */
static bool rshell_running = false;
static int  rs_conn_idx = -1;
static char rs_cmdbuf[RSHELL_MAX_CMD];
static int  rs_cmdlen = 0;
static bool rs_banner_sent = false;

/* --------------------------------------------------------------------------
 * get_conn: Get our TCP connection pointer safely
 * -------------------------------------------------------------------------- */
static tcp_conn_t* get_conn(void) {
    if (rs_conn_idx < 0) return NULL;
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    if (rs_conn_idx >= count) return NULL;
    tcp_conn_t* c = (tcp_conn_t*)&all[rs_conn_idx];
    if (!c->active) return NULL;
    return c;
}

/* --------------------------------------------------------------------------
 * rs_send: Send a string to the connected client
 * -------------------------------------------------------------------------- */
static void rs_send(const char* s) {
    tcp_conn_t* c = get_conn();
    if (c && c->connected) {
        tcp_send(c, s, strlen(s));
    }
}

/* --------------------------------------------------------------------------
 * rs_exec: Execute a remote command
 * -------------------------------------------------------------------------- */
static void rs_exec(const char* cmd) {
    char buf[200];
    char tmp[16];

    if (strcmp(cmd, "help") == 0) {
        rs_send("  help about uptime date mem ifconfig exit\r\n");
    }
    else if (strcmp(cmd, "about") == 0) {
        rs_send("  NexusOS v3.0 Remote Shell\r\n");
    }
    else if (strcmp(cmd, "uptime") == 0) {
        uint32_t secs = system_ticks / 18;
        uint32_t mins = secs / 60; secs %= 60;
        strcpy(buf, "  ");
        int_to_str(mins, tmp); strcat(buf, tmp);
        strcat(buf, "m ");
        int_to_str(secs, tmp); strcat(buf, tmp);
        strcat(buf, "s\r\n");
        rs_send(buf);
    }
    else if (strcmp(cmd, "date") == 0) {
        rtc_time_t t; rtc_read(&t);
        char tb[9], db[11];
        rtc_format_time(&t, tb); rtc_format_date(&t, db);
        strcpy(buf, "  "); strcat(buf, db);
        strcat(buf, " "); strcat(buf, tb);
        strcat(buf, "\r\n");
        rs_send(buf);
    }
    else if (strcmp(cmd, "mem") == 0) {
        strcpy(buf, "  Free: ");
        int_to_str(pmm_get_free_pages() * 4, tmp); strcat(buf, tmp);
        strcat(buf, " KB / ");
        int_to_str(pmm_get_total_pages() * 4, tmp); strcat(buf, tmp);
        strcat(buf, " KB\r\n");
        rs_send(buf);
    }
    else if (strcmp(cmd, "ifconfig") == 0) {
        const net_config_t* cfg = ip_get_config();
        char ip[16];
        ip_to_string(cfg->ip, ip);
        strcpy(buf, "  IP: "); strcat(buf, ip); strcat(buf, "\r\n");
        rs_send(buf);
        ip_to_string(cfg->gateway, ip);
        strcpy(buf, "  GW: "); strcat(buf, ip); strcat(buf, "\r\n");
        rs_send(buf);
    }
    else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        rs_send("  Bye!\r\n");
        tcp_conn_t* c = get_conn();
        if (c) tcp_close(c);
        rs_cmdlen = 0;
        rs_banner_sent = false;
        return;
    }
    else if (cmd[0] != '\0') {
        rs_send("  Unknown: "); rs_send(cmd); rs_send("\r\n");
    }

    rs_send("nexus> ");
}

/* --------------------------------------------------------------------------
 * rshell_init
 * -------------------------------------------------------------------------- */
void rshell_init(void) {
    rshell_running = false;
    rs_conn_idx = -1;
    rs_cmdlen = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Remote shell initialized (port 2323)\n");
}

/* --------------------------------------------------------------------------
 * rshell_start
 * -------------------------------------------------------------------------- */
void rshell_start(void) {
    if (rshell_running) return;

    tcp_conn_t* conn = tcp_listen(RSHELL_PORT);
    if (!conn) {
        vga_print_color("  RSHELL: No free TCP slots\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }

    rshell_running = true;
    rs_cmdlen = 0;
    rs_banner_sent = false;

    /* Find index */
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    rs_conn_idx = -1;
    for (int i = 0; i < count; i++) {
        if (&all[i] == conn) { rs_conn_idx = i; break; }
    }

    vga_print_color("  RSHELL: Listening on port 2323\n",
                    VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * rshell_stop
 * -------------------------------------------------------------------------- */
void rshell_stop(void) {
    if (!rshell_running) return;

    tcp_conn_t* c = get_conn();
    if (c) tcp_close(c);

    rshell_running = false;
    rs_conn_idx = -1;
    rs_cmdlen = 0;
    rs_banner_sent = false;

    vga_print_color("  RSHELL: Stopped\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * rshell_poll: Check for connections and process input
 * -------------------------------------------------------------------------- */
void rshell_poll(void) {
    if (!rshell_running || rs_conn_idx < 0) return;

    tcp_conn_t* conn = get_conn();

    if (!conn) {
        /* Connection was cleaned up, try to re-listen */
        tcp_conn_t* nc = tcp_listen(RSHELL_PORT);
        if (nc) {
            int count;
            const tcp_conn_t* all = tcp_get_connections(&count);
            for (int i = 0; i < count; i++) {
                if (&all[i] == nc) { rs_conn_idx = i; break; }
            }
            rs_banner_sent = false;
            rs_cmdlen = 0;
        } else {
            rshell_running = false;
            rs_conn_idx = -1;
        }
        return;
    }

    /* Send banner on new connection */
    if (conn->connected && !rs_banner_sent) {
        rs_send("\r\nNexusOS v3.0 Remote Shell\r\nType 'help'\r\n\r\nnexus> ");
        rs_banner_sent = true;
        return;
    }

    /* Process input */
    if (conn->connected && conn->rx_len > 0) {
        for (int i = 0; i < conn->rx_len && rs_cmdlen < RSHELL_MAX_CMD - 1; i++) {
            char c = conn->rx_buf[i];
            if (c == '\n' || c == '\r') {
                rs_cmdbuf[rs_cmdlen] = '\0';
                rs_send("\r\n");
                rs_exec(rs_cmdbuf);
                rs_cmdlen = 0;
            } else if (c >= 32 && c < 127) {
                rs_cmdbuf[rs_cmdlen++] = c;
            }
        }
        conn->rx_len = 0;
    }
}

/* --------------------------------------------------------------------------
 * rshell_is_running
 * -------------------------------------------------------------------------- */
bool rshell_is_running(void) {
    return rshell_running;
}
