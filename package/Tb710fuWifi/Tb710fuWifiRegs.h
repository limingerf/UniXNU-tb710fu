/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Every board address, bus number, GPIO, interrupt and regulator in this subsystem, with its exact provenance.
 *
 * The addresses and board wiring come ONLY from the TB710FU kernel sources:
 *   .local/input-sources/sm8650-lenovo-tb710fu.dts   (board: wcn7850-pmu, &pcie0, &pcieport0/wifi@0, pinmux)
 *   .local/input-sources/sm8650.dtsi                 (SoC: pcie@1c00000, phy@1c06000, gcc, tcsr, tlmm, smmu, its)
 * Nothing is taken from SM8550/sheng addresses: each value below names the node it was read from.
 *
 * Register *fields* (not addresses) that the DTS cannot express are from the pinned upstream Linux sources, all
 * GPL-2.0-only/BSD-3-Clause-Clear and used here only as field documentation (no code copied):
 *   drivers/clk/qcom/gcc-sm8650.c        PCIe GCC branches and GDSCs
 *   drivers/clk/qcom/gdsc.c              GDSC status bits
 *   drivers/clk/qcom/tcsrcc-sm8650.c     TCSR_PCIE_0_CLKREF_EN (0x31100 bit0)
 *   drivers/phy/qualcomm/phy-qcom-qmp-pcie.c
 *                                        "qcom,sm8650-qmp-gen3x2-pcie-phy" uses sm8550_qmp_gen3x2_pciephy_cfg
 *   drivers/pci/controller/dwc/pcie-qcom.c  PARF register offsets and fields
 *   drivers/net/wireless/ath/ath12k/{mhi.h,htc.h,wmi.h}
 *                                        MHI register/context layout, HTC frame and message ids, WMI TLV header
 * The WCN7850 chip-side power-up recipe and MHI context records follow UniXNU-sheng's hardware-verified
 * package/{WifiProbe.cpp,MhiBoot.c} (BSD-2-Clause-Patent); only the register map is reused.
 */
#ifndef TB710FU_WIFI_REGS_H
#define TB710FU_WIFI_REGS_H

#include <stdint.h>

/* ---- PCIe root complex: sm8650.dtsi pcie@1c00000 (compatible "qcom,pcie-sm8650","qcom,pcie-sm8550") ---- */
#define TB710FU_WIFI_PCIE_PARF_BASE   UINT64_C(0x01c00000) /* reg[0] name "parf" size 0x3000 */
#define TB710FU_WIFI_PCIE_PARF_BYTES  0x3000u
#define TB710FU_WIFI_PCIE_PHY_BASE    UINT64_C(0x01c06000) /* phy@1c06000 reg[0] size 0x2000 */
#define TB710FU_WIFI_PCIE_PHY_BYTES   0x2000u
#define TB710FU_WIFI_PCIE_DBI_BASE    UINT64_C(0x60000000) /* reg[1] name "dbi" size 0xf1d */
#define TB710FU_WIFI_PCIE_DBI_BYTES   0x1000u              /* one page is enough for the RC config header */
#define TB710FU_WIFI_PCIE_ELBI_BASE   UINT64_C(0x60000f20) /* reg[2] name "elbi" size 0xa8 */
#define TB710FU_WIFI_PCIE_ELBI_BYTES  0x100u
#define TB710FU_WIFI_PCIE_ATU_BASE    UINT64_C(0x60001000) /* reg[3] name "atu" size 0x1000 */
#define TB710FU_WIFI_PCIE_ATU_BYTES   0x1000u
#define TB710FU_WIFI_PCIE_CFG_BASE    UINT64_C(0x60100000) /* reg[4] name "config" size 0x100000 */
#define TB710FU_WIFI_PCIE_CFG_BYTES   0x1000u              /* only bus 1 device 0 function 0 is used */
#define TB710FU_WIFI_PCIE_MMIO_BASE   UINT64_C(0x60300000) /* ranges: MMIO window, length 0x3d00000 */
#define TB710FU_WIFI_PCIE_MMIO_BYTES  UINT64_C(0x3d00000)
#define TB710FU_WIFI_PCIE_NUM_LANES   2u                   /* num-lanes = <2> */

