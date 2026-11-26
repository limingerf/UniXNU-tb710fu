/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>

#include "Tb710fuBluetoothPower.h"

/* One module-wide provider slot, written once while the driver initialises and read afterwards. */
static const TbBtRailOps *gRailProvider;

void tb710fu_bluetooth_set_rail_provider(const TbBtRailOps *rails)
{
    gRailProvider = rails;
}

const TbBtRailOps *tb710fu_bluetooth_rail_provider(void)
{
    return gRailProvider;
}

uint32_t tb_bt_power_pin_ctl(uint32_t func, uint32_t drive, uint32_t pull, bool output_enable)
{
    uint32_t ctl = (pull & TB_BT_TLMM_PULL_MASK);
    ctl |= (func & TB_BT_TLMM_MUX_MASK) << TB_BT_TLMM_MUX_SHIFT;
    ctl |= (drive & TB_BT_TLMM_DRIVE_MASK) << TB_BT_TLMM_DRIVE_SHIFT;
    if (output_enable) ctl |= 1u << TB_BT_TLMM_OE_BIT;
    return ctl;
}

static void set_io(const TbBtPowerIo *io, uint32_t pin, bool high)
{
    io->write(io->ctx, TB_BT_TLMM_IO(pin), high ? (1u << TB_BT_TLMM_IO_OUT_BIT) : 0u);
}

void tb_bt_power_configure_pins(const TbBtPowerIo *io)
{
    if (!io || !io->write) return;
    /* BT_EN: output, low until tb_bt_power_on drives it. OE on with a low output first keeps the line
     * from glitching high when the pad is switched from input to output. */
    io->write(io->ctx, TB_BT_TLMM_IO(TB_BT_BT_EN_GPIO), 0u);
    io->write(io->ctx, TB_BT_TLMM_CTL(TB_BT_BT_EN_GPIO),
              tb_bt_power_pin_ctl(TB_BT_GPIO_FUNC, TB_BT_BT_EN_DRIVE, TB_BT_BT_EN_BIAS, true));
    /* SW_CTRL: input with pull-down. */
    io->write(io->ctx, TB_BT_TLMM_CTL(TB_BT_SW_CTRL_GPIO),
              tb_bt_power_pin_ctl(TB_BT_GPIO_FUNC, TB_BT_SW_CTRL_DRIVE, TB_BT_SW_CTRL_BIAS, false));
    /* UART: TX/RX pulled up, CTS/RTS pulled down, precisely as qup_uart14_default / _cts_rts ask. */
    io->write(io->ctx, TB_BT_TLMM_CTL(TB_BT_PIN_TX),
              tb_bt_power_pin_ctl(TB_BT_UART_FUNC, TB_BT_UART_DRIVE, TB_BT_UART_TXRX_BIAS, false));
    io->write(io->ctx, TB_BT_TLMM_CTL(TB_BT_PIN_RX),
              tb_bt_power_pin_ctl(TB_BT_UART_FUNC, TB_BT_UART_DRIVE, TB_BT_UART_TXRX_BIAS, false));
    io->write(io->ctx, TB_BT_TLMM_CTL(TB_BT_PIN_CTS),
              tb_bt_power_pin_ctl(TB_BT_UART_FUNC, TB_BT_UART_DRIVE, TB_BT_UART_CTS_RTS_BIAS, false));
    io->write(io->ctx, TB_BT_TLMM_CTL(TB_BT_PIN_RTS),
              tb_bt_power_pin_ctl(TB_BT_UART_FUNC, TB_BT_UART_DRIVE, TB_BT_UART_CTS_RTS_BIAS, false));
}

int tb_bt_power_on(const TbBtPowerIo *io, const TbBtRailOps *rails, TbBtPowerState *state)
{
    if (!io || !io->write || !io->read || !io->delay_ms) return -1;
    if (state) {
        state->bt_en_high = false;
        state->sw_ctrl_high = false;
        state->sw_ctrl_samples = 0u;
        state->rail_provider = rails != NULL;
    }
    if (rails && rails->set_power && !rails->set_power(rails->ctx, true)) return -1;

    tb_bt_power_configure_pins(io);

    /* hci_qca's enable pulse: low, 50 ms, high, 50 ms. */
    set_io(io, TB_BT_BT_EN_GPIO, false);
    io->delay_ms(io->ctx, 50u);
    set_io(io, TB_BT_BT_EN_GPIO, true);
    io->delay_ms(io->ctx, 50u);
    if (state) state->bt_en_high = true;

    /* SW_CTRL is the module's "rails good, radio running" line; sample it as evidence, not as a gate:
     * some boards tie it to a test point that reads back low even when the module is up. */
    const uint32_t io_val = io->read(io->ctx, TB_BT_TLMM_IO(TB_BT_SW_CTRL_GPIO));
    if (state) {
        state->sw_ctrl_samples++;
        state->sw_ctrl_high = (io_val & (1u << TB_BT_TLMM_IO_IN_BIT)) != 0u;
    }
    return 0;
}

int tb_bt_power_off(const TbBtPowerIo *io, const TbBtRailOps *rails)
{
    if (!io) return -1;
    if (io->write) set_io(io, TB_BT_BT_EN_GPIO, false);
    if (io->delay_ms) io->delay_ms(io->ctx, 50u);
    if (rails && rails->set_power) rails->set_power(rails->ctx, false);
    return 0;
}
