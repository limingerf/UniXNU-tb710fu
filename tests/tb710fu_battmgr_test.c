/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host test for the TB710FU battery manager codec: a fake DSP answers the requests the codec makes, the codec's
 * snapshot is compared with the values that were answered. No hardware, no IOKit. Build and run:
 *   python3 scripts/check-tb710fu-power-host.py
 */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../package/Tb710fuPower/Tb710fuBattmgr.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

typedef struct {
    uint32_t opcode, property, value, result;
} Answer;

static const Answer kAnswers[] = {
    {TB710FU_BATTMGR_OP_BATTERY_GET, 0, 1, 0},          /* status: charging */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 2, 1, 0},          /* present */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 4, 7240, 0},       /* capacity 72.40 % */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 7, 4265000, 0},    /* voltage 4265 mV */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 9, (uint32_t)-1500000, 0},  /* current: discharge positive, -1500 mA means charging */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 12, 2050, 0},      /* temperature 20.50 C */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 14, 7100000, 0},   /* charge counter 7100 mAh */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 20, 0, 0},         /* time to empty: none */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 19, 3600, 0},      /* time to full 60 min */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 17, 9800000, 0},   /* full 9800 mAh */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 16, 9900000, 0},   /* design 9900 mAh */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 15, 172, 0},       /* cycles */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 3, 1, 0},          /* charge type */
    {TB710FU_BATTMGR_OP_BATTERY_GET, 10, 3000000, 0},   /* charge control limit 3000 mA */
    {TB710FU_BATTMGR_OP_USB_GET, 0, 1, 0},              /* USB online */
    {TB710FU_BATTMGR_OP_USB_GET, 6, 6, 0},              /* USB type: PD */
    {TB710FU_BATTMGR_OP_USB_GET, 1, 9000000, 0},        /* 9 V */
    {TB710FU_BATTMGR_OP_USB_GET, 3, 2000000, 0},        /* 2 A */
    {TB710FU_BATTMGR_OP_USB_GET, 7, 6, 0},              /* adapter type */
    {TB710FU_BATTMGR_OP_USB_GET, 5, 2000000, 0},        /* input current limit */
    {TB710FU_BATTMGR_OP_USB_GET, 4, 3000000, 0},        /* port's most */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0x8a, 3600, 0},      /* gauge 1 remaining */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0x8b, 4900, 0},      /* gauge 1 full */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0xa6, 3500, 0},      /* gauge 2 remaining */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0xa7, 4900, 0},      /* gauge 2 full */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0x07, 6, 0},         /* adapter type as the board sees it */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0x33, 18, 0},        /* adapter watts */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0x18, 1, 0},         /* PD authenticated */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0x03, 1, 0},         /* cell 1 authenticated */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0xa5, 1, 0},         /* cell 2 authenticated */
    {TB710FU_BATTMGR_OP_BOARD_GET, 0x2a, 0, 0},         /* input not suspended */
};

