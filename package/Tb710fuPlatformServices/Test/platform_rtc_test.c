// SPDX-License-Identifier: BSD-2-Clause-Patent
// platform_rtc_test.c - PMK8550 wall clock against the host arbiter model.
//
// This is the stage-2 port of the stage-1 RTC test: the public surface is now
// tb710fu_rtc_open() + tb710fu_rtc_read()/tb710fu_rtc_set_epoch(), the offset
// lives in reading.offset instead of reading.cell, and the counter read is the
// mainline four-byte read followed by an LSB re-read. The scenarios the old
// file pinned (agreement, carry, instability, stopped control, empty and
// foreign cells, overflow, missing device, arbiter errors, every store guard)
// are all still here, plus the stage-2 fine-source fraction rules.
#include "../Tb710fuPmk8550Rtc.h"
#include "../Tb710fuRtcTime.h"
#include "host_spmi_model.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_COUNTER 1000u
#define TEST_COUNTER_WALL 0x65000000u /* above TB710FU_RTC_EARLIEST */
#define TEST_CELL 0x6a000000u
#define TEST_SECONDS 0x6a001000u

static struct host_spmi_model model;
static struct tb710fu_spmi_io g_io;

static uint64_t g_fine_values[16];
static unsigned g_fine_count;
static unsigned g_fine_reads;

static uint64_t fake_nanoseconds(void *context)
{
    (void)context;
    assert(g_fine_reads < g_fine_count);
    return g_fine_values[g_fine_reads++];
}

static void set_fine(uint64_t first, uint64_t second)
{
    g_fine_values[0] = first;
    g_fine_values[1] = second;
    g_fine_count = 2u;
    g_fine_reads = 0u;
}

static void fixture(void)
{
    host_spmi_init(&model);
    host_spmi_set_counter(&model, TEST_COUNTER);
    g_io = host_spmi_io(&model);
    g_fine_count = 0u;
    g_fine_reads = 0u;
}

static int open_clock(struct tb710fu_pmk8550_rtc *clock, int allow_store,
                      const struct tb710fu_rtc_fine_source *fine)
{
    struct tb710fu_pmk8550_rtc_config config;

    tb710fu_rtc_config_default(&config, 0u);
    config.allow_store = allow_store;
    return tb710fu_rtc_open(clock, &g_io, &config, fine);
}

static void test_contract(void)
{
    assert(TB710FU_PMK8550_SID == 0u);
    assert(TB710FU_PMK8550_RTC_PPID == 0x061u);
    assert(TB710FU_PMK8550_RTC_USID == 0x061u);
    assert(TB710FU_PMK8550_SDAM2_PPID == 0x071u);
    assert(TB710FU_PMK8550_SDAM2_USID == 0x071u);
    assert(TB710FU_PMK8550_RTC_CONTROL == 0x46u);
    assert(TB710FU_PMK8550_RTC_RUNNING == 0x80u);
    assert(TB710FU_PMK8550_RTC_COUNTER == 0x48u);
    assert(TB710FU_PMK8550_RTC_COUNTER_BYTES == 4u);
    assert(TB710FU_PMK8550_SDAM2_REBOOT_REASON == 0x48u);
    assert(TB710FU_PMK8550_SDAM2_RTC_OFFSET == 0xbcu);
    assert(tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_READL, 0x48u, 4u) ==
           0x08000483u);
    assert(tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_WRITEL, 0xbcu, 4u) ==
           0x00000bc3u);
    assert(tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_READL, 0x46u, 1u) ==
           0x08000460u);
    assert(TB710FU_SPMI_POLL_LIMIT == 100000u);
    assert(TB710FU_RTC_EARLIEST == 1672531200u);
    assert(TB710FU_RTC_READ_TRIES == 4u);
}

static void test_names(void)
{
    const int results[] = {
        TB710FU_RTC_OK,       TB710FU_RTC_EMPTY,     TB710FU_RTC_UNSTABLE,
        TB710FU_RTC_STOPPED,  TB710FU_RTC_FOREIGN,   TB710FU_RTC_RANGE,
        TB710FU_RTC_READ_ONLY, TB710FU_RTC_CURRENT,  TB710FU_RTC_MISMATCH,
        TB710FU_RTC_NOT_OWNER, TB710FU_RTC_DENIED,   TB710FU_RTC_FAILED,
        TB710FU_RTC_DROPPED,  TB710FU_RTC_TIMEOUT,   TB710FU_RTC_NO_DEVICE,
        TB710FU_RTC_ARGUMENT,
    };
    size_t i;

    for (i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        const char *name = tb710fu_rtc_result_name(results[i]);

        assert(name != NULL);
        assert(name[0] != '\0');
    }
    assert(strcmp(tb710fu_rtc_result_name(TB710FU_RTC_DENIED),
                  tb710fu_rtc_result_name(TB710FU_RTC_FAILED)) != 0);
    assert(strcmp(tb710fu_rtc_result_name(TB710FU_RTC_OK),
                  tb710fu_rtc_result_name(TB710FU_RTC_UNSTABLE)) != 0);
    assert(tb710fu_rtc_result_name(12345) != NULL);
}

