/* ============================================================================
 * NexusOS — Remote Shell — Phase 30
 * ============================================================================
 * Telnet-style remote shell on TCP port 2323.
 * Accepts one connection at a time, provides basic commands.
 * Non-blocking via rshell_poll().
 * ============================================================================ */

#include "rshell.h"
#include "version.h"
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "rtc.h"
#include "memory.h"
#include "heap.h"
#include "vga.h"
#include "string.h"
#include "users.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* State */
static bool rshell_running = false;
static int  rs_conn_idx = -1;
static uint32_t rs_conn_gen = 0;   /* Phase 55: TCB slot-reuse guard */
static char rs_cmdbuf[RSHELL_MAX_CMD];
static int  rs_cmdlen = 0;
static bool rs_banner_sent = false;

/* Phase 54: per-connection authentication. A client MUST log in with the OS's
 * own account database before any command runs:
 *     LOGIN <user> <pass>         (one line), or
 *     USER <name>  then  PASS <pass>
 * On success the session runs as that uid, so file/permission checks apply. */
static bool     rs_authed = false;
static uint32_t rs_uid = UID_INVALID;
static char     rs_pending_user[USER_NAME_MAX];
static bool     rs_have_user = false;

static void rs_reset_auth(void) {
    rs_authed = false;
    rs_uid = UID_INVALID;
    rs_have_user = false;
    rs_pending_user[0] = '\0';
}

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
    if (c->gen != rs_conn_gen) return NULL;   /* slot recycled for someone else */
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
        rs_send("  " NEXUS_VERSION_STRING " Remote Shell\r\n");
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
    else if (strcmp(cmd, "whoami") == 0) {
        rs_send("  "); rs_send(users_name_of(rs_uid)); rs_send("\r\n");
    }
    else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        rs_send("  Bye!\r\n");
        tcp_conn_t* c = get_conn();
        if (c) tcp_close(c);
        rs_cmdlen = 0;
        rs_banner_sent = false;
        rs_reset_auth();
        return;
    }
    else if (cmd[0] != '\0') {
        rs_send("  Unknown: "); rs_send(cmd); rs_send("\r\n");
    }

    rs_send("nexus> ");
}

/* --------------------------------------------------------------------------
 * rs_try_auth: handle the pre-login handshake (LOGIN / USER / PASS)
 * -------------------------------------------------------------------------- */
static void rs_finish_login(const char* user, const char* pass) {
    uint32_t uid = users_authenticate(user, pass);
    rs_have_user = false;
    rs_pending_user[0] = '\0';
    if (uid == UID_INVALID) {
        rs_send("  530 Login incorrect\r\nlogin: ");
        return;
    }
    rs_authed = true;
    rs_uid = uid;
    rs_send("  230 Logged in as ");
    rs_send(users_name_of(uid));
    rs_send("\r\nType 'help'.\r\nnexus> ");
}

static void rs_try_auth(const char* line) {
    /* skip leading spaces */
    while (*line == ' ') line++;

    if (strncmp(line, "LOGIN ", 6) == 0) {
        char user[USER_NAME_MAX]; int ui = 0;
        const char* p = line + 6;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && ui < USER_NAME_MAX - 1) user[ui++] = *p++;
        user[ui] = '\0';
        while (*p == ' ') p++;
        rs_finish_login(user, p);          /* rest of line = password */
        return;
    }
    if (strncmp(line, "USER ", 5) == 0) {
        const char* p = line + 5;
        while (*p == ' ') p++;
        strncpy(rs_pending_user, p, USER_NAME_MAX - 1);
        rs_pending_user[USER_NAME_MAX - 1] = '\0';
        rs_have_user = true;
        rs_send("  331 Password required\r\nPASS: ");
        return;
    }
    if (strncmp(line, "PASS ", 5) == 0) {
        if (!rs_have_user) { rs_send("  503 Send USER first\r\nlogin: "); return; }
        const char* p = line + 5;
        while (*p == ' ') p++;
        rs_finish_login(rs_pending_user, p);
        return;
    }
    if (line[0] != '\0')
        rs_send("  530 Please log in: LOGIN <user> <pass>\r\nlogin: ");
}