/* Endpoint: pcieport0 bus-range starts at 0x01; wifi@0 reg <0x10000 ...> is bus 1 device 0 function 0. */
#define TB710FU_WIFI_PCI_BUS          1u
#define TB710FU_WIFI_PCI_DEV          0u
#define TB710FU_WIFI_PCI_FN           0u
#define TB710FU_WIFI_PCI_VENDOR       0x17cbu /* compatible "pci17cb,1107" */
#define TB710FU_WIFI_PCI_DEVICE       0x1107u

/* SMMU/ITS stream ids: iommu-map and msi-map: bus 0 -> 0x1400, bus 1 (the endpoint) -> 0x1401. */
#define TB710FU_WIFI_SMMU_BASE        UINT64_C(0x15000000) /* apps_smmu iommu@15000000 size 0x100000 */
#define TB710FU_WIFI_SMMU_BYTES       0x100000u
#define TB710FU_WIFI_RC_SID           0x1400u
#define TB710FU_WIFI_ENDPOINT_SID     0x1401u
#define TB710FU_WIFI_ITS_BASE         UINT64_C(0x17140000) /* gic_its msi-controller@17140000 size 0x40000 */
#define TB710FU_WIFI_ITS_BYTES        0x40000u

/* MSI: interrupts = SPI 141..148 (msi0..7), SPI 140 (global); interrupt-map INTx 149..152.  Not attached in
 * this milestone: the endpoint writes its completions to a DMA sink that the driver polls. */
#define TB710FU_WIFI_MSI_FIRST_SPI    141u
#define TB710FU_WIFI_MSI_COUNT        8u
#define TB710FU_WIFI_MSI_GLOBAL_SPI   140u
#define TB710FU_WIFI_INTX_FIRST_SPI   149u

/* ---- Board power: sm8650-lenovo-tb710fu.dts wcn7850-pmu and &pcie0 ---- */
#define TB710FU_WIFI_GPIO_WLAN_EN     16u /* wcn7850-pmu wlan-enable-gpios <&tlmm 16 GPIO_ACTIVE_HIGH> */
#define TB710FU_WIFI_GPIO_BT_EN       17u /* wcn7850-pmu bt-enable-gpios  <&tlmm 17 GPIO_ACTIVE_HIGH> */
#define TB710FU_WIFI_GPIO_PERST       94u /* &pcie0 perst-gpios <&tlmm 94 GPIO_ACTIVE_LOW> */
#define TB710FU_WIFI_GPIO_CLKREQ      95u /* sm8650.dtsi pcie0_default_state clkreq-pins, function pcie0_clk_req_n */
#define TB710FU_WIFI_GPIO_WAKE        96u /* &pcie0 wake-gpios <&tlmm 96 GPIO_ACTIVE_HIGH> */

/* Pinmux from the board's own states:
 *   wlan_en:   function gpio, drive-strength 8,  bias-pull-down
 *   bt_default: function gpio, drive-strength 16, bias-disable
 *   pcie0:     perst function gpio drive 2 pull-down; wake function gpio drive 2 pull-up
 * TLMM_CFG layout (pinctrl-msm): pull 1:0 (2 = up, 1 = down), function 5:2, drive 8:6 (mA/2 - 1), OE 9.
 */
#define TB710FU_WIFI_TLMM_BASE        UINT64_C(0x0f100000) /* pinctrl@f100000 size 0x300000 */
#define TB710FU_WIFI_TLMM_BYTES       0x60000u             /* enough for GPIO 96 */
#define TB710FU_WIFI_TLMM_STRIDE      0x1000u
#define TB710FU_WIFI_TLMM_IO_OFFSET   0x4u                  /* 2 = driven high, 0 = driven low */

/* PMIC rails the board wires to the module (documented, not voted from here: RPMh owns them):
 *   wcn7850-pmu: vdd=vreg_s4i_0p3, vddio=vreg_l15b_1p8, vddio1p2=vreg_l3c_1p2, vddaon=vreg_s2c_0p5,
 *                vdddig=vreg_s3c_0p9, vddrfa1p2=vreg_s1c_1p2, vddrfa1p8=vreg_s6c_1p8,
 *                clocks = <&rpmhcc RPMH_RF_CLK1>
 *   wifi@0:      vddrfacmn=vreg_pmu_rfa_cmn, vddaon=vreg_pmu_aon_0p59, vddwlcx=vreg_pmu_wlcx_0p8,
 *                vddwlmx=vreg_pmu_wlmx_0p85, vddrfa0p8=vreg_pmu_rfa_0p8, vddrfa1p2=vreg_pmu_rfa_1p2,
 *                vddrfa1p8=vreg_pmu_rfa_1p8, vddpcie0p9=vreg_pmu_pcie_0p9, vddpcie1p8=vreg_pmu_pcie_1p8
 *   &pcie0_phy:  vdda-phy=vreg_l1i_0p88, vdda-pll=vreg_l3i_1p2
 */