static void frame(uint8_t out[24], uint32_t opcode, uint32_t property, uint32_t value, uint32_t result)
{
    memset(out, 0, 24);
    out[0] = (uint8_t)TB710FU_BATTMGR_OWNER; out[1] = (uint8_t)(TB710FU_BATTMGR_OWNER >> 8);
    out[4] = (uint8_t)TB710FU_BATTMGR_TYPE_REQUEST;
    out[8] = (uint8_t)opcode;
    out[12] = (uint8_t)property;
    out[16] = (uint8_t)value; out[17] = (uint8_t)(value >> 8); out[18] = (uint8_t)(value >> 16); out[19] = (uint8_t)(value >> 24);
    out[20] = (uint8_t)result; out[21] = (uint8_t)(result >> 8); out[22] = (uint8_t)(result >> 16); out[23] = (uint8_t)(result >> 24);
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Answers one request; returns the codec's result, or -1 when no request was due. */
static int answer(Tb710fuBattmgr *bm, int overrideGauges)
{
    uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES];
    const uint32_t n = tb710fu_battmgr_next(bm, out);
    if (!n) return -1;
    CHECK(n == TB710FU_BATTMGR_REQUEST_BYTES);
    CHECK(get32(out) == TB710FU_BATTMGR_OWNER);
    CHECK(get32(out + 4) == TB710FU_BATTMGR_TYPE_REQUEST);
    const uint32_t opcode = get32(out + 8), property = get32(out + 16);
    if (opcode == TB710FU_BATTMGR_OP_USB_SET) {
        CHECK(property == 5);
        uint8_t reply[24];
        frame(reply, opcode, 5, 0, 0);
        return tb710fu_battmgr_receive(bm, reply, 24);
    }
    const Answer *found = NULL;
    for (unsigned i = 0; i < sizeof kAnswers / sizeof kAnswers[0]; ++i) {
        if (kAnswers[i].opcode == opcode && kAnswers[i].property == property) { found = &kAnswers[i]; break; }
    }
    CHECK(found != NULL);
    if (!found) return -1;
    uint8_t reply[24];
    const int gauge = property == 0x8a || property == 0x8b || property == 0xa6 || property == 0xa7;
    frame(reply, opcode, property, found->value, gauge && overrideGauges ? 1 : found->result);
    return tb710fu_battmgr_receive(bm, reply, 24);
}

static void runCycle(Tb710fuBattmgr *bm, int overrideGauges)
{
    for (unsigned guard = 0; guard < 64; ++guard) {
        const int r = answer(bm, overrideGauges);
        if (r < 0) break;
        if (r == BmCycleDone) return;
    }
    CHECK(0 && "cycle did not finish");
}

static void test_full_cycle(void)
{
    Tb710fuBattmgr bm;
    tb710fu_battmgr_init(&bm);
    tb710fu_battmgr_refresh(&bm, 1);
    runCycle(&bm, 0);
    CHECK(bm.cycles_done == 1);
    CHECK(bm.responses >= 29);

    Tb710fuBatterySnapshot s;
    memset(&s, 0, sizeof s);
    tb710fu_battmgr_snapshot(&bm, &s);
    CHECK(s.present == 1);
    CHECK(s.external == 1);
    CHECK(s.charging == 1);
    CHECK(s.fully_charged == 0);
    CHECK(s.critical == 0);
    CHECK(s.percent == 72);
    CHECK(s.charge_known == 1);
    CHECK(s.voltage_mv == 4265);
    CHECK(s.current_ma == 1500);           /* the firmware's discharge-positive value negated */
    CHECK(s.temperature_valid == 1);
    CHECK(s.temperature == 2050);
    CHECK(s.raw_mah == 7100);
    CHECK(s.full_mah == 9800);
    CHECK(s.design_mah == 9900);
    CHECK(s.cycles == 172);
    CHECK(s.time_to_full_min == 60);
    CHECK(s.time_to_empty_min == -1);
    CHECK(s.adapter_type == 6);
    CHECK(s.adapter_mv == 9000);
    CHECK(s.adapter_ma == 2000);
    CHECK(s.adapter_watts == 18);
    CHECK(s.ctrl_limit_ua == 3000000);
}

static void test_capacity_fallback(void)
{
    Tb710fuBattmgr bm;
    tb710fu_battmgr_init(&bm);
    tb710fu_battmgr_refresh(&bm, 1);
    runCycle(&bm, 1);  /* every gauge answers an error */
    Tb710fuBatterySnapshot s;
    memset(&s, 0, sizeof s);
    tb710fu_battmgr_snapshot(&bm, &s);
    CHECK(s.charge_known == 1);
    CHECK(s.percent == 72);  /* 7240 hundredths of a percent, rounded */
}

