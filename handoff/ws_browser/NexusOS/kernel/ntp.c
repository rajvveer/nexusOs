/* ============================================================================
 * NexusOS — NTP Client — Phase 30
 * ============================================================================
 * Sends NTP request, parses response, and updates the RTC.
 * ============================================================================ */

#include "ntp.h"
#include "udp.h"
#include "ip.h"
#include "dns.h"
#include "net.h"
#include "rtc.h"
#include "vga.h"
#include "string.h"
#include "keyboard.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Local port for NTP */
#define NTP_LOCAL_PORT 10123

/* --------------------------------------------------------------------------
 * ntp_init
 * -------------------------------------------------------------------------- */
void ntp_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("NTP client initialized\n");
}

/* --------------------------------------------------------------------------
 * days_in_month: Get days in a given month (1-12)
 * -------------------------------------------------------------------------- */
static int days_in_month(int month, int year) {
    static const int dm[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        return 29;
    return dm[month];
}

/* --------------------------------------------------------------------------
 * unix_to_rtc: Convert Unix timestamp to rtc_time_t
 * -------------------------------------------------------------------------- */
static void unix_to_rtc(uint32_t ts, rtc_time_t* t) {
    /* Calculate time of day */
    uint32_t daytime = ts % 86400;
    t->hour   = daytime / 3600;
    t->minute = (daytime % 3600) / 60;
    t->second = daytime % 60;

    /* Calculate date */
    uint32_t days = ts / 86400;
    int year = 1970;

    while (1) {
        int yd = 365;
        if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) yd = 366;
        if (days < (uint32_t)yd) break;
        days -= yd;
        year++;
    }
    t->year = year;

    int month = 1;
    while (month <= 12) {
        int md = days_in_month(month, year);
        if (days < (uint32_t)md) break;
        days -= md;
        month++;
    }
    t->month = month;
    t->day = days + 1;
}

/* --------------------------------------------------------------------------
 * ntp_sync_ip: Sync from a specific NTP server IP
 * -------------------------------------------------------------------------- */
bool ntp_sync_ip(uint32_t server_ip) {
    if (server_ip == 0) return false;

    /* Bind local port */
    udp_bind(NTP_LOCAL_PORT);

    /* Build NTP request (version 3, client mode) */
    ntp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.li_vn_mode = (3 << 3) | 3;  /* Version 3, Mode 3 (client) */

    /* Send */
    udp_send(server_ip, NTP_LOCAL_PORT, NTP_PORT,
             &pkt, NTP_PACKET_SIZE);

    /* Wait for response */
    uint32_t start = system_ticks;
    bool got_reply = false;

    while ((system_ticks - start) < NTP_TIMEOUT_TICKS) {
        if (keyboard_has_key()) {
            char k = keyboard_getchar();
            if (k == 27) break;
        }

        udp_datagram_t* dgram = udp_recv(NTP_LOCAL_PORT);
        if (!dgram) {
            net_poll();
            __asm__ volatile("hlt");
            continue;
        }

        if (dgram->length < NTP_PACKET_SIZE) continue;

        memcpy(&pkt, dgram->data, NTP_PACKET_SIZE);
        got_reply = true;
        break;
    }

    udp_unbind(NTP_LOCAL_PORT);

    if (!got_reply) {
        vga_print_color("  NTP: Timeout\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return false;
    }

    /* Extract transmit timestamp (seconds since 1900-01-01) */
    uint32_t ntp_secs = ntohl(pkt.tx_ts_sec);
    if (ntp_secs == 0) {
        vga_print_color("  NTP: Invalid response\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return false;
    }

    /* Convert NTP epoch (1900) to Unix epoch (1970) */
    uint32_t unix_ts = ntp_secs - NTP_EPOCH_OFFSET;

    /* Convert to time struct */
    rtc_time_t new_time;
    unix_to_rtc(unix_ts, &new_time);

    /* Set RTC */
    rtc_set(&new_time);

    /* Print synced time */
    char time_buf[9], date_buf[11];
    rtc_format_time(&new_time, time_buf);
    rtc_format_date(&new_time, date_buf);

    vga_print_color("  NTP: Synced to ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(date_buf);
    vga_print(" ");
    vga_print(time_buf);
    vga_print("\n");

    return true;
}

/* --------------------------------------------------------------------------
 * ntp_sync: Sync from pool.ntp.org (or gateway as fallback)
 * -------------------------------------------------------------------------- */
bool ntp_sync(void) {
    vga_print_color("  NTP: Resolving time server...\n",
                    VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Try pool.ntp.org first */
    uint32_t server_ip = dns_resolve("pool.ntp.org");

    /* Fallback to gateway (QEMU user-mode forwards NTP) */
    if (server_ip == 0) {
        const net_config_t* cfg = ip_get_config();
        server_ip = cfg->gateway;
        vga_print_color("  NTP: Using gateway as server\n",
                        VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    }

    return ntp_sync_ip(server_ip);
}
