/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The battery manager TB710FU's audio DSP runs (PMIC GLINK, channel "PMIC_RTR_ADSP_APPS", protection domain
 * "msm/adsp/charger_pd"): messages start with a 12-byte header (owner, type, opcode); a property read is a request
 * {battery 0, property, 0} answered by {property, value, result}; the DSP also sends notifications when something
 * changed. This is the message codec, the refresh cycle (one request in flight, the next one only when it is
 * answered or the owner's timer gives up on it) and the mapping to what macOS reads from a power source. No I/O,
 * no waits: the owner sends the bytes and feeds back what arrives. Charging itself stays with the DSP firmware;
 * the only setting written here is the USB input current limit on a computer's port, and only when a boot
 * argument asks for it.
 *
 * Provenance:
 *  - Channel, protection domain and the child node "qcom,battery_charger": this board's live DTB
 *    (/soc/qcom,pmic_glink, .local/input-sources/sm8650-lenovo-tb710fu.dts) and its vendor_dlkm
 *    (qti_battery_charger.ko, modules.load).
 *  - Owner 32778 and the message framing/opcodes: UniXNU-sheng's ShengBattmgr.c, hardware-verified on SM8550
 *    (porting.toml "battery properties after 11 are shifted by one" note), and mainline qcom_battmgr. The mainline
 *    driver (drivers/power/supply/qcom_battmgr.c) sends exactly the same 24-byte property request
 *    {owner, type, opcode, battery 0, property, value} and the same 28-byte notification-enable frame, and its
 *    SM8550 variant is what runs on this board's Linux port; its notification classes (0x80 status, 0x81 information,
 *    0x83 charging state) are used here to decide when the slow fields are re-read.
 *  - Property numbers: NOT sheng's. They are this device's own order, read from qti_battery_charger.ko's
 *    "battery_props" and "usb_props" string tables in .rodata (file offsets 113201..113631 and 115649..115892).
 *    sheng's list inserted thermal level / its max / charge current before temperature, shifting 10..21 by one;
 *    here BATT_CHG_CTRL_LIM(10) and BATT_CHG_CTRL_LIM_MAX(11) sit where sheng had thermal level and its max, so
 *    temperature is 12, not 13, and charge counter is 14, not 15. Reading sheng's numbers on this board would
 *    report the charger's current limit as battery temperature.
 *  - The board-level ids (0x8a/0x8b, 0xa6/0xa7, 0xa5, 0x18, 0x2a, 0x33, 0x03, 0x07) are the OPPO/ColorOS
 *    "FAC" property family this vendor image ships (FAC_POWER_SUPPLY_PROP_* strings). They are read only when
 *    they answer; the snapshot falls back to BATT_CAPACITY when a gauge is not there, so a board without the
 *    second cell does not get a wrong percentage.
 */
#ifndef TB710FU_BATTMGR_H
#define TB710FU_BATTMGR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_BATTMGR_OWNER 32778u  /* the battery manager; 32779 is the USB-C port manager on the same channel */
#define TB710FU_BATTMGR_TYPE_REQUEST 1u
#define TB710FU_BATTMGR_TYPE_NOTIFY 2u
#define TB710FU_BATTMGR_OP_REQUEST_NOTIFICATION 0x04u
#define TB710FU_BATTMGR_OP_NOTIFICATION 0x07u
#define TB710FU_BATTMGR_OP_BATTERY_GET 0x30u
#define TB710FU_BATTMGR_OP_USB_GET 0x32u
#define TB710FU_BATTMGR_OP_USB_SET 0x33u
#define TB710FU_BATTMGR_OP_BOARD_GET 0x50u
#define TB710FU_BATTMGR_REQUEST_BYTES 24u
#define TB710FU_BATTMGR_ENABLE_BYTES 28u