static void test_error_retention_and_timeout(void)
{
    /* Cycle A: the charger is unplugged (USB online 0, status 2). */
    Tb710fuBattmgr bm;
    tb710fu_battmgr_init(&bm);
    tb710fu_battmgr_refresh(&bm, 1);
    for (unsigned guard = 0; guard < 64; ++guard) {
        uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES];
        const uint32_t n = tb710fu_battmgr_next(&bm, out);
        if (!n) break;
        const uint32_t opcode = get32(out + 8), property = get32(out + 16);
        const Answer *found = NULL;
        for (unsigned i = 0; i < sizeof kAnswers / sizeof kAnswers[0]; ++i)
            if (kAnswers[i].opcode == opcode && kAnswers[i].property == property) { found = &kAnswers[i]; break; }
        CHECK(found != NULL);
        if (!found) break;
        uint32_t value = found->value;
        if (opcode == TB710FU_BATTMGR_OP_USB_GET && property == 0) value = 0;  /* unplugged */
        if (opcode == TB710FU_BATTMGR_OP_BATTERY_GET && property == 0) value = 2;  /* discharging */
        uint8_t reply[24];
        frame(reply, opcode, property, value, 0);
        if (tb710fu_battmgr_receive(&bm, reply, 24) == BmCycleDone) break;
    }
    CHECK(bm.cycles_done == 1);
    Tb710fuBatterySnapshot a;
    memset(&a, 0, sizeof a);
    tb710fu_battmgr_snapshot(&bm, &a);
    CHECK(a.external_known == 1);
    CHECK(a.external == 0);
    CHECK(a.charging == 0);

    /* Cycle B: the USB-online request is never answered and the owner's timer gives up on it. The known
     * disconnect must survive: a timed-out field is unknown, not a disconnect. */
    tb710fu_battmgr_refresh(&bm, 0);
    unsigned skipped = 0;
    for (unsigned guard = 0; guard < 96; ++guard) {
        uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES];
        const uint32_t n = tb710fu_battmgr_next(&bm, out);
        if (!n) break;
        const uint32_t opcode = get32(out + 8), property = get32(out + 16);
        if (opcode == TB710FU_BATTMGR_OP_USB_GET && property == 0 && !skipped) {
            skipped = 1;
            tb710fu_battmgr_timeout(&bm);   /* the field is skipped, the cycle continues */
            CHECK(bm.last_timeout == BmUsbOnline);
            continue;
        }
        const Answer *found = NULL;
        for (unsigned i = 0; i < sizeof kAnswers / sizeof kAnswers[0]; ++i)
            if (kAnswers[i].opcode == opcode && kAnswers[i].property == property) { found = &kAnswers[i]; break; }
        CHECK(found != NULL);
        if (!found) break;
        uint8_t reply[24];
        frame(reply, opcode, property, found->value, 0);
        if (tb710fu_battmgr_receive(&bm, reply, 24) == BmCycleDone) break;
    }
    CHECK(skipped == 1);
    CHECK(bm.cycles_done == 2);
    CHECK(bm.timeouts >= 1);
    Tb710fuBatterySnapshot s;
    memset(&s, 0, sizeof s);
    tb710fu_battmgr_snapshot(&bm, &s);
    CHECK(s.external_known == 1);
    CHECK(s.external == 0);   /* the earlier disconnect survived the timeout */
    CHECK(s.status_known == 1);
}

static void test_notification_and_enable(void)
{
    Tb710fuBattmgr bm;
    tb710fu_battmgr_init(&bm);
    uint8_t enable[TB710FU_BATTMGR_ENABLE_BYTES];
    CHECK(tb710fu_battmgr_enable_request(enable) == TB710FU_BATTMGR_ENABLE_BYTES);
    CHECK(get32(enable + 8) == TB710FU_BATTMGR_OP_REQUEST_NOTIFICATION);
    CHECK(get32(enable + 20) == 1);
    CHECK(get32(enable + 24) == 100);
    uint8_t reply[24];
    frame(reply, TB710FU_BATTMGR_OP_REQUEST_NOTIFICATION, 0, 0, 0);
    CHECK(tb710fu_battmgr_receive(&bm, reply, 24) == BmEnabled);
    CHECK(bm.enabled == 1);

    frame(reply, TB710FU_BATTMGR_OP_NOTIFICATION, 0, 0, 0);
    reply[4] = (uint8_t)TB710FU_BATTMGR_TYPE_NOTIFY;
    CHECK(tb710fu_battmgr_receive(&bm, reply, 24) == BmNotified);
    CHECK(bm.notifications == 1);
    CHECK(bm.cursor == 0);  /* a notification starts the cycle at once */
}