/* ---- GCC: sm8650.dtsi clock-controller@100000 ("qcom,sm8650-gcc", reg 0x00100000 size 0x1f4200) ---- */
#define TB710FU_WIFI_GCC_BASE         UINT64_C(0x00100000)
#define TB710FU_WIFI_GCC_BYTES        0x80000u /* covers 0x6c000 and 0x52008 */
#define TB710FU_WIFI_GCC_VOTE0        0x52000u /* gcc-sm8650.c: enable_reg of the VOTE0 branches */
#define TB710FU_WIFI_GCC_VOTE8        0x52008u /* gcc-sm8650.c: enable_reg of the VOTE8 branches */
#define TB710FU_WIFI_GCC_COLLAPSE     0x5214cu /* collapse_ctrl of both PCIe GDSCs */
#define TB710FU_WIFI_GCC_PCIE0_GDSC   0x6b004u
#define TB710FU_WIFI_GCC_PCIE0_PHY_GDSC 0x6c000u
#define TB710FU_WIFI_GCC_GDSC_CFG_DELTA 0x4u   /* CFG_GDSCR sits at gdscr + 4 */

/* Halt registers (read-only state) and the vote bit each branch needs. */
#define TB710FU_WIFI_GCC_HALT_SLV      0x6b020u /* gcc_pcie_0_slv_axi_clk,      VOTE8 bit0 */
#define TB710FU_WIFI_GCC_HALT_MSTR     0x6b02cu /* gcc_pcie_0_mstr_axi_clk,     VOTE8 bit1 */
#define TB710FU_WIFI_GCC_HALT_CFG      0x6b038u /* gcc_pcie_0_cfg_ahb_clk,      VOTE8 bit2 */
#define TB710FU_WIFI_GCC_HALT_AUX      0x6b03cu /* gcc_pcie_0_aux_clk,          VOTE8 bit3 */
#define TB710FU_WIFI_GCC_HALT_PIPE     0x6b048u /* gcc_pcie_0_pipe_clk,         VOTE8 bit4 */
#define TB710FU_WIFI_GCC_HALT_Q2A      0x6b01cu /* gcc_pcie_0_slv_q2a_axi_clk,  VOTE8 bit5 */
#define TB710FU_WIFI_GCC_HALT_CNOC_SF  0x10058u /* gcc_cnoc_pcie_sf_axi_clk,    VOTE8 bit6 */
#define TB710FU_WIFI_GCC_HALT_QMIP     0x6b018u /* gcc_qmip_pcie_ahb_clk,       VOTE0 bit11 */
#define TB710FU_WIFI_GCC_HALT_AGGRE    0x10064u /* gcc_aggre_noc_pcie_axi_clk,  VOTE0 bit12 */
#define TB710FU_WIFI_GCC_HALT_DDRSS    0x10074u /* gcc_ddrss_pcie_sf_qtb_clk,   VOTE0 bit19 */
#define TB710FU_WIFI_GCC_HALT_CFG_NOC  0x10050u /* gcc_cfg_noc_pcie_anoc_ahb_clk, VOTE0 bit20 */
#define TB710FU_WIFI_GCC_HALT_RCHNG    0x6b054u /* gcc_pcie_0_phy_rchng_clk,    VOTE0 bit22 */
#define TB710FU_WIFI_GCC_RCG_AUX       0x6b074u /* gcc_pcie_0_aux_clk_src */
#define TB710FU_WIFI_GCC_RCG_RCHNG     0x6b058u /* gcc_pcie_0_phy_rchng_clk_src, 100 MHz from GPLL0_EVEN/3 */
#define TB710FU_WIFI_GCC_PIPE_MUX      0x6b070u /* gcc_pcie_0_pipe_clk_src */
/* gcc-sm8650.c reset map: [GCC_PCIE_0_BCR] = 0x6b000, [GCC_PCIE_0_PHY_BCR] = 0x6c01c; bit0 asserts the reset. */
#define TB710FU_WIFI_GCC_PCIE0_BCR     0x6b000u
#define TB710FU_WIFI_GCC_PCIE0_PHY_BCR 0x6c01cu
/* gcc-sm8650.c collapse_ctrl 0x5214c: bit0 = PCIE_0_GDSC, bit3 = PCIE_0_PHY_GDSC. */
#define TB710FU_WIFI_GCC_PCIE0_COLLAPSE_BIT    0u
#define TB710FU_WIFI_GCC_PCIE0PHY_COLLAPSE_BIT 3u