/* The fields one refresh reads, in order (at most 32: valid has a bit per field). */
enum {
    BmStatus, BmPresent, BmCapacity, BmVoltage, BmCurrent, BmTemperature, BmCharge, BmUsbOnline, BmUsbType, BmUsbVoltage,
    BmUsbCurrent, BmTimeToEmpty, BmTimeToFull, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmFull,
    BmDesign, BmCycles, BmUsbAdapter, BmUsbLimit, BmUsbMax, BmChargeType, BmCtrlLimit, BmRealType, BmPowerMax,
    BmPdVerified, BmAuthentic, BmSlaveAuthentic, BmInputSuspend, BmFields
};

typedef struct {
    int32_t value[BmFields];
    uint32_t valid;               /* bit per field in the latest refresh */
    uint32_t charge_known;        /* last successful status/USB-online readings survive transient errors */
    uint8_t cursor, in_flight, enabled, slow_due;
    uint32_t cycles_done, responses, notifications, errors, timeouts, ignored;
    uint32_t refresh_wanted;      /* a notification asked for a new cycle while one ran */
    int last_timeout;             /* the field the last timeout skipped, -1 none */
    /* the input current limit on a computer's port: what a standard port (SDP) gets, 0 to leave the firmware's;
     * the port type the tries count for; the value due to be sent, the one in flight, tries, sets, the last result */
    uint32_t sdp_limit_ua;
    int32_t limit_port;
    uint32_t limit_due, limit_in_flight, limit_tries, limit_sets;
    int32_t limit_result;
} Tb710fuBattmgr;

/* What a power source shows. Temperatures are hundredths of a degree Celsius, as the firmware reports them. The
 * charge comes from the board's fuel gauges (remaining over full, both summed) when they all answer, else from
 * BATT_CAPACITY; the board reports discharge positive, the power source charge positive. */
typedef struct {
    int present, external, charging, fully_charged, critical;
    int percent;                  /* 0..100 */
    int charge_known;             /* percent comes from a reading, not a default */
    int external_known, status_known; /* unknown charger state must not publish a disconnect */
    int voltage_mv, current_ma;   /* current: positive while the battery takes charge */
    int temperature, temperature_valid;  /* valid: a reading in the range a battery can have */
    int raw_mah, full_mah, design_mah, cycles;
    int time_to_empty_min, time_to_full_min;  /* -1 unknown */
    int adapter_type;             /* POWER_SUPPLY_USB_TYPE, 0 unknown */
    int adapter_mv, adapter_ma, adapter_watts;
    int ctrl_limit_ua;            /* BATT_CHG_CTRL_LIM, the charger's charge current limit, 0 unknown */
} Tb710fuBatterySnapshot;

enum {BmIgnored = 0, BmAnswer = 1, BmCycleDone = 2, BmNotified = 3, BmEnabled = 4};

void tb710fu_battmgr_init(Tb710fuBattmgr *b);
/* The notification enable request (sent once the channel is up). */
uint32_t tb710fu_battmgr_enable_request(uint8_t out[TB710FU_BATTMGR_ENABLE_BYTES]);
/* Starts a refresh cycle (no effect while one runs: it is remembered and runs after). slow: also the fields that
 * hardly change. */
void tb710fu_battmgr_refresh(Tb710fuBattmgr *b, int slow);
/* The next request: a due input current limit, or the running cycle's next field; 0 when nothing is to be sent now
 * (one in flight, or nothing due). */
uint32_t tb710fu_battmgr_next(Tb710fuBattmgr *b, uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES]);
/* The owner's request timer ran out: the field is skipped (a limit set counts as tried). */
void tb710fu_battmgr_timeout(Tb710fuBattmgr *b);
/* A message from the channel; returns one of the Bm* results. */
int tb710fu_battmgr_receive(Tb710fuBattmgr *b, const uint8_t *data, uint32_t length);
/* The power source values from the fields read so far. */
void tb710fu_battmgr_snapshot(const Tb710fuBattmgr *b, Tb710fuBatterySnapshot *s);

#ifdef __cplusplus
}
#endif
#endif
