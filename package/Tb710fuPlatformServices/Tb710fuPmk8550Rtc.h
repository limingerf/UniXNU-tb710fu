// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuPmk8550Rtc.h - PMK8550 wall clock for TB710FU.
//
// Board facts (Linux mainline v6.12):
//
//   arch/arm64/boot/dts/qcom/pmk8550.dtsi:47-52
//     pmk8550_rtc: rtc@6100, compatible "qcom,pmk8350-rtc",
//     reg = <0x6100>, <0x6200> (rtc, alarm), interrupt <0x0 0x62 0x1>.
//     The high byte of the register is the peripheral id, so the RTC is
//     SID 0 / PPID 0x061 and the alarm is SID 0 / PPID 0x062.
//
//   drivers/rtc/rtc-pm8xxx.c:440-446 (pmk8350_regs)
//     control 0x6146, write base 0x6140, read 0x6148 (four bytes),
//     alarm control 0x6246, alarm control2 0x6248, alarm data 0x6240.
//   rtc-pm8xxx.c:23  the control's bit 7 is PM8xxx_RTC_ENABLE, set while the
//                    counter runs.
//   rtc-pm8xxx.c:121-149  the counter is not latched: the four bytes are read
//                    and the LSB is read again; when the second LSB read is
//                    smaller than the first, a carry straddled the transfer and
//                    the whole read is repeated.
//   rtc-pm8xxx.c:260-275  wall time is counter + stored offset; the offset is a
//                    little-endian u32 from an nvmem cell (rtc-pm8xxx.c:63-105).
//
// The TB710FU devicetree declares no such nvmem cell. This core therefore
// treats the offset as zero when SDAM2's 0xbc cell is empty (exactly what
// rtc-pm8xxx.c does when the cell is absent) and uses the counter alone. It
// refuses, rather than repairs, a nonzero cell that does not describe a
// plausible time, because on this board the cell's owner is unverified.
// rtc-pm8xxx.c:152-176 is the offset path used for set-time when the RTC may
// not be written; the counter is never rewritten here (this ticket only asks
// for a read clock, and rewriting the counter also changes what Android reads).
//
// Sub-second: the PMK8350 block has four seconds bytes and no sub-second
// register, so a microsecond field cannot come from it. An optional fine
// source (XNU's mach_absolute_time / CNTPCT, converted to nanoseconds by the
// caller) supplies the fraction; it is anchored when a read starts and is
// reported as invalid when no fine source exists. The seconds field is the
// only field this driver claims from hardware.
#ifndef TB710FU_PMK8550_RTC_H
#define TB710FU_PMK8550_RTC_H

#include "Tb710fuSpmiArbiter.h"

/* RTC block, byte addresses inside PPID 0x061 (absolute + 0x6100). */
#define TB710FU_PMK8550_RTC_CONTROL 0x46u
#define TB710FU_PMK8550_RTC_RUNNING 0x80u
#define TB710FU_PMK8550_RTC_COUNTER 0x48u
#define TB710FU_PMK8550_RTC_COUNTER_BYTES 4u
#define TB710FU_PMK8550_RTC_WRITE_BASE 0x40u

/* Clock policy. EARLIEST is a plausibility threshold, not a hardware fact:
 * macOS 27 and this board's first boot are years later, so a counter that
 * reads before 2023 has never been set and must not become 1970. SET_BACK
 * bounds how far a stored offset may move the clock backwards. */
#define TB710FU_RTC_READ_TRIES 4u
#define TB710FU_RTC_EARLIEST 1672531200u /* 2023-01-01T00:00:00Z */
#define TB710FU_RTC_SET_BACK 86400u

