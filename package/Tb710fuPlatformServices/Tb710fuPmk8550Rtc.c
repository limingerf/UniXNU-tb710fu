// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuPmk8550Rtc.c - the PMK8550 wall clock.
//
// The register facts, the carry algorithm and the policy are documented in
// Tb710fuPmk8550Rtc.h. Nothing here touches a real address: every access goes
// through struct tb710fu_spmi_io, so this file runs on the host model and
// behind ml_io_map unchanged.
#include "Tb710fuPmk8550Rtc.h"

#include <stddef.h>
#include <string.h>

#include "Tb710fuRtcTime.h" /* TB710FU_RTC_USEC_LIMIT, shared with the blob */

#define TB710FU_NS_PER_SECOND 1000000000ull
#define TB710FU_NS_PER_USEC 1000ull
#define TB710FU_U32_MAX 0xffffffffu

static int tb710fu_rtc_from_spmi(int result)
{
    switch (result) {
    case TB710FU_SPMI_OK: return TB710FU_RTC_OK;
    case TB710FU_SPMI_FAILED: return TB710FU_RTC_FAILED;
    case TB710FU_SPMI_DENIED: return TB710FU_RTC_DENIED;
    case TB710FU_SPMI_DROPPED: return TB710FU_RTC_DROPPED;
    case TB710FU_SPMI_TIMEOUT: return TB710FU_RTC_TIMEOUT;
    case TB710FU_SPMI_NO_DEVICE: return TB710FU_RTC_NO_DEVICE;
    case TB710FU_SPMI_BAD_APID: return TB710FU_RTC_NO_DEVICE;
    case TB710FU_SPMI_NOT_OWNER: return TB710FU_RTC_NOT_OWNER;
    case TB710FU_SPMI_READ_ONLY: return TB710FU_RTC_READ_ONLY;
    case TB710FU_SPMI_ARGUMENT: return TB710FU_RTC_ARGUMENT;
    }
    return TB710FU_RTC_FAILED;
}

void tb710fu_rtc_config_default(struct tb710fu_pmk8550_rtc_config *config,
                                unsigned ee)
{
    if (config == NULL)
        return;
    config->ee = ee;
    config->allow_store = 0;
}

/* Two reads that must agree. Used for the offset cell, which has no latch:
 * another execution environment can rewrite it between accesses. */
static int tb710fu_rtc_read_cell(const struct tb710fu_pmk8550_rtc *clock,
                                 uint32_t *cell)
{
    uint32_t first = 0;
    uint32_t second = 0;

    for (unsigned attempt = 0; attempt < TB710FU_RTC_READ_TRIES; ++attempt) {
        int rc = tb710fu_rtc_from_spmi(tb710fu_spmi_read(
            clock->io, clock->config.ee, clock->sdam_apid,
            (uint8_t)TB710FU_PMK8550_SDAM2_RTC_OFFSET, 4u, &first, NULL));
        if (rc != TB710FU_RTC_OK)
            return rc;
        rc = tb710fu_rtc_from_spmi(tb710fu_spmi_read(
            clock->io, clock->config.ee, clock->sdam_apid,
            (uint8_t)TB710FU_PMK8550_SDAM2_RTC_OFFSET, 4u, &second, NULL));
        if (rc != TB710FU_RTC_OK)
            return rc;
        if (first == second) {
            *cell = first;
            return TB710FU_RTC_OK;
        }
    }
    return TB710FU_RTC_UNSTABLE;
}

/* rtc-pm8xxx.c:121-149. The counter is free running and not latched, so the
 * four bytes are read and the LSB read again; a smaller LSB means a carry
 * crossed the transfer and the sequence repeats. */
