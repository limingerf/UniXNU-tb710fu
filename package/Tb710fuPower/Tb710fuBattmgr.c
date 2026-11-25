/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuBattmgr.h"

/* Battery properties as this board's firmware numbers them: the order of qti_battery_charger.ko's "battery_props"
 * strings for this device's vendor_dlkm, verified against that module's .rodata (offsets in the header comment).
 * status 0, health 1, present 2, charge type 3, capacity 4 (hundredths of a percent), SOH 5, OCV 6, voltage now 7
 * (uV), voltage max 8, current now 9 (uA, discharge positive), charge control limit 10 (uA), its max 11,
 * temperature 12 (hundredths of a degree), technology 13, charge counter 14 (uAh), cycle count 15, full design 16
 * (uAh), full 17 (uAh), model 18 (a string), time to full 19 (s), time to empty 20 (s), then the FAC family.
 * USB: online 0, voltage now 1 (uV), voltage max 2, current now 3 (uA), current max 4, input current limit 5,
 * type 6, adapter type 7, moisture/real type/Type-C at 8..15.
 * Board (FAC ids, read only when they answer): battery authenticated 3, adapter type 7, PD adapter authenticated
 * 0x18, input suspended 0x2a, adapter watts 0x33, fuel gauges 0x8a/0x8b and 0xa6/0xa7, second cell 0xa5. */
static const struct {uint8_t opcode, property, slow;} kFields[BmFields] = {
    [BmStatus] = {TB710FU_BATTMGR_OP_BATTERY_GET, 0, 0},
    [BmPresent] = {TB710FU_BATTMGR_OP_BATTERY_GET, 2, 0},
    [BmCapacity] = {TB710FU_BATTMGR_OP_BATTERY_GET, 4, 0},
    [BmVoltage] = {TB710FU_BATTMGR_OP_BATTERY_GET, 7, 0},
    [BmCurrent] = {TB710FU_BATTMGR_OP_BATTERY_GET, 9, 0},
    [BmTemperature] = {TB710FU_BATTMGR_OP_BATTERY_GET, 12, 0},
    [BmCharge] = {TB710FU_BATTMGR_OP_BATTERY_GET, 14, 0},
    [BmUsbOnline] = {TB710FU_BATTMGR_OP_USB_GET, 0, 0},
    [BmUsbType] = {TB710FU_BATTMGR_OP_USB_GET, 6, 0},
    [BmUsbVoltage] = {TB710FU_BATTMGR_OP_USB_GET, 1, 0},
    [BmUsbCurrent] = {TB710FU_BATTMGR_OP_USB_GET, 3, 0},
    [BmTimeToEmpty] = {TB710FU_BATTMGR_OP_BATTERY_GET, 20, 0},
    [BmTimeToFull] = {TB710FU_BATTMGR_OP_BATTERY_GET, 19, 0},
    /* the fuel gauges: remaining and full charge of each (mAh) */
    [BmGauge1Remaining] = {TB710FU_BATTMGR_OP_BOARD_GET, 0x8a, 0},
    [BmGauge1Full] = {TB710FU_BATTMGR_OP_BOARD_GET, 0x8b, 0},
    [BmGauge2Remaining] = {TB710FU_BATTMGR_OP_BOARD_GET, 0xa6, 0},
    [BmGauge2Full] = {TB710FU_BATTMGR_OP_BOARD_GET, 0xa7, 0},
    [BmFull] = {TB710FU_BATTMGR_OP_BATTERY_GET, 17, 1},
    [BmDesign] = {TB710FU_BATTMGR_OP_BATTERY_GET, 16, 1},
    [BmCycles] = {TB710FU_BATTMGR_OP_BATTERY_GET, 15, 1},
    [BmUsbAdapter] = {TB710FU_BATTMGR_OP_USB_GET, 7, 0},
    [BmUsbLimit] = {TB710FU_BATTMGR_OP_USB_GET, 5, 0},
    [BmUsbMax] = {TB710FU_BATTMGR_OP_USB_GET, 4, 0},
    [BmChargeType] = {TB710FU_BATTMGR_OP_BATTERY_GET, 3, 0},
    [BmCtrlLimit] = {TB710FU_BATTMGR_OP_BATTERY_GET, 10, 0},
    [BmRealType] = {TB710FU_BATTMGR_OP_BOARD_GET, 0x07, 0},
    [BmPowerMax] = {TB710FU_BATTMGR_OP_BOARD_GET, 0x33, 1},
    [BmPdVerified] = {TB710FU_BATTMGR_OP_BOARD_GET, 0x18, 1},
    [BmAuthentic] = {TB710FU_BATTMGR_OP_BOARD_GET, 0x03, 1},
    [BmSlaveAuthentic] = {TB710FU_BATTMGR_OP_BOARD_GET, 0xa5, 1},
    [BmInputSuspend] = {TB710FU_BATTMGR_OP_BOARD_GET, 0x2a, 0},
};
#define NO_CYCLE 0xff

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void header(uint8_t *out, uint32_t opcode)
{
    put32(out, TB710FU_BATTMGR_OWNER);
    put32(out + 4, TB710FU_BATTMGR_TYPE_REQUEST);
    put32(out + 8, opcode);
}

