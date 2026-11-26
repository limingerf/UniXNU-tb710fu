// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuThermalCore.h - the SM8650 TSENS arithmetic a host test can run.
//
// Three TSENS v2 controllers, from sm8650.dtsi:
//   tsens0: tm 0x0c228000, srot 0x0c222000, #qcom,sensors = <15>
//   tsens1: tm 0x0c229000, srot 0x0c223000, #qcom,sensors = <16>
//   tsens2: tm 0x0c22a000, srot 0x0c224000, #qcom,sensors = <13>
// each a 0x1000 window, compatible "qcom,tsens-v2", interrupts 506/640,
// 507/641 and 508/642. The register offsets inside those windows are the
// block's, from drivers/thermal/qcom/tsens-v2.c and tsens.c (the same block
// the sheng board's port reads, cross-checked in ThermalProbe.h there):
//   TM Sn_STATUS at 0xa0 + 4 * n: bits 11:0 temperature in tenths of a degree
//   Celsius, two's complement; bit 21 valid; bits 20:16 the five threshold
//   flags (min, lower, upper, critical, max).
//   SROT 0x0 the version (major 31:28, minor 27:16); SROT 0x4 the control word,
//   bit 0 the block enable and bits 18:3 one enable per sensor.
// tsens.c reads a status word up to three times until the valid bit is set and
// otherwise accepts a value two consecutive reads agree on.
//
// What each sensor measures is sm8650.dtsi's thermal-zones table. Two things
// about that table are worth writing down because they are easy to get wrong:
//   - the CPU sensors are numbered by the board's cpu nodes, whose
//     qcom,freq-domain values are 0 (cpu0-1), 3 (cpu2-4), 1 (cpu5-6) and 2
//     (cpu7) - the domains are not in numeric order;
//   - tsens2's sensors 6..11 are named twice, by the gpu/modem zones and by the
//     nsphvx/nsphmx zones. #qcom,sensors = <13> bounds the controller, and the
//     SoC zones (gpuss0..7 at 1..8, modem0..3 at 9..12) are the ones reported
//     here; the duplicate nsph names are not used, and every raw sensor is
//     printed anyway, so nothing is hidden by that choice.
// tsens1's sensors 6..11 have no thermal zone and stay unnamed.
#ifndef TB710FU_THERMAL_CORE_H
#define TB710FU_THERMAL_CORE_H

#include <stdint.h>

#define TB710FU_TSENS_CONTROLLERS 3u
#define TB710FU_TSENS_SENSORS 16u  /* a full status window; DTS counts below */
#define TB710FU_TSENS_WINDOW 0x1000u

#define TB710FU_TSENS_TM_STATUS 0xa0u /* + 4 * sensor */
#define TB710FU_TSENS_TEMP_MASK 0x0fffu
#define TB710FU_TSENS_VALID (1u << 21)
#define TB710FU_TSENS_FLAGS_SHIFT 16u
#define TB710FU_TSENS_FLAGS_MASK 0x1fu

#define TB710FU_TSENS_SROT_VERSION 0x0u
#define TB710FU_TSENS_SROT_CTRL 0x4u
#define TB710FU_TSENS_CTRL_ENABLE (1u << 0)
#define TB710FU_TSENS_CTRL_SENSOR_SHIFT 3u
#define TB710FU_TSENS_CTRL_SENSOR_BITS 0x7ffffu /* 19 sensors, 18:3 */

#define TB710FU_TSENS_READ_TRIES 3u
#define TB710FU_TSENS_NONE INT32_MIN

/* Physical windows, sm8650.dtsi thermal-sensor@... nodes. */
#define TB710FU_TSENS0_TM_PHYS 0x0c228000u
#define TB710FU_TSENS0_SROT_PHYS 0x0c222000u
#define TB710FU_TSENS1_TM_PHYS 0x0c229000u
#define TB710FU_TSENS1_SROT_PHYS 0x0c223000u
#define TB710FU_TSENS2_TM_PHYS 0x0c22a000u
#define TB710FU_TSENS2_SROT_PHYS 0x0c224000u

/* The groups a boot-log row reports: the hottest sensor of each. */
enum {
    TB710FU_THERMAL_GROUP_CPU01,
    TB710FU_THERMAL_GROUP_CPU24,
    TB710FU_THERMAL_GROUP_CPU56,
    TB710FU_THERMAL_GROUP_CPU7,
    TB710FU_THERMAL_GROUP_CPUSS,
    TB710FU_THERMAL_GROUP_GPU,
    TB710FU_THERMAL_GROUP_MODEM,
    TB710FU_THERMAL_GROUP_OTHER,
    TB710FU_THERMAL_GROUP_UNNAMED,
    TB710FU_THERMAL_GROUPS
};

extern const char *const tb710fu_thermal_group_name[TB710FU_THERMAL_GROUPS];
extern const unsigned tb710fu_thermal_sensor_count[TB710FU_TSENS_CONTROLLERS];
extern const uint8_t tb710fu_thermal_group_of[TB710FU_TSENS_CONTROLLERS]
                                               [TB710FU_TSENS_SENSORS];

/* Millidegrees Celsius of a status word's temperature field, sign-extended
 * from bit 11 (the field is tenths of a degree). */
int32_t tb710fu_thermal_status_mc(uint32_t status);

/* One sensor from the status words of up to three reads, in the order read:
 * the first valid one, otherwise a value two consecutive reads agree on,
 * otherwise none. Returns 1 with *mc set, 0 for none. */
int tb710fu_thermal_pick(const uint32_t *status, unsigned reads, int32_t *mc);

/* 1 when a controller's control word has the block and the sensor enabled. */
int tb710fu_thermal_enabled(uint32_t control, unsigned sensor);

/* The control word that enables `count` sensors and the block. */
uint32_t tb710fu_thermal_control_for(unsigned count);

/* The hottest reading of each group; a group without a reading gets
 * TB710FU_TSENS_NONE. mc holds per-controller sensor readings, with
 * TB710FU_TSENS_NONE for a sensor that was not read or is out of range. */
void tb710fu_thermal_groups(const int32_t mc[TB710FU_TSENS_CONTROLLERS]
                                   [TB710FU_TSENS_SENSORS],
                            int32_t out[TB710FU_THERMAL_GROUPS]);

/* The hottest sensor over all controllers, or TB710FU_TSENS_NONE. */
int32_t tb710fu_thermal_hottest(const int32_t mc[TB710FU_TSENS_CONTROLLERS]
                                       [TB710FU_TSENS_SENSORS]);

/* 1 when a reading is inside the range the block can express
 * (-40.0 .. 119.9 degrees Celsius, the 12-bit tenths field). */
int tb710fu_thermal_plausible(int32_t milli_celsius);

#endif