static int tb710fu_rtc_read_counter(const struct tb710fu_pmk8550_rtc *clock,
                                    uint32_t *counter)
{
    for (unsigned attempt = 0; attempt < TB710FU_RTC_READ_TRIES; ++attempt) {
        uint32_t value = 0;
        uint32_t lsb = 0;
        int rc = tb710fu_rtc_from_spmi(tb710fu_spmi_read(
            clock->io, clock->config.ee, clock->rtc_apid,
            (uint8_t)TB710FU_PMK8550_RTC_COUNTER,
            TB710FU_PMK8550_RTC_COUNTER_BYTES, &value, NULL));
        if (rc != TB710FU_RTC_OK)
            return rc;
        rc = tb710fu_rtc_from_spmi(tb710fu_spmi_read(
            clock->io, clock->config.ee, clock->rtc_apid,
            (uint8_t)TB710FU_PMK8550_RTC_COUNTER, 1u, &lsb, NULL));
        if (rc != TB710FU_RTC_OK)
            return rc;
        if ((lsb & 0xffu) >= (value & 0xffu)) {
            *counter = value;
            return TB710FU_RTC_OK;
        }
    }
    return TB710FU_RTC_UNSTABLE;
}

static int tb710fu_rtc_read_control(
    const struct tb710fu_pmk8550_rtc *clock, uint32_t *control)
{
    uint32_t first = 0;
    uint32_t second = 0;

    for (unsigned attempt = 0; attempt < TB710FU_RTC_READ_TRIES; ++attempt) {
        int rc = tb710fu_rtc_from_spmi(tb710fu_spmi_read(
            clock->io, clock->config.ee, clock->rtc_apid,
            (uint8_t)TB710FU_PMK8550_RTC_CONTROL, 1u, &first, NULL));
        if (rc != TB710FU_RTC_OK)
            return rc;
        rc = tb710fu_rtc_from_spmi(tb710fu_spmi_read(
            clock->io, clock->config.ee, clock->rtc_apid,
            (uint8_t)TB710FU_PMK8550_RTC_CONTROL, 1u, &second, NULL));
        if (rc != TB710FU_RTC_OK)
            return rc;
        if (first == second) {
            *control = first;
            return TB710FU_RTC_OK;
        }
    }
    return TB710FU_RTC_UNSTABLE;
}

int tb710fu_rtc_open(struct tb710fu_pmk8550_rtc *clock,
                     const struct tb710fu_spmi_io *io,
                     const struct tb710fu_pmk8550_rtc_config *config,
                     const struct tb710fu_rtc_fine_source *fine)
{
    struct tb710fu_spmi_peripheral peripheral;
    struct tb710fu_pmk8550_rtc_config defaults;
    int rc;

    if (clock == NULL || io == NULL)
        return TB710FU_RTC_ARGUMENT;
    memset(clock, 0, sizeof(*clock));
    if (config == NULL) {
        tb710fu_rtc_config_default(&defaults, TB710FU_SPMI_EE);
        config = &defaults;
    }
    if (config->ee > TB710FU_SPMI_MAX_EE)
        return TB710FU_RTC_ARGUMENT;
    clock->io = io;
    clock->config = *config;
    if (fine != NULL)
        clock->fine = *fine;

    rc = tb710fu_spmi_find(io, config->ee, TB710FU_PMK8550_RTC_USID,
                           &peripheral);
    if (rc != TB710FU_SPMI_OK)
        return tb710fu_rtc_from_spmi(rc);
    clock->rtc_apid = peripheral.apid;

    /* SDAM2 is optional, like the nvmem cell rtc-pm8xxx.c reads if present. */
    rc = tb710fu_spmi_find(io, config->ee, TB710FU_PMK8550_SDAM2_USID,
                           &peripheral);
    if (rc == TB710FU_SPMI_OK) {
        clock->sdam_valid = 1;
        clock->sdam_apid = peripheral.apid;
    } else if (rc != TB710FU_SPMI_NO_DEVICE) {
        return tb710fu_rtc_from_spmi(rc);
    }
    clock->ready = 1;
    return TB710FU_RTC_OK;
}

int tb710fu_rtc_read(struct tb710fu_pmk8550_rtc *clock,
                     struct tb710fu_rtc_reading *reading)
{
    uint64_t fine_start = 0;
    uint64_t fine_end = 0;
    uint64_t seconds;
    uint32_t control = 0;
    uint32_t counter = 0;
    uint32_t cell = 0;
    int rc;

