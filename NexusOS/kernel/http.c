/* ============================================================================
 * NexusOS — HTTP Client — Phase 28
 * ============================================================================
 * Simple HTTP/1.1 GET client: parses URL, resolves DNS, connects via TCP,
 * sends GET request, receives and parses response.
 * ============================================================================ */

#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "ip.h"
#include "string.h"
#include "vga.h"
#include "keyboard.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Ephemeral port counter for HTTP */
static uint16_t http_port = 50080;

/* --------------------------------------------------------------------------
 * http_init: Initialize HTTP client
 * -------------------------------------------------------------------------- */
void http_init(void) {
    http_port = 50080;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("HTTP client initialized\n");
}

/* --------------------------------------------------------------------------
 * parse_url: Extract host, port, path from URL
 * Format: "http://host[:port]/path"
 * Returns true on success.
 * -------------------------------------------------------------------------- */
static bool parse_url(const char* url, char* host, uint16_t* port, char* path) {
    /* Skip "http://" */
    const char* p = url;
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == ':' && p[5] == '/' && p[6] == '/') {
        p += 7;
    }

    /* Extract host (until ':', '/', or end) */
    int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < HTTP_MAX_HOST - 1) {
        host[hi++] = *p++;
    }
    host[hi] = '\0';
    if (hi == 0) return false;

    /* Optional port */
    *port = HTTP_DEFAULT_PORT;
    if (*p == ':') {
        p++;
        uint16_t pt = 0;
        while (*p >= '0' && *p <= '9') {
            pt = pt * 10 + (*p++ - '0');
        }
        if (pt > 0) *port = pt;
    }

    /* Path (default to "/") */
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < HTTP_MAX_PATH - 1) {
            path[pi++] = *p++;
        }
        path[pi] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }

    return true;
}

/* --------------------------------------------------------------------------
 * build_request: Build HTTP/1.1 GET request string
 * Returns length of request.
 * -------------------------------------------------------------------------- */
static int build_request(const char* host, const char* path,
                         char* buf, int buf_size) {
    /* "GET /path HTTP/1.1\r\nHost: host\r\nConnection: close\r\n\r\n" */
    int pos = 0;

    /* Request line */
    const char* get = "GET ";
    while (*get && pos < buf_size - 1) buf[pos++] = *get++;
    const char* pp = path;
    while (*pp && pos < buf_size - 1) buf[pos++] = *pp++;
    const char* ver = " HTTP/1.1\r\n";
    while (*ver && pos < buf_size - 1) buf[pos++] = *ver++;

    /* Host header */
    const char* hh = "Host: ";
    while (*hh && pos < buf_size - 1) buf[pos++] = *hh++;
    const char* hp = host;
    while (*hp && pos < buf_size - 1) buf[pos++] = *hp++;
    buf[pos++] = '\r'; buf[pos++] = '\n';

    /* User-Agent */
    const char* ua = "User-Agent: NexusOS/2.8\r\n";
    while (*ua && pos < buf_size - 1) buf[pos++] = *ua++;

    /* Connection: close */
    const char* cc = "Connection: close\r\n";
    while (*cc && pos < buf_size - 1) buf[pos++] = *cc++;

    /* Accept */
    const char* ac = "Accept: */*\r\n";
    while (*ac && pos < buf_size - 1) buf[pos++] = *ac++;

    /* End of headers */
    buf[pos++] = '\r'; buf[pos++] = '\n';
    buf[pos] = '\0';

    return pos;
}

/* --------------------------------------------------------------------------
 * parse_status: Parse "HTTP/1.x NNN" status code
 * -------------------------------------------------------------------------- */
