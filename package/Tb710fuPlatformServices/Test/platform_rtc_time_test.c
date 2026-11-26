// SPDX-License-Identifier: BSD-2-Clause-Patent
// platform_rtc_time_test.c - Gregorian conversion and the IORTC blob.
//
// The conversion is proleptic Gregorian with no leap-second table, weekday 0
// is Sunday, and 1970-01-01 was a Thursday. The epochs below are pinned
// against `date -u`; the leap-year cases cover the 4/100/400 rule.
#include "../Tb710fuRtcTime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void check_civil(uint64_t epoch, int year, unsigned month, unsigned day,
                        unsigned hour, unsigned minute, unsigned second,
                        unsigned weekday)
{
    struct tb710fu_civil_time civil;

    assert(tb710fu_rtc_epoch_to_civil(epoch, &civil) == 0);
    assert(civil.year == year);
    assert(civil.month == month);
    assert(civil.day == day);
    assert(civil.hour == hour);
    assert(civil.minute == minute);
    assert(civil.second == second);
    assert(civil.weekday == weekday);
}

static uint64_t civil_epoch(int year, unsigned month, unsigned day,
                            unsigned hour, unsigned minute, unsigned second)
{
    struct tb710fu_civil_time civil;
    uint64_t epoch = 0;

    memset(&civil, 0, sizeof(civil));
    civil.year = year;
    civil.month = month;
    civil.day = day;
    civil.hour = hour;
    civil.minute = minute;
    civil.second = second;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == 0);
    return epoch;
}

static void test_known_epochs(void)
{
    check_civil(0u, 1970, 1u, 1u, 0u, 0u, 0u, 4u); /* Thursday */
    check_civil(86400u, 1970, 1u, 2u, 0u, 0u, 0u, 5u); /* Friday */
    check_civil(31536000u, 1971, 1u, 1u, 0u, 0u, 0u, 5u);
    check_civil(946684800u, 2000, 1u, 1u, 0u, 0u, 0u, 6u); /* Saturday */
    check_civil(951782400u, 2000, 2u, 29u, 0u, 0u, 0u, 2u);
    check_civil(1672531200u, 2023, 1u, 1u, 0u, 0u, 0u, 0u); /* Sunday */
    check_civil(1709210096u, 2024, 2u, 29u, 12u, 34u, 56u, 4u);
    check_civil(2147483647u, 2038, 1u, 19u, 3u, 14u, 7u, 2u);
    check_civil(4294967295u, 2106, 2u, 7u, 6u, 28u, 15u, 0u);
}

static void test_civil_to_epoch(void)
{
    assert(civil_epoch(1970, 1u, 1u, 0u, 0u, 0u) == 0u);
    assert(civil_epoch(1970, 1u, 2u, 0u, 0u, 0u) == 86400u);
    assert(civil_epoch(1971, 1u, 1u, 0u, 0u, 0u) == 31536000u);
    assert(civil_epoch(2000, 1u, 1u, 0u, 0u, 0u) == 946684800u);
    assert(civil_epoch(2000, 2u, 29u, 0u, 0u, 0u) == 951782400u);
    assert(civil_epoch(2000, 3u, 1u, 0u, 0u, 0u) == 951868800u);
    assert(civil_epoch(2023, 1u, 1u, 0u, 0u, 0u) == 1672531200u);
    assert(civil_epoch(2024, 2u, 29u, 12u, 34u, 56u) == 1709210096u);
    assert(civil_epoch(2038, 1u, 19u, 3u, 14u, 7u) == 2147483647u);
    assert(civil_epoch(2106, 2u, 7u, 6u, 28u, 15u) == 4294967295u);
    assert(civil_epoch(2023, 12u, 31u, 23u, 59u, 59u) == 1704067199u);
    /* The weekday field is an output, not an input: a bogus value must not
     * change the conversion. */
    assert(civil_epoch(2024, 1u, 1u, 1u, 2u, 3u) == 1704070923u);
}

static void test_leap_years(void)
{
    struct tb710fu_civil_time civil;
    uint64_t epoch = 0;

    /* 2000 is divisible by 400: leap. 2100 is divisible by 100 but not 400:
     * not leap. 2400 is leap again. */
    assert(civil_epoch(2000, 2u, 29u, 0u, 0u, 0u) == 951782400u);
    memset(&civil, 0, sizeof(civil));
    civil.year = 2100;
    civil.month = 2u;
    civil.day = 29u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.day = 28u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == 0);
    civil.year = 2400;
    civil.day = 29u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == 0);
    civil.year = 2023;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.year = 2024;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == 0);
}

static void test_invalid_dates(void)
{
    struct tb710fu_civil_time civil;
    uint64_t epoch = 0;

    memset(&civil, 0, sizeof(civil));
    civil.year = 2024;
    civil.month = 4u;
    civil.day = 31u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.month = 6u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.month = 12u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == 0);

    memset(&civil, 0, sizeof(civil));
    civil.year = 2024;
    civil.month = 1u;
    civil.day = 1u;
    civil.hour = 24u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.hour = 23u;
    civil.minute = 60u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.minute = 59u;
    civil.second = 60u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.second = 59u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == 0);

    memset(&civil, 0, sizeof(civil));
    civil.year = 1969;
    civil.month = 12u;
    civil.day = 31u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.year = 2024;
    civil.month = 0u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.month = 13u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.month = 1u;
    civil.day = 0u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);
    civil.day = 32u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, &epoch) == -2);

    assert(tb710fu_rtc_civil_to_epoch(NULL, &epoch) == -1);
    civil.year = 2024;
    civil.month = 1u;
    civil.day = 1u;
    civil.hour = 0u;
    civil.minute = 0u;
    civil.second = 0u;
    assert(tb710fu_rtc_civil_to_epoch(&civil, NULL) == -1);
    assert(tb710fu_rtc_epoch_to_civil(0u, NULL) == -1);
}

