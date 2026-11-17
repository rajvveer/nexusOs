/* ============================================================================
 * NexusOS — NTP Client Header — Phase 30
 * ============================================================================
 * Network Time Protocol client for time synchronization.
 * ============================================================================ */

#ifndef NTP_H
#define NTP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define NTP_PORT            123
#define NTP_PACKET_SIZE     48
#define NTP_TIMEOUT_TICKS   54      /* ~3 seconds */
#define NTP_EPOCH_OFFSET    2208988800UL  /* Seconds from 1900 to 1970 */

/* --------------------------------------------------------------------------
 * NTP Packet (48 bytes)
 * -------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;        /* LI(2), VN(3), Mode(3) */
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t rx_ts_sec;
    uint32_t rx_ts_frac;
    uint32_t tx_ts_sec;
    uint32_t tx_ts_frac;
} ntp_packet_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize NTP client */
void ntp_init(void);

/* Sync time from NTP server (blocking, uses DNS for pool.ntp.org) */
bool ntp_sync(void);

/* Sync time from a specific IP */
bool ntp_sync_ip(uint32_t server_ip);

#endif /* NTP_H */
