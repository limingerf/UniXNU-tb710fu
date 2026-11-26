/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB710FU (Lenovo, SM8650Q-AB) Bluetooth board facts: everything this driver knows about where the WCN7850
 * Bluetooth radio sits on the SoC, and nothing else. Each constant names the device tree or driver file it
 * was read from, with the line it appears on, so a reviewer can check the port against the board without
 * trusting this comment.
 *
 * Device tree inputs (d:\unixnu-tb710fu-clean\.local\input-sources):
 *   sm8650-lenovo-tb710fu.dts  the board (rev 6.12 sources of the tb710fu Linux port)
 *   sm8650.dtsi                the SoC
 *   pm8550.dtsi / pmk8550.dtsi the PMICs (only for the regulator list below)
 * IP facts that no device tree carries (GENI/UART register layout, TLMM register layout, GCC RCG encodings)
 * are in Tb710fuBluetoothGeni.h, Tb710fuBluetoothClock.h and Tb710fuBluetoothPower.h with their own
 * provenance. Nothing here is inherited from the SM8550 (sheng) port: that board's UART SE, GPIO numbers,
 * firmware names and clock roots are all different.
 */
#ifndef TB710FU_BLUETOOTH_BOARD_H
#define TB710FU_BLUETOOTH_BOARD_H

/* ---- GENI UART serial engine ----------------------------------------------------------------
 * sm8650.dtsi:2489-2500  uart14: serial@898000, compatible "qcom,geni-uart", reg <0 0x00898000 0 0x4000>,
 * interrupts <GIC_SPI 461 IRQ_TYPE_LEVEL_HIGH 0>, clocks <&gcc GCC_QUPV3_WRAP2_S6_CLK> ("se").
 * This is QUP wrapper 2, serial engine 6. The board selects it as serial1 (dts:30) and attaches the
 * Bluetooth node to it (dts:1223-1239). It is not the debug console: uart15 (0x0089c000, SE7) is. */
#define TB_BT_SE_PHYS 0x00898000u
#define TB_BT_SE_BYTES 0x4000u
#define TB_BT_SPI 461u                       /* GIC_SPI 461 -> INTID 32 + 461 = 493 */
#define TB_BT_INTID (32u + TB_BT_SPI)

/* QUP wrapper 2 core ("qupv3_id_1: geniqup@8c0000", sm8650.dtsi:2026-2028). The wrapper's own 0x2000-byte
 * block holds QUPV3_HW_VER and the common config registers; SEs 0..7 sit at 0x880000 + n * 0x4000, so SE6
 * (uart14) at 0x898000 is inside this wrapper even though its address is below the core's. */
#define TB_BT_WRAPPER_PHYS 0x008c0000u
#define TB_BT_WRAPPER_BYTES 0x2000u

/* ---- GCC (clock controller) ------------------------------------------------------------------
 * sm8650.dtsi:1967-1969  gcc: clock-controller@100000, reg <0 0x00100000 0 0x1f4200>.
 * The SM8650 GCC clock driver (linux-gcc-sm8650.c in .work/usb-cdc-hardware-reference) describes the
 * UART14 source exactly:
 *   gcc_qupv3_wrap2_s6_clk_src  cmd_rcgr = 0x1e760, mnd_width 16, hid_width 5, parent_map_10,
 *                               ftbl_gcc_qupv3_wrap2_s6_clk_src            (lines 1191-1198)
 *   gcc_qupv3_wrap2_s6_clk      halt_reg 0x1e754, enable_reg 0x52010, enable_mask BIT(10)  (2788-2793)
 *   gcc_qupv3_wrap2_core_clk    halt_reg 0x23294, enable_reg 0x52010, enable_mask BIT(0)   (2627-2632)
 * parent_map_10 (lines 389-394) puts GCC_GPLL0_OUT_EVEN at index 6; every ftbl entry for this root is
 * GPLL0_EVEN at pre-div 1 with a 16-bit M/N, e.g. F(7372800, GPLL0_EVEN, 1, 384, 15625) (1167-1180). */
#define TB_BT_GCC_PHYS 0x00100000u
#define TB_BT_GCC_BYTES 0x1f4200u
#define TB_BT_GCC_RCG_OFF 0x1e760u           /* gcc_qupv3_wrap2_s6_clk_src.cmd_rcgr */
#define TB_BT_GCC_BRANCH_OFF 0x52010u        /* both wrap2 branch votes live in this register */
#define TB_BT_GCC_S6_BRANCH_BIT 10u          /* gcc_qupv3_wrap2_s6_clk.enable_mask */
#define TB_BT_GCC_CORE_BRANCH_BIT 0u         /* gcc_qupv3_wrap2_core_clk.enable_mask */
#define TB_BT_GCC_S6_HALT_OFF 0x1e754u       /* BRANCH_HALT_VOTED: bit 31 is the vote */
#define TB_BT_GCC_CORE_HALT_OFF 0x23294u
#define TB_BT_GCC_HALT_VOTED_BIT 31u

/* ---- TLMM (pin controller) -------------------------------------------------------------------
 * sm8650.dtsi:6113-6125  tlmm: pinctrl@f100000, reg <0 0x0f100000 0 0x300000>, gpio-ranges 211 pins,
 * interrupts <GIC_SPI 208 IRQ_TYPE_LEVEL_HIGH 0>. Register layout (.ctl at pin + 0x0, .io at + 0x4,
 * 0x1000 per pin, mux_bit 2, drv_bit 6, pull_bit 0, oe_bit 9, in_bit 0, out_bit 1) is pinctrl-sm8650.c
 * PINGROUP() (lines 30-58) and is mirrored in Tb710fuBluetoothPower.h. */
