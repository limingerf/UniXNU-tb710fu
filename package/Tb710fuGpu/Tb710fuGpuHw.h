/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8650 (Adreno 750 / Gen70900) board facts for the staged gpu-power= chain.
 *
 * Every number below names its source. The live ADT is authoritative for the
 * TB710FU windows (docs/GPU_REFERENCE_A750.md records the dump); the mainline
 * sm8650.dtsi and gpucc-sm8650.c/gcc-sm8650.c agree on the register layout.
 *
 *   GPU        0x03d00000 + 0x40000  ADT qcom,kgsl-3d0 (kgsl_3d0_reg_memory)
 *   cx_dbgc    0x03d61000 + 0x3000   ADT qcom,kgsl-3d0 (cx_dbgc)
 *   cx_misc    0x03d9e000 + 0x2000   ADT qcom,kgsl-3d0 (cx_misc)
 *   GMU        0x03d68000 + 0x37000  ADT qcom,gen7-gmu (gmu); zorn K80 table agrees
 *   GMU PDC    0x0b280000 + 0x10000  ADT qcom,gen7-gmu (gmu_pdc)
 *   GPUCC      0x03d90000 + 0x0a000  mainline sm8650.dtsi clock-controller@3d90000
 *   GPU SMMU   0x03da0000 + 0x40000  ADT qcom,kgsl-smmu@3da0000 (iommu@3da0000)
 *   GCC        0x00100000            sm8650.dtsi clock-controller@100000
 *
 * GPUCC register offsets are gpucc-sm8650.c (gdsc/clock tables); the GCC branch
 * offsets are gcc-sm8650.c. GDSCR bit positions are gdsc.c, RCG fields are
 * clk-rcg2.c (the "half divider" encoding: field = 2*divisor - 1). SMMU global
 * space offsets are the SMMUv2 architecture (the UFS probe reads the same).
 *
 * The windows the chain may map, in order: GCC GPU page and GCC vote page are
 * always safe; GPUCC needs gcc_gpu_cfg_ahb_clk enabled; the GPU SMMU needs the
 * CX domain and its HLOS vote clock; the GPU window needs GX, which the GMU owns
 * and this milestone never powers. Nothing here is read while its clock is off.
 */
#ifndef TB710FU_GPU_HW_H
#define TB710FU_GPU_HW_H
#include <stdint.h>

enum {
    /* gpu-power= levels. Stage N is the level whose work N brought up. */
    Tb710fuGpuStageCensus = 1,      /* ADT + addon census (read-only) */
    Tb710fuGpuStageMmio = 2,        /* map + read-only probe of the clock/power state */
    Tb710fuGpuStageCx = 3,          /* CX GDSC on, GPUCC AHB + SMMU vote clocks on */
    Tb710fuGpuStageSmmu = 4,        /* GPU SMMU global space inventory (read-only) */
    Tb710fuGpuStageGmuClocks = 5,   /* GPLL0 votes, PLL1, HUB/GMU RCGs, GMU branches */
    Tb710fuGpuStageFirmware = 6,    /* the four gen70900 images verified by SHA-256 */
    Tb710fuGpuStageColdStart = 7,   /* GMU release + HFI ready (needs its SMMU context) */
    Tb710fuGpuStageGx = 8,          /* GX on via HFI votes (needs the AOP command DB) */
    Tb710fuGpuStageAbi = 9          /* user client ABI published; ring still needs 7+8 */
};

