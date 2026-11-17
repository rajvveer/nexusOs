/* ============================================================================
 * NexusOS — Phase 57 network shell commands
 * ============================================================================
 *   wget <url> [path]        stream a URL into a ramfs file, with progress
 *   curl [-I|-q|-k] <url>    print a body (or headers) to the console
 *   tlsinfo <host[:port]>    TLS 1.3 handshake diagnostics + certificate chain
 *   netstat                  (extra section) streams in use, TCP counters
 * ============================================================================ */
#include "netcmd.h"
#include "netstream.h"
#include "tls.h"
#include "tcp.h"
#include "dns.h"
#include "ip.h"
#include "net.h"
#include "crypto.h"
#include "ramfs.h"
#include "vfs.h"
#include "users.h"
#include "keyboard.h"
#include "string.h"
#include "vga.h"
#include "port.h"
#include "bignum.h"
#include "cryptokat.h"

extern volatile uint32_t system_ticks;

/* Mirror test-relevant results to COM1 as "[NET] ..." lines so the headless
 * harness can grep them from the serial log. */
static void ser(const char* s) {
    while (*s) {
        while ((port_byte_in(0x3F8 + 5) & 0x20) == 0) { }
        port_byte_out(0x3F8, (uint8_t)*s++);
    }
}
static void sern(uint32_t v) { char b[12]; int_to_str((int)v, b); ser(b); }
static void serhex(uint32_t v) { char b[12]; hex_to_str(v, b); ser(b + 2); }
extern fs_node_t* shell_get_cwd(void);                     /* shell.c (Phase 57) */
extern int ns_raw_headers(net_stream_t* s, char* out, int max);   /* netstream.c private */

#define C_OK    VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)
#define C_ERR   VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)
#define C_WARN  VGA_COLOR(VGA_YELLOW, VGA_BLACK)
#define C_DIM   VGA_COLOR(VGA_DARK_GREY, VGA_BLACK)
#define C_INFO  VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK)

static void pr(const char* s) { vga_print(s); }
static void prc(const char* s, uint8_t c) { vga_print_color(s, c); }
static void prn(uint32_t v) { char b[12]; int_to_str((int)v, b); vga_print(b); }
static void prhex(uint32_t v) { char b[12]; hex_to_str(v, b); vga_print(b + 2); }

static bool esc_pressed(void) {
    if (!keyboard_has_key()) return false;
    return keyboard_getchar() == 27;
}

/* KB/s with one decimal, from bytes and ticks (18.2 Hz) */
static void pr_speed(uint32_t bytes, uint32_t ticks) {
    if (ticks == 0) ticks = 1;
    /* bytes per second = bytes * 18.2 / ticks ; show as KB/s with one decimal.
     * 32-bit only (no libgcc): switch to KB units for big transfers. */
    uint32_t kbs10;
    if (bytes < 20000000u) kbs10 = (bytes * 182u / ticks) / 1024u;     /* KB/s * 10 */
    else                   kbs10 = (bytes / 1024u) * 182u / ticks;
    prn(kbs10 / 10); pr("."); prn(kbs10 % 10); pr(" KB/s");
}

static void pr_secs(uint32_t ticks) {
    uint32_t t10 = ticks * 10 / 18;
    prn(t10 / 10); pr("."); prn(t10 % 10); pr(" s");
}

/* Open + poll until headers (following up to 4 redirects). Prints progress
 * lines. Returns the stream (state >= BODY) or NULL after printing the error. */
