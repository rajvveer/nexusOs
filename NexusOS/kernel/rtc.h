/* ============================================================================
 * NexusOS — Real-Time Clock Driver (Header)
 * ============================================================================
 * Reads time/date from CMOS RTC via ports 0x70/0x71.
 * ============================================================================ */

#ifndef RTC_H
#define RTC_H

#include "types.h"

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

/* Read current time/date from RTC */
void rtc_read(rtc_time_t* t);

/* Format time as "HH:MM:SS" into buffer (needs 9 bytes) */
void rtc_format_time(rtc_time_t* t, char* buf);

/* Format date as "DD/MM/YYYY" into buffer (needs 11 bytes) */
void rtc_format_date(rtc_time_t* t, char* buf);

/* Set time/date on RTC */
void rtc_set(rtc_time_t* t);

/* Initialize RTC */
void rtc_init(void);

#endif /* RTC_H */