static void test_open_arguments(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_pmk8550_rtc_config config;

    fixture();
    assert(tb710fu_rtc_open(NULL, &g_io, NULL, NULL) ==
           TB710FU_RTC_ARGUMENT);
    assert(tb710fu_rtc_open(&clock, NULL, NULL, NULL) ==
           TB710FU_RTC_ARGUMENT);
    tb710fu_rtc_config_default(&config, 8u);
    assert(tb710fu_rtc_open(&clock, &g_io, &config, NULL) ==
           TB710FU_RTC_ARGUMENT);
    tb710fu_rtc_config_default(NULL, 0u); /* must be a no-op */

    assert(tb710fu_rtc_open(&clock, &g_io, NULL, NULL) == TB710FU_RTC_OK);
    assert(clock.ready == 1);
    assert(clock.config.ee == 0u);
    assert(clock.config.allow_store == 0);
    assert(clock.rtc_apid == 2u);
    assert(clock.sdam_valid == 1);
    assert(clock.sdam_apid == 3u);

    memset(&clock, 0, sizeof(clock));
    assert(tb710fu_rtc_read(&clock, NULL) == TB710FU_RTC_ARGUMENT);
    {
        struct tb710fu_rtc_reading reading;

        assert(tb710fu_rtc_read(NULL, &reading) == TB710FU_RTC_ARGUMENT);
    }
}

static void test_read_ok(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.seconds_valid == 1);
    assert(reading.seconds == (uint64_t)TEST_COUNTER + TEST_CELL);
    assert(reading.counter == TEST_COUNTER);
    assert(reading.offset == TEST_CELL);
    assert(reading.useconds_valid == 0);
    assert(reading.useconds == 0u);
    assert(model.read_commands == 6u); /* control x2, counter+LSB, cell x2 */
    assert(model.counter_reads == 1u);
    assert(model.counter_lsb_reads == 1u);
    assert(model.cell_reads == 2u);
}

static void test_read_counter_alone(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_set_counter(&model, TEST_COUNTER_WALL);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.seconds == TEST_COUNTER_WALL);
    assert(reading.counter == TEST_COUNTER_WALL);
    assert(reading.offset == 0u);
    assert(reading.seconds_valid == 1);
    assert(reading.useconds_valid == 0);
}

static void test_counter_carry_retry(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    const uint32_t counters[] = {0x650000ffu, 0x65000100u};
    const uint32_t lsbs[] = {0x00u, 0x00u};

    fixture();
    host_spmi_set_counter_sequence(&model, counters, 2u);
    host_spmi_set_counter_lsb_sequence(&model, lsbs, 2u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    /* The first LSB was smaller than the four-byte read's LSB, so the carry
     * sequence repeated and the second value is the accepted one. */
    assert(reading.counter == 0x65000100u);
    assert(reading.seconds == 0x65000100u);
    assert(model.counter_reads == 2u);
    assert(model.counter_lsb_reads == 2u);
    assert(model.read_commands == 8u); /* control x2 + two attempt pairs + cell x2 */
}

static void test_counter_unstable(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    const uint32_t counters[] = {0x65000001u, 0x65000002u, 0x65000003u,
                                 0x65000004u};
    const uint32_t lsbs[] = {0x00u, 0x00u, 0x00u, 0x00u};

    fixture();
    host_spmi_set_counter_sequence(&model, counters, 4u);
    host_spmi_set_counter_lsb_sequence(&model, lsbs, 4u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_UNSTABLE);
    assert(reading.seconds_valid == 0);
    assert(reading.seconds == 0u);
    assert(reading.counter == 0u);
    assert(model.counter_reads == TB710FU_RTC_READ_TRIES);
    assert(model.counter_lsb_reads == TB710FU_RTC_READ_TRIES);
}

static void test_control_stopped(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_set_control(&model, 0u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_STOPPED);
    assert(reading.seconds_valid == 0);
    assert(reading.seconds == 0u);
    assert(model.counter_reads == 0u);
    assert(model.read_commands == 2u);

    /* Bit 7 is the only running bit: another set bit does not qualify. */
    fixture();
    host_spmi_set_control(&model, 0x40u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_STOPPED);
}

static void test_cell_unstable(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    const uint32_t cells[] = {0x00000100u, 0x00000200u, 0x00000300u,
                              0x00000400u, 0x00000500u, 0x00000600u,
                              0x00000700u, 0x00000800u};

    fixture();
    host_spmi_set_cell_sequence(&model, cells, 8u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_UNSTABLE);
    assert(reading.seconds_valid == 0);
    assert(reading.offset == 0u);
    /* Two reads per attempt, four attempts. */
    assert(model.cell_reads == 2u * TB710FU_RTC_READ_TRIES);
}

static void test_empty_cell(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_set_cell(&model, 0u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_EMPTY);
    assert(reading.seconds_valid == 0);
    assert(reading.seconds == 0u);
    assert(reading.counter == TEST_COUNTER);
    assert(reading.offset == 0u);
}

static void test_foreign_cell(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_set_cell(&model, 4u); /* counter + 4 is 1970, not a wall time */
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_FOREIGN);
    assert(reading.seconds_valid == 0);
    assert(reading.seconds == 0u);
    assert(reading.counter == TEST_COUNTER);
    assert(reading.offset == 0u);
}