/* ---- TCSR: sm8650.dtsi tcsr@1fc0000 size 0xa0000; tcsrcc-sm8650.c PCIE_0 CLKREF at +0x31100 bit0 ---- */
#define TB710FU_WIFI_TCSR_BASE        UINT64_C(0x01fc0000)
#define TB710FU_WIFI_TCSR_BYTES       0xa0000u
#define TB710FU_WIFI_TCSR_CLKREF_OFF  0x31100u
#define TB710FU_WIFI_TCSR_CLKREF_EN   0x1u

/* ---- PARF register offsets and fields: pcie-qcom.c (identical for qcom,pcie-sm8550 fallback) ---- */
#define TB710FU_WIFI_PARF_SYS_CTRL    0x000u
#define TB710FU_WIFI_PARF_PM_CTRL     0x020u
#define TB710FU_WIFI_PARF_PHY_CTRL    0x040u
#define TB710FU_WIFI_PARF_PHY_REFCLK  0x04cu
#define TB710FU_WIFI_PARF_CONFIG_BITS 0x050u
#define TB710FU_WIFI_PARF_MHI_CLOCK_RESET_CTRL 0x174u
#define TB710FU_WIFI_PARF_AXI_MSTR_WR_ADDR_HALT_V2 0x1a8u
#define TB710FU_WIFI_PARF_Q2A_FLUSH   0x1acu
#define TB710FU_WIFI_PARF_LTSSM       0x1b0u
#define TB710FU_WIFI_PARF_DBI_BASE_V2 0x350u
#define TB710FU_WIFI_PARF_DBI_BASE_V2_HI 0x354u
#define TB710FU_WIFI_PARF_SLV_SPACE_V2 0x358u
#define TB710FU_WIFI_PARF_SLV_SPACE_V2_HI 0x35cu
#define TB710FU_WIFI_PARF_ATU_BASE    0x634u
#define TB710FU_WIFI_PARF_ATU_BASE_HI 0x638u
#define TB710FU_WIFI_PARF_DEVICE_TYPE 0x1000u
#define TB710FU_WIFI_PARF_BDF_TO_SID  0x2000u
#define TB710FU_WIFI_PARF_BDF_TO_SID_CFG 0x2c00u
#define TB710FU_WIFI_PARF_DEVICE_TYPE_RC 0x4u

#define TB710FU_WIFI_PARF_SYS_CTRL_MSTR_ACLK_CGC_DIS (1u << 10)
#define TB710FU_WIFI_PARF_SYS_CTRL_SLV_ACLK_CGC_DIS  (1u << 9)
#define TB710FU_WIFI_PARF_SYS_CTRL_CORE_CLK_CGC_DIS  (1u << 6)
#define TB710FU_WIFI_PARF_SYS_CTRL_AUX_PWR_DET       (1u << 4)
#define TB710FU_WIFI_PARF_SYS_CTRL_L23_CLK_RMV_DIS   (1u << 2)
#define TB710FU_WIFI_PARF_SYS_CTRL_L1_CLK_RMV_DIS    (1u << 1)
#define TB710FU_WIFI_PARF_MHI_AHB_CLK_EN     (1u << 0)
#define TB710FU_WIFI_PARF_MHI_MSTR_AXI_CLK_EN (1u << 1)
#define TB710FU_WIFI_PARF_LTSSM_EN           (1u << 8)
#define TB710FU_WIFI_PARF_LTSSM_STATE_MASK   0x3fu
#define TB710FU_WIFI_PARF_PHY_TEST_PWR_DOWN  (1u << 0)
#define TB710FU_WIFI_PARF_PHY_REFCLK_USE_PAD (1u << 12)
#define TB710FU_WIFI_PARF_BDF_TO_SID_BYPASS  (1u << 0)

