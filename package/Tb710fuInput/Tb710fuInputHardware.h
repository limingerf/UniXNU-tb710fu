/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB710FU input hardware facts.
 *
 * Every address, bus number, GPIO, interrupt and compatible below is taken
 * from the four pinned board sources and from nothing else:
 *
 *   TB : sm8650-lenovo-tb710fu.dts   (board wiring)
 *   SoC: sm8650.dtsi                 (SE / QUP / TLMM / SPMI / GCC windows)
 *   PMIC: pmk8550.dtsi, pm8550.dtsi  (PON and GPIO peripherals)
 *
 * The comment above each group names the exact node it comes from. Anything
 * this port could not establish from those files is *not* defined here; the
 * driver either reads it at run time or refuses to act, never guesses.
 *
 * Register offsets inside the GENI SE and the SPMI arbiter are architectural
 * (QUPv3 GENI SPI and the arbiter v7 observer channel); they are not board
 * facts and are defined in Tb710fuGeni.h / Tb710fuSpmi.h.
 */
#ifndef TB710FU_INPUT_HARDWARE_H
#define TB710FU_INPUT_HARDWARE_H

#include <stdint.h>

/* ---- Touch controller bus: sm8650.dtsi qupv3_id_0 / spi4 ----
 *
 *   spi4: spi@a90000 {
 *       compatible = "qcom,geni-spi";
 *       reg = <0 0x00a90000 0 0x4000>;
 *       interrupts = <GIC_SPI 357 IRQ_TYPE_LEVEL_HIGH 0>;
 *       clocks = <&gcc GCC_QUPV3_WRAP1_S4_CLK>;
 *       dmas = <&gpi_dma1 0 4 QCOM_GPI_SPI>, <&gpi_dma1 1 4 QCOM_GPI_SPI>;
 *   };
 *   qupv3_id_0: geniqup@ac0000 { reg = <0 0x00ac0000 0 0x2000>; ... }
 *   spi4 is a child of qupv3_id_0 and its pinctrl-0 is
 *   { qup_spi4_data_clk, qup_spi4_cs }.
 */
#define TB710FU_SE4_BASE            UINT64_C(0x00A90000)  /* sm8650.dtsi spi@a90000 */
#define TB710FU_SE4_BYTES           0x4000u
#define TB710FU_SE4_GIC_SPI         357u                  /* GIC_SPI 357 */
#define TB710FU_SE4_INTID           (TB710FU_SE4_GIC_SPI + 32u) /* XNU VT vector base */
#define TB710FU_QUP_WRAP1_BASE      UINT64_C(0x00AC0000)  /* sm8650.dtsi geniqup@ac0000 */
#define TB710FU_QUP_WRAP1_BYTES     0x2000u
#define TB710FU_SE_INDEX            4u                    /* spi4 = SE4 of wrapper 1 */

/* sm8650.dtsi qup_spi4_data_clk / qup_spi4_cs: function "qup1_se4", drive
 * strength 2 mA, bias disable. MISO 48, MOSI 49, CLK 50, CS 51. */
#define TB710FU_GPIO_SPI_MISO       48u
#define TB710FU_GPIO_SPI_MOSI       49u
#define TB710FU_GPIO_SPI_CLK        50u
#define TB710FU_GPIO_SPI_CS         51u
#define TB710FU_FUNC_QUP1_SE4       1u

/* sm8650-lenovo-tb710fu.dts touchscreen@0:
 *   compatible = "novatek,NVT-ts-spi";
 *   reg = <0>;
 *   spi-max-frequency = <9600000>;
 *   irq-gpios = <&tlmm 162 0x2001>;      (ACTIVE_LOW | PULL_UP)
 *   firmware-name = "novatek/novatek_nt36532_fw.bin";
 * The node declares no reset-gpios and no supply rails: reset and power are
 * owned by the display/PMIC side, so this driver drives neither.
 */
#define TB710FU_TOUCH_COMPATIBLE    "novatek,NVT-ts-spi"
#define TB710FU_TOUCH_SPI_HZ        9600000u              /* spi-max-frequency */
#define TB710FU_GPIO_TS_IRQ         162u                  /* irq-gpios <&tlmm 162 0x2001> */
#define TB710FU_TS_IRQ_FLAGS        0x2001u               /* ACTIVE_LOW | PULL_UP */
#define TB710FU_TS_CS               0u                    /* reg = <0> */

/* Display panel that the digitizer is bonded to, same board DTS:
 *   panel@0 { compatible = "lenovo,nt36532-wqhd"; reset-gpios = <&tlmm 133 0>; }
 * The DTS fixes the compatible and the shared reset line, not the pixel
 * geometry; 3200x2000 is the panel's declared mode (docs/HARDWARE.md) and is
 * the compiled default logical extent. The controller's own report is
 * clamped to it, and the tracking layer never emits a coordinate outside it.
 */
#define TB710FU_PANEL_COMPATIBLE    "lenovo,nt36532-wqhd"
#define TB710FU_GPIO_PANEL_RESET    133u                  /* panel@0 reset-gpios */
#define TB710FU_PANEL_WIDTH         3200u
#define TB710FU_PANEL_HEIGHT        2000u