    if (clock == NULL || reading == NULL || !clock->ready)
        return TB710FU_RTC_ARGUMENT;
    memset(reading, 0, sizeof(*reading));
    if (clock->fine.nanoseconds != NULL)
        fine_start = clock->fine.nanoseconds(clock->fine.context);

    rc = tb710fu_rtc_read_control(clock, &control);
    if (rc != TB710FU_RTC_OK)
        return rc;
    /* Bit 7 clear means the counter is held; its value is not a clock. */
    if (!(control & TB710FU_PMK8550_RTC_RUNNING))
        return TB710FU_RTC_STOPPED;
    rc = tb710fu_rtc_read_counter(clock, &counter);
    if (rc != TB710FU_RTC_OK)
        return rc;
    if (clock->sdam_valid) {
        rc = tb710fu_rtc_read_cell(clock, &cell);
        if (rc != TB710FU_RTC_OK)
            return rc;
    }
    if (clock->fine.nanoseconds != NULL)
        fine_end = clock->fine.nanoseconds(clock->fine.context);

    reading->counter = counter;
    if (cell == 0u) {
        /* No stored offset: rtc-pm8xxx.c then uses the counter alone. */
        if (counter < TB710FU_RTC_EARLIEST)
            return TB710FU_RTC_EMPTY;
        seconds = counter;
        reading->offset = 0u;
    } else {
        uint64_t sum = (uint64_t)counter + (uint64_t)cell;
        if (sum > TB710FU_U32_MAX)
            return TB710FU_RTC_RANGE;
        /* A stored offset that yields a pre-2023 time was not written by this
         * port. Refuse it instead of silently fixing it up. */
        if (sum < TB710FU_RTC_EARLIEST)
            return TB710FU_RTC_FOREIGN;
        seconds = sum;
        reading->offset = cell;
        reading->counter = counter;
    }
    reading->seconds = seconds;
    reading->seconds_valid = 1;

    if (clock->fine.nanoseconds == NULL)
        return TB710FU_RTC_OK;
    /* A read that crossed a second boundary, or one that took a second, cannot
     * place the fraction: keep whole seconds and say so. */
    if (fine_end < fine_start ||
        fine_end - fine_start >= TB710FU_NS_PER_SECOND)
        return TB710FU_RTC_OK;
    {
        const uint64_t middle = fine_start + (fine_end - fine_start) / 2u;

        if (!clock->anchor_valid || clock->anchor_second != seconds ||
            middle < clock->anchor_ns) {
            clock->anchor_valid = 1;
            clock->anchor_ns = middle;
            clock->anchor_second = (uint32_t)seconds;
            reading->useconds = 0u;
        } else {
            const uint64_t elapsed =
                (middle - clock->anchor_ns) / TB710FU_NS_PER_USEC;
            reading->useconds = elapsed >= TB710FU_RTC_USEC_LIMIT
                ? TB710FU_RTC_USEC_LIMIT - 1u
                : (uint32_t)elapsed;
        }
        reading->useconds_valid = 1;
    }
    return TB710FU_RTC_OK;
}

int tb710fu_rtc_set_epoch(struct tb710fu_pmk8550_rtc *clock, uint64_t seconds,
                          struct tb710fu_rtc_set_result *result)
{
    struct tb710fu_rtc_set_result local;
    uint32_t counter = 0;
    uint32_t before = 0;
    uint32_t after = 0;
    uint32_t offset;
    unsigned owner = 0;
    int rc;

