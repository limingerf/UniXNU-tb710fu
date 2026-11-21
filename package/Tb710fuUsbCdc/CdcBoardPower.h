/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Board wiring from tb710fu-linux@4d04410798b538fbe4b50664e3a66d9500a2ed31. */
#ifndef TB710FU_CDC_BOARD_POWER_H
#define TB710FU_CDC_BOARD_POWER_H

#include <stddef.h>
#include <stdint.h>

#include "CdcHardware.h"

struct tb710fu_cdc_board_rail {
    const char *resource;
    uint32_t millivolts;
};

static const struct tb710fu_cdc_board_rail tb710fu_cdc_phy_rails[] = {
    {"ldoi1", 880},
    {"ldoi3", 1200},
    {"ldob15", 1800},
    {"ldob5", 3104}
};

#define TB710FU_CDC_PHY_RAIL_COUNT \
    (sizeof(tb710fu_cdc_phy_rails) / sizeof(tb710fu_cdc_phy_rails[0]))

#define TB710FU_CDC_SPMI_EE 0u
#define TB710FU_CDC_SPMI_REPEATER_PPID 0x7fdu
#define TB710FU_CDC_SPMI_REPEATER_BASE 0xfd00u
#define TB710FU_CDC_REPEATER_ENABLE 0x46u
#define TB710FU_CDC_REPEATER_STATUS 0x08u
#define TB710FU_CDC_REPEATER_READY (1u << 7)

#define TB710FU_CDC_REPEATER_TUNE_CURRENT 0x51u
#define TB710FU_CDC_REPEATER_TUNE_SQUELCH 0x54u
#define TB710FU_CDC_REPEATER_TUNE_PREEMPHASIS 0x57u

#define TB710FU_CDC_PHY_REF_HZ 38400000u

/* Linux phy-snps-eusb2.c QCOM sequence for the board's 38.4 MHz ref clock. */
static const struct tb710fu_cdc_masked_write tb710fu_cdc_phy_init[] = {
    {0x94, 0x02, 0x02}, {0x50, 0x02, 0x02}, {0x54, 0x09, 0x09},
    {0x130, 0x04, 0x04}, {0x98, 0x40, 0x00}, {0xb8, 0x01, 0x01},
    {0x54, 0x70, 0x40}, {0x5c, 0xff, 0xc8}, {0x60, 0x0f, 0x00},
    {0x58, 0xfe, 0x00}, {0x68, 0xfc, 0x20}, {0x68, 0x03, 0x01},
    {0x6c, 0x1f, 0x10}, {0x70, 0x07, 0x00}, {0x6c, 0xc0, 0x40},
    {0x64, 0x10, 0x10}, {0x7c, 0x07, 0x00}, {0x7c, 0x60, 0x40},
    {0x7c, 0x18, 0x08}, {0x78, 0x38, 0x18}, {0x78, 0xc0, 0x00},
    {0x64, 0x0c, 0x0c}, {0x3c, 0x01, 0x01}, {0x54, 0x02, 0x02},
    {0x54, 0x04, 0x00}, {0x50, 0x02, 0x00}, {0x64, 0x08, 0x00},
    {0x94, 0x02, 0x00}
};

#define TB710FU_CDC_PHY_INIT_COUNT \
    (sizeof(tb710fu_cdc_phy_init) / sizeof(tb710fu_cdc_phy_init[0]))

#endif