/* Dispatch one received line: authenticate first, then run commands as the
 * authenticated uid so file/permission checks apply. */
static void rs_handle_line(const char* line) {
    if (!rs_authed) { rs_try_auth(line); return; }
    uint32_t prev = users_current_uid();
    users_set_current(rs_uid);
    rs_exec(line);
    users_set_current(prev);
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
    rs_reset_auth();

    /* Find index */
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    rs_conn_idx = -1;
    for (int i = 0; i < count; i++) {
        if (&all[i] == conn) { rs_conn_idx = i; break; }
    }
    rs_conn_gen = conn->gen;

    vga_print_color("  RSHELL: Listening on port 2323\n",
                    VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print_color("  RSHELL: login required (LOGIN <user> <pass>) against the OS user DB\n",
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
    rs_reset_auth();

    vga_print_color("  RSHELL: Stopped\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * rshell_poll: Check for connections and process input
 * -------------------------------------------------------------------------- */
/* Re-arm the rshell listener in a free TCB slot (adopts a client that
 * connected while we were between listeners) and reset the session. */
static bool rshell_relisten(void) {
    tcp_conn_t* nc = tcp_listen(RSHELL_PORT);
    if (!nc) { rshell_running = false; rs_conn_idx = -1; return false; }
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    for (int i = 0; i < count; i++)
        if (&all[i] == nc) { rs_conn_idx = i; break; }
    rs_conn_gen = nc->gen;
    rs_banner_sent = false;
    rs_cmdlen = 0;
    rs_reset_auth();
    return true;
}

void rshell_poll(void) {
    if (!rshell_running || rs_conn_idx < 0) return;

    tcp_conn_t* conn = get_conn();

    if (!conn) {
        /* TCB slot freed (connection fully closed) — re-arm the listener. */
        rshell_relisten();
        return;
    }

    /* Phase 55: the client closed (or our close progressed past ESTABLISHED,
     * e.g. after 'exit') — tear our side down and re-listen immediately so the
     * next client isn't dropped into the gap. rshell previously only relistened
     * once the TCB went fully inactive, which left a window where a prompt
     * reconnect was never served (no banner). */
    if (conn->closed || (conn->connected && conn->state != TCP_ESTABLISHED)) {
        tcp_close(conn);
        rshell_relisten();
        return;
    }

    /* Send banner on new connection */
    if (conn->connected && !rs_banner_sent) {
        rs_send("\r\n" NEXUS_VERSION_STRING " Remote Shell\r\n"
                "Login required: LOGIN <user> <pass>  (or USER then PASS)\r\nlogin: ");
        rs_banner_sent = true;
        rs_reset_auth();
        return;
    }

    /* Process input */
    if (conn->connected && conn->rx_len > 0) {
        /* Copy out via tcp_recv (IRQ-safe vs the timer-IRQ net_poll), then
         * process the local copy. */
        char in[TCP_RX_BUF_SIZE];
        int n = tcp_recv(conn, in, sizeof(in));
        for (int i = 0; i < n && rs_cmdlen < RSHELL_MAX_CMD - 1; i++) {
            char c = in[i];
            if (c == '\n' || c == '\r') {
                rs_cmdbuf[rs_cmdlen] = '\0';
                rs_send("\r\n");
                rs_handle_line(rs_cmdbuf);
                rs_cmdlen = 0;
            } else if (c >= 32 && c < 127) {
                rs_cmdbuf[rs_cmdlen++] = c;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * rshell_is_running
 * -------------------------------------------------------------------------- */
bool rshell_is_running(void) {
    return rshell_running;
}
