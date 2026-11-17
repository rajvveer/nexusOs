/* ============================================================================
 * NexusOS — DNS Resolver — Phase 28
 * ============================================================================
 * Sends DNS A-record queries via UDP to configured DNS server.
 * Parses responses and caches results.
 * ============================================================================ */

#include "dns.h"
#include "udp.h"
#include "ip.h"
#include "net.h"
#include "string.h"
#include "vga.h"
#include "keyboard.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* DNS cache */
static dns_cache_entry_t cache[DNS_MAX_CACHE];

/* Transaction ID counter */
static uint16_t dns_txid = 1;

/* Port for DNS responses */
#define DNS_LOCAL_PORT 10053

/* --------------------------------------------------------------------------
 * dns_init: Initialize DNS resolver
 * -------------------------------------------------------------------------- */
void dns_init(void) {
    memset(cache, 0, sizeof(cache));
    dns_txid = 1;

    /* Bind local port for DNS responses */
    udp_bind(DNS_LOCAL_PORT);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("DNS resolver initialized\n");
}

/* --------------------------------------------------------------------------
 * cache_lookup: Check DNS cache for hostname
 * -------------------------------------------------------------------------- */
static uint32_t cache_lookup(const char* name) {
    for (int i = 0; i < DNS_MAX_CACHE; i++) {
        if (!cache[i].valid) continue;

        /* Check TTL expiry */
        uint32_t age = system_ticks - cache[i].timestamp;
        if (age > cache[i].ttl) {
            cache[i].valid = false;
            continue;
        }

        if (strcmp(cache[i].name, name) == 0) {
            return cache[i].ip;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * cache_store: Add entry to DNS cache
 * -------------------------------------------------------------------------- */
static void cache_store(const char* name, uint32_t ip, uint32_t ttl_seconds) {
    /* Find free slot or oldest */
    int slot = -1;
    uint32_t oldest_age = 0;
    int oldest_slot = 0;

    for (int i = 0; i < DNS_MAX_CACHE; i++) {
        if (!cache[i].valid) { slot = i; break; }
        uint32_t age = system_ticks - cache[i].timestamp;
        if (age > oldest_age) {
            oldest_age = age;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;

    /* Store */
    int len = strlen(name);
    if (len >= DNS_MAX_NAME) len = DNS_MAX_NAME - 1;
    memcpy(cache[slot].name, name, len);
    cache[slot].name[len] = '\0';
    cache[slot].ip = ip;
    cache[slot].ttl = ttl_seconds * 18; /* Convert to ticks */
    cache[slot].timestamp = system_ticks;
    cache[slot].valid = true;
}

/* --------------------------------------------------------------------------
 * encode_qname: Encode hostname as DNS QNAME labels
 * "www.example.com" -> [3]www[7]example[3]com[0]
 * Returns number of bytes written.
 * -------------------------------------------------------------------------- */
static int encode_qname(const char* hostname, uint8_t* buf) {
    int pos = 0;
    const char* p = hostname;

    while (*p) {
        /* Find the next dot or end */
        const char* dot = p;
        while (*dot && *dot != '.') dot++;

        int label_len = dot - p;
        if (label_len > 63 || label_len == 0) return -1;

        buf[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) {
            buf[pos++] = (uint8_t)p[i];
        }

        p = dot;
        if (*p == '.') p++;
    }
    buf[pos++] = 0; /* Root label */
    return pos;
}

/* --------------------------------------------------------------------------
 * skip_qname: Skip over a QNAME in a DNS packet (handles compression)
 * Returns pointer past the QNAME, or NULL on error.
 * -------------------------------------------------------------------------- */
static const uint8_t* skip_qname(const uint8_t* ptr, const uint8_t* pkt_end) {
    while (ptr < pkt_end) {
        uint8_t len = *ptr;
        if (len == 0) { return ptr + 1; }       /* Root label */
        if ((len & 0xC0) == 0xC0) { return ptr + 2; }  /* Compression pointer */
        ptr += 1 + len;
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * dns_resolve: Resolve hostname to IPv4 address
 * -------------------------------------------------------------------------- */
uint32_t dns_resolve(const char* hostname) {
    if (!hostname || hostname[0] == '\0') return 0;

    /* Check if it's already an IP address (digits and dots only) */
    bool is_ip = true;
    for (int i = 0; hostname[i]; i++) {
        if (hostname[i] != '.' && (hostname[i] < '0' || hostname[i] > '9')) {
            is_ip = false;
            break;
        }
    }
    if (is_ip) return ip_parse(hostname);

    /* Check cache */
    uint32_t cached = cache_lookup(hostname);
    if (cached) return cached;

    /* Build DNS query */
    uint8_t pkt[DNS_MAX_PACKET];
    memset(pkt, 0, sizeof(pkt));

    dns_header_t* hdr = (dns_header_t*)pkt;
    uint16_t txid = dns_txid++;
    hdr->id = htons(txid);
    hdr->flags = htons(DNS_FLAG_RD);  /* Standard query, recursion desired */
    hdr->qdcount = htons(1);

    /* Encode question */
    int qname_len = encode_qname(hostname, pkt + sizeof(dns_header_t));
    if (qname_len < 0) return 0;

    int qpos = sizeof(dns_header_t) + qname_len;

    /* QTYPE = A (1) */
    pkt[qpos++] = 0;
    pkt[qpos++] = DNS_TYPE_A;

    /* QCLASS = IN (1) */
    pkt[qpos++] = 0;
    pkt[qpos++] = DNS_CLASS_IN;

    /* Send to DNS server */
    const net_config_t* cfg = ip_get_config();
    udp_send(cfg->dns, DNS_LOCAL_PORT, DNS_PORT, pkt, qpos);

    /* Wait for response */
    uint32_t start = system_ticks;
    while ((system_ticks - start) < DNS_TIMEOUT_TICKS) {
        /* Check for Escape abort */
        if (keyboard_has_key()) {
            char k = keyboard_getchar();
            if (k == 27) return 0;
        }
        udp_datagram_t* dgram = udp_recv(DNS_LOCAL_PORT);
        if (!dgram) {
            net_poll();
            __asm__ volatile("hlt");
            continue;
        }

        /* Validate response */
        if (dgram->length < sizeof(dns_header_t)) continue;

        const dns_header_t* resp = (const dns_header_t*)dgram->data;
        if (ntohs(resp->id) != txid) continue;
        if (!(ntohs(resp->flags) & DNS_FLAG_QR)) continue; /* Not a response */

        uint16_t ancount = ntohs(resp->ancount);
        if (ancount == 0) return 0; /* No answers */

        /* Skip question section */
        const uint8_t* ptr = dgram->data + sizeof(dns_header_t);
        const uint8_t* pkt_end = dgram->data + dgram->length;

        /* Skip QNAME */
        ptr = skip_qname(ptr, pkt_end);
        if (!ptr) return 0;
        ptr += 4; /* Skip QTYPE + QCLASS */

        /* Parse answer records */
        for (int i = 0; i < ancount && ptr < pkt_end; i++) {
            /* Skip NAME */
            ptr = skip_qname(ptr, pkt_end);
            if (!ptr || ptr + 10 > pkt_end) return 0;

            uint16_t rtype  = (ptr[0] << 8) | ptr[1];
            /* uint16_t rclass = (ptr[2] << 8) | ptr[3]; */
            uint32_t rttl   = (ptr[4] << 24) | (ptr[5] << 16) |
                              (ptr[6] << 8)  | ptr[7];
            uint16_t rdlen  = (ptr[8] << 8) | ptr[9];
            ptr += 10;

            if (ptr + rdlen > pkt_end) return 0;

            if (rtype == DNS_TYPE_A && rdlen == 4) {
                /* IPv4 address! */
                uint32_t ip = (ptr[0] << 24) | (ptr[1] << 16) |
                              (ptr[2] << 8)  | ptr[3];
                cache_store(hostname, ip, rttl > 0 ? rttl : 300);
                return ip;
            }

            /* Skip this record's data */
            ptr += rdlen;
        }

        return 0; /* No A record found */
    }

    return 0; /* Timeout */
}

/* --------------------------------------------------------------------------
 * dns_get_cache: Get cache entries for display
 * -------------------------------------------------------------------------- */
const dns_cache_entry_t* dns_get_cache(int* count) {
    if (count) *count = DNS_MAX_CACHE;
    return cache;
}
