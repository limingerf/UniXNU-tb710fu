/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Reads one gen70900 image through the board's firmware reader and verifies it
 * against the manifest pin (size and SHA-256) before any byte could reach the
 * GPU.
 *
 * Two sources, in this order:
 *   1. the boot addon entry "Gpu/Firmware/<name>" (/chosen/memory-map GpuAddon,
 *      the same NGPUADD2 container the loader hands over);
 *   2. the Data-volume path "/private/var/ngpu/Gpu/Firmware/<name>" the kernel's
 *      gpu-addon mount writes (docs/GPU_METAL_M1.md).
 * The readers are package/Tb710fuAdsp's tb710fu_fw_open/addon path/read/close;
 * that archive is always in this board's kernel and already owns the zlib
 * inflate for addon entries.
 *
 * The SHA-256 is the one the framework driver uses (NativeAdrenoSha256.cpp,
 * MIT); it is one-shot over the whole image, which the pins keep small
 * (78,588 / 85,300 / 33,176 / 12,088 bytes). */
#ifndef TB710FU_GPU_FIRMWARE_H
#define TB710FU_GPU_FIRMWARE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Tb710fuGpuFirmware {
    uint64_t bytes;
    uint8_t digest[32];
    uint32_t source;      /* 1 addon, 2 Data volume */
};

/* 0 when the image was found, read, and matched both pins. Negative on failure:
 * -1 open, -2 read/size, -3 digest, -4 no memory, -5 bad pin. */
int tb710fu_gpu_firmware_verify(const char *name, uint64_t expected_bytes,
                                const char *expected_hex, struct Tb710fuGpuFirmware *out);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_GPU_FIRMWARE_H */
