// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuBacklightCore.h - the parts of the TB710FU panel backlight a host test can run.
//
// Which device actually lights this panel, from the board devicetree only
// (arch/arm64/boot/dts/qcom/sm8650-lenovo-tb710fu.dts):
//
//   &i2c6 {  backlight: backlight@2e { compatible = "silergy,sy7758-master";
//                                       reg = <0x2e>; ... };
//            panel: panel@0 { compatible = "lenovo,nt36532-wqhd";
//                             backlight = <&backlight>; ... }; };
//
// so brightness travels over I2C to a SY7758 at 0x2e on QUP wrap-1 SE6
// (i2c@a98000), and not over a DSI DCS 0x51 write, the PMIC's WLED block or a
// PWM channel: the panel node declares no qcom,mdss-dsi-bl-pmic-control-type
// and pmk8550.dtsi's PWM node is disabled. The DCS path would in any case need
// the display driver's command channel, which this package does not own.
//
// The register map is the SY7758's own, from mainline
// drivers/video/backlight/sy7758.c (Kancy Joe, Neil Armstrong; v3, May 2026),
// which drives the same part on the SM8650 Ayaneo Pocket S2 - also at 0x2e:
//   REG_BRT_8BIT 0x00, REG_DEV_CTL 0x01, REG_STATUS 0x02, REG_DEV_ID 0x03
//   (reads 0x63), REG_DIRECT_CTL 0x04, REG_STATUS2 0x05,
//   REG_BRT_12BIT_L 0x10, REG_BRT_12BIT_H 0x11, REG_LED_ENABLE 0x16.
//   DEV_CTL bits 2:1 select the brightness mode (1 = 8 bit, 2 = 12 bit).
//   The largest 12-bit value is 0xFF0 = 4080, which the driver uses as
//   max_brightness.
// The devicetree's compatible strings are "silergy,sy7758-master" and
// "...-salve" (i2c2 0x2e), a vendor split the mainline binding does not have;
// this package only drives the master the panel's backlight phandle names.
//
// Two deliberate limits, both visible in the boot log:
//   - only the two brightness bytes are ever written. DEV_CTL (mode), LED_ENABLE
//     and the OTP trim registers are read, never rewritten: the vendor's
//     master/salve split is not the mainline part's single-chip init, and a
//     wrong mode or channel mask leaves the panel dark with no console.
//   - a 12-bit write only has an effect when DEV_CTL already selects 12-bit
//     mode, which the boot loader must have set. When it does not, the service
//     says so and stays out of the write path instead of pretending to dim.
//
#ifndef TB710FU_BACKLIGHT_CORE_H
#define TB710FU_BACKLIGHT_CORE_H

#include <stdint.h>

/* I2C identity, from sm8650-lenovo-tb710fu.dts backlight@2e of &i2c6. */
#define TB710FU_SY7758_ADDRESS 0x2eu

/* SY7758 registers, mainline drivers/video/backlight/sy7758.c. */
#define TB710FU_SY7758_REG_BRT_8BIT 0x00u
#define TB710FU_SY7758_REG_DEV_CTL 0x01u
#define TB710FU_SY7758_REG_DEV_ID 0x03u
#define TB710FU_SY7758_REG_BRT_12BIT_L 0x10u
#define TB710FU_SY7758_REG_BRT_12BIT_H 0x11u

#define TB710FU_SY7758_DEV_ID_VALUE 0x63u
#define TB710FU_SY7758_BRT_MODE_MASK 0x06u
#define TB710FU_SY7758_BRT_MODE_12BIT 0x04u
#define TB710FU_SY7758_MAX_LEVEL 4080u /* the driver's max_brightness, 0xFF0 */
#define TB710FU_SY7758_LEVEL_MASK 0x0fffu

/* The slider's luminance scale. These are NOT measurements: no photometer has
 * been on this panel, so they only place CoreBrightness's slider, exactly as
 * the sheng board's 2/140/500 do. Override them from the boot log's numbers
 * once the panel is measured; nothing else depends on them. */
#define TB710FU_BACKLIGHT_MIN_NITS 2u
#define TB710FU_BACKLIGHT_MID_NITS 140u
#define TB710FU_BACKLIGHT_MAX_NITS 500u

/* Never rounds a lit panel down to off: any positive request becomes at least
 * level 1. */
unsigned tb710fu_backlight_level_for_nits(int32_t fixed_16_16,
                                          unsigned max_nits);

/* The inverse, for publishing BrightnessMilliNits: level -> nits * 1000. */
unsigned tb710fu_backlight_milli_nits_for_level(unsigned level, unsigned max_nits);

/* Level clamped to the part's range. */
unsigned tb710fu_backlight_clamp(unsigned level);

/* The two bytes of a 12-bit level, as the part lays them out: 0x10 holds bits
 * 7:0 and 0x11 holds bits 11:8 in its low nibble. join12 is its inverse, and
 * masks the high byte so a stray upper nibble cannot invent bits. */
void tb710fu_backlight_split12(unsigned level, uint8_t *low, uint8_t *high);
unsigned tb710fu_backlight_join12(uint8_t low, uint8_t high);

/* 1 when DEV_CTL selects 12-bit brightness (bits 2:1 == 2). */
int tb710fu_backlight_mode_12bit(uint8_t dev_ctl);

/* One step of a fade, in levels: from at elapsed 0 to exactly to at
 * elapsed >= period. period 0 means "jump", elapsed beyond the period cannot
 * overshoot, and a level never leaves the range between from and to. */
unsigned tb710fu_backlight_fade_level(unsigned from, unsigned to,
                                      uint64_t elapsed_ns, uint64_t period_ns);

#endif
