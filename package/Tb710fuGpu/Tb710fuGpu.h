/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Entry points of the TB710FU GPU/Metal archive (milestone 1 + milestone 2).
 *
 * Nothing in this archive runs unless the kernel is booted with gpu-metal=1 or
 * gpu-power=<n>: the default boot path reads both arguments once, finds them
 * absent and returns, exactly as the board did before this archive existed.
 *
 * Metal stages (gpu-metal=1), unchanged from milestone 1:
 *   0  not asked
 *   1  base class probe started
 *   2  IOAccelerator metaclass resolved with the expected 136-byte extent
 *   3  accelerator object allocated and initialised
 *   4  attached to the platform expert
 *   5  registered (IOServiceMatching("IOAccelerator") can now find it)
 *
 * GpuPower stages (gpu-power=<n>), see docs/GPU_METAL_M2.md:
 *   1  census only (ADT + addon), no mapping and no write
 *   2  map + read-only probe of GCC/GPUCC/SMMU/GPU state
 *   3  CX GDSC on, GPUCC AHB + SMMU vote clocks on
 *   4  GPU SMMU global space inventory (read-only)
 *   5  GMU clock tree (GPLL0 votes, PLL1, HUB/GMU RCGs, GMU branches)
 *   6  the four gen70900 images verified by size and SHA-256
 *   7  GMU cold start (refused: needs its SMMU context and SM8650 RSC/PDC)
 *   8  GX power (refused: needs HFI startup votes from the AOP command DB)
 *   9  user client ABI published; ring stays 0 until 7 and 8 exist
 * A negative stage is never used; tb710fu_gpu_stats[TB710FU_GPU_STATUS] holds
 * the IOReturn of the step that stopped.
 */
#ifndef TB710FU_GPU_H
#define TB710FU_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TB710FU_GPU_STAGE = 0,          /* Metal stage chain, see above */
    TB710FU_GPU_STATUS = 1,         /* IOReturn of the step that stopped */
    TB710FU_GPU_REGISTERED = 2,     /* registerService() returned */
    TB710FU_GPU_BASE_SIZE = 3,      /* live IOAccelerator instance extent */
    TB710FU_GPU_OPENS = 4,          /* accelerator user-client requests */
    TB710FU_GPU_OPEN_TYPE = 5,      /* the last request's type */
    TB710FU_GPU_PROPERTY_READS = 6, /* registry property reads by Metal discovery */
    TB710FU_GPU_POWER_LEVEL = 7,    /* gpu-power as asked */
    TB710FU_GPU_ADDON_STAGE = 8,    /* native_gpu_addon_stage, gpu-addon mount */
    TB710FU_GPU_ADDON_STATUS = 9,   /* native_gpu_addon_status */
    TB710FU_GPU_BUNDLE = 10,        /* 1 when the addon carries the plugin */
    TB710FU_GPU_PUBLISH_MARKER = 11,/* 1 when the addon carries NativeAdrenoMetal.publish */
    TB710FU_GPU_FIRMWARE = 12,      /* gen70900 blobs found in the addon, 0..4 */
    TB710FU_GPU_POWER_STAGE = 13,   /* 0 off, 1 census read, 2 census published */
    TB710FU_GPU_MODEL = 14,         /* 1 when the ADT named an A750 node */
    TB710FU_GPU_BRINGUP = 15,       /* highest gpu-power stage reached */
    TB710FU_GPU_MMIO_MAPPED = 16,   /* bitmask: 1 gcc, 2 vote, 4 gpucc, 8 smmu, 16 gpu */
    TB710FU_GPU_CX = 17,            /* 1 when gpu_cc_cx_gdsc reads PWR_ON */
    TB710FU_GPU_CLOCKS = 18,        /* 1 when the SMMU HLOS vote clock runs */
    TB710FU_GPU_SMMU_ID = 19,       /* SMMU IDR2 as read */
    TB710FU_GPU_SMMU_SMRS = 20,     /* valid SMRs found in global space 0 */
    TB710FU_GPU_GMU_CLOCKS = 21,    /* GMU clock step reached, 0 on failure */
    TB710FU_GPU_FIRMWARE_VERIFIED = 22, /* images that matched size and digest, 0..4 */
    TB710FU_GPU_RING_READY = 23,    /* 0 in milestone 2; needs stages 7 and 8 */
    TB710FU_GPU_ABI = 24,           /* 1 when the user client ABI is published */
    TB710FU_GPU_CLIENTS = 25,       /* user clients created */
    TB710FU_GPU_BO_ALLOCS = 26,     /* BO allocations served */
    TB710FU_GPU_ABI_REFUSED = 27,   /* GPU-address methods refused for wants of the ring */
    TB710FU_GPU_LAST_METHOD = 28,   /* last external method selector */
    TB710FU_GPU_PRIORITY = 29,      /* foreground | background << 8 from SetPriority */
    TB710FU_GPU_STAT_COUNT = 32
};

/* Read-only view of what the boot addon carries for this GPU. */
typedef struct {
    uint32_t announced;   /* the NGPUADD2 container is present */
    uint32_t bundle;      /* GPUBundles/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal */
    uint32_t publish;     /* GPUBundles/NativeAdrenoMetal.publish */
    uint32_t firmware;    /* of gmu_gen70900.bin, gen70900_sqe.fw, gen70900_aqe.fw, gen70900_zap.mbn */
} Tb710fuGpuCensus;

extern volatile uint32_t tb710fu_gpu_stats[TB710FU_GPU_STAT_COUNT];

/* Resolve the prelinked IOAccelerator metaclass. Returns 1 when the extent is
 * the reviewed 136 bytes. Called before the module's metaclass window opens. */
int tb710fu_gpu_probe(void);

/* Install this archive's metaclasses inside the preModLoad window. Both are
 * no-ops when their prerequisites are absent, so a kernel booted without the
 * arguments registers nothing. The user client's metaclass is installed by
 * tb710fu_gpu_meta_init after the accelerator's. */
void tb710fu_gpu_meta_init(void);
void tb710fu_gpu_power_meta_init(void);

/* After postModLoad: the census, the staged bring-up, the optional power-domain
 * node and, with gpu-metal=1, the accelerator publication. */
void tb710fu_gpu_start(void);

/* 1 when either argument asks for work; also records the asked level. */
int tb710fu_gpu_wanted(void);

/* Read-only stage 1: the ADT's kgsl/gmu nodes, one line. */
void tb710fu_gpu_power_survey(void);

/* Read-only: the boot addon's GPU entries. */
void tb710fu_gpu_census_read(Tb710fuGpuCensus *census);

/* Stages 2..9, each gated by the asked level (see docs/GPU_METAL_M2.md). */
void tb710fu_gpu_bring_up(void);

/* gpu-power>=2: publish Tb710fuGpuPower with the census and bring-up facts. */
void tb710fu_gpu_power_publish(const Tb710fuGpuCensus *census);

/* 1 when stage 9 published the user client ABI; 1 when the CP ring is up (never
 * in milestone 2, which refuses stages 7 and 8 by design). */
int tb710fu_gpu_abi_ready(void);
int tb710fu_gpu_ring_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_GPU_H */