static net_stream_t* open_follow(const char* url, const char* method, const char* extra, bool verbose) {
    char cur[NS_MAX_URL];
    strncpy(cur, url, NS_MAX_URL - 1); cur[NS_MAX_URL - 1] = '\0';
    for (int redirects = 0; redirects <= 4; redirects++) {
        net_stream_t* s = ns_open(cur, method, extra);
        if (!s) { prc("  Cannot open URL (bad URL or no free stream)\n", C_ERR); return NULL; }
        uint32_t last = system_ticks;
        int r;
        while ((r = ns_poll(s)) == 0) {
            if (esc_pressed()) { prc("  Aborted.\n", C_WARN); ns_close(s); return NULL; }
            if (system_ticks - last > 45 * 18) { prc("  Timeout.\n", C_ERR); ns_close(s); return NULL; }
            __asm__ volatile("hlt");
        }
        if (r < 0) {
            prc("  Failed: ", C_ERR); prc(ns_error(s), C_ERR); pr("\n");
            const ns_info_t* in = ns_info(s);
            if (in->secure && in->tls_info[0]) { prc("  ", C_DIM); prc(in->tls_info, C_DIM); pr("\n"); }
            ser("[NET] fail url="); ser(cur); ser(" err="); ser(ns_error(s)); ser("\n");
            ns_close(s);
            return NULL;
        }
        const ns_info_t* in = ns_info(s);
        if (in->secure && verbose) {
            prc("  ", C_DIM); prc(in->tls_info, in->trusted ? C_DIM : C_WARN); pr("\n");
            if (!in->trusted) prc("  WARNING: certificate chain is NOT trusted\n", C_WARN);
            ser("[NET] tls url="); ser(cur); ser(" trusted="); sern(in->trusted ? 1 : 0); ser(" info="); ser(in->tls_info); ser("\n");
        }
        if ((in->status == 301 || in->status == 302 || in->status == 303 || in->status == 307 || in->status == 308) &&
            in->location[0] && redirects < 4) {
            if (verbose) { prc("  HTTP ", C_DIM); prn((uint32_t)in->status); prc(" -> ", C_DIM); prc(in->location, C_DIM); pr("\n"); }
            strncpy(cur, in->location, NS_MAX_URL - 1); cur[NS_MAX_URL - 1] = '\0';
            ns_close(s);
            continue;
        }
        return s;
    }
    prc("  Too many redirects.\n", C_ERR);
    return NULL;
}

static void print_status(const ns_info_t* in) {
    prc("  HTTP ", in->status < 400 ? C_OK : C_ERR); prn((uint32_t)in->status);
    if (in->content_type[0]) { pr("  "); pr(in->content_type); }
    if (in->chunked) prc("  [chunked]", C_DIM);
    else if (in->content_length) { pr("  "); prn(in->content_length); pr(" bytes"); }
    else prc("  [length unknown]", C_DIM);
    pr("\n");
}

/* ============================================================================
 * wget
 * ============================================================================ */
static void url_basename(const char* url, char* out, int max) {
    /* path part after the host, last segment, without query */
    const char* p = url;
    const char* ss = strstr(url, "://");
    if (ss) p = ss + 3;
    while (*p && *p != '/') p++;
    const char* last = NULL;
    for (const char* q = p; *q && *q != '?' && *q != '#'; q++) if (*q == '/') last = q + 1;
    int n = 0;
    if (last) while (last[n] && last[n] != '?' && last[n] != '#' && n < max - 1) { out[n] = last[n]; n++; }
    out[n] = '\0';
    if (n == 0) strcpy(out, "index.html");
}