void tb710fu_battmgr_init(Tb710fuBattmgr *b)
{
    for (unsigned i = 0; i < sizeof *b; ++i) ((uint8_t *)b)[i] = 0;
    b->cursor = NO_CYCLE;
    b->last_timeout = -1;
    b->sdp_limit_ua = 0;  /* leave the firmware's input limit alone unless a boot argument asks for more */
    b->limit_port = -1;
}

uint32_t tb710fu_battmgr_enable_request(uint8_t out[TB710FU_BATTMGR_ENABLE_BYTES])
{
    header(out, TB710FU_BATTMGR_OP_REQUEST_NOTIFICATION);
    put32(out + 12, 0);    /* battery 0 */
    put32(out + 16, 0);    /* power state */
    put32(out + 20, 1);    /* low capacity notification threshold, percent */
    put32(out + 24, 100);  /* high */
    return TB710FU_BATTMGR_ENABLE_BYTES;
}

static void skip_to_wanted(Tb710fuBattmgr *b)
{
    while (b->cursor < BmFields && kFields[b->cursor].slow && !b->slow_due) b->cursor++;
}

static void advance(Tb710fuBattmgr *b)
{
    b->in_flight = 0;
    b->cursor++;
    skip_to_wanted(b);
}

void tb710fu_battmgr_refresh(Tb710fuBattmgr *b, int slow)
{
    if (b->cursor != NO_CYCLE) {b->refresh_wanted = 1 + (slow != 0); return;}
    b->slow_due = slow || !(b->valid & (1u << BmFull));
    b->cursor = 0;
    b->in_flight = 0;
    skip_to_wanted(b);
}

uint32_t tb710fu_battmgr_next(Tb710fuBattmgr *b, uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES])
{
    if (b->in_flight || b->limit_in_flight) return 0;
    if (b->limit_due) {
        header(out, TB710FU_BATTMGR_OP_USB_SET);
        put32(out + 12, 0);
        put32(out + 16, 5);  /* input current limit */
        put32(out + 20, b->limit_due);
        b->limit_in_flight = b->limit_due;
        b->limit_due = 0;
        b->limit_tries++;
        return TB710FU_BATTMGR_REQUEST_BYTES;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields) return 0;
    header(out, kFields[b->cursor].opcode);
    put32(out + 12, 0);
    put32(out + 16, kFields[b->cursor].property);
    put32(out + 20, 0);
    b->in_flight = 1;
    return TB710FU_BATTMGR_REQUEST_BYTES;
}

/* The input current limit a port type gets from the host side: a standard port (SDP 1) the configured value, a
 * charging downstream port (CDP 3) its 1.5 A; others (dedicated chargers, Type-C current, PD contracts) are the
 * firmware's own. Zero (the default) never writes. */