/* ---- DBI (root port config header) offsets ---- */
#define TB710FU_WIFI_PCI_COMMAND      0x004u
#define TB710FU_WIFI_PCI_BUS_NUMBERS  0x018u
#define TB710FU_WIFI_PCI_BAR0         0x010u
#define TB710FU_WIFI_PCI_CAP_PTR      0x034u
#define TB710FU_WIFI_PCI_LINK_CONTROL 0x080u
#define TB710FU_WIFI_PCI_PORT_DEBUG0  0x728u
#define TB710FU_WIFI_PCI_PORT_DEBUG1  0x72cu
#define TB710FU_WIFI_PCI_COMMAND_MEM_EN   (1u << 1)
#define TB710FU_WIFI_PCI_COMMAND_BUS_MASTER (1u << 2)

/* ---- iATU (unrolled, region n at ATU + 0x200*n), pcie-designware layout ---- */
#define TB710FU_WIFI_ATU_REGION_STRIDE 0x200u
#define TB710FU_WIFI_ATU_CTRL1         0x00u
#define TB710FU_WIFI_ATU_CTRL2         0x04u
#define TB710FU_WIFI_ATU_LOWER_BASE    0x08u
#define TB710FU_WIFI_ATU_UPPER_BASE    0x0cu
#define TB710FU_WIFI_ATU_LOWER_LIMIT   0x10u
#define TB710FU_WIFI_ATU_UPPER_LIMIT   0x20u
#define TB710FU_WIFI_ATU_LOWER_TARGET  0x14u
#define TB710FU_WIFI_ATU_UPPER_TARGET  0x18u
#define TB710FU_WIFI_ATU_CTRL1_CFG0    0x4u  /* type 4: config type 0 */
#define TB710FU_WIFI_ATU_CTRL1_MEM     0x0u  /* type 0: memory */
#define TB710FU_WIFI_ATU_CTRL2_ENABLE  (1u << 31)

/* ---- MHI registers (BAR-relative, MHI 1.0 + ath12k mhi.h), from MhiBoot.h ---- */
#define TB710FU_WIFI_MHI_REGLEN        0x00u
#define TB710FU_WIFI_MHI_CFG           0x10u
#define TB710FU_WIFI_MHI_ERDB          0x20u
#define TB710FU_WIFI_MHI_BHIOFF        0x28u
#define TB710FU_WIFI_MHI_BHIEOFF       0x2cu
#define TB710FU_WIFI_MHI_CTRL          0x38u
#define TB710FU_WIFI_MHI_STATUS        0x48u
#define TB710FU_WIFI_MHI_CTX_CH_BASE   0x58u /* CCABAP */
#define TB710FU_WIFI_MHI_CTX_EV_BASE   0x60u /* ECABAP */
#define TB710FU_WIFI_MHI_CTX_CMD_BASE  0x68u /* CRCBAP */
#define TB710FU_WIFI_MHI_CTRL_BASE     0x80u
#define TB710FU_WIFI_MHI_CTRL_LIMIT    0x88u
#define TB710FU_WIFI_MHI_DATA_BASE     0x98u
#define TB710FU_WIFI_MHI_DATA_LIMIT    0xa0u
#define TB710FU_WIFI_MHI_CTRL_RESET    (1u << 1)
#define TB710FU_WIFI_MHI_STATUS_READY  (1u << 0)
#define TB710FU_WIFI_MHI_STATUS_SYSERR (1u << 2)
#define TB710FU_WIFI_MHI_STATE_SHIFT   8u
#define TB710FU_WIFI_MHI_CFG_NER_SHIFT 16u
#define TB710FU_WIFI_MHI_CFG_NER_MASK  0xffu

#define TB710FU_WIFI_MHI_BHI_IMGADDR_LOW  0x08u
#define TB710FU_WIFI_MHI_BHI_IMGADDR_HIGH 0x0cu
#define TB710FU_WIFI_MHI_BHI_IMGSIZE      0x10u
#define TB710FU_WIFI_MHI_BHI_IMGTXDB      0x18u
#define TB710FU_WIFI_MHI_BHI_INTVEC       0x20u
#define TB710FU_WIFI_MHI_BHI_EXECENV      0x28u
#define TB710FU_WIFI_MHI_BHI_STATUS       0x2cu
#define TB710FU_WIFI_MHI_BHI_ERRCODE      0x30u
#define TB710FU_WIFI_MHI_BHIE_TXVECADDR_LOW  0x2cu
#define TB710FU_WIFI_MHI_BHIE_TXVECADDR_HIGH 0x30u
#define TB710FU_WIFI_MHI_BHIE_TXVECSIZE      0x34u
#define TB710FU_WIFI_MHI_BHIE_TXVECDB        0x3cu
#define TB710FU_WIFI_MHI_BHIE_TXVECSTATUS    0x44u
#define TB710FU_WIFI_MHI_SEQ_MASK      0x3fffffffu
#define TB710FU_WIFI_MHI_XFER_SUCCESS  2u
#define TB710FU_WIFI_MHI_XFER_ERROR    3u

