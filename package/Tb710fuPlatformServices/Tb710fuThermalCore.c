// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuThermalCore.c - TSENS decoding and grouping, no I/O.
#include "Tb710fuThermalCore.h"

#include <stddef.h>

const char *const tb710fu_thermal_group_name[TB710FU_THERMAL_GROUPS] = {
    "cpu0-1", "cpu2-4", "cpu5-6", "cpu7", "cpuss", "gpu", "modem", "other",
    "unnamed"
};

/* sm8650.dtsi #qcom,sensors on each thermal-sensor node. */
const unsigned tb710fu_thermal_sensor_count[TB710FU_TSENS_CONTROLLERS] = {
    15u, 16u, 13u
};

#define G(x) TB710FU_THERMAL_GROUP_##x
const uint8_t tb710fu_thermal_group_of[TB710FU_TSENS_CONTROLLERS]
                                       [TB710FU_TSENS_SENSORS] = {
    /* aoss0, cpuss0..3, cpu2 t/b, cpu3 t/b, cpu4 t/b, cpu5 t/b, cpu6 t/b */
    {G(OTHER), G(CPUSS), G(CPUSS), G(CPUSS), G(CPUSS), G(CPU24), G(CPU24),
     G(CPU24), G(CPU24), G(CPU24), G(CPU24), G(CPU56), G(CPU56), G(CPU56),
     G(CPU56), G(UNNAMED)},
    /* aoss1, cpu7 t/m/b, cpu0, cpu1, six without a zone, video, ddr,
     * camera0, camera1 */
    {G(OTHER), G(CPU7), G(CPU7), G(CPU7), G(CPU01), G(CPU01), G(UNNAMED),
     G(UNNAMED), G(UNNAMED), G(UNNAMED), G(UNNAMED), G(UNNAMED), G(OTHER),
     G(OTHER), G(OTHER), G(OTHER)},
    /* aoss2, gpuss0..7, modem0..3 */
    {G(OTHER), G(GPU), G(GPU), G(GPU), G(GPU), G(GPU), G(GPU), G(GPU), G(GPU),
     G(MODEM), G(MODEM), G(MODEM), G(MODEM), G(UNNAMED), G(UNNAMED),
     G(UNNAMED)}
};
#undef G

int32_t tb710fu_thermal_status_mc(uint32_t status)
{
    const uint32_t field = status & TB710FU_TSENS_TEMP_MASK;
    const int32_t tenths =
        (field & 0x800u) ? (int32_t)field - 0x1000 : (int32_t)field;

    return (int32_t)(tenths * 100);
}

int tb710fu_thermal_pick(const uint32_t *status, unsigned reads, int32_t *mc)
{
    if (status == NULL || mc == NULL || reads == 0u) {
        return 0;
    }
    for (unsigned index = 0; index < reads; ++index) {
        if ((status[index] & TB710FU_TSENS_VALID) != 0u) {
            *mc = tb710fu_thermal_status_mc(status[index]);
            return 1;
        }
    }
    /* Nothing valid: a value that did not move between two consecutive reads
     * is the block's last conversion, as tsens.c accepts it. */
    for (unsigned index = 1; index < reads; ++index) {
        if ((status[index] & TB710FU_TSENS_TEMP_MASK) ==
            (status[index - 1u] & TB710FU_TSENS_TEMP_MASK)) {
            *mc = tb710fu_thermal_status_mc(status[index]);
            return 1;
        }
    }
    return 0;
}

int tb710fu_thermal_enabled(uint32_t control, unsigned sensor)
{
    if (sensor >= 19u) {
        return 0; /* bits 18:3 hold nineteen sensors */
    }
    return (control & TB710FU_TSENS_CTRL_ENABLE) != 0u &&
        ((control >> (TB710FU_TSENS_CTRL_SENSOR_SHIFT + sensor)) & 1u) != 0u;
}

uint32_t tb710fu_thermal_control_for(unsigned count)
{
    uint32_t control = TB710FU_TSENS_CTRL_ENABLE;

    if (count > 19u) {
        count = 19u; /* the 18:3 field only has room for nineteen sensors */
    }
    for (unsigned sensor = 0; sensor < count; ++sensor) {
        control |= 1u << (TB710FU_TSENS_CTRL_SENSOR_SHIFT + sensor);
    }
    return control;
}

void tb710fu_thermal_groups(const int32_t mc[TB710FU_TSENS_CONTROLLERS]
                                   [TB710FU_TSENS_SENSORS],
                            int32_t out[TB710FU_THERMAL_GROUPS])
{
    if (mc == NULL || out == NULL) {
        return;
    }
    for (unsigned group = 0; group < TB710FU_THERMAL_GROUPS; ++group) {
        out[group] = TB710FU_TSENS_NONE;
    }
    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        for (unsigned sensor = 0; sensor < TB710FU_TSENS_SENSORS; ++sensor) {
            const unsigned group = tb710fu_thermal_group_of[controller][sensor];
            const int32_t value = mc[controller][sensor];

            if (group >= TB710FU_THERMAL_GROUPS || value == TB710FU_TSENS_NONE) {
                continue;
            }
            if (out[group] == TB710FU_TSENS_NONE || value > out[group]) {
                out[group] = value;
            }
        }
    }
}

int32_t tb710fu_thermal_hottest(const int32_t mc[TB710FU_TSENS_CONTROLLERS]
                                       [TB710FU_TSENS_SENSORS])
{
    int32_t hottest = TB710FU_TSENS_NONE;

    if (mc == NULL) {
        return TB710FU_TSENS_NONE;
    }
    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        for (unsigned sensor = 0; sensor < TB710FU_TSENS_SENSORS; ++sensor) {
            if (mc[controller][sensor] == TB710FU_TSENS_NONE) {
                continue;
            }
            if (hottest == TB710FU_TSENS_NONE || mc[controller][sensor] > hottest) {
                hottest = mc[controller][sensor];
            }
        }
    }
    return hottest;
}

int tb710fu_thermal_plausible(int32_t milli_celsius)
{
    return milli_celsius >= -40000 && milli_celsius <= 119900;
}
