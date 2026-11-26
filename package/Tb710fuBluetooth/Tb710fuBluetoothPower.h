/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* WCN7850 Hamilton power sequencing and TLMM pin configuration.
 *
 * TLMM register layout and field positions are pinctrl-sm8650.c PINGROUP() (lines 30-58 of that file,
 * which pinctrl-msm consumes with mux_bit 2, drv_bit 6, pull_bit 0, oe_bit 9, in_bit 0, out_bit 1):
 *   CTL at pin + 0x000 (0x1000 per pin) : [1:0] pull, [5:2] mux, [8:6] drive, [9] output enable
 *   IO  at pin + 0x004                : [0] input level, [1] output level
 *   pull: 0 none, 1 pull-down, 2 keeper, 3 pull-up; drive: (value + 1) * 2 mA
 *
 * Pin facts (Tb710fuBluetoothBoard.h cites the device tree lines):
 *   gpio17 BT_EN    function gpio (0), drive 7 (16 mA), bias-disable, output
 *   gpio18 SW_CTRL  function gpio (0), drive 0, bias-pull-down, input
 *   gpio26 TX / 27 RX          function qup2_se6 (1), drive 0 (2 mA), bias-pull-up
 *   gpio24 CTS / 25 RTS        function qup2_se6 (1), drive 0 (2 mA), bias-pull-down
 *
 * Sequence (hci_qca's regulator path for the WCN family): assert enable low, hold 50 ms, release high,
 * hold 50 ms, then sample SW_CTRL, which the module raises when its own rail sequencing finished. The
 * rails themselves are PMU LDOs behind the WCN7850 PMU interface (board dts:273-310), which this driver
 * does not program: either the boot chain left them on, or a power provider installs itself through
 * tb710fu_bluetooth_set_rail_provider() before the Bluetooth driver starts.
 */
#ifndef TB710FU_BLUETOOTH_POWER_H
#define TB710FU_BLUETOOTH_POWER_H

#include <stdbool.h>
#include <stdint.h>
#include "Tb710fuBluetoothBoard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_BT_TLMM_CTL(pin) (0x000u + 0x1000u * (pin))
#define TB_BT_TLMM_IO(pin) (0x004u + 0x1000u * (pin))
#define TB_BT_TLMM_PULL_MASK 0x3u
#define TB_BT_TLMM_MUX_SHIFT 2u
#define TB_BT_TLMM_MUX_MASK 0xfu
#define TB_BT_TLMM_DRIVE_SHIFT 6u
#define TB_BT_TLMM_DRIVE_MASK 0x7u
#define TB_BT_TLMM_OE_BIT 9u
#define TB_BT_TLMM_IO_IN_BIT 0u
#define TB_BT_TLMM_IO_OUT_BIT 1u

typedef struct {
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} TbBtPowerIo;

/* Optional board rail control, installed by a WCN power provider. Return false when the rails could not
 * be turned on, which fails the bring-up stage instead of letting it time out inside the QCA loader. */
typedef struct {
    bool (*set_power)(void *ctx, bool on);
    void *ctx;
} TbBtRailOps;

typedef struct {
    bool bt_en_high;
    bool sw_ctrl_high;
    uint32_t sw_ctrl_samples;
    bool rail_provider;
} TbBtPowerState;

uint32_t tb_bt_power_pin_ctl(uint32_t func, uint32_t drive, uint32_t pull, bool output_enable);
void tb_bt_power_configure_pins(const TbBtPowerIo *io);
int tb_bt_power_on(const TbBtPowerIo *io, const TbBtRailOps *rails, TbBtPowerState *state);
int tb_bt_power_off(const TbBtPowerIo *io, const TbBtRailOps *rails);
void tb710fu_bluetooth_set_rail_provider(const TbBtRailOps *rails);
const TbBtRailOps *tb710fu_bluetooth_rail_provider(void);

#ifdef __cplusplus
}
#endif
#endif /* TB710FU_BLUETOOTH_POWER_H */