static void test_time_range(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_set_counter(&model, 0xffffff00u);
    host_spmi_set_cell(&model, 0x200u); /* sum crosses 32 bits */
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_RANGE);
    assert(reading.seconds_valid == 0);
    assert(reading.seconds == 0u);
}

static void test_sdam_absent(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_unmap(&model, 3u);
    host_spmi_set_counter(&model, TEST_COUNTER_WALL);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(clock.sdam_valid == 0);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.seconds == TEST_COUNTER_WALL);
    assert(reading.offset == 0u);
    assert(model.read_commands == 4u); /* no cell transfer */
}

static void test_no_device(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    host_spmi_unmap(&model, 2u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_NO_DEVICE);

    fixture();
    model.core[TB710FU_SPMI_VERSION / 4u] = TB710FU_SPMI_VERSION_V8_MIN;
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_NO_DEVICE);

    fixture();
    model.core[TB710FU_SPMI_FEATURES / 4u] = 0u;
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_NO_DEVICE);

    /* With an RTC but no SDAM the clock still opens; only the offset is gone. */
    fixture();
    host_spmi_unmap(&model, 3u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(clock.sdam_valid == 0);
    host_spmi_set_control(&model, 0u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_STOPPED);
}

static void test_arbiter_errors(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;

    fixture();
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DENIED);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_DENIED);
    assert(reading.seconds_valid == 0);

    host_spmi_set_error(&model, HOST_SPMI_ERROR_FAILURE);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_FAILED);
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DROPPED);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_DROPPED);
    host_spmi_set_error(&model, HOST_SPMI_ERROR_SILENT);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_TIMEOUT);
    assert(reading.seconds_valid == 0);
}

static void test_read_epoch_helper(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    uint64_t seconds = 0u;

    fixture();
    host_spmi_set_counter(&model, TEST_COUNTER_WALL);
    assert(tb710fu_rtc_read_epoch(&g_io, 0u, &seconds) == TB710FU_RTC_OK);
    assert(seconds == TEST_COUNTER_WALL);

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(tb710fu_rtc_read_epoch(&g_io, 0u, &seconds) == TB710FU_RTC_OK);
    assert(seconds == (uint64_t)TEST_COUNTER + TEST_CELL);

    fixture();
    host_spmi_set_control(&model, 0u);
    seconds = 0xfeedfaceu;
    assert(tb710fu_rtc_read_epoch(&g_io, 0u, &seconds) ==
           TB710FU_RTC_STOPPED);
    assert(seconds == 0xfeedfaceu); /* untouched on failure */
    assert(tb710fu_rtc_read_epoch(&g_io, 0u, NULL) == TB710FU_RTC_ARGUMENT);

    fixture();
    host_spmi_set_control(&model, 0u);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_STOPPED);
}

static void test_fine_source_anchor(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    const struct tb710fu_rtc_fine_source fine = {NULL, fake_nanoseconds};

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(open_clock(&clock, 0, &fine) == TB710FU_RTC_OK);

    set_fine(0u, 1000000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.seconds_valid == 1);
    assert(reading.useconds_valid == 1);
    assert(reading.useconds == 0u); /* the first read establishes the anchor */
    assert(clock.anchor_valid == 1);
    assert(clock.anchor_ns == 500000u);
    assert(clock.anchor_second == (uint32_t)reading.seconds);

    set_fine(1000000u, 2000000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.useconds_valid == 1);
    assert(reading.useconds == 1000u); /* 1 ms after the anchor */

    set_fine(2000000u, 3000000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.useconds == 2000u);
}