/* MHI context record (44 bytes) + ring element (16 bytes); ath12k mhi.h and sheng WifiProbe.cpp. */
#define TB710FU_WIFI_MHI_CHANNELS     128u
#define TB710FU_WIFI_MHI_CTX_BYTES    44u
#define TB710FU_WIFI_MHI_EVENTS       2u
#define TB710FU_WIFI_MHI_ELEM_BYTES   16u
#define TB710FU_WIFI_MHI_EV0_ELEMS    32u
#define TB710FU_WIFI_MHI_EV1_ELEMS    256u
#define TB710FU_WIFI_MHI_CMD_ELEMS    8u
#define TB710FU_WIFI_MHI_CTX_MODERATION 0u
#define TB710FU_WIFI_MHI_CTX_TYPE      4u
#define TB710FU_WIFI_MHI_CTX_MSIVEC    8u
#define TB710FU_WIFI_MHI_CTX_BASE      12u
#define TB710FU_WIFI_MHI_CTX_LEN       20u
#define TB710FU_WIFI_MHI_CTX_RP        28u
#define TB710FU_WIFI_MHI_CTX_WP        36u
#define TB710FU_WIFI_MHI_ER_TYPE_VALID 1u

/* ---- WCN7850 chip registers through BAR0 (ath12k_pci_power_up; verified by sheng WifiProbe.cpp) ---- */
#define TB710FU_WIFI_CHIP_WINDOW_REG   0x310cu /* window select: 0x40000000 | ((off >> 19) & 0x3f) */
#define TB710FU_WIFI_CHIP_WINDOW_BASE  0x80000u
#define TB710FU_WIFI_CHIP_MHI_BASE     0x1e0e100u
#define TB710FU_WIFI_CHIP_LTSSM_RESET  0x1e081b0u
#define TB710FU_WIFI_CHIP_HOT_RESET    0x1e40304u
#define TB710FU_WIFI_CHIP_IRQ_CLR      0x1e08228u
#define TB710FU_WIFI_CHIP_WARM_CTRL    0x1f8031cu
#define TB710FU_WIFI_CHIP_Q6_COOKIE    0x1f80500u
#define TB710FU_WIFI_CHIP_WARM_ENTRY   0x1f80504u
#define TB710FU_WIFI_CHIP_RESET_CAUSE  0x1f8060cu
#define TB710FU_WIFI_CHIP_GLOBAL_RESET 0x3008u
#define TB710FU_WIFI_CHIP_LTSSM_VALUE  0x111u

/* ---- Firmware, from .local/input-sources/firmware/MD5SUMS and the shipped tarball ---- */
#define TB710FU_WIFI_FW_DIR           "/private/var/firmware/ath12k/WCN7850/hw2.0"
#define TB710FU_WIFI_FW_ADDON_PREFIX  "EFI/UniXNU/firmware/Wifi/"
#define TB710FU_WIFI_FW_AMSS_NAME     "amss.bin"
#define TB710FU_WIFI_FW_M3_NAME       "m3.bin"
#define TB710FU_WIFI_FW_BOARD_NAME    "board-2.bin"
#define TB710FU_WIFI_FW_AMSS_BYTES    6090816u  /* stock image shipped in tb710fu-firmware-20261005.tar.gz */
#define TB710FU_WIFI_FW_M3_BYTES      299660u
#define TB710FU_WIFI_FW_BOARD_BYTES   2253964u
#define TB710FU_WIFI_FW_AMSS_MAX      (16u << 20)
#define TB710FU_WIFI_FW_M3_MAX        (4u << 20)
#define TB710FU_WIFI_FW_BOARD_MAX     (8u << 20)
#define TB710FU_WIFI_FW_SEGMENT       (512u << 10) /* BHI SBL = first 512 KiB, BHIe segments of the same size */
#define TB710FU_WIFI_FW_MAX_SEGMENTS  32u

#endif /* TB710FU_WIFI_REGS_H */
