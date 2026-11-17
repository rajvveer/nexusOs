/* ============================================================================
 * NexusOS — DNS Resolver Header — Phase 28
 * ============================================================================
 * Resolves hostnames to IPv4 addresses via UDP queries to DNS server.
 * ============================================================================ */

#ifndef DNS_H
#define DNS_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define DNS_PORT            53
#define DNS_MAX_CACHE       8       /* Cached DNS entries */
#define DNS_MAX_NAME        64      /* Max hostname length */
#define DNS_TIMEOUT_TICKS   36      /* ~2 seconds at 18.2 Hz */
#define DNS_MAX_PACKET      512     /* Max DNS packet size */

/* DNS record types */
#define DNS_TYPE_A          1       /* IPv4 address */
#define DNS_TYPE_CNAME      5       /* Canonical name */
#define DNS_TYPE_AAAA       28      /* IPv6 (ignored) */

/* DNS classes */
#define DNS_CLASS_IN        1       /* Internet */

/* DNS flags */
#define DNS_FLAG_QR         0x8000  /* Query/Response */
#define DNS_FLAG_RD         0x0100  /* Recursion Desired */
#define DNS_FLAG_RA         0x0080  /* Recursion Available */

/* --------------------------------------------------------------------------
 * DNS Header (12 bytes)
 * -------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t id;            /* Transaction ID */
    uint16_t flags;         /* Flags and codes */
    uint16_t qdcount;       /* Questions */
    uint16_t ancount;       /* Answers */
    uint16_t nscount;       /* Authority records */
    uint16_t arcount;       /* Additional records */
} dns_header_t;

/* --------------------------------------------------------------------------
 * DNS Cache Entry
 * -------------------------------------------------------------------------- */
typedef struct {
    char     name[DNS_MAX_NAME];
    uint32_t ip;            /* Resolved IPv4 address */
    uint32_t ttl;           /* Time-to-live (ticks) */
    uint32_t timestamp;     /* When entry was cached */
    bool     valid;
} dns_cache_entry_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize DNS resolver */
void dns_init(void);

/* Resolve hostname to IPv4 (returns 0 on failure) */
uint32_t dns_resolve(const char* hostname);

/* Get cache for display */
const dns_cache_entry_t* dns_get_cache(int* count);

#endif /* DNS_H */