static void test_fine_source_invalid(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    const struct tb710fu_rtc_fine_source fine = {NULL, fake_nanoseconds};

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(open_clock(&clock, 0, &fine) == TB710FU_RTC_OK);

    set_fine(0u, 1000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.useconds_valid == 1);
    assert(clock.anchor_valid == 1);
    assert(clock.anchor_ns == 500u);

    /* A fine source that runs backwards must not produce a fraction. */
    set_fine(2000u, 1000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.seconds_valid == 1);
    assert(reading.useconds_valid == 0);
    assert(clock.anchor_ns == 500u);

    /* A read that spans a whole second cannot place the fraction either. */
    set_fine(0u, 1000000001u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.seconds_valid == 1);
    assert(reading.useconds_valid == 0);
    assert(clock.anchor_ns == 500u);
}

static void test_fine_source_second_change(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    const struct tb710fu_rtc_fine_source fine = {NULL, fake_nanoseconds};

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(open_clock(&clock, 0, &fine) == TB710FU_RTC_OK);
    set_fine(0u, 1000000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.useconds == 0u);
    assert(clock.anchor_second == (uint32_t)reading.seconds);

    /* The wall second moved, so the old anchor no longer describes it. */
    host_spmi_set_cell(&model, TEST_CELL + 1u);
    set_fine(1000000u, 2000000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.useconds_valid == 1);
    assert(reading.useconds == 0u);
    assert(clock.anchor_second == (uint32_t)reading.seconds);

    /* ... but the same second still reuses it. */
    set_fine(2000000u, 3000000u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.useconds == 1000u);
}

static void test_fine_source_clamp(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    const struct tb710fu_rtc_fine_source fine = {NULL, fake_nanoseconds};

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(open_clock(&clock, 0, &fine) == TB710FU_RTC_OK);
    set_fine(0u, 0u);
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(clock.anchor_ns == 0u);

    set_fine(1500000000u, 1600000000u); /* the read itself took 100 ms */
    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.useconds_valid == 1);
    assert(reading.useconds == TB710FU_RTC_USEC_LIMIT - 1u);
}

static void test_store_read_only(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(open_clock(&clock, 0, NULL) == TB710FU_RTC_OK);
    assert(clock.config.allow_store == 0);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_READ_ONLY);
    assert(model.commands == 0u);
    assert(model.cell_writes == 0u);
    assert(model.cell_values[0] == TEST_CELL);
}

static void test_store_no_sdam(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;

    fixture();
    host_spmi_unmap(&model, 3u);
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    assert(clock.sdam_valid == 0);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_NO_DEVICE);
    assert(model.commands == 0u);
    assert(model.cell_writes == 0u);
}

static void test_store_range(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;
    const uint64_t too_late = (uint64_t)0xffffffffu + 1u;

    fixture();
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);

    host_spmi_set_counter(&model, TEST_COUNTER_WALL);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_COUNTER_WALL, &store) ==
           TB710FU_RTC_RANGE); /* equal to the counter cannot be an offset */
    assert(tb710fu_rtc_set_epoch(&clock, 1000u, &store) == TB710FU_RTC_RANGE);
    assert(tb710fu_rtc_set_epoch(&clock, too_late, &store) ==
           TB710FU_RTC_RANGE);
    assert(store.written == 0);
    assert(model.cell_writes == 0u);
}

static void test_store_foreign_cell(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;

    fixture();
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    host_spmi_set_cell(&model, 0xdeadbeefu); /* occupies the cell, far ahead */
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_FOREIGN);
    assert(store.before == 0xdeadbeefu);
    assert(store.written == 0);
    assert(model.cell_writes == 0u);

    host_spmi_set_cell(&model, 4u); /* nonzero but below any wall clock */
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_FOREIGN);
    assert(store.before == 4u);
    assert(model.cell_writes == 0u);
}

static void test_store_not_owner(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;

    fixture();
    host_spmi_set_owner(&model, 3u, 1u);
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_NOT_OWNER);
    assert(store.written == 0);
    assert(store.counter == 0u); /* stopped before the cell was touched */
    assert(model.cell_writes == 0u);
}