void netcmd_wget(int argc, char** argv) {
    if (argc < 2) { pr("  Usage: wget <url> [path]\n"); return; }
    static uint8_t chunk[4096];
    char dest[FS_PATH_MAX];
    if (argc >= 3) { strncpy(dest, argv[2], FS_PATH_MAX - 1); dest[FS_PATH_MAX - 1] = '\0'; }
    else url_basename(argv[1], dest, FS_PATH_MAX);

    prc("  Fetching ", C_DIM); pr(argv[1]); pr("\n");
    net_stream_t* s = open_follow(argv[1], "GET", NULL, true);
    if (!s) { ser("[NET] wget url="); ser(argv[1]); ser(" ok=0\n"); return; }
    const ns_info_t* in = ns_info(s);
    print_status(in);

    /* destination node */
    fs_node_t* cwd = shell_get_cwd();
    fs_node_t* node = ramfs_resolve(dest, cwd);
    if (node && node->type != FS_FILE) { prc("  Destination is a directory.\n", C_ERR); ns_close(s); return; }
    if (!node) {
        char leaf[FS_NAME_MAX];
        fs_node_t* parent = ramfs_resolve_parent(dest, cwd, leaf);
        if (!parent || !vfs_check_perm(parent, users_current_uid(), ACC_WRITE)) {
            prc("  Cannot create ", C_ERR); prc(dest, C_ERR); pr("\n"); ns_close(s); return;
        }
        node = ramfs_mknode(parent, leaf, FS_FILE);
        if (!node) { prc("  Cannot create file (name too long / table full).\n", C_ERR); ns_close(s); return; }
    } else if (!vfs_check_perm(node, users_current_uid(), ACC_WRITE)) {
        prc("  Permission denied.\n", C_ERR); ns_close(s); return;
    }
    node->size = 0;                                   /* overwrite */

    uint32_t written = 0, received = 0, start = system_ticks, last_print = system_ticks, last_data = system_ticks;
    bool truncated = false, failed = false;
    for (;;) {
        int n = ns_read(s, chunk, sizeof(chunk));
        if (n > 0) {
            received += (uint32_t)n;
            last_data = system_ticks;
            if (!truncated) {
                int32_t w = vfs_write(node, written, (uint32_t)n, chunk);
                if (w < n) truncated = true;
                if (w > 0) written += (uint32_t)w;
            }
            if (system_ticks - last_print >= 18) {
                last_print = system_ticks;
                pr("  "); prn(received);
                if (in->content_length) { pr("/"); prn(in->content_length); pr(" bytes ("); prn(received * 100 / in->content_length); pr("%)  "); }
                else pr(" bytes  ");
                pr_speed(received, system_ticks - start); pr("\n");
            }
            continue;
        }
        if (n == -1) break;
        if (n == -2) { prc("  Error: ", C_ERR); prc(ns_error(s), C_ERR); pr("\n"); failed = true; break; }
        if (esc_pressed()) { prc("  Aborted.\n", C_WARN); failed = true; break; }
        ns_poll(s);
        if (system_ticks - last_data > 30 * 18) { prc("  Timeout (no data for 30 s).\n", C_ERR); failed = true; break; }
        __asm__ volatile("hlt");
    }
    uint32_t ticks = system_ticks - start;
    ns_close(s);
    char abspath[FS_PATH_MAX];
    ramfs_abspath(node, abspath, sizeof(abspath));
    prc(failed ? "  Partial: " : "  Saved ", failed ? C_WARN : C_OK);
    prn(written); pr(" bytes to "); pr(abspath); pr("\n");
    if (truncated) {
        prc("  NOTE: file capped at ", C_WARN); prn(RAMFS_MAX_FILE_SIZE);
        prc(" bytes (ramfs limit); ", C_WARN); prn(received); prc(" bytes were downloaded.\n", C_WARN);
    }
    pr("  "); prn(received); pr(" bytes in "); pr_secs(ticks); pr(" ("); pr_speed(received, ticks); pr(")\n");
    ser("[NET] wget url="); ser(argv[1]); ser(" status="); sern((uint32_t)in->status); ser(" bytes="); sern(received);
    ser(" written="); sern(written); ser(" path="); ser(abspath); ser(" ticks="); sern(ticks);
    ser(" ok="); sern(failed ? 0 : 1); ser("\n");
}

/* ============================================================================
 * curl
 * ============================================================================ */
