/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A self-contained SMMUv2 (ARM MMU-500 family) 4 KiB stage-1 context for the
 * SM8650 GPU SMMU at 0x03da0000, derived from the hardware-proven
 * UniXNU ArmSmmu4K kit (kits/ArmSmmu4K, MIT) rather than from guesses:
 *
 *   - IDR0/IDR1/IDR2 decide the number of SMRs, the context-bank layout
 *     (pages = 1 << (((IDR1 >> 28) & 7) + 1), stride = 0x1000 or 0x10000),
 *     and the output address width (IDR2.OAS).
 *   - A context is a 4 KiB root table in one physically-contiguous pool; the
 *     pool is ordinary cacheable kernel memory, so every table page is cleaned
 *     to the point of coherency (dc cvac + dsb sy) after it is written and
 *     before SCTLR.M is set. The walk is non-coherent (TCR.CACHE_WALK=1), which
 *     is what the kit validated on this SMMU family.
 *   - Leaf/table descriptor encoding, TCR/TCR2/MAIR/SCTLR and the stream
 *     attach order (S2CR=FAULT, then SMR, then S2CR=bank) are the kit's.
 *   - TLBI is issued inside the context bank at +0x610/+0x7f0/+0x7f4 exactly as
 *     the kit does. M3 never remaps an address that is already live, so a
 *     translation is never stale even if the TLB maintenance is ignored.
 *
 * Only banks 0..1 are owned here: bank 0 is the GPU's VA space (SID 0 and 1),
 * bank 1 is the GMU's (SID 5), matching sm8650-lenovo-tb710fu.dts.
 */
#ifndef TB710FU_GPU_SMMU_H
#define TB710FU_GPU_SMMU_H
#include "Tb710fuGpuMmio.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

/* Map flags for tb710fu_gpu_smmu_map. */
enum {
    Tb710fuGpuMapRead = 1u << 0,
    Tb710fuGpuMapWrite = 1u << 1,
    Tb710fuGpuMapWriteback = 1u << 2,
    Tb710fuGpuMapShareShift = 8,
    Tb710fuGpuMapShareMask = 3u << 8
};

struct Tb710fuGpuSmmu {
    Tb710fuGpuWindow *window;
    uint32_t id0, id1, id2;
    uint32_t pages;         /* context-bank pages per the IDR1 layout */
    uint32_t stride;        /* 0x1000 or 0x10000 */
    uint32_t bankOffset[2]; /* byte offset of bank 0 and bank 1 */
    uint32_t banks;         /* IDR1.NUMCB */
    uint8_t *pool;
    uint64_t poolPa;
    uint32_t poolBytes;
    uint32_t tablesTotal;
    uint32_t tablesUsed;
    IOBufferMemoryDescriptor *poolMemory;
    uint32_t rootTable[2];
    uint32_t outputBits;
    uint32_t shareability;
};

/* Reads the ID registers, validates the global state (SCR0), and allocates the
 * table pool. 0 on success; negative on a missing window, an implausible ID
 * layout, an unavailable pool, or a global state this driver will not change. */
int tb710fu_gpu_smmu_init(Tb710fuGpuSmmu *smmu, Tb710fuGpuWindow *window, uint32_t poolBytes);

/* Clears the root table of `bank`. Must precede its map calls. */
int tb710fu_gpu_smmu_begin(Tb710fuGpuSmmu *smmu, uint32_t bank);

/* Maps [iova, iova+bytes) of the descriptor with 4 KiB leaves. A descriptor
 * page that is not 4 KiB aligned is refused. 0 on success. */
int tb710fu_gpu_smmu_map(Tb710fuGpuSmmu *smmu, uint32_t bank, uint64_t iova,
                         IOMemoryDescriptor *memory, uint64_t bytes, uint32_t flags);

/* Clears [iova, iova+bytes) from the bank's tables and invalidates the bank's
 * TLB (TLBIALL). Used by BoFree; M3 never remaps a live address. */
int tb710fu_gpu_smmu_unmap(Tb710fuGpuSmmu *smmu, uint32_t bank, uint64_t iova, uint64_t bytes);

/* Writes the context, verifies the read-back, then sets SCTLR.M. The root table
 * must already contain every mapping: M3 calls program() after the last map. */
int tb710fu_gpu_smmu_program(Tb710fuGpuSmmu *smmu, uint32_t bank, uint32_t asid);

/* Attaches SIDs to a bank (S2CR -> FAULT, SMR, S2CR -> bank). */
int tb710fu_gpu_smmu_attach(Tb710fuGpuSmmu *smmu, uint32_t bank,
                            const uint32_t *sids, uint32_t count);

/* The global fault status and the bank's fault status; nonzero means a fault. */
uint32_t tb710fu_gpu_smmu_global_fault(const Tb710fuGpuSmmu *smmu);
uint32_t tb710fu_gpu_smmu_bank_fault(const Tb710fuGpuSmmu *smmu, uint32_t bank);

#endif /* TB710FU_GPU_SMMU_H */