#define TB_BT_TLMM_PHYS 0x0f100000u
#define TB_BT_TLMM_BYTES 0x300000u

/* BT_EN is the WCN7850 PMU's enable line, not a Bluetooth-node property:
 *   dts:254-261   wcn7850-pmu { bt-enable-gpios = <&tlmm 17 GPIO_ACTIVE_HIGH>; }
 *   dts:1020-1026 bt-en-pins: pins "gpio17", function "gpio", drive-strength 16 (mA), bias-disable
 * SW_CTRL is the module's own "radio booted" line, read as an input:
 *   dts:1028-1032 sw-ctrl-pins: pins "gpio18", function "gpio", bias-pull-down (no drive-strength) */
#define TB_BT_BT_EN_GPIO 17u
#define TB_BT_SW_CTRL_GPIO 18u
#define TB_BT_BT_EN_DRIVE 7u                 /* 16 mA: (7 + 1) * 2 */
#define TB_BT_BT_EN_BIAS 0u                   /* bias-disable */
#define TB_BT_SW_CTRL_DRIVE 0u               /* Linux default, 2 mA */
#define TB_BT_SW_CTRL_BIAS 1u                /* bias-pull-down */
#define TB_BT_GPIO_FUNC 0u                   /* function "gpio" is index 0 of every pingroup */

/* UART14 pins, from sm8650.dtsi qup_uart14_default / qup_uart14_cts_rts:
 *   TX, RX   = "gpio26", "gpio27", function "qup2_se6", drive-strength 2, bias-pull-up
 *   CTS, RTS = "gpio24", "gpio25", function "qup2_se6", drive-strength 2, bias-pull-down
 * pinctrl-sm8650.c [24]..[27] list qup2_se6 as the *second* function of each pin (gpio is first), and
 * pinctrl-msm programs the index inside that per-pin list, so the hardware mux value is 1, not the
 * global enum value 103. */
#define TB_BT_PIN_TX 26u
#define TB_BT_PIN_RX 27u
#define TB_BT_PIN_CTS 24u
#define TB_BT_PIN_RTS 25u
#define TB_BT_UART_FUNC 1u                   /* qup2_se6 in each pin's own function list */
#define TB_BT_UART_DRIVE 0u                  /* drive-strength 2 mA: (0 + 1) * 2 */
#define TB_BT_UART_TXRX_BIAS 3u              /* bias-pull-up */
#define TB_BT_UART_CTS_RTS_BIAS 1u           /* bias-pull-down */

/* ---- Bluetooth node ---------------------------------------------------------------------------
 * dts:1223-1239  &uart14 { status "okay"; bluetooth { compatible "qcom,wcn7850-bt";
 *                vddrfacmn/vddaon/vddwlcx/vddwlmx/vddrfa0p8/vddrfa1p2/vddrfa1p8 <- vreg_pmu_*;
 *                max-speed = <3200000>; }; };
 * No enable-gpios and no swctrl-gpios: both lines belong to the PMU node above. max-speed is the
 * operating speed this driver switches to after the QCA ROM version read. */
#define TB_BT_COMPATIBLE "qcom,wcn7850-bt"
#define TB_BT_MAX_SPEED 3200000u
#define TB_BT_INIT_SPEED 115200u

/* Board rails, for the record and for a power provider that may want the names:
 *   dts:263-269  vdd = vreg_s4i_0p3, vddio = vreg_l15b_1p8, vddio1p2 = vreg_l3c_1p2,
 *                vddaon = vreg_s2c_0p5, vdddig = vreg_s3c_0p9, vddrfa1p2 = vreg_s1c_1p2,
 *                vddrfa1p8 = vreg_s6c_1p8, clocks = <&rpmhcc RPMH_RF_CLK1>
 *   dts:273-310  the PMU's own LDOs (ldo0..ldo6), reached over the WCN7850 PMU interface, not over RPMh.
 * This driver does not program PMIC or RPMh registers: the rails are either already up (the boot chain
 * powered them for the Android Bluetooth stack) or a WCN power provider installs itself through
 * tb710fu_bluetooth_set_rail_provider() before this driver runs. See docs/BLUETOOTH_WCN7850.md. */

/* ---- Firmware ----------------------------------------------------------------------------------
 * .local/input-sources/firmware/MD5SUMS lists the board's stock members and their digests:
 *   bcaca7b2a1e02be62fbffaf746d52440 *lib/firmware/qca/hmtbtfw20.tlv   274128 bytes, QCA TLV type 1
 *   c047013753ce0c3bec603a590b9fa7e4 *lib/firmware/qca/hmtnv20.bin     9592 bytes,  QCA TLV type 2
 * The addon carries the two files under their basenames; the VFS fallback directory is
 * /private/var/firmware/qca (or the bt-fw boot argument). */
#define TB_BT_FW_PATCH_NAME "hmtbtfw20.tlv"
#define TB_BT_FW_NVM_NAME "hmtnv20.bin"
#define TB_BT_FW_DIR "/private/var/firmware/qca"

#endif /* TB710FU_BLUETOOTH_BOARD_H */