static uint32_t wanted_limit(const Tb710fuBattmgr *b, int port)
{
    if (!b->sdp_limit_ua) return 0;
    return port == 1 ? b->sdp_limit_ua : port == 3 ? 1500000u : 0u;
}

/* After a cycle: a set is due when the port wants a higher limit than the firmware reports, at most three tries
 * per port type. */
static void check_limit(Tb710fuBattmgr *b)
{
    if (!b->sdp_limit_ua) return;
    const int port = (b->valid >> BmUsbAdapter) & 1 ? b->value[BmUsbAdapter] : (b->valid >> BmUsbType) & 1 ? b->value[BmUsbType] : 0;
    if (port != b->limit_port) {b->limit_port = port; b->limit_tries = 0;}
    const uint32_t want = wanted_limit(b, port);
    if (!want || b->limit_tries >= 3) return;
    if ((b->valid >> BmUsbLimit) & 1 && b->value[BmUsbLimit] >= (int32_t)want - 100000) return;  /* 100 mA under: input limiting */
    b->limit_due = want;
}

/* Ends the cycle when the cursor ran past the last field; starts the remembered one. Returns 1 when a cycle ended. */
static int finish(Tb710fuBattmgr *b)
{
    if (b->cursor == NO_CYCLE || b->cursor < BmFields) return 0;
    b->cursor = NO_CYCLE;
    b->cycles_done++;
    check_limit(b);
    if (b->refresh_wanted) {
        const int slow = b->refresh_wanted == 2;
        b->refresh_wanted = 0;
        tb710fu_battmgr_refresh(b, slow);
    }
    return 1;
}

void tb710fu_battmgr_timeout(Tb710fuBattmgr *b)
{
    if (b->limit_in_flight) {b->limit_in_flight = 0; b->limit_result = -60; b->timeouts++; return;}
    if (b->cursor == NO_CYCLE || !b->in_flight) return;
    b->timeouts++;
    b->last_timeout = b->cursor;
    b->valid &= ~(1u << b->cursor);
    advance(b);
    finish(b);
}

int tb710fu_battmgr_receive(Tb710fuBattmgr *b, const uint8_t *data, uint32_t length)
{
    if (length < 12 || le32(data) != TB710FU_BATTMGR_OWNER) {b->ignored++; return BmIgnored;}
    const uint32_t type = le32(data + 4), opcode = le32(data + 8);
    if (opcode == TB710FU_BATTMGR_OP_NOTIFICATION) {
        b->notifications++;
        tb710fu_battmgr_refresh(b, 0);
        return BmNotified;
    }
    if (type != TB710FU_BATTMGR_TYPE_REQUEST) {b->ignored++; return BmIgnored;}
    if (opcode == TB710FU_BATTMGR_OP_REQUEST_NOTIFICATION) {b->enabled = 1; return BmEnabled;}
    if (opcode == TB710FU_BATTMGR_OP_USB_SET) {
        if (!b->limit_in_flight || length < 24 || le32(data + 12) != 5) {b->ignored++; return BmIgnored;}
        b->limit_result = (int32_t)le32(data + 20);
        if (!b->limit_result) b->limit_sets++;
        b->limit_in_flight = 0;
        tb710fu_battmgr_refresh(b, 0);  /* read the limit back */
        return BmAnswer;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields || !b->in_flight || length < 24) {b->ignored++; return BmIgnored;}
    const uint32_t property = le32(data + 12), value = le32(data + 16), result = le32(data + 20);
    if (opcode != kFields[b->cursor].opcode || property != kFields[b->cursor].property) {b->ignored++; return BmIgnored;}
    b->responses++;
    if (result) {
        b->errors++;
        b->valid &= ~(1u << b->cursor);
    } else {
        b->value[b->cursor] = (int32_t)value;
        b->valid |= 1u << b->cursor;
        b->charge_known |= (1u << b->cursor) & ((1u << BmStatus) | (1u << BmUsbOnline));
    }
    advance(b);
    return finish(b) ? BmCycleDone : BmAnswer;
}

