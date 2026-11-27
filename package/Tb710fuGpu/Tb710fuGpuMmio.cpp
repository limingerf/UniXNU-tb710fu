// SPDX-License-Identifier: BSD-2-Clause-Patent
// The one place that maps and touches a device window (see Tb710fuGpuMmio.h).
//
// Mapping follows package/Tb710fuAdsp/Tb710fuAdsp.cpp: IODeviceMemory::withRange
// then map(kIOMapAnywhere | kIOMapInhibitCache). That page is the board's
// working register mapping for the UFS/GCC and ADSP blocks, so the GPU windows
// use the same attributes rather than inventing a new one.
#include "Tb710fuGpuMmio.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

int tb710fu_gpu_window_map(Tb710fuGpuWindow *window, uint64_t base, uint64_t bytes, uint32_t cache)
{
    if (!window) {
        return -1;
    }
    window->map = nullptr;
    window->va = nullptr;
    window->bytes = 0;
    if (!bytes || bytes > 0xffffffffull) {
        return -1;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(base, (IOByteCount)bytes);
    if (!memory) {
        return -1;
    }
    window->map = memory->map(kIOMapAnywhere | (IOOptionBits)cache);
    memory->release();
    if (!window->map) {
        return -1;
    }
    window->va = reinterpret_cast<volatile uint8_t *>(window->map->getVirtualAddress());
    window->bytes = (uint32_t)bytes;
    return window->va ? 0 : -1;
}

void tb710fu_gpu_window_unmap(Tb710fuGpuWindow *window)
{
    if (!window) {
        return;
    }
    if (window->map) {
        window->map->release();
    }
    window->map = nullptr;
    window->va = nullptr;
    window->bytes = 0;
}

int tb710fu_gpu_read32(const Tb710fuGpuWindow *window, uint32_t offset, uint32_t *value)
{
    if (!window || !window->va || !value || (offset & 3u) || offset + 4u > window->bytes) {
        return -1;
    }
    *value = *reinterpret_cast<volatile uint32_t *>(window->va + offset);
    return 0;
}

int tb710fu_gpu_write32(const Tb710fuGpuWindow *window, uint32_t offset, uint32_t value)
{
    if (!window || !window->va || (offset & 3u) || offset + 4u > window->bytes) {
        return -1;
    }
    *reinterpret_cast<volatile uint32_t *>(window->va + offset) = value;
    tb710fu_gpu_barrier();
    return 0;
}

int tb710fu_gpu_read64(const Tb710fuGpuWindow *window, uint32_t offset, uint64_t *value)
{
    if (!window || !window->va || !value || (offset & 7u) || offset + 8u > window->bytes) {
        return -1;
    }
    *value = *reinterpret_cast<volatile uint64_t *>(window->va + offset);
    return 0;
}

int tb710fu_gpu_write64(const Tb710fuGpuWindow *window, uint32_t offset, uint64_t value)
{
    if (!window || !window->va || (offset & 7u) || offset + 8u > window->bytes) {
        return -1;
    }
    *reinterpret_cast<volatile uint64_t *>(window->va + offset) = value;
    tb710fu_gpu_barrier();
    return 0;
}

void tb710fu_gpu_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

void tb710fu_gpu_delay_us(uint32_t microseconds)
{
    /* IOSleep is millisecond granularity; a poll shorter than 1 ms spins. */
    if (microseconds >= 1000u) {
        IOSleep(microseconds / 1000u);
    } else {
        for (volatile uint32_t spin = 0; spin < 100u * microseconds; spin = spin + 1) {
            __asm__ volatile("yield" ::: "memory");
        }
    }
}

int tb710fu_gpu_mmio_copy(const Tb710fuGpuWindow *window, uint32_t offset, const void *source, uint32_t bytes)
{
    if (!window || !window->va || !source || offset + bytes > window->bytes || offset + bytes < offset) {
        return -1;
    }
    const uint8_t *from = static_cast<const uint8_t *>(source);
    uint32_t at = 0;
    /* Aligned wide stores first; the byte tail handles firmware headers and odd
     * lengths. The destination is never read as normal memory. */
    while (at + 8u <= bytes && ((offset + at) & 7u) == 0) {
        uint64_t word = 0;
        __builtin_memcpy(&word, from + at, sizeof(word));
        *reinterpret_cast<volatile uint64_t *>(window->va + offset + at) = word;
        at += 8u;
    }
    while (at + 4u <= bytes && ((offset + at) & 3u) == 0) {
        uint32_t word = 0;
        __builtin_memcpy(&word, from + at, sizeof(word));
        *reinterpret_cast<volatile uint32_t *>(window->va + offset + at) = word;
        at += 4u;
    }
    while (at < bytes) {
        *reinterpret_cast<volatile uint8_t *>(window->va + offset + at) = from[at];
        ++at;
    }
    tb710fu_gpu_barrier();
    return 0;
}

int tb710fu_gpu_mmio_copy32(const Tb710fuGpuWindow *window, uint32_t offset, const void *source, uint32_t bytes)
{
    if (!window || !window->va || !source || (offset & 3u) || (bytes & 3u) ||
        offset + bytes > window->bytes || offset + bytes < offset) {
        return -1;
    }
    const uint8_t *from = static_cast<const uint8_t *>(source);
    for (uint32_t at = 0; at < bytes; at += 4u) {
        uint32_t word = 0;
        __builtin_memcpy(&word, from + at, sizeof(word));
        *reinterpret_cast<volatile uint32_t *>(window->va + offset + at) = word;
    }
    tb710fu_gpu_barrier();
    return 0;
}
