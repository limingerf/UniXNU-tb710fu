/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Entry points of the TB710FU GPU/Metal milestone-1 archive.
 *
 * Nothing in this archive runs unless the kernel is booted with gpu-metal=1 or
 * gpu-power=<n>: the default boot path reads both arguments once, finds them
 * absent and returns, exactly as the board did before this archive existed.
 *
 * Stages (Metal):
 *   0  not asked
 *   1  base class probe started
 *   2  IOAccelerator metaclass resolved with the expected 136-byte extent
 *   3  accelerator object allocated and initialised
 *   4  attached to the platform expert
 *   5  registered (IOServiceMatching("IOAccelerator") can now find it)
 * A negative stage is never used; tb710fu_gpu_stats[TB710FU_GPU_STATUS] holds
 * the IOReturn of the step that stopped.
 *
 * GpuPower: the milestone-1 implementation is a read-only census (ADT + boot
 * addon). It performs no MMIO write and maps no device memory: the GPU power
 * domain, GMU firmware handshake and SMMU attachment stay closed until their
 * ownership is qualified, which is the same order the reference SM8650 project
 * keeps (docs/GPU_METAL_M1.md). */
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
    TB710FU_GPU_STAT_COUNT = 16
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
 * arguments registers nothing. */
void tb710fu_gpu_meta_init(void);
void tb710fu_gpu_power_meta_init(void);

/* After postModLoad: the census, the optional power-domain node and, with
 * gpu-metal=1, the accelerator publication. */
void tb710fu_gpu_start(void);

/* 1 when either argument asks for work; also records the asked level. */
int tb710fu_gpu_wanted(void);

/* Read-only: the ADT's kgsl-3d0 description, one line. */
void tb710fu_gpu_power_survey(void);

/* Read-only: the boot addon's GPU entries. */
void tb710fu_gpu_census_read(Tb710fuGpuCensus *census);

/* gpu-power>=2: publish Tb710fuGpuPower with the census as properties. */
void tb710fu_gpu_power_publish(const Tb710fuGpuCensus *census);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_GPU_H */