/* Windows. */
#define TBGPU_GCC_BASE          0x00100000ull
#define TBGPU_GCC_GPU_PAGE      0x00171000ull   /* gcc-sm8650.c GPU branches live here */
#define TBGPU_GCC_VOTE_PAGE     0x00152000ull   /* gcc_gpu_gpll0_clk_src vote, bits 15/16 */
#define TBGPU_GCC_PAGE_BYTES    0x1000u
#define TBGPU_GPU_BASE          0x03d00000ull
#define TBGPU_GPU_BYTES         0x40000ull
#define TBGPU_GPU_PROBE_BYTES   0x1000u
#define TBGPU_GPUCC_BASE        0x03d90000ull
#define TBGPU_GPUCC_BYTES       0x0a000ull
#define TBGPU_SMMU_BASE         0x03da0000ull
#define TBGPU_SMMU_GR0_BYTES    0x1000u          /* global space 0 only (see docs) */
#define TBGPU_GMU_BASE          0x03d68000ull
#define TBGPU_GMU_BYTES         0x37000ull
#define TBGPU_GMU_PDC_BASE      0x0b280000ull
#define TBGPU_GMU_PDC_BYTES     0x10000ull
#define TBGPU_CX_MISC_BASE      0x03d9e000ull
#define TBGPU_CX_MISC_BYTES     0x2000ull
#define TBGPU_CX_DBGC_BASE      0x03d61000ull
#define TBGPU_CX_DBGC_BYTES     0x3000ull

/* gcc-sm8650.c, relative to the 0x71000 page. */
#define TBGPU_GCC_GPU_CFG_AHB        0x004
#define TBGPU_GCC_GPU_QMIP_AHB       0x008
#define TBGPU_GCC_GPU_MEMNOC_GFX     0x010
#define TBGPU_GCC_GPU_SNOC_DVM_GFX   0x018
#define TBGPU_GCC_DDRSS_GPU_AXI      0x154   /* same page; not written (see Power.cpp) */
/* gcc-sm8650.c gcc_gpu_gpll0_clk_src / _div_clk_src, relative to the 0x52000 page. */
#define TBGPU_GCC_PLL_VOTE           0x000
#define TBGPU_GPLL0_VOTES            ((1u << 15) | (1u << 16))

/* gpucc-sm8650.c. */
#define TBGPU_CC_CX_GDSCR            0x9108
#define TBGPU_CC_CX_GDS_HW_CTRL      0x9168   /* SM8650 differs from SM8550's 0x953c */
#define TBGPU_CC_GX_GDSCR            0x905c
#define TBGPU_CC_GX_CLAMP_IO         0x9504
#define TBGPU_CC_AHB_CLK             0x911c
#define TBGPU_CC_CX_GMU_CLK          0x913c
#define TBGPU_CC_CXO_CLK             0x9144
#define TBGPU_CC_CXO_AON_CLK         0x9004
#define TBGPU_CC_DEMET_CLK           0x900c
#define TBGPU_CC_HUB_CX_INT_CLK      0x9148
#define TBGPU_CC_SMMU_VOTE_CLK       0x7000   /* gpu_cc_hlos1_vote_gpu_smmu_clk */
#define TBGPU_CC_GMU_RCGR            0x9318   /* gpu_cc_gmu_clk_src, CFG at +4 */
#define TBGPU_CC_HUB_RCGR            0x93ec   /* gpu_cc_hub_clk_src, CFG at +4 */
#define TBGPU_CC_PLL0                0x0000
#define TBGPU_CC_PLL1                0x1000

/* gdsc.c bit positions (PWR_ON_MASK, SW_COLLAPSE_MASK, RETAIN_*). */
#define TBGPU_GDSC_PWR_ON            (1u << 31)
#define TBGPU_GDSC_SW_COLLAPSE       (1u << 0)
#define TBGPU_GDSC_RETAIN_MEM        (1u << 14)
#define TBGPU_GDSC_RETAIN_PERIPH     (1u << 13)
#define TBGPU_GDSC_RETAIN_FF         (1u << 11)
#define TBGPU_GDSC_POLL_US           2000u

/* clk-branch.c: a branch with halt_check reads CLK_OFF=1 until it runs; a voted
 * branch without hardware gating keeps that meaning (the UFS probe relies on it). */
#define TBGPU_CBCR_CLK_OFF           (1u << 31)
#define TBGPU_CBCR_ENABLE            (1u << 0)