/* ---- Clocks and power: sm8650.dtsi ----
 *
 *   clock-controller@100000 { compatible = "qcom,gcc-sm8650"; reg = <0 0x00100000 0 0x1f4200>; }
 *   spi4 clocks = <&gcc GCC_QUPV3_WRAP1_S4_CLK>
 *   spi4 power-domains = <&rpmhpd RPMHPD_CX>
 *
 * Only the window base is a DTS fact. The branch/RCG register offsets inside
 * GCC are not described by any pinned file, so this port does not write them:
 * it requires the SE to be already clocked by the firmware that handed the
 * device over (see Tb710fuInputProbe.cpp, stage 2 "clock present" check).
 */
#define TB710FU_GCC_BASE            UINT64_C(0x00100000)
#define TB710FU_GCC_BYTES           0x1F4200u

/* ---- Pin control: sm8650.dtsi pinctrl@f100000 ----
 *   reg = <0 0x0f100000 0 0x300000>; gpio-controller; #gpio-cells = <2>;
 *   gpio-ranges = <&tlmm 0 0 211>; interrupts = <GIC_SPI 208 ...>;
 * The pin function is already set by the firmware that hands the SE over
 * ("qup1_se4" on pins 48..51), so this port never maps or reprograms TLMM;
 * these constants are provenance only and no code reads them.
 */
#define TB710FU_TLMM_BASE           UINT64_C(0x0F100000)
#define TB710FU_TLMM_BYTES          0x300000u
#define TB710FU_TLMM_GPIO_COUNT     211u
#define TB710FU_TLMM_SUMMARY_GIC_SPI 208u

/* ---- PMIC keys ----
 *
 * sm8650-lenovo-tb710fu.dts:
 *   gpio-keys { key-volume-up { gpios = <&pm8550_gpios 6 GPIO_ACTIVE_LOW>;
 *                               debounce-interval = <15>; linux,code = KEY_VOLUMEUP; }; };
 *   &pon_pwrkey { status = "okay"; };                      (KEY_POWER from pmk8550.dtsi)
 *   &pon_resin  { linux,code = <KEY_VOLUMEDOWN>; status = "okay"; };
 *
 * pmk8550.dtsi: pon@1300 { reg = <0x1300>, <0x800>;
 *                          pon_pwrkey { interrupts-extended = <&pmk8550 0x13 7 EDGE_BOTH>; };
 *                          pon_resin  { interrupts-extended = <&pmk8550 0x13 6 EDGE_BOTH>; }; };  (SID 0)
 * pm8550.dtsi:  gpio@8800 { gpio-ranges = <&pm8550_gpios 0 0 13>; };  (SID 1)
 *
 * RT_STS bits come from pm8941-pwrkey.c and pinctrl-spmi-gpio.c (GEN3 KPDPWR
 * bit 7, GEN3 RESIN bit 6, one 0x100-byte block per PMIC GPIO, RT_STS at +0x10).
 */
#define TB710FU_SPMI_CORE_BASE      UINT64_C(0x0C400000)  /* spmi@c400000 reg[0] 0x3000 */
#define TB710FU_SPMI_CORE_BYTES     0x3000u
#define TB710FU_SPMI_OBS_BASE       UINT64_C(0x0C440000)  /* reg[2] 0x80000 */
#define TB710FU_SPMI_OBS_BYTES      0x80000u
#define TB710FU_SPMI_CNFG_BASE      UINT64_C(0x0C42D000)  /* reg[4] 0x4000 (ownership table) */
#define TB710FU_SPMI_CNFG_BYTES     0x4000u
#define TB710FU_SPMI_EE              0u                   /* the only EE this AP owns */

#define TB710FU_PMIC_SID_PMK8550    0u                   /* pmk8550.dtsi */
#define TB710FU_PMIC_SID_PM8550     1u                   /* pm8550.dtsi  */
#define TB710FU_PON_PID             0x13u                /* pon@1300 */
#define TB710FU_PON_RT_STS          0x10u                /* pm8941-pwrkey.c base+0x10 */
#define TB710FU_PON_GEN3_KPDPWR     (1u << 7)            /* PON_GEN3_KPDPWR_N_SET */
#define TB710FU_PON_GEN3_RESIN      (1u << 6)            /* PON_GEN3_RESIN_N_SET */
#define TB710FU_PM8550_GPIO_BASE_PID 0x88u               /* gpio@8800  */
#define TB710FU_PM8550_VOLUME_UP_GPIO 6u                /* &pm8550_gpios 6 */
/* pinctrl-spmi-gpio.c: pad address = base + (pin - 1) * 0x100, one PID per
 * 0x100 bytes, so GPIO6 lands on PID 0x88 + 5 = 0x8D. */
#define TB710FU_PM8550_GPIO_PID(gpio) \
    (TB710FU_PM8550_GPIO_BASE_PID + ((gpio) - 1u))
#define TB710FU_PM8550_VOLUP_PID    TB710FU_PM8550_GPIO_PID(TB710FU_PM8550_VOLUME_UP_GPIO)
#define TB710FU_PM_GPIO_RT_STS      0x10u                /* PMIC_MPP_REG_RT_STS */
#define TB710FU_PM_GPIO_RT_STS_LOW  0x01u                /* active-low pad: 0 = pressed */

#define TB710FU_KEY_DEBOUNCE_MS     15u                  /* volume-up debounce-interval */
#define TB710FU_KEY_POLL_MS         10u

/* ---- Pin-function read-back values (sm8650.dtsi pinctrl bindings) ---- */
#define TB710FU_TLMM_FUNC_MASK      0xFu
#define TB710FU_TLMM_FUNC_GPIO      0u
#define TB710FU_TLMM_FUNC_QUP1_SE4  1u

#endif /* TB710FU_INPUT_HARDWARE_H */