void netcmd_curl(int argc, char** argv) {
    bool head = false, quiet = false, insecure = false;
    const char* url = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-I") == 0 || strcmp(argv[i], "-i") == 0) head = true;
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "-s") == 0) quiet = true;
        else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--insecure") == 0) insecure = true;
        else url = argv[i];
    }
    if (!url) { pr("  Usage: curl [-I] [-q] [-k] <url>\n    -I headers only  -q count only (no body echo)  -k ignore certificate errors\n"); return; }
    static uint8_t chunk[4096];
    static char hdrs[4096];

    net_stream_t* s = open_follow(url, head ? "HEAD" : "GET", insecure ? "X-Nexus-Insecure: 1\r\n" : NULL, true);
    if (!s) { ser("[NET] curl url="); ser(url); ser(" ok=0\n"); return; }
    const ns_info_t* in = ns_info(s);
    if (head) {
        int n = ns_raw_headers(s, hdrs, sizeof(hdrs));
        for (int i = 0; i < n; i++) {
            char c = hdrs[i];
            if (c == '\r') continue;
            if (c == '\n' || (c >= 32 && c < 127)) vga_putchar(c);
        }
        ser("[NET] curl url="); ser(url); ser(" status="); sern((uint32_t)in->status); ser(" head=1 hdrbytes="); sern((uint32_t)n); ser(" ok=1\n");
        ns_close(s);
        return;
    }
    print_status(in);
    uint32_t received = 0, printed = 0, crc = 0xFFFFFFFFu, start = system_ticks, last_data = system_ticks;
    uint8_t first[16]; int nfirst = 0;
    bool failed = false;
    for (;;) {
        int n = ns_read(s, chunk, sizeof(chunk));
        if (n > 0) {
            last_data = system_ticks;
            crc = crc32_update(crc, chunk, (uint32_t)n);
            for (int i = 0; i < n && nfirst < 16; i++) first[nfirst++] = chunk[i];
            if (!quiet) {
                for (int i = 0; i < n; i++) {
                    if (printed >= 8192) { prc("\n  ... (console echo stopped at 8 KB, still downloading)\n", C_DIM); quiet = true; break; }
                    char c = (char)chunk[i];
                    if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) vga_putchar(c);
                    else if (c != '\r') vga_putchar('.');
                    printed++;
                }
            }
            received += (uint32_t)n;
            continue;
        }
        if (n == -1) break;
        if (n == -2) { pr("\n"); prc("  Error: ", C_ERR); prc(ns_error(s), C_ERR); pr("\n"); failed = true; break; }
        if (esc_pressed()) { prc("\n  Aborted.\n", C_WARN); failed = true; break; }
        ns_poll(s);
        if (system_ticks - last_data > 30 * 18) { prc("\n  Timeout (no data for 30 s).\n", C_ERR); failed = true; break; }
        __asm__ volatile("hlt");
    }
    uint32_t ticks = system_ticks - start;
    ns_close(s);
    if (!quiet && printed) pr("\n");
    prc(failed ? "  Incomplete: " : "  Received ", failed ? C_WARN : C_OK);
    prn(received); pr(" bytes in "); pr_secs(ticks); pr(" ("); pr_speed(received, ticks); pr(")  CRC32 ");
    prhex(~crc); pr("\n");
    if (nfirst) {
        prc("  first bytes: ", C_DIM);
        for (int i = 0; i < nfirst; i++) { char c = (char)first[i]; vga_putchar((c >= 32 && c < 127) ? c : '.'); }
        pr("\n");
    }
    ser("[NET] curl url="); ser(url); ser(" status="); sern((uint32_t)in->status); ser(" bytes="); sern(received);
    ser(" crc="); serhex(~crc); ser(" ticks="); sern(ticks); ser(" chunked="); sern(in->chunked ? 1 : 0);
    ser(" clen="); sern(in->content_length); ser(" ok="); sern(failed ? 0 : 1); ser("\n");
}

/* ============================================================================
 * tlsinfo
 * ============================================================================ */
static const char* key_name(int kt) {
    switch (kt) {
        case X509_KEY_RSA:  return "RSA";
        case X509_KEY_P256: return "ECDSA P-256";
        case X509_KEY_P384: return "ECDSA P-384";
        default:            return "unsupported key";
    }
}
static const char* sig_name(int alg, int hash) {
    if (alg == X509_SIG_RSA_PKCS1) return hash == HASH_SHA256 ? "sha256WithRSA" : hash == HASH_SHA384 ? "sha384WithRSA" : "sha512WithRSA";
    if (alg == X509_SIG_RSA_PSS)   return hash == HASH_SHA256 ? "RSA-PSS/SHA256" : hash == HASH_SHA384 ? "RSA-PSS/SHA384" : "RSA-PSS/SHA512";
    if (alg == X509_SIG_ECDSA)     return hash == HASH_SHA256 ? "ecdsa-with-SHA256" : hash == HASH_SHA384 ? "ecdsa-with-SHA384" : "ecdsa-with-SHA512";
    if (alg == X509_SIG_UNSUPPORTED) return "unsupported (sha1?)";
    return "unknown";
}