static int parse_status(const char* data, int len) {
    /* Find "HTTP/1." */
    for (int i = 0; i < len - 12; i++) {
        if (data[i] == 'H' && data[i+1] == 'T' && data[i+2] == 'T' &&
            data[i+3] == 'P' && data[i+4] == '/') {
            /* Skip "HTTP/1.x " */
            int j = i + 9;
            int code = 0;
            while (j < len && data[j] >= '0' && data[j] <= '9') {
                code = code * 10 + (data[j] - '0');
                j++;
            }
            return code;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * find_body: Find "\r\n\r\n" header/body separator
 * Returns offset to body start, or -1 if not found.
 * -------------------------------------------------------------------------- */
static int find_body(const char* data, int len) {
    for (int i = 0; i < len - 3; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' &&
            data[i+2] == '\r' && data[i+3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * http_get: Perform an HTTP GET request
 * -------------------------------------------------------------------------- */
http_response_t http_get(const char* url) {
    http_response_t resp;
    memset(&resp, 0, sizeof(resp));

    /* Parse URL */
    char host[HTTP_MAX_HOST];
    char path[HTTP_MAX_PATH];
    uint16_t port;

    if (!parse_url(url, host, &port, path)) {
        return resp;
    }

    /* --- Phase 1: DNS resolution --- */
    vga_print_color("  Resolving ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print(host);
    vga_print("...\n");

    uint32_t server_ip = dns_resolve(host);
    if (server_ip == 0) {
        vga_print_color("  DNS failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return resp;
    }

    /* Check for Escape abort */
    if (keyboard_has_key()) {
        char k = keyboard_getchar();
        if (k == 27) return resp;
    }

    /* Allocate ephemeral port */
    uint16_t local_port = http_port++;
    if (http_port > 50200) http_port = 50080;

    /* --- Phase 2: TCP connect --- */
    vga_print_color("  Connecting...\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    tcp_conn_t* conn = tcp_connect(server_ip, port, local_port);
    if (!conn) {
        vga_print_color("  Connection failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return resp;
    }

    /* Wait for connection to establish (with abort check) */
    uint32_t start = system_ticks;
    while (!conn->connected && (system_ticks - start) < HTTP_CONNECT_TICKS) {
        if (keyboard_has_key()) {
            char k = keyboard_getchar();
            if (k == 27) { tcp_close(conn); return resp; }
        }
        net_poll();
        __asm__ volatile("hlt");
    }
    if (!conn->connected) {
        vga_print_color("  Connect timeout\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        tcp_close(conn);
        return resp;
    }

    /* --- Phase 3: Send request --- */
    vga_print_color("  Sending request...\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    char request[512];
    int req_len = build_request(host, path, request, sizeof(request));
    tcp_send(conn, request, req_len);

    /* --- Phase 4: Receive response --- */
    vga_print_color("  Receiving data...\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    char rx_buf[HTTP_MAX_BODY + 512]; /* Headers + body */
    int total_rx = 0;

    start = system_ticks;
    while ((system_ticks - start) < HTTP_TIMEOUT_TICKS && total_rx < (int)sizeof(rx_buf) - 1) {
        /* Check for Escape abort */
        if (keyboard_has_key()) {
            char k = keyboard_getchar();
            if (k == 27) { tcp_close(conn); return resp; }
        }
        int got = tcp_recv(conn, rx_buf + total_rx, sizeof(rx_buf) - 1 - total_rx);
        if (got > 0) {
            total_rx += got;
            start = system_ticks; /* Reset timeout on data */
        } else if (conn->state != TCP_ESTABLISHED) {
            break; /* Connection closed by server */
        } else {
            net_poll();
            __asm__ volatile("hlt");
        }
    }
    rx_buf[total_rx] = '\0';

    /* Close connection */
    tcp_close(conn);

    if (total_rx == 0) return resp;

    /* Parse response */
    resp.status_code = parse_status(rx_buf, total_rx);
    resp.success = (resp.status_code >= 200 && resp.status_code < 400);

    /* Find body */
    int body_off = find_body(rx_buf, total_rx);
    if (body_off >= 0 && body_off < total_rx) {
        int body_avail = total_rx - body_off;
        resp.body_len = body_avail < HTTP_MAX_BODY ? body_avail : HTTP_MAX_BODY - 1;
        memcpy(resp.body, rx_buf + body_off, resp.body_len);
        resp.body[resp.body_len] = '\0';
    }

    return resp;
}
