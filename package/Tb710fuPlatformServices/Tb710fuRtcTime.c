// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuRtcTime.c - Gregorian conversion and blob packing, no dependencies.
#include "Tb710fuRtcTime.h"

#include <stddef.h>

#define TB710FU_SECONDS_PER_DAY 86400u
#define TB710FU_EPOCH_YEAR 1970
#define TB710FU_EPOCH_WEEKDAY 4u /* 1970-01-01 was a Thursday, 0 = Sunday */

static int tb710fu_is_leap(unsigned year)
{
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static unsigned tb710fu_days_in_month(unsigned year, unsigned month)
{
    static const unsigned days[12] = {
        31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
    };

    if (month < 1u || month > 12u)
        return 0u;
    if (month == 2u && tb710fu_is_leap(year))
        return 29u;
    return days[month - 1u];
}

/* Days since 1970-01-01 for a proleptic Gregorian date. */
static int64_t tb710fu_days_from_civil(int year, unsigned month, unsigned day)
{
    const int adjusted = year - (month <= 2u ? 1 : 0);
    const int era = (adjusted >= 0 ? adjusted : adjusted - 399) / 400;
    const unsigned year_of_era = (unsigned)(adjusted - era * 400);
    const unsigned month_prime = month > 2u ? month - 3u : month + 9u;
    const unsigned day_of_year =
        (153u * month_prime + 2u) / 5u + day - 1u;
    const unsigned day_of_era = year_of_era * 365u + year_of_era / 4u -
        year_of_era / 100u + day_of_year;

    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static void tb710fu_civil_from_days(int64_t days, int *year, unsigned *month,
                                    unsigned *day)
{
    const int64_t shifted = days + 719468;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const unsigned day_of_era = (unsigned)(shifted - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460u + day_of_era / 36524u -
         day_of_era / 146096u) / 365u;
    const unsigned day_of_year = day_of_era -
        (365u * year_of_era + year_of_era / 4u - year_of_era / 100u);
    const unsigned month_index = (5u * day_of_year + 2u) / 153u;
    const unsigned day_of_month =
        day_of_year - (153u * month_index + 2u) / 5u + 1u;
    const unsigned m = month_index < 10u ? month_index + 3u
                                         : month_index - 9u;

    *year = (int)year_of_era + (int)era * 400 + (m <= 2u ? 1 : 0);
    *month = m;
    *day = day_of_month;
}

int tb710fu_rtc_epoch_to_civil(uint64_t epoch,
                               struct tb710fu_civil_time *out)
{
    int64_t days;
    unsigned second_of_day;

    if (out == NULL)
        return -1;
    days = (int64_t)(epoch / TB710FU_SECONDS_PER_DAY);
    second_of_day = (unsigned)(epoch % TB710FU_SECONDS_PER_DAY);
    tb710fu_civil_from_days(days, &out->year, &out->month, &out->day);
    out->hour = second_of_day / 3600u;
    out->minute = (second_of_day / 60u) % 60u;
    out->second = second_of_day % 60u;
    out->weekday = (unsigned)((days + (int64_t)TB710FU_EPOCH_WEEKDAY) % 7);
    return 0;
}

int tb710fu_rtc_civil_to_epoch(const struct tb710fu_civil_time *in,
                               uint64_t *epoch)
{
    int64_t days;
    uint64_t seconds;

    if (in == NULL || epoch == NULL)
        return -1;
    if (in->year < TB710FU_EPOCH_YEAR ||
        in->month < 1u || in->month > 12u ||
        in->day < 1u || in->day > tb710fu_days_in_month((unsigned)in->year,
                                                        in->month) ||
        in->hour > 23u || in->minute > 59u || in->second > 59u)
        return -2;
    days = tb710fu_days_from_civil(in->year, in->month, in->day);
    if (days < 0)
        return -2;
    seconds = (uint64_t)days * TB710FU_SECONDS_PER_DAY +
        (uint64_t)in->hour * 3600u + (uint64_t)in->minute * 60u +
        in->second;
    *epoch = seconds;
    return 0;
}

int tb710fu_rtc_blob_pack(uint64_t seconds, uint32_t useconds, uint8_t *out,
                          unsigned capacity, unsigned *length)
{
    if (out == NULL || length == NULL || useconds >= TB710FU_RTC_USEC_LIMIT)
        return -1;
    if (capacity >= TB710FU_RTC_BLOB_BYTES) {
        for (unsigned i = 0; i < 4u; ++i) {
            out[i] = (uint8_t)((seconds >> (8u * i)) & 0xffu);
            out[4u + i] = (uint8_t)((useconds >> (8u * i)) & 0xffu);
        }
        *length = TB710FU_RTC_BLOB_BYTES;
        return 0;
    }
    if (capacity >= TB710FU_RTC_BLOB_SECONDS_BYTES) {
        for (unsigned i = 0; i < 4u; ++i)
            out[i] = (uint8_t)((seconds >> (8u * i)) & 0xffu);
        *length = TB710FU_RTC_BLOB_SECONDS_BYTES;
        return 0;
    }
    return -1;
}

int tb710fu_rtc_blob_unpack(const uint8_t *blob, unsigned length,
                            uint64_t *seconds, uint32_t *useconds)
{
    uint64_t value = 0;
    uint32_t fraction = 0;

    if (blob == NULL || seconds == NULL || useconds == NULL)
        return -1;
    if (length != TB710FU_RTC_BLOB_SECONDS_BYTES &&
        length != TB710FU_RTC_BLOB_BYTES)
        return -1;
    for (unsigned i = 0; i < 4u; ++i)
        value |= (uint64_t)blob[i] << (8u * i);
    if (length == TB710FU_RTC_BLOB_BYTES) {
        for (unsigned i = 0; i < 4u; ++i)
            fraction |= (uint32_t)blob[4u + i] << (8u * i);
        if (fraction >= TB710FU_RTC_USEC_LIMIT)
            return -2;
    }
    *seconds = value;
    *useconds = fraction;
    return 0;
}
