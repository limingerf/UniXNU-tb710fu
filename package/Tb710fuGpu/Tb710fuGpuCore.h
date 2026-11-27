/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Milestone 3 GPU core: SM8650 (Adreno 750 / gen70900) GMU cold start, GX
 * power, CP SQE/AQE + RB0, and the live BO/submit/fence services behind the
 * NativeAdrenoABI v2 user client.
 *
 * The sequencing and every register value come from the mainline Linux
 * a6xx/a7xx driver on disk (a6xx_gmu.c a6xx_gmu_fw_start/a6xx_gmu_start/
 * a6xx_gmu_rpmh_init/a6xx_hfi_start, a6xx_hfi.c, a6xx_gpu.c a7xx_cp_init,
 * a6xx.xml/a6xx_gmu.xml), the hardware-proven UniXNU ArmSmmu4K kit, and the
 * Adreno740 macOS driver's ring/fence/CP-init. Where the board's exact value is
 * not pinned (RSC SEQ_MEM layout, HFI perf-table votes) the code probes or
 * falls back and reports that in the stage line instead of inventing success.
 */
#ifndef TB710FU_GPU_CORE_H
#define TB710FU_GPU_CORE_H
#include "Tb710fuGpuMmio.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

struct Tb710fuGpuCoreWindows {
    Tb710fuGpuWindow *gpu;    /* 0x03d00000, 0x40000 (CX-domain registers are safe before GX) */
    Tb710fuGpuWindow *gmu;    /* 0x03d68000, 0x37000 */
    Tb710fuGpuWindow *rscc;   /* 0x03d50000, 0x10000 */
    Tb710fuGpuWindow *pdc;    /* 0x0b280000, 0x10000 */
    Tb710fuGpuWindow *smmu;   /* 0x03da0000, 0x40000 */
    Tb710fuGpuWindow *cxMisc; /* 0x03d9e000, 0x2000 */
};

struct Tb710fuGpuCoreReport {
    uint32_t stage7Step;      /* last completed stage-7 step (0..12) */
    uint32_t stage8Step;      /* last completed stage-8 step (0..9) */
    uint32_t gmuBoot;         /* CM3_FW_INIT_RESULT after the release */
    uint32_t gmuVersion;      /* DTCM+0xff8 firmware version */
    uint32_t gmuChipId;       /* GENERAL_10 read-back */
    uint32_t hfiSeq;          /* last HFI sequence number sent */
    uint32_t hfiError;        /* firmware error packet payload (0 = none) */
    uint32_t gxPower;         /* 1 when CX_GMU_RPMH_POWER_STATE reached 0xf */
    uint32_t gxPowerState;    /* the last value read from that register */
    uint32_t rbbmStatus;      /* RBBM_STATUS after GX on */
    uint32_t cpStep;          /* 1 SQE/AQE loaded, 2 ring programmed, 3 fence seen */
    uint32_t fenceValue;      /* the fence word the GPU wrote */
    uint32_t ringReady;       /* 1 only after a real CP fence advanced in RAM */
    uint32_t chipId;          /* ADRENO chip id from the pin/board */
    uint32_t gmemBytes;
    uint64_t gmemBase;
    uint32_t smmuId0, smmuId1, smmuId2;
    uint32_t smmuTables;
    uint32_t smmuFault;
    uint32_t boMapped;
    uint32_t submitCount;
    uint32_t lastFence;
    uint32_t reserved;
};

struct Tb710fuGpuCoreDeviceInfo {
    uint32_t chipId;
    uint32_t gmemBytes;
    uint64_t gmemBase;
    uint64_t ucheTrapBase;
    uint64_t vaStart;
    uint64_t vaSize;
    uint32_t highestBankBit;
    uint32_t ubwcSwizzle;
    uint32_t macrotileMode;
    uint32_t ubwcVersion;
    uint32_t flags;
};

/* Runs stages 7..8 as far as `level` allows (7 = cold start, 8/9 = GX + ring).
 * Fills the report even on failure. 0 on success, a negative step otherwise. */
int tb710fu_gpu_core_start(const Tb710fuGpuCoreWindows *windows, uint32_t level,
                           Tb710fuGpuCoreReport *report);

/* Whether the CP ring passed the hardware fence self-test. */
bool tb710fu_gpu_core_ring_ready(void);

/* Maps/unmaps one BO's pages in GPU SMMU bank 0 at the caller's iova. */
int tb710fu_gpu_core_map_bo(IOMemoryDescriptor *memory, uint64_t iova, uint64_t bytes,
                            uint32_t permissions, uint32_t writeback, uint32_t shareability);
int tb710fu_gpu_core_unmap_bo(uint64_t iova, uint64_t bytes);

/* Ring submission: one CP_INDIRECT_BUFFER per (iova,bytes), then a fence. The
 * buffers must already be mapped in bank 0. Returns the fence sequence in
 * *fence (nonzero) and waits for any earlier `waitFence` first. */
int tb710fu_gpu_core_submit(const uint64_t *iovas, const uint64_t *bytes, uint32_t count,
                            uint64_t waitFence, uint64_t timeoutNs, uint64_t *fence);
int tb710fu_gpu_core_wait_fence(uint64_t fence, uint64_t timeoutNs, uint32_t *observed);
uint64_t tb710fu_gpu_core_timestamp(void);
void tb710fu_gpu_core_device_info(Tb710fuGpuCoreDeviceInfo *info);

#endif /* TB710FU_GPU_CORE_H */