    if (clock == NULL || !clock->ready)
        return TB710FU_RTC_ARGUMENT;
    if (result == NULL)
        result = &local;
    memset(result, 0, sizeof(*result));
    if (!clock->config.allow_store)
        return TB710FU_RTC_READ_ONLY;
    if (!clock->sdam_valid)
        return TB710FU_RTC_NO_DEVICE;
    if (seconds < TB710FU_RTC_EARLIEST || seconds > TB710FU_U32_MAX)
        return TB710FU_RTC_RANGE;
    rc = tb710fu_rtc_read_counter(clock, &counter);
    if (rc != TB710FU_RTC_OK)
        return rc;
    /* The cell stores counter-to-wall-clock, a u32: it cannot be negative. */
    if (seconds <= counter)
        return TB710FU_RTC_RANGE;
    offset = (uint32_t)(seconds - (uint64_t)counter);
    rc = tb710fu_rtc_from_spmi(
        tb710fu_spmi_ownership(clock->io, clock->sdam_apid, &owner));
    if (rc != TB710FU_RTC_OK)
        return rc;
    if (owner != clock->config.ee)
        return TB710FU_RTC_NOT_OWNER;
    rc = tb710fu_rtc_read_cell(clock, &before);
    if (rc != TB710FU_RTC_OK)
        return rc;
    result->counter = counter;
    result->before = before;
    result->offset = offset;
    if (before != 0u) {
        const uint64_t held = (uint64_t)counter + (uint64_t)before;
        if (held > TB710FU_U32_MAX)
            return TB710FU_RTC_RANGE;
        if (held < TB710FU_RTC_EARLIEST)
            return TB710FU_RTC_FOREIGN;
        /* A stored time far ahead of the requested one would pull the clock
         * back by more than a day: that is not a correction, it is damage. */
        if (held > seconds + TB710FU_RTC_SET_BACK)
            return TB710FU_RTC_FOREIGN;
    }
    if (before == offset) {
        result->after = before;
        return TB710FU_RTC_CURRENT;
    }
    rc = tb710fu_rtc_from_spmi(tb710fu_spmi_write_apid(
        clock->io, clock->config.ee, clock->sdam_apid,
        (uint8_t)TB710FU_PMK8550_SDAM2_RTC_OFFSET, 4u, offset, NULL));
    if (rc != TB710FU_RTC_OK)
        return rc;
    result->written = 1;
    rc = tb710fu_rtc_read_cell(clock, &after);
    if (rc != TB710FU_RTC_OK)
        return rc;
    result->after = after;
    if (after != offset)
        return TB710FU_RTC_MISMATCH;
    return TB710FU_RTC_OK;
}

int tb710fu_rtc_read_epoch(const struct tb710fu_spmi_io *io, unsigned ee,
                           uint64_t *seconds)
{
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_pmk8550_rtc_config config;
    struct tb710fu_rtc_reading reading;
    int rc;

    if (seconds == NULL)
        return TB710FU_RTC_ARGUMENT;
    tb710fu_rtc_config_default(&config, ee);
    rc = tb710fu_rtc_open(&clock, io, &config, NULL);
    if (rc != TB710FU_RTC_OK)
        return rc;
    rc = tb710fu_rtc_read(&clock, &reading);
    if (rc != TB710FU_RTC_OK)
        return rc;
    *seconds = reading.seconds;
    return rc;
}

const char *tb710fu_rtc_result_name(int result)
{
    switch (result) {
    case TB710FU_RTC_OK: return "ok";
    case TB710FU_RTC_EMPTY: return "empty";
    case TB710FU_RTC_UNSTABLE: return "unstable";
    case TB710FU_RTC_STOPPED: return "stopped";
    case TB710FU_RTC_FOREIGN: return "foreign";
    case TB710FU_RTC_RANGE: return "range";
    case TB710FU_RTC_READ_ONLY: return "read-only";
    case TB710FU_RTC_CURRENT: return "current";
    case TB710FU_RTC_MISMATCH: return "mismatch";
    case TB710FU_RTC_NOT_OWNER: return "not-owner";
    case TB710FU_RTC_DENIED: return "denied";
    case TB710FU_RTC_FAILED: return "failed";
    case TB710FU_RTC_DROPPED: return "dropped";
    case TB710FU_RTC_TIMEOUT: return "timeout";
    case TB710FU_RTC_NO_DEVICE: return "no-device";
    case TB710FU_RTC_ARGUMENT: return "argument";
    }
    return "unknown";
}