void netcmd_tlsinfo(int argc, char** argv) {
    static tls_ctx_t tls;                         /* ~37 KB: static */
    if (argc < 2) { pr("  Usage: tlsinfo <host[:port]> [-k]\n"); return; }
    bool insecure = (argc >= 3 && strcmp(argv[2], "-k") == 0);
    char host[128];
    uint16_t port = 443;
    int hi = 0;
    const char* p = argv[1];
    if (strstr(p, "://")) p = strstr(p, "://") + 3;
    while (*p && *p != ':' && *p != '/' && hi < 127) host[hi++] = *p++;
    host[hi] = '\0';
    if (*p == ':') { p++; uint32_t v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0'); if (v && v < 65536) port = (uint16_t)v; }
    if (!host[0]) { pr("  Usage: tlsinfo <host[:port]>\n"); return; }

    prc("  Resolving ", C_DIM); pr(host); pr("...\n");
    uint32_t ip = dns_resolve(host);
    if (!ip) { prc("  DNS lookup failed.\n", C_ERR); return; }
    char ips[16]; ip_to_string(ip, ips);
    prc("  Connecting to ", C_DIM); pr(ips); pr(":"); prn(port); pr("...\n");
    tcp_conn_t* c = tcp_connect(ip, port, tcp_ephemeral_port());
    if (!c) { prc("  No free TCP connection.\n", C_ERR); return; }
    uint32_t gen = c->gen, start = system_ticks;
    while (c->active && c->gen == gen && !c->connected && system_ticks - start < 10 * 18) {
        if (esc_pressed()) { tcp_close(c); return; }
        net_poll(); __asm__ volatile("hlt");
    }
    if (!(c->active && c->gen == gen && c->connected)) { prc("  Connect failed/timeout.\n", C_ERR); if (c->active && c->gen == gen) tcp_close(c); return; }

    prc("  TLS 1.3 handshake...\n", C_DIM);
    uint32_t t0 = system_ticks;
    int r = tls_start(&tls, c, host, insecure);
    while (r == 0 && (r = tls_poll(&tls)) == 0) {
        if (esc_pressed()) { prc("  Aborted.\n", C_WARN); tcp_close(c); return; }
        if (system_ticks - t0 > 30 * 18) { prc("  Handshake timeout.\n", C_ERR); tcp_close(c); return; }
        net_poll(); __asm__ volatile("hlt");
    }
    uint32_t ticks = system_ticks - t0;

    pr("\n");
    prc("  Host:      ", C_INFO); pr(host); pr(" ("); pr(ips); pr(":"); prn(port); pr(")\n");
    if (r == 1) {
        prc("  Protocol:  ", C_INFO); pr("TLS 1.3   Cipher: TLS_AES_128_GCM_SHA256   Key exchange: x25519   ALPN: ");
        pr(tls.alpn_http11 ? "http/1.1" : "none"); pr("\n");
        prc("  Handshake: ", C_INFO); prn(ticks * 1000 / 18); pr(" ms, "); prn(tls.bytes_rx); pr(" bytes received, "); prn(tls.bytes_tx); pr(" bytes sent\n");
    } else {
        prc("  Handshake FAILED: ", C_ERR); prc(tls.error, C_ERR); pr("\n");
    }
    if (tls.cert_count) {
        prc("  Certificate chain (", C_INFO); prn((uint32_t)tls.cert_count); prc("):\n", C_INFO);
        for (int i = 0; i < tls.cert_count; i++) {
            const tls_cert_info_t* ci = &tls.certs[i];
            pr("    ["); prn((uint32_t)i); pr("] CN="); pr(ci->subject);
            prc("  (", C_DIM); prc(key_name(ci->key_type), C_DIM);
            if (ci->key_type == X509_KEY_RSA) { prc(" ", C_DIM); prn((uint32_t)ci->key_bits); }
            prc(", sig ", C_DIM); prc(sig_name(ci->sig_alg, ci->sig_hash), C_DIM); prc(")\n", C_DIM);
            pr("        issuer: "); pr(ci->issuer); pr("\n");
            pr("        valid:  "); pr(ci->not_before); pr(" .. "); pr(ci->not_after); pr("\n");
        }
        if (tls.trusted) { prc("  Trust:     TRUSTED", C_OK); pr(" (root: "); pr(tls.root_name); pr(")\n"); }
        else { prc("  Trust:     NOT TRUSTED: ", C_WARN); prc(tls.reason[0] ? tls.reason : tls.error, C_WARN); pr("\n"); }
    }
    pr("\n");
    /* serial mirror */
    for (int i = 0; i < tls.cert_count; i++) {
        const tls_cert_info_t* ci = &tls.certs[i];
        ser("[NET] cert["); sern((uint32_t)i); ser("] subj="); ser(ci->subject); ser(" | issuer="); ser(ci->issuer);
        ser(" | key="); ser(key_name(ci->key_type)); ser(" "); sern((uint32_t)ci->key_bits);
        ser(" | sig="); ser(sig_name(ci->sig_alg, ci->sig_hash)); ser(" | valid="); ser(ci->not_before); ser(".."); ser(ci->not_after); ser("\n");
    }
    ser("[NET] tlsinfo host="); ser(host); ser(" result="); ser(r == 1 ? "ok" : "fail");
    ser(" trusted="); sern(tls.trusted ? 1 : 0); ser(" root="); ser(tls.root_name); ser(" certs="); sern((uint32_t)tls.cert_count);
    ser(" ms="); sern(ticks * 1000 / 18); ser(" rx="); sern(tls.bytes_rx); ser(" tx="); sern(tls.bytes_tx);
    ser(" alpn="); sern(tls.alpn_http11 ? 1 : 0); ser(" reason="); ser(tls.reason); ser(" err="); ser(tls.error); ser("\n");
    tls_close(&tls);
    if (c->active && c->gen == gen) tcp_close(c);
}

/* ============================================================================
 * cryptotest: known-answer tests for every primitive (vectors: gen_kat.py)
 * ============================================================================ */
static int kat_fails;
static void kat(const char* name, bool ok) {
    pr("  "); prc(ok ? "PASS " : "FAIL ", ok ? C_OK : C_ERR); pr(name); pr("\n");
    ser("[NET] kat "); ser(ok ? "PASS " : "FAIL "); ser(name); ser("\n");
    if (!ok) kat_fails++;
}

void netcmd_cryptotest(void) {
    static uint8_t d[64], buf[128], ct[64], dec[64];
    static x509_cert_t key, c1, c2;
    uint32_t t0;
    kat_fails = 0;
    prc("  Crypto known-answer tests\n", C_INFO);

    sha256("abc", 3, d);                              kat("sha256(abc)", memcmp(d, KAT_SHA256_ABC, 32) == 0);
    hash_compute(HASH_SHA384, "abc", 3, d);           kat("sha384(abc)", memcmp(d, KAT_SHA384_ABC, 48) == 0);
    hash_compute(HASH_SHA512, "abc", 3, d);           kat("sha512(abc)", memcmp(d, KAT_SHA512_ABC, 64) == 0);
    sha256(KAT_BIG, sizeof(KAT_BIG), d);              kat("sha256(1280 B)", memcmp(d, KAT_SHA256_BIG, 32) == 0);
    { sha256_ctx_t c; sha256_init(&c); sha256_update(&c, KAT_BIG, 7); sha256_update(&c, KAT_BIG + 7, 100);
      sha256_update(&c, KAT_BIG + 107, sizeof(KAT_BIG) - 107); sha256_final(&c, d);
      kat("sha256 incremental", memcmp(d, KAT_SHA256_BIG, 32) == 0); }
    hash_compute(HASH_SHA384, KAT_BIG, sizeof(KAT_BIG), d); kat("sha384(1280 B)", memcmp(d, KAT_SHA384_BIG, 48) == 0);
    hmac_sha256((const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, d);
    kat("hmac-sha256 rfc4231#2", memcmp(d, KAT_HMAC_JEFE, 32) == 0);
    { uint8_t ikm[22], salt[13], info[10], prk[32];
      memset(ikm, 0x0b, 22); for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i; for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
      hkdf_extract(salt, 13, ikm, 22, prk); hkdf_expand(prk, info, 10, buf, 42);
      kat("hkdf rfc5869#1", memcmp(buf, KAT_HKDF_OKM, 42) == 0); }
    { aes128_ctx_t a; uint8_t k16[16], pt16[16];
      for (int i = 0; i < 16; i++) { k16[i] = (uint8_t)i; pt16[i] = (uint8_t)(i * 0x11); }
      aes128_set_key(&a, k16); aes128_encrypt_block(&a, pt16, d);
      kat("aes-128 fips-197", memcmp(d, KAT_AES_CT, 16) == 0); }
    { gcm_ctx_t g; uint8_t tag[16];
      gcm_set_key(&g, KAT_GCM_KEY);
      gcm_encrypt(&g, KAT_GCM_IV, KAT_GCM_AAD, sizeof(KAT_GCM_AAD), KAT_GCM_PT, ct, sizeof(KAT_GCM_PT), tag);
      kat("aes-128-gcm encrypt+tag", memcmp(ct, KAT_GCM_CT, sizeof(KAT_GCM_CT)) == 0 && memcmp(tag, KAT_GCM_TAG, 16) == 0);
      int r = gcm_decrypt(&g, KAT_GCM_IV, KAT_GCM_AAD, sizeof(KAT_GCM_AAD), KAT_GCM_CT, dec, sizeof(KAT_GCM_CT), KAT_GCM_TAG);
      kat("aes-128-gcm decrypt", r == 0 && memcmp(dec, KAT_GCM_PT, sizeof(KAT_GCM_PT)) == 0);
      memcpy(tag, KAT_GCM_TAG, 16); tag[3] ^= 0x80;
      kat("aes-128-gcm rejects bad tag", gcm_decrypt(&g, KAT_GCM_IV, KAT_GCM_AAD, sizeof(KAT_GCM_AAD), KAT_GCM_CT, dec, sizeof(KAT_GCM_CT), tag) != 0); }
    { uint8_t xo[32], sc[32];
      t0 = system_ticks; x25519(xo, KAT_X_SCALAR, KAT_X_U);
      kat("x25519 rfc7748", memcmp(xo, KAT_X_OUT, 32) == 0);
      for (int i = 0; i < 32; i++) sc[i] = (uint8_t)i;
      x25519_base(xo, sc); kat("x25519 base point", memcmp(xo, KAT_X_BASE_PUB, 32) == 0);
      pr("    (x25519: "); prn((system_ticks - t0) * 1000 / 18 / 2); pr(" ms each)\n"); }
    { memcpy(buf, KAT_MSG, sizeof(KAT_MSG)); buf[0] ^= 1;
      memset(&key, 0, sizeof(key)); x509_parse_spki(KAT_P256_SPKI, sizeof(KAT_P256_SPKI), &key);
      t0 = system_ticks;
      int r = x509_verify_raw(&key, X509_SIG_ECDSA, HASH_SHA256, KAT_MSG, sizeof(KAT_MSG), KAT_P256_SIG, sizeof(KAT_P256_SIG));
      pr("    (ecdsa p-256 verify: "); prn((system_ticks - t0) * 1000 / 18); pr(" ms)\n");
      kat("ecdsa p-256 verify", r == 0);
      kat("ecdsa p-256 rejects altered msg", x509_verify_raw(&key, X509_SIG_ECDSA, HASH_SHA256, buf, sizeof(KAT_MSG), KAT_P256_SIG, sizeof(KAT_P256_SIG)) != 0);
      memset(&key, 0, sizeof(key)); x509_parse_spki(KAT_P384_SPKI, sizeof(KAT_P384_SPKI), &key);
      t0 = system_ticks;
      r = x509_verify_raw(&key, X509_SIG_ECDSA, HASH_SHA384, KAT_MSG, sizeof(KAT_MSG), KAT_P384_SIG, sizeof(KAT_P384_SIG));
      pr("    (ecdsa p-384 verify: "); prn((system_ticks - t0) * 1000 / 18); pr(" ms)\n");
      kat("ecdsa p-384 verify", r == 0);
      kat("ecdsa p-384 rejects altered msg", x509_verify_raw(&key, X509_SIG_ECDSA, HASH_SHA384, buf, sizeof(KAT_MSG), KAT_P384_SIG, sizeof(KAT_P384_SIG)) != 0);
      memset(&key, 0, sizeof(key)); x509_parse_spki(KAT_RSA_SPKI, sizeof(KAT_RSA_SPKI), &key);
      t0 = system_ticks;
      r = x509_verify_raw(&key, X509_SIG_RSA_PKCS1, HASH_SHA256, KAT_MSG, sizeof(KAT_MSG), KAT_RSA_SIG_PKCS1, sizeof(KAT_RSA_SIG_PKCS1));
      pr("    (rsa-2048 verify: "); prn((system_ticks - t0) * 1000 / 18); pr(" ms)\n");
      kat("rsa-2048 pkcs1 verify", r == 0);
      kat("rsa-2048 pss/sha256 verify", x509_verify_raw(&key, X509_SIG_RSA_PSS, HASH_SHA256, KAT_MSG, sizeof(KAT_MSG), KAT_RSA_SIG_PSS, sizeof(KAT_RSA_SIG_PSS)) == 0);
      kat("rsa-2048 pss/sha384 verify", x509_verify_raw(&key, X509_SIG_RSA_PSS, HASH_SHA384, KAT_MSG, sizeof(KAT_MSG), KAT_RSA_SIG_PSS384, sizeof(KAT_RSA_SIG_PSS384)) == 0);
      kat("rsa-2048 rejects altered msg", x509_verify_raw(&key, X509_SIG_RSA_PKCS1, HASH_SHA256, buf, sizeof(KAT_MSG), KAT_RSA_SIG_PKCS1, sizeof(KAT_RSA_SIG_PKCS1)) != 0);
      kat("rsa pss rejects pkcs1 sig", x509_verify_raw(&key, X509_SIG_RSA_PSS, HASH_SHA256, KAT_MSG, sizeof(KAT_MSG), KAT_RSA_SIG_PKCS1, sizeof(KAT_RSA_SIG_PKCS1)) != 0); }
    { int r = x509_parse(KAT_CERT_ISRG_X1, sizeof(KAT_CERT_ISRG_X1), &c1);
      kat("x509 parse ISRG Root X1", r == 0 && c1.key_type == X509_KEY_RSA && c1.self_signed && c1.is_ca && c1.sig_alg == X509_SIG_RSA_PKCS1);
      t0 = system_ticks;
      kat("ISRG X1 self-signature (RSA-4096/sha256)", x509_verify_signature(&c1, &c1) == 0);
      pr("    (rsa-4096 verify: "); prn((system_ticks - t0) * 1000 / 18); pr(" ms)\n");
      kat("ISRG X1 validity period contains today", x509_check_validity(&c1) == 0);
      kat("hostname mismatch detected", !x509_match_hostname(&c1, "example.com"));
      r = x509_parse(KAT_CERT_ISRG_X2, sizeof(KAT_CERT_ISRG_X2), &c2);
      kat("x509 parse ISRG Root X2", r == 0 && c2.key_type == X509_KEY_P384 && c2.sig_alg == X509_SIG_ECDSA && c2.sig_hash == HASH_SHA384);
      kat("ISRG X2 self-signature (P-384/sha384)", x509_verify_signature(&c2, &c2) == 0);
      kat("ISRG X2 signature with wrong key rejected", x509_verify_signature(&c2, &c1) != 0);
      x509_chain_result_t res;
      x509_verify_chain(&c1, 1, "example.com", true, &res);
      kat("root store finds ISRG Root X1", strcmp(res.root_name, "ISRG Root X1") == 0);
      x509_verify_chain(&c2, 1, "example.com", false, &res);
      kat("hostname mismatch is a hard failure", res.hard_fail && !res.trusted); }
    pr("  "); prc(kat_fails ? "FAILURES: " : "ALL PASSED", kat_fails ? C_ERR : C_OK);
    if (kat_fails) prn((uint32_t)kat_fails);
    pr("\n");
    ser("[NET] cryptotest done fails="); sern((uint32_t)kat_fails); ser("\n");
}

/* ============================================================================
 * netstat extras
 * ============================================================================ */
void netcmd_netstat_extra(void) {
    static uint32_t last_rtx = 0, last_tick = 0;
    const tcp_stats_t* st = tcp_get_stats();
    prc("  Streams: ", C_INFO); prn((uint32_t)ns_active_count()); pr("/"); prn(NS_MAX_STREAMS); pr(" HTTP(S) streams in use\n");
    prc("  TCP:     ", C_INFO);
    prn(st->segs_sent); pr(" segs sent, "); prn(st->segs_rcvd); pr(" rcvd, ");
    prn(st->retransmits); pr(" retransmits (+"); prn(st->fast_retransmits); pr(" fast");
    if (last_tick) {
        uint32_t dt = system_ticks - last_tick; if (!dt) dt = 1;
        uint32_t d = st->retransmits - last_rtx;
        pr(", "); prn(d); pr(" since last netstat = "); prn(d * 18 * 10 / dt / 10); pr("/s");
    }
    pr(")\n           ");
    prn(st->giveups); pr(" aborted, "); prn(st->ooo_dropped); pr(" out-of-order dropped, ");
    prn(st->rx_full_drops); pr(" B dropped (window full), "); prn(st->zero_wnd_probes); pr(" zero-window probes, RST ");
    prn(st->rst_sent); pr(" sent / "); prn(st->rst_rcvd); pr(" rcvd\n");
    last_rtx = st->retransmits; last_tick = system_ticks;
    /* serial mirror: active (non-LISTEN) TCBs + listeners, for leak checks */
    int count, active = 0, listen = 0;
    const tcp_conn_t* tc = tcp_get_connections(&count);
    for (int i = 0; i < count; i++) {
        if (!tc[i].active) continue;
        if (tc[i].state == TCP_LISTEN) listen++; else active++;
    }
    ser("[NET] netstat tcbs="); sern((uint32_t)active); ser(" listen="); sern((uint32_t)listen);
    ser(" streams="); sern((uint32_t)ns_active_count()); ser(" rtx="); sern(st->retransmits);
    ser(" fast="); sern(st->fast_retransmits); ser(" giveups="); sern(st->giveups); ser(" ooo="); sern(st->ooo_dropped);
    ser(" rxfull="); sern(st->rx_full_drops); ser(" zwp="); sern(st->zero_wnd_probes);
    ser(" segs="); sern(st->segs_sent); ser("/"); sern(st->segs_rcvd); ser("\n");
}