enum tb710fu_rtc_result {
    TB710FU_RTC_OK = 0,
    TB710FU_RTC_EMPTY,      /* the counter is not a wall clock and no offset is stored */
    TB710FU_RTC_UNSTABLE,   /* the counter or the cell changed during the read */
    TB710FU_RTC_STOPPED,    /* control bit 7 is clear */
    TB710FU_RTC_FOREIGN,    /* a nonzero cell does not describe a plausible offset */
    TB710FU_RTC_RANGE,      /* the time does not fit 32-bit seconds */
    TB710FU_RTC_READ_ONLY,  /* store not enabled by policy */
    TB710FU_RTC_CURRENT,    /* the cell already holds this offset */
    TB710FU_RTC_MISMATCH,   /* the cell read back differs from what was written */
    TB710FU_RTC_NOT_OWNER,  /* the SDAM belongs to another execution environment */
    TB710FU_RTC_DENIED,     /* the arbiter reported a denied transfer */
    TB710FU_RTC_FAILED,     /* the arbiter reported a failed transfer */
    TB710FU_RTC_DROPPED,    /* the arbiter reported a dropped transfer */
    TB710FU_RTC_TIMEOUT,    /* the arbiter never reported DONE */
    TB710FU_RTC_NO_DEVICE,  /* the arbiter maps no such peripheral */
    TB710FU_RTC_ARGUMENT    /* a null or out-of-range argument */
};

/* Read-only unless the caller explicitly sets allow_store. */
struct tb710fu_pmk8550_rtc_config {
    unsigned ee;
    int allow_store;
};

struct tb710fu_rtc_fine_source {
    void *context;
    /* Monotonic nanoseconds. Must never run backwards; a source that does is
     * treated as invalid instead of producing a negative fraction. */
    uint64_t (*nanoseconds)(void *context);
};

struct tb710fu_rtc_reading {
    uint32_t counter;   /* last accepted raw counter word */
    uint32_t offset;    /* cell contents, 0 when empty */
    uint64_t seconds;   /* epoch seconds, valid only when seconds_valid is 1 */
    uint32_t useconds;  /* 0..999999, valid only when useconds_valid is 1 */
    int seconds_valid;
    int useconds_valid;
};

struct tb710fu_rtc_set_result {
    uint32_t counter;
    uint32_t before;
    uint32_t offset;
    uint32_t after;
    int written;
};

struct tb710fu_pmk8550_rtc {
    const struct tb710fu_spmi_io *io;
    struct tb710fu_pmk8550_rtc_config config;
    struct tb710fu_rtc_fine_source fine;
    unsigned rtc_apid;
    unsigned sdam_apid;
    int sdam_valid;
    int ready;
    int anchor_valid;
    uint64_t anchor_ns;
    uint32_t anchor_second;
};

void tb710fu_rtc_config_default(struct tb710fu_pmk8550_rtc_config *config,
                                unsigned ee);

/* Resolve the APIDs once. The RTC is required; SDAM2 is optional, exactly like
 * an absent nvmem cell in rtc-pm8xxx.c. fine may be NULL. */
int tb710fu_rtc_open(struct tb710fu_pmk8550_rtc *clock,
                     const struct tb710fu_spmi_io *io,
                     const struct tb710fu_pmk8550_rtc_config *config,
                     const struct tb710fu_rtc_fine_source *fine);

/* Read the running counter, the stored offset and the wall clock. Every
 * non-OK result leaves seconds_valid zero; nothing invents a time. */
int tb710fu_rtc_read(struct tb710fu_pmk8550_rtc *clock,
                     struct tb710fu_rtc_reading *reading);

/* Store a new offset (rtc-pm8xxx.c's set-time path when the counter must not
 * be written). Guarded: policy, SDAM ownership, the cell's current contents,
 * the write whitelist, and a read-back. */
int tb710fu_rtc_set_epoch(struct tb710fu_pmk8550_rtc *clock, uint64_t seconds,
                          struct tb710fu_rtc_set_result *result);

/* Convenience for the platform expert: open with the default read-only policy
 * and return the epoch. */
int tb710fu_rtc_read_epoch(const struct tb710fu_spmi_io *io, unsigned ee,
                           uint64_t *seconds);

const char *tb710fu_rtc_result_name(int result);

#endif
