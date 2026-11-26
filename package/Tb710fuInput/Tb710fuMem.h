/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Aligned MMIO accessors.
 *
 * SM8650 raises ESR 0x96000061 (alignment fault) on an unaligned Device-GRE
 * access, and clang -Werror,-Wdeprecated-volatile rejects volatile++
 * arithmetic. Every access below is therefore a single aligned 32-bit or
 * 64-bit load/store through a volatile pointer, never a memcpy, memset or
 * bzero over a mapping, and no counter is expressed as ++ on a volatile
 * object. Callers that need read-modify-write spell it out as
 *   value = read(); write(value | bits);
 */
#ifndef TB710FU_MEM_H
#define TB710FU_MEM_H

#include <stdint.h>

#if defined(__aarch64__)
#define TB710FU_DMA_BARRIER() __asm__ volatile("dmb osh" ::: "memory")
#define TB710FU_IO_BARRIER()  __asm__ volatile("dmb osh" ::: "memory")
#else
#define TB710FU_DMA_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define TB710FU_IO_BARRIER()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

static inline uint32_t *tb710fu_reg32(void *base, uint32_t offset)
{
    return (uint32_t *)((uintptr_t)base + (uintptr_t)offset);
}

static inline const uint32_t *tb710fu_reg32_const(const void *base, uint32_t offset)
{
    return (const uint32_t *)((uintptr_t)base + (uintptr_t)offset);
}

static inline uint64_t *tb710fu_reg64(void *base, uint32_t offset)
{
    return (uint64_t *)((uintptr_t)base + (uintptr_t)offset);
}

/* A 32-bit register access. `offset` must be 4-byte aligned, `base` must be
 * at least 4-byte aligned; both hold for every window this driver maps. */
static inline uint32_t tb710fu_mmio_read32(const void *base, uint32_t offset)
{
    return *(const volatile uint32_t *)tb710fu_reg32_const(base, offset);
}

static inline void tb710fu_mmio_write32(void *base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)tb710fu_reg32(base, offset) = value;
}

/* A 64-bit register access; used only where an 8-byte aligned register pair is
 * defined (the SPMI observer data words are 32-bit, so this is for diagnostics
 * and for the aligned 64-bit read of two adjacent status registers). */
static inline uint64_t tb710fu_mmio_read64(const void *base, uint32_t offset)
{
    return *(const volatile uint64_t *)tb710fu_reg64((void *)base, offset);
}

static inline void tb710fu_mmio_write64(void *base, uint32_t offset, uint64_t value)
{
    *(volatile uint64_t *)tb710fu_reg64(base, offset) = value;
}

static inline void tb710fu_mmio_fence(void)
{
    TB710FU_IO_BARRIER();
}

#endif /* TB710FU_MEM_H */
