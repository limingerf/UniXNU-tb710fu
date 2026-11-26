// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuRtcTime.h - epoch/civil conversion and the IORTC byte layout.
//
// The PMK8550 counter is plain epoch seconds (rtc-pm8xxx.c hands them to
// rtc_time64_to_tm unchanged after adding the stored offset), so the calendar
// has to be computed here: XNU's pexpert cannot call libc's gmtime, and the
// kernel glue needs day/month/year for logs and for the IORTCController blob.
//
// The conversion is the civil-from-days algorithm (proleptic Gregorian,
// 400-year cycle, no leap-second table). Weekday 0 is Sunday; 1970-01-01 was
// a Thursday, which the tests pin.
//
// The blob is this port's contract with its own IORTCController: four bytes
// of little-endian epoch seconds, then - when the caller has room - four
// bytes of little-endian microseconds. Nothing else in the tree consumes it,
// so the definition lives here rather than in a copy of Apple's on-disk RTC
// layout, which this board does not implement.
#ifndef TB710FU_RTC_TIME_H
#define TB710FU_RTC_TIME_H

#include <stdint.h>

struct tb710fu_civil_time {
    int year;         /* 1970.. */
    unsigned month;   /* 1..12 */
    unsigned day;     /* 1..31 */
    unsigned hour;    /* 0..23 */
    unsigned minute;  /* 0..59 */
    unsigned second;  /* 0..59 */
    unsigned weekday; /* 0 = Sunday */
};

#define TB710FU_RTC_BLOB_BYTES 8u
#define TB710FU_RTC_BLOB_SECONDS_BYTES 4u
#define TB710FU_RTC_BLOB_USEC_BYTES 4u
#define TB710FU_RTC_USEC_LIMIT 1000000u

/* 0 on success, TB710FU_RTC_ARGUMENT-style negative values otherwise. */
int tb710fu_rtc_epoch_to_civil(uint64_t epoch,
                               struct tb710fu_civil_time *out);
int tb710fu_rtc_civil_to_epoch(const struct tb710fu_civil_time *in,
                               uint64_t *epoch);

/* Pack/unpack the IORTCController blob. pack writes eight bytes when capacity
 * allows, four otherwise; *length receives the count written. */
int tb710fu_rtc_blob_pack(uint64_t seconds, uint32_t useconds, uint8_t *out,
                          unsigned capacity, unsigned *length);
int tb710fu_rtc_blob_unpack(const uint8_t *blob, unsigned length,
                            uint64_t *seconds, uint32_t *useconds);

#endif