static void test_ignores_foreign_and_malformed(void)
{
    Tb710fuBattmgr bm;
    tb710fu_battmgr_init(&bm);
    uint8_t foreign[24] = {0};
    foreign[0] = 9;  /* not owner 32778 */
    CHECK(tb710fu_battmgr_receive(&bm, foreign, 24) == BmIgnored);
    uint8_t tiny[4] = {0};
    CHECK(tb710fu_battmgr_receive(&bm, tiny, 4) == BmIgnored);
    uint8_t usbAnswer[24];
    frame(usbAnswer, TB710FU_BATTMGR_OP_USB_GET, 5, 0, 0);
    CHECK(tb710fu_battmgr_receive(&bm, usbAnswer, 24) == BmIgnored);  /* nothing in flight */
    CHECK(bm.ignored == 3);
}

static void test_input_limit_policy(void)
{
    Tb710fuBattmgr bm;
    tb710fu_battmgr_init(&bm);
    bm.sdp_limit_ua = 900000;  /* usb-sdp-ma=900 */
    tb710fu_battmgr_refresh(&bm, 0);
    runCycle(&bm, 0);
    /* the answered limit is 2 A, above 900 mA, so nothing is due */
    CHECK(bm.limit_due == 0);
    CHECK(bm.limit_sets == 0);

    Tb710fuBattmgr low;
    tb710fu_battmgr_init(&low);
    low.sdp_limit_ua = 900000;
    tb710fu_battmgr_refresh(&low, 0);
    for (unsigned guard = 0; guard < 64; ++guard) {
        uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES];
        const uint32_t n = tb710fu_battmgr_next(&low, out);
        if (!n) break;
        if (get32(out + 8) == TB710FU_BATTMGR_OP_USB_SET) {
            uint8_t reply[24];
            frame(reply, TB710FU_BATTMGR_OP_USB_SET, 5, 0, 0);
            CHECK(tb710fu_battmgr_receive(&low, reply, 24) == BmAnswer);
            continue;
        }
        const uint32_t opcode = get32(out + 8), property = get32(out + 16);
        const Answer *found = NULL;
        for (unsigned i = 0; i < sizeof kAnswers / sizeof kAnswers[0]; ++i)
            if (kAnswers[i].opcode == opcode && kAnswers[i].property == property) { found = &kAnswers[i]; break; }
        CHECK(found != NULL);
        if (!found) break;
        uint8_t reply[24];
        /* SDP with a 500 mA limit: the policy must ask for 900 mA */
        uint32_t value = found->value;
        if (property == 6 || property == 7) value = 1;  /* SDP adapter */
        if (property == 5) value = 500000;
        frame(reply, opcode, property, value, 0);
        tb710fu_battmgr_receive(&low, reply, 24);
        if (low.limit_due) break;
    }
    CHECK(low.limit_due == 900000 || low.limit_sets > 0);
}

static void test_zero_limit_never_writes(void)
{
    Tb710fuBattmgr bm;
    tb710fu_battmgr_init(&bm);
    tb710fu_battmgr_refresh(&bm, 0);
    runCycle(&bm, 0);
    uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES];
    CHECK(tb710fu_battmgr_next(&bm, out) == 0);
    CHECK(bm.limit_due == 0);
    CHECK(bm.limit_sets == 0);
}

int main(void)
{
    printf("TB710FU battery codec host test\n");
    test_full_cycle();
    test_capacity_fallback();
    test_error_retention_and_timeout();
    test_notification_and_enable();
    test_ignores_foreign_and_malformed();
    test_input_limit_policy();
    test_zero_limit_never_writes();
    if (failures) {
        printf("result: %d check(s) failed\n", failures);
        return 1;
    }
    printf("result: all checks passed\n");
    return 0;
}
