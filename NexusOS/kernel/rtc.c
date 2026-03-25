/* ============================================================================
 * NexusOS — Real-Time Clock Driver (Implementation)
 * ============================================================================
 * Reads time/date from CMOS RTC chip using I/O ports 0x70 (index) and 0x71
 * (data). Values are in BCD format, converted to binary.
 * ============================================================================ */

#include "rtc.h"
#include "vga.h"
#include "port.h"

/* CMOS I/O ports */
#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

/* CMOS registers */
#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

/* --------------------------------------------------------------------------
 * Read a CMOS register
 * -------------------------------------------------------------------------- */
static uint8_t cmos_read(uint8_t reg) {
    port_byte_out(CMOS_ADDRESS, reg);
    return port_byte_in(CMOS_DATA);
}

/* --------------------------------------------------------------------------
 * Check if RTC update is in progress
 * -------------------------------------------------------------------------- */
static int rtc_updating(void) {
    port_byte_out(CMOS_ADDRESS, RTC_STATUS_A);
    return (port_byte_in(CMOS_DATA) & 0x80);
}

/* --------------------------------------------------------------------------
 * BCD to binary conversion
 * -------------------------------------------------------------------------- */
static uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

/* --------------------------------------------------------------------------
 * rtc_read: Read current time and date
 * -------------------------------------------------------------------------- */
void rtc_read(rtc_time_t* t) {
    /* Wait until update is not in progress */
    while (rtc_updating());

    t->second = cmos_read(RTC_SECONDS);
    t->minute = cmos_read(RTC_MINUTES);
    t->hour   = cmos_read(RTC_HOURS);
    t->day    = cmos_read(RTC_DAY);
    t->month  = cmos_read(RTC_MONTH);
    t->year   = cmos_read(RTC_YEAR);

    /* Check if values are in BCD format (status register B, bit 2) */
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    if (!(status_b & 0x04)) {
        /* Convert BCD to binary */
        t->second = bcd_to_bin(t->second);
        t->minute = bcd_to_bin(t->minute);
        t->hour   = bcd_to_bin(t->hour & 0x7F) | (t->hour & 0x80);
        t->day    = bcd_to_bin(t->day);
        t->month  = bcd_to_bin(t->month);
        t->year   = bcd_to_bin(t->year);
    }

    /* Convert 12-hour to 24-hour if needed */
    if (!(status_b & 0x02) && (t->hour & 0x80)) {
        t->hour = ((t->hour & 0x7F) + 12) % 24;
    }

    /* Year is 2-digit, assume 2000s */
    t->year += 2000;
}

/* --------------------------------------------------------------------------
 * rtc_format_time: Format as "HH:MM:SS"
 * -------------------------------------------------------------------------- */
void rtc_format_time(rtc_time_t* t, char* buf) {
    buf[0] = '0' + (t->hour / 10);
    buf[1] = '0' + (t->hour % 10);
    buf[2] = ':';
    buf[3] = '0' + (t->minute / 10);
    buf[4] = '0' + (t->minute % 10);
    buf[5] = ':';
    buf[6] = '0' + (t->second / 10);
    buf[7] = '0' + (t->second % 10);
    buf[8] = '\0';
}

/* --------------------------------------------------------------------------
 * rtc_format_date: Format as "DD/MM/YYYY"
 * -------------------------------------------------------------------------- */
void rtc_format_date(rtc_time_t* t, char* buf) {
    buf[0] = '0' + (t->day / 10);
    buf[1] = '0' + (t->day % 10);
    buf[2] = '/';
    buf[3] = '0' + (t->month / 10);
    buf[4] = '0' + (t->month % 10);
    buf[5] = '/';
    uint16_t y = t->year;
    buf[6] = '0' + (y / 1000); y %= 1000;
    buf[7] = '0' + (y / 100);  y %= 100;
    buf[8] = '0' + (y / 10);   y %= 10;
    buf[9] = '0' + y;
    buf[10] = '\0';
}

/* --------------------------------------------------------------------------
 * Write a CMOS register
 * -------------------------------------------------------------------------- */
static void cmos_write(uint8_t reg, uint8_t val) {
    port_byte_out(CMOS_ADDRESS, reg);
    port_byte_out(CMOS_DATA, val);
}

/* --------------------------------------------------------------------------
 * Binary to BCD conversion
 * -------------------------------------------------------------------------- */
static uint8_t bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}

/* --------------------------------------------------------------------------
 * rtc_set: Set time and date on the CMOS RTC
 * -------------------------------------------------------------------------- */
void rtc_set(rtc_time_t* t) {
    /* Wait for any update to finish */
    while (rtc_updating());

    /* Check if RTC uses BCD (status register B, bit 2) */
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    bool is_bcd = !(status_b & 0x04);

    uint8_t sec  = t->second;
    uint8_t min  = t->minute;
    uint8_t hr   = t->hour;
    uint8_t day  = t->day;
    uint8_t mon  = t->month;
    uint8_t yr   = (uint8_t)(t->year % 100);

    if (is_bcd) {
        sec = bin_to_bcd(sec);
        min = bin_to_bcd(min);
        hr  = bin_to_bcd(hr);
        day = bin_to_bcd(day);
        mon = bin_to_bcd(mon);
        yr  = bin_to_bcd(yr);
    }

    /* Disable RTC updates while writing */
    cmos_write(RTC_STATUS_B, status_b | 0x80);

    cmos_write(RTC_SECONDS, sec);
    cmos_write(RTC_MINUTES, min);
    cmos_write(RTC_HOURS, hr);
    cmos_write(RTC_DAY, day);
    cmos_write(RTC_MONTH, mon);
    cmos_write(RTC_YEAR, yr);

    /* Re-enable RTC updates */
    cmos_write(RTC_STATUS_B, status_b & ~0x80);
}

/* --------------------------------------------------------------------------
 * rtc_init: Initialize RTC
 * -------------------------------------------------------------------------- */
void rtc_init(void) {
    rtc_time_t t;
    rtc_read(&t);

    char time_buf[9], date_buf[11];
    rtc_format_time(&t, time_buf);
    rtc_format_date(&t, date_buf);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("RTC initialized (");
    vga_print(date_buf);
    vga_print(" ");
    vga_print(time_buf);
    vga_print(")\n");
}

