/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Bounds-checked, aligned access to one mapped device window.
 *
 * The board's drivers already map registers this way (Tb710fuAdsp.cpp): an
 * IODeviceMemory range mapped kIOMapInhibitCache, then volatile dword accesses.
 * The GPU windows keep that idiom, add a bound and an alignment test before
 * every access, and put the barrier in one place. No register is ever touched
 * through a cast of a physical address, and a firmware copy into device memory
 * never uses memcpy on the destination (the destination is Device-GRE). */
#ifndef TB710FU_GPU_MMIO_H
#define TB710FU_GPU_MMIO_H
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

struct Tb710fuGpuWindow {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint32_t bytes;
};

/* 0 on success; -1 when the range cannot be mapped. The window is retained for
 * the life of the kernel (these are the board's own controllers). */
int tb710fu_gpu_window_map(Tb710fuGpuWindow *window, uint64_t base, uint64_t bytes, uint32_t cache);
void tb710fu_gpu_window_unmap(Tb710fuGpuWindow *window);

/* 0 on success, -1 when the access would leave the window or is misaligned. */
int tb710fu_gpu_read32(const Tb710fuGpuWindow *window, uint32_t offset, uint32_t *value);
int tb710fu_gpu_write32(const Tb710fuGpuWindow *window, uint32_t offset, uint32_t value);
int tb710fu_gpu_read64(const Tb710fuGpuWindow *window, uint32_t offset, uint64_t *value);
int tb710fu_gpu_write64(const Tb710fuGpuWindow *window, uint32_t offset, uint64_t value);

/* dsb sy: every write above is ordered before the next register access. */
void tb710fu_gpu_barrier(void);
void tb710fu_gpu_delay_us(uint32_t microseconds);

/* Copy `bytes` from ordinary kernel memory into the device window at `offset`.
 * 8-byte, then 4-byte, then byte stores, each volatile with a barrier after the
 * last. Never memcpy: a Device-GRE destination may not be accessed as normal
 * memory on arm64. Returns 0 or -1 (out of window / unaligned). */
int tb710fu_gpu_mmio_copy(const Tb710fuGpuWindow *window, uint32_t offset, const void *source, uint32_t bytes);
/* 32-bit stores only: the GMU CM3 ITCM/DTCM windows are 32-bit register
 * memories (a6xx_gmu.c gmu_write_bulk), so a wide store is not acceptable
 * there even when the address looks aligned. Returns 0 or -1. */
int tb710fu_gpu_mmio_copy32(const Tb710fuGpuWindow *window, uint32_t offset, const void *source, uint32_t bytes);

#endif /* TB710FU_GPU_MMIO_H */