static void test_store_current(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;
    const uint32_t offset = (uint32_t)(TEST_SECONDS - TEST_COUNTER);

    fixture();
    host_spmi_set_cell(&model, offset);
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_CURRENT);
    assert(store.before == offset);
    assert(store.offset == offset);
    assert(store.after == offset);
    assert(store.written == 0);
    assert(model.cell_writes == 0u);
}

static void test_store_ok(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;
    const uint32_t offset = (uint32_t)(TEST_SECONDS - TEST_COUNTER);

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_OK);
    assert(store.counter == TEST_COUNTER);
    assert(store.before == TEST_CELL);
    assert(store.offset == offset);
    assert(store.after == offset);
    assert(store.written == 1);
    assert(model.cell_values[0] == offset);
    assert(model.cell_writes == 1u);
    assert(model.write_commands == 1u);

    /* The result is optional. */
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS + 5u, NULL) ==
           TB710FU_RTC_OK);
    assert(model.cell_values[0] == offset + 5u);
}

static void test_store_mismatch(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;

    fixture();
    host_spmi_set_cell(&model, TEST_CELL);
    host_spmi_set_drop_writes(&model, 1);
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_MISMATCH);
    assert(store.before == TEST_CELL);
    assert(store.after == TEST_CELL);
    assert(store.written == 1);
    assert(model.cell_writes == 1u);
    assert(model.cell_values[0] == TEST_CELL);
}

static void test_store_unstable(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;
    const uint32_t cells[] = {0x100u, 0x200u, 0x300u, 0x400u,
                              0x500u, 0x600u, 0x700u, 0x800u};

    fixture();
    host_spmi_set_cell_sequence(&model, cells, 8u);
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_UNSTABLE);
    assert(store.written == 0);
    assert(model.cell_writes == 0u);
}

static void test_store_denied(void)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_set_result store;

    fixture();
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DENIED);
    assert(open_clock(&clock, 1, NULL) == TB710FU_RTC_OK);
    assert(tb710fu_rtc_set_epoch(&clock, TEST_SECONDS, &store) ==
           TB710FU_RTC_DENIED);
    assert(store.written == 0);
    assert(model.cell_writes == 0u);
}

int main(void)
{
    printf("platform_rtc_test: PMK8550 wall clock\n");
    test_contract();
    printf("ok  register and command contract\n");
    test_names();
    printf("ok  result names\n");
    test_open_arguments();
    printf("ok  open defaults and argument checks\n");
    test_read_ok();
    printf("ok  counter plus stored offset\n");
    test_read_counter_alone();
    printf("ok  counter alone with an empty cell\n");
    test_counter_carry_retry();
    printf("ok  carry detected through the LSB re-read\n");
    test_counter_unstable();
    printf("ok  unstable counter refused\n");
    test_control_stopped();
    printf("ok  stopped control refused before any time\n");
    test_cell_unstable();
    printf("ok  unstable offset cell refused\n");
    test_empty_cell();
    printf("ok  empty cell below the plausibility floor\n");
    test_foreign_cell();
    printf("ok  foreign cell refused\n");
    test_time_range();
    printf("ok  32-bit overflow refused\n");
    test_sdam_absent();
    printf("ok  optional SDAM2 absent\n");
    test_no_device();
    printf("ok  missing or foreign arbiter layout\n");
    test_arbiter_errors();
    printf("ok  denied, failed, dropped and timed-out transfers\n");
    test_read_epoch_helper();
    printf("ok  read_epoch convenience\n");
    test_fine_source_anchor();
    printf("ok  fine source anchoring and fraction\n");
    test_fine_source_invalid();
    printf("ok  backwards and whole-second fine reads\n");
    test_fine_source_second_change();
    printf("ok  anchor resets when the wall second changes\n");
    test_fine_source_clamp();
    printf("ok  fraction clamps below one second\n");
    test_store_read_only();
    printf("ok  default store policy is read-only\n");
    test_store_no_sdam();
    printf("ok  store without SDAM2 refused\n");
    test_store_range();
    printf("ok  store range guards\n");
    test_store_foreign_cell();
    printf("ok  foreign and occupied cell refused\n");
    test_store_not_owner();
    printf("ok  non-owner refused\n");
    test_store_current();
    printf("ok  current offset is a no-op\n");
    test_store_ok();
    printf("ok  guarded store and read-back\n");
    test_store_mismatch();
    printf("ok  read-back mismatch detected\n");
    test_store_unstable();
    printf("ok  unstable cell refused before writing\n");
    test_store_denied();
    printf("ok  denied transfer refused\n");
    printf("platform_rtc_test: PASS\n");
    return 0;
}