/* clk-alpha-pll.c / gpucc-sm8650.c lucid_ole registers. */
#define TBGPU_PLL_MODE               0x00
#define TBGPU_PLL_OPMODE             0x04
#define TBGPU_PLL_L_VAL              0x10
#define TBGPU_PLL_ALPHA              0x14
#define TBGPU_PLL_USER_CTL           0x18
#define TBGPU_PLL_USER_CTL_HI        0x1c
#define TBGPU_PLL_CONFIG_CTL         0x20
#define TBGPU_PLL_CONFIG_CTL_HI      0x24
#define TBGPU_PLL_CONFIG_CTL_HI1     0x28
#define TBGPU_PLL_TEST_CTL           0x2c
#define TBGPU_PLL_TEST_CTL_HI        0x30
#define TBGPU_PLL_TEST_CTL_HI1       0x34
#define TBGPU_PLL_TEST_CTL_HI2       0x38
#define TBGPU_PLL_LOCK_DET           (1u << 31)
#define TBGPU_PLL_RESET_N            (1u << 2)
#define TBGPU_PLL_OUT_CTRL           (1u << 0)
#define TBGPU_PLL_OUT_MASK           0x7u
#define TBGPU_PLL_RUN                1u

/* The SM8650 lucid_ole configurations from gpucc-sm8650.c: PLL0 l=0x20/alpha=0x4aaa,
 * PLL1 l=0x1b/alpha=0x1555. The remaining words are the same for both PLLs. */
#define TBGPU_PLL0_L                 0x20u
#define TBGPU_PLL0_ALPHA             0x4aaau
#define TBGPU_PLL1_L                 0x1bu
#define TBGPU_PLL1_ALPHA             0x1555u

/* clk-rcg2.c: CMD_UPDATE/CMD_ROOT_EN/CMD_ROOT_OFF, CFG fields, and the half divider. */
#define TBGPU_RCG_UPDATE             (1u << 0)
#define TBGPU_RCG_ROOT_EN            (1u << 1)
#define TBGPU_RCG_ROOT_OFF           (1u << 31)
#define TBGPU_RCG_CFG_CLEAR          (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))
#define TBGPU_RCG_FIELD(divisor)     ((2u * (divisor)) - 1u)

/* gpucc-sm8650.c parent maps: hub 200 MHz = GPLL0 sel 5 div 3;
 * gmu 260 MHz = PLL1 sel 3 div 2 (ftbl_gpu_cc_gmu_clk_src). */
#define TBGPU_HUB_SEL                5u
#define TBGPU_HUB_DIV                3u
#define TBGPU_GMU_SEL                3u
#define TBGPU_GMU_DIV                2u

/* SMMUv2 global space 0 (SMMU_GR0). */
#define TBGPU_SMMU_SCR0              0x000
#define TBGPU_SMMU_ID0               0x020
#define TBGPU_SMMU_ID1               0x024
#define TBGPU_SMMU_ID2               0x028
#define TBGPU_SMMU_GFSR              0x048
#define TBGPU_SMMU_SMR(n)            (0x800u + 4u * (n))
#define TBGPU_SMMU_S2CR(n)           (0xc00u + 4u * (n))
#define TBGPU_SMMU_MAX_SMR           128u

/* a6xx.xml RBBM_STATUS dword 0x0210; register accessors take byte offsets. */
#define TBGPU_RBBM_STATUS_BYTES      (0x0210u * 4u)

/* The Adreno 750 (Gen70900) chip id the ADT reports (docs/GPU_REFERENCE_A750.md, live dump). */
#define TBGPU_CHIPID_A750            0x43051401u

static inline int tb710fu_gpu_clock_on(uint32_t cbcr) { return (cbcr & TBGPU_CBCR_CLK_OFF) == 0; }
static inline int tb710fu_gpu_branch_enabled(uint32_t cbcr) { return (cbcr & TBGPU_CBCR_ENABLE) != 0; }
static inline int tb710fu_gpu_gdsc_on(uint32_t gdscr) { return (gdscr & TBGPU_GDSC_PWR_ON) != 0; }
static inline unsigned tb710fu_gpu_num_smr(uint32_t id0) { return id0 & 0xffu; }   /* IDR0.NUMSMRG */
static inline unsigned tb710fu_gpu_num_cb(uint32_t id1) { return id1 & 0xffu; }    /* IDR1.NUMCB */
static inline unsigned tb710fu_gpu_oas(uint32_t id2) { return (id2 >> 6) & 0x1fu; }/* IDR2.OAS */

#endif /* TB710FU_GPU_HW_H */