static void test_round_trip(void)
{
    const uint64_t epochs[] = {
        0u, 86400u, 946684800u, 951782400u, 1672531200u, 1709210096u,
        2147483647u, 4294967295u,
    };
    size_t i;

    for (i = 0; i < sizeof(epochs) / sizeof(epochs[0]); ++i) {
        struct tb710fu_civil_time civil;
        uint64_t back = 0;

        assert(tb710fu_rtc_epoch_to_civil(epochs[i], &civil) == 0);
        assert(tb710fu_rtc_civil_to_epoch(&civil, &back) == 0);
        assert(back == epochs[i]);
    }
}

static void test_blob_pack(void)
{
    uint8_t blob[8];
    unsigned length = 0;

    memset(blob, 0xaau, sizeof(blob));
    assert(tb710fu_rtc_blob_pack(0x11223344u, 0x000a0b0cu, blob, 8u,
                                 &length) == 0);
    assert(length == 8u);
    assert(blob[0] == 0x44u);
    assert(blob[1] == 0x33u);
    assert(blob[2] == 0x22u);
    assert(blob[3] == 0x11u);
    assert(blob[4] == 0x0cu);
    assert(blob[5] == 0x0bu);
    assert(blob[6] == 0x0au);
    assert(blob[7] == 0x00u);

    /* Seven bytes of capacity still gets the seconds-only layout. */
    memset(blob, 0xaau, sizeof(blob));
    length = 0;
    assert(tb710fu_rtc_blob_pack(0x11223344u, 123u, blob, 7u, &length) == 0);
    assert(length == 4u);
    assert(blob[0] == 0x44u);
    assert(blob[1] == 0x33u);
    assert(blob[2] == 0x22u);
    assert(blob[3] == 0x11u);
    assert(blob[4] == 0xaau);

    memset(blob, 0xaau, sizeof(blob));
    length = 0;
    assert(tb710fu_rtc_blob_pack(0x100000001u, 0u, blob, 4u, &length) == 0);
    assert(length == 4u);
    assert(blob[0] == 0x01u);
    assert(blob[1] == 0x00u);
    assert(blob[2] == 0x00u);
    assert(blob[3] == 0x00u);

    length = 0xffffu;
    assert(tb710fu_rtc_blob_pack(0u, 0u, blob, 3u, &length) == -1);
    assert(length == 0xffffu);
    assert(tb710fu_rtc_blob_pack(0u, TB710FU_RTC_USEC_LIMIT, blob, 8u,
                                 &length) == -1);
    assert(tb710fu_rtc_blob_pack(0u, 0u, NULL, 8u, &length) == -1);
    assert(tb710fu_rtc_blob_pack(0u, 0u, blob, 8u, NULL) == -1);
}

static void test_blob_unpack(void)
{
    const uint8_t four[4] = {0x44u, 0x33u, 0x22u, 0x11u};
    const uint8_t eight[8] = {0x44u, 0x33u, 0x22u, 0x11u,
                              0x0cu, 0x0bu, 0x0au, 0x00u};
    const uint8_t bad_fraction[8] = {0x00u, 0x00u, 0x00u, 0x00u,
                                     0x40u, 0x42u, 0x0fu, 0x00u};
    uint64_t seconds = 0xfeedfaceu;
    uint32_t useconds = 0xdeadbeefu;
    unsigned length;

    assert(tb710fu_rtc_blob_unpack(four, 4u, &seconds, &useconds) == 0);
    assert(seconds == 0x11223344u);
    assert(useconds == 0u);
    assert(tb710fu_rtc_blob_unpack(eight, 8u, &seconds, &useconds) == 0);
    assert(seconds == 0x11223344u);
    assert(useconds == 0x000a0b0cu);

    /* 0x000f4240 is exactly TB710FU_RTC_USEC_LIMIT. */
    seconds = 0xfeedfaceu;
    useconds = 0xdeadbeefu;
    assert(tb710fu_rtc_blob_unpack(bad_fraction, 8u, &seconds, &useconds) ==
           -2);
    assert(seconds == 0xfeedfaceu);
    assert(useconds == 0xdeadbeefu);

    for (length = 0u; length <= 8u; ++length) {
        if (length == 4u || length == 8u)
            continue;
        assert(tb710fu_rtc_blob_unpack(eight, length, &seconds, &useconds) ==
               -1);
    }
    assert(tb710fu_rtc_blob_unpack(NULL, 4u, &seconds, &useconds) == -1);
    assert(tb710fu_rtc_blob_unpack(eight, 4u, NULL, &useconds) == -1);
    assert(tb710fu_rtc_blob_unpack(eight, 4u, &seconds, NULL) == -1);
}

int main(void)
{
    printf("platform_rtc_time_test: epoch/civil conversion and blob\n");
    test_known_epochs();
    printf("ok  known epochs and weekdays\n");
    test_civil_to_epoch();
    printf("ok  civil to epoch\n");
    test_leap_years();
    printf("ok  4/100/400 leap rule\n");
    test_invalid_dates();
    printf("ok  invalid fields refused\n");
    test_round_trip();
    printf("ok  round trip\n");
    test_blob_pack();
    printf("ok  blob pack layouts and limits\n");
    test_blob_unpack();
    printf("ok  blob unpack layouts and limits\n");
    printf("platform_rtc_time_test: PASS\n");
    return 0;
}