void tb710fu_battmgr_snapshot(const Tb710fuBattmgr *b, Tb710fuBatterySnapshot *s)
{
#define HAS(f) (((b->valid | b->charge_known) >> (f)) & 1u)
#define V(f) (b->value[(f)])
    const int status = HAS(BmStatus) ? V(BmStatus) : 0;  /* POWER_SUPPLY_STATUS: 1 charging, 2 discharging, 3 not charging, 4 full */
    s->present = HAS(BmPresent) ? V(BmPresent) != 0 : HAS(BmCapacity);
    s->external_known = HAS(BmUsbOnline);
    s->status_known = HAS(BmStatus);
    s->external = HAS(BmUsbOnline) && V(BmUsbOnline) != 0;
    const int gauges = HAS(BmGauge1Remaining) && HAS(BmGauge1Full) && HAS(BmGauge2Remaining) && HAS(BmGauge2Full);
    const int64_t remaining = gauges ? (int64_t)(uint32_t)V(BmGauge1Remaining) + (uint32_t)V(BmGauge2Remaining) : 0;
    const int64_t full = gauges ? (int64_t)(uint32_t)V(BmGauge1Full) + (uint32_t)V(BmGauge2Full) : 0;
    int percent = full > 0 ? (int)((remaining * 100 + full / 2) / full) : HAS(BmCapacity) ? (V(BmCapacity) + 50) / 100 : 0;
    s->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s->charge_known = full > 0 || HAS(BmCapacity);
    s->charging = s->external && status == 1;
    s->fully_charged = s->external && (status == 4 || s->percent == 100);
    s->critical = s->present && s->external_known && !s->external && s->percent <= 2;
    s->voltage_mv = HAS(BmVoltage) ? V(BmVoltage) / 1000 : 0;
    s->current_ma = HAS(BmCurrent) ? -V(BmCurrent) / 1000 : 0;
    /* -40 to 90 C; the firmware answers a fixed out-of-range value when it has no reading */
    s->temperature_valid = HAS(BmTemperature) && V(BmTemperature) >= -4000 && V(BmTemperature) <= 9000;
    s->temperature = s->temperature_valid ? V(BmTemperature) : 0;
    s->raw_mah = HAS(BmCharge) ? V(BmCharge) / 1000 : 0;
    s->full_mah = HAS(BmFull) ? V(BmFull) / 1000 : 0;
    s->design_mah = HAS(BmDesign) ? V(BmDesign) / 1000 : 0;
    s->cycles = HAS(BmCycles) ? V(BmCycles) : 0;
    s->ctrl_limit_ua = HAS(BmCtrlLimit) ? V(BmCtrlLimit) : 0;
    /* The gauge reports times in seconds, and a negative or huge value when it has none. */
    s->time_to_empty_min = HAS(BmTimeToEmpty) && V(BmTimeToEmpty) > 0 && V(BmTimeToEmpty) < 100 * 3600 ? V(BmTimeToEmpty) / 60 : -1;
    s->time_to_full_min = HAS(BmTimeToFull) && V(BmTimeToFull) > 0 && V(BmTimeToFull) < 100 * 3600 ? V(BmTimeToFull) / 60 : -1;
    if (s->external) {
        s->adapter_type = HAS(BmUsbType) ? V(BmUsbType) : 0;
        s->adapter_mv = HAS(BmUsbVoltage) ? V(BmUsbVoltage) / 1000 : 0;
        s->adapter_ma = HAS(BmUsbCurrent) ? V(BmUsbCurrent) / 1000 : 0;
        if (s->adapter_mv < 0) s->adapter_mv = 0;
        if (s->adapter_ma < 0) s->adapter_ma = -s->adapter_ma;
        s->adapter_watts = (int)(((int64_t)s->adapter_mv * s->adapter_ma + 500000) / 1000000);
    } else {
        s->adapter_type = 0;
        s->adapter_mv = s->adapter_ma = s->adapter_watts = 0;
    }
#undef HAS
#undef V
}
