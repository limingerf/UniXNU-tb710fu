// SPDX-License-Identifier: BSD-2-Clause-Patent
// SMMUv2 4 KiB stage-1 contexts for the SM8650 GPU SMMU (see the header).
#include "Tb710fuGpuSmmu.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <string.h>

namespace {
const uint32_t kPageBytes = 4096;
const uint32_t kDescriptorsPerTable = 512;
const uint64_t kDescriptorPaMask = 0x0000fffffffff000ull;
const uint32_t kTcrCacheWalk = 0x2000;   // TCR.CACHE_WALK: non-coherent table walk
const uint32_t kAfNgn = (1u << 11) | (1u << 10); // nG | AF on a leaf
const uint32_t kSctlrEnable = 0x1067;    // M | A | C | ASIDPNE (ArmSmmu4K kit)
const uint32_t kCbar = 0x1f300;          // stage-1 translation, stage-2 bypass
const uint32_t kCba2r = 1;
const uint32_t kMair0 = 0xff44;
__attribute__((unused)) const uint32_t kS2crFault = 2u << 16;

void cleanRange(const void *base, uint32_t bytes)
{
    const uint8_t *at = static_cast<const uint8_t *>(base);
    const uint8_t *end = at + (bytes & ~63u);
    for (; at < end; at += 64) {
        __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t widthCode(uint32_t bits)
{
    switch (bits) {
    case 32: return 0;
    case 36: return 1;
    case 40: return 2;
    case 42: return 3;
    case 44: return 4;
    case 48: return 5;
    default: return 2;
    }
}

uint32_t outputBitsFromId2(uint32_t id2)
{
    static const uint32_t bits[6] = {32, 36, 40, 42, 44, 48};
    const uint32_t code = (id2 >> 4) & 0xfu;
    return code < 6 ? bits[code] : 40;
}

uint32_t exids(uint32_t id0) { return (id0 >> 8) & 1u; }
uint32_t validSmr(uint32_t id0) { return exids(id0) ? 0 : (1u << 31); }
uint32_t validS2cr(uint32_t id0) { return exids(id0) ? (1u << 10) : 0; }

uint8_t *tableAt(Tb710fuGpuSmmu *smmu, uint32_t index)
{
    if (index >= smmu->tablesTotal) {
        return nullptr;
    }
    return smmu->pool + index * kPageBytes;
}

uint64_t tablePa(const Tb710fuGpuSmmu *smmu, uint32_t index)
{
    return smmu->poolPa + uint64_t(index) * kPageBytes;
}

uint32_t indexOfPa(const Tb710fuGpuSmmu *smmu, uint64_t pa)
{
    if (pa < smmu->poolPa || pa >= smmu->poolPa + smmu->poolBytes) {
        return smmu->tablesTotal;
    }
    return uint32_t((pa - smmu->poolPa) >> 12);
}

uint32_t allocateTable(Tb710fuGpuSmmu *smmu)
{
    if (smmu->tablesUsed >= smmu->tablesTotal) {
        return smmu->tablesTotal;
    }
    const uint32_t index = smmu->tablesUsed++;
    uint8_t *table = tableAt(smmu, index);
    memset(table, 0, kPageBytes);
    cleanRange(table, kPageBytes);
    return index;
}

/* One 4 KiB leaf. `flags` uses Tb710fuGpuMap*. Returns 0 or -1. */
int mapPage(Tb710fuGpuSmmu *smmu, uint32_t bank, uint64_t iova, uint64_t pa, uint32_t flags)
{
    const uint32_t root = smmu->rootTable[bank];
    if (root >= smmu->tablesTotal) {
        return -1;
    }
    uint8_t *rootTable = tableAt(smmu, root);
    const uint32_t i1 = uint32_t((iova >> 30) & 3u);
    const uint32_t i2 = uint32_t((iova >> 21) & 511u);
    const uint32_t i3 = uint32_t((iova >> 12) & 511u);

    uint64_t *entry = reinterpret_cast<uint64_t *>(rootTable) + i1;
    uint8_t *l2 = nullptr;
    if ((*entry & 3u) == 0) {
        const uint32_t index = allocateTable(smmu);
        if (index >= smmu->tablesTotal) {
            return -1;
        }
        *entry = tablePa(smmu, index) | 3u;
        cleanRange(rootTable, kPageBytes);
        l2 = tableAt(smmu, index);
    } else {
        const uint32_t index = indexOfPa(smmu, *entry & kDescriptorPaMask);
        if (index >= smmu->tablesTotal) {
            return -1;
        }
        l2 = tableAt(smmu, index);
    }
    uint64_t *entry2 = reinterpret_cast<uint64_t *>(l2) + i2;
    uint8_t *l3 = nullptr;
    if ((*entry2 & 3u) == 0) {
        const uint32_t index = allocateTable(smmu);
        if (index >= smmu->tablesTotal) {
            return -1;
        }
        *entry2 = tablePa(smmu, index) | 3u;
        cleanRange(l2, kPageBytes);
        l3 = tableAt(smmu, index);
    } else {
        const uint32_t index = indexOfPa(smmu, *entry2 & kDescriptorPaMask);
        if (index >= smmu->tablesTotal) {
            return -1;
        }
        l3 = tableAt(smmu, index);
    }
    const uint32_t share = (flags & Tb710fuGpuMapShareMask) >> Tb710fuGpuMapShareShift;
    uint64_t leaf = pa | kAfNgn | 3u;
    if (bank == 0 && iova >= 0x10000000ull && iova < 0x50000000ull) {
        leaf |= (1u << 6);   // AP[1]=1: unprivileged access for user client BOs only (keep GMU/kernel privileged)
    }
    leaf |= uint64_t(share & 3u) << 8;
    if (flags & Tb710fuGpuMapWriteback) {
        leaf |= 1u << 2;   // AttrIndx 1: MAIR attribute 1 (write-back)
    }
    if ((flags & Tb710fuGpuMapWrite) == 0) {
        leaf |= 1u << 7;   // AP[2]: read-only
    }
    reinterpret_cast<uint64_t *>(l3)[i3] = leaf;
    cleanRange(l3, kPageBytes);
    return 0;
}
}  // namespace

int tb710fu_gpu_smmu_init(Tb710fuGpuSmmu *smmu, Tb710fuGpuWindow *window, uint32_t poolBytes)
{
    if (!smmu || !window) {
        return -1;
    }
    memset(smmu, 0, sizeof(*smmu));
    smmu->window = window;
    if (window->bytes < 0x2c || poolBytes < 8u * kPageBytes || poolBytes > 4u * 1024u * 1024u ||
        (poolBytes & (kPageBytes - 1)) != 0) {
        return -1;
    }
    if (tb710fu_gpu_read32(window, 0x20, &smmu->id0) != 0 ||
        tb710fu_gpu_read32(window, 0x24, &smmu->id1) != 0 ||
        tb710fu_gpu_read32(window, 0x28, &smmu->id2) != 0) {
        return -1;
    }
    if ((smmu->id0 & (1u << 27)) == 0 || (smmu->id0 & 0xffu) == 0) {
        return -1;   // no SMMU arch or no SMRs
    }
    smmu->banks = smmu->id1 & 0xffu;
    smmu->pages = 1u << (((smmu->id1 >> 28) & 7u) + 1u);
    smmu->stride = (smmu->id1 & (1u << 31)) ? 0x10000u : 0x1000u;
    if (smmu->banks < 2 || smmu->pages > 128 || smmu->banks > smmu->pages) {
        return -1;
    }
    if (2u * smmu->pages * smmu->stride > window->bytes) {
        return -1;
    }
    for (unsigned bank = 0; bank < 2; ++bank) {
        smmu->bankOffset[bank] = (smmu->pages + bank) * smmu->stride;
        if (smmu->bankOffset[bank] + 0x800u > window->bytes) {
            return -1;
        }
    }
    smmu->outputBits = outputBitsFromId2(smmu->id2);

    /* The kit requires SCR0 = 0x406: translation enabled, global fault
     * reporting on, unmatched streams fault. Refuse rather than silently
     * changing a global state this milestone did not derive. */
    uint32_t scr0 = 0;
    if (tb710fu_gpu_read32(window, 0x0, &scr0) != 0 || (scr0 & 0x407u) != 0x406u) {
        return -2;
    }

    IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                           static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    smmu->poolMemory = IOBufferMemoryDescriptor::withOptions(options, (vm_size_t)poolBytes, kPageBytes);
    if (!smmu->poolMemory) {
        return -3;
    }
    smmu->pool = static_cast<uint8_t *>(smmu->poolMemory->getBytesNoCopy());
    IOByteCount length = 0;
    const addr64_t physical = smmu->poolMemory->getPhysicalSegment(0, &length,
                                                                  static_cast<IOOptionBits>(kIOMemoryMapperNone));
    if (!smmu->pool || !physical || (physical & (kPageBytes - 1)) || length < poolBytes) {
        return -3;
    }
    smmu->poolPa = physical;
    smmu->poolBytes = poolBytes;
    smmu->tablesTotal = poolBytes / kPageBytes;
    smmu->tablesUsed = 0;
    smmu->rootTable[0] = smmu->tablesTotal;
    smmu->rootTable[1] = smmu->tablesTotal;
    memset(smmu->pool, 0, poolBytes);
    cleanRange(smmu->pool, poolBytes);
    return 0;
}

int tb710fu_gpu_smmu_begin(Tb710fuGpuSmmu *smmu, uint32_t bank)
{
    if (!smmu || !smmu->pool || bank > 1) {
        return -1;
    }
    const uint32_t index = allocateTable(smmu);
    if (index >= smmu->tablesTotal) {
        return -1;
    }
    smmu->rootTable[bank] = index;
    return 0;
}

int tb710fu_gpu_smmu_map(Tb710fuGpuSmmu *smmu, uint32_t bank, uint64_t iova,
                         IOMemoryDescriptor *memory, uint64_t bytes, uint32_t flags)
{
    if (!smmu || !memory || bank > 1 || (iova & (kPageBytes - 1)) != 0 ||
        (bytes & (kPageBytes - 1)) != 0 || !bytes || bytes > (uint64_t)smmu->tablesTotal * kDescriptorsPerTable * kPageBytes ||
        iova + bytes > 0x100000000ull || iova + bytes < iova) {
        return -1;
    }
    if (smmu->rootTable[bank] >= smmu->tablesTotal || (flags & Tb710fuGpuMapRead) == 0) {
        return -1;
    }
    uint64_t offset = 0;
    while (offset < bytes) {
        IOByteCount length = 0;
        const addr64_t pa = memory->getPhysicalSegment(offset, &length,
                                                      static_cast<IOOptionBits>(kIOMemoryMapperNone));
        if (!pa || (pa & (kPageBytes - 1)) || length < kPageBytes) {
            return -1;
        }
        const uint64_t limit = (smmu->outputBits >= 64) ? ~0ull : ((uint64_t)1 << smmu->outputBits);
        if (pa >= limit) {
            return -1;
        }
        if (mapPage(smmu, bank, iova + offset, pa, flags) != 0) {
            return -1;
        }
        offset += kPageBytes;
    }
    tb710fu_gpu_barrier();
    return 0;
}

int tb710fu_gpu_smmu_program(Tb710fuGpuSmmu *smmu, uint32_t bank, uint32_t asid)
{
    if (!smmu || bank > 1 || asid == 0 || asid > 0xffffu) {
        return -1;
    }
    const uint32_t root = smmu->rootTable[bank];
    if (root >= smmu->tablesTotal) {
        return -1;
    }
    const uint32_t cb = smmu->bankOffset[bank];
    const uint32_t gr1 = smmu->stride;
    const uint32_t tcr2 = (7u << 15) | (1u << 4) | widthCode(smmu->outputBits);
    const uint32_t tcr = (2u << 30) | kTcrCacheWalk | (64u - 32u) | (1u << 23);
    const uint64_t ttbr0 = (uint64_t(asid) << 48) | tablePa(smmu, root);

    cleanRange(smmu->pool, smmu->poolBytes);
    if (tb710fu_gpu_write32(smmu->window, cb, 0) != 0) {
        return -1;
    }
    tb710fu_gpu_barrier();
    if (tb710fu_gpu_write32(smmu->window, gr1 + 0x800u + bank * 4u, kCba2r) != 0 ||
        tb710fu_gpu_write32(smmu->window, gr1 + bank * 4u, kCbar) != 0 ||
        tb710fu_gpu_write32(smmu->window, cb + 0x10, tcr2) != 0 ||
        tb710fu_gpu_write32(smmu->window, cb + 0x30, tcr) != 0 ||
        tb710fu_gpu_write64(smmu->window, cb + 0x20, ttbr0) != 0 ||
        tb710fu_gpu_write64(smmu->window, cb + 0x28, 0) != 0 ||
        tb710fu_gpu_write32(smmu->window, cb + 0x38, kMair0) != 0 ||
        tb710fu_gpu_write32(smmu->window, cb + 0x3c, 0) != 0 ||
        tb710fu_gpu_write32(smmu->window, cb + 0x58, 0xffffffffu) != 0) {
        return -1;
    }
    tb710fu_gpu_barrier();

    uint32_t sctlr = 1, readTcr = 0, readTcr2 = 0, readCbar = 0;
    uint64_t readTtbr = 0;
    if (tb710fu_gpu_read32(smmu->window, cb, &sctlr) != 0 ||
        tb710fu_gpu_read32(smmu->window, cb + 0x30, &readTcr) != 0 ||
        tb710fu_gpu_read32(smmu->window, cb + 0x10, &readTcr2) != 0 ||
        tb710fu_gpu_read64(smmu->window, cb + 0x20, &readTtbr) != 0 ||
        tb710fu_gpu_read32(smmu->window, gr1 + bank * 4u, &readCbar) != 0) {
        return -1;
    }
    if ((sctlr & 1u) != 0 || readTcr != tcr || (readTcr2 & ~0x60u) != tcr2 || readTtbr != ttbr0 || readCbar != kCbar) {
        IOLog("TB710FU GPU: smmu program bank %u mismatch sctlr=0x%x tcr=0x%x(want 0x%x) tcr2=0x%x(want 0x%x) ttbr=0x%llx(want 0x%llx) cbar=0x%x(want 0x%x)\n",
              bank, sctlr, readTcr, tcr, readTcr2, tcr2, (unsigned long long)readTtbr, (unsigned long long)ttbr0, readCbar, kCbar);
        return -1;
    }

    if (tb710fu_gpu_write32(smmu->window, cb, kSctlrEnable) != 0) {
        return -1;
    }
    tb710fu_gpu_barrier();
    if (tb710fu_gpu_read32(smmu->window, cb, &sctlr) != 0 || (sctlr & 1u) == 0) {
        return -1;
    }
    uint32_t fsr = 0;
    if (tb710fu_gpu_read32(smmu->window, cb + 0x58, &fsr) != 0 || (fsr & 0xc0000000u) != 0) {
        return -1;
    }
    return 0;
}

int tb710fu_gpu_smmu_attach(Tb710fuGpuSmmu *smmu, uint32_t bank, const uint32_t *sids, uint32_t count)
{
    if (!smmu || bank > 1 || !sids || !count || count > 8) {
        return -1;
    }
    const uint32_t smrCount = smmu->id0 & 0xffu;
    uint32_t slots[8] = {};
    for (uint32_t i = 0; i < count; ++i) {
        if (sids[i] > 0xffffu) {
            return -1;
        }
        uint32_t chosen = smrCount;
        for (uint32_t slot = 0; slot < smrCount; ++slot) {
            uint32_t smr = 0, s2cr = 0;
            if (tb710fu_gpu_read32(smmu->window, 0x800u + 4u * slot, &smr) != 0 ||
                tb710fu_gpu_read32(smmu->window, 0xc00u + 4u * slot, &s2cr) != 0) {
                return -1;
            }
            const uint32_t live = exids(smmu->id0) ? ((s2cr & (1u << 10)) != 0)
                                                   : ((smr & (1u << 31)) != 0);
            if (live && (smr & 0xffffu) == sids[i]) {
                chosen = slot;   // already ours (the ADT stream lease): reprogram it
                break;
            }
            if (!live || (s2cr & (3u << 16)) != 0) {
                bool reserved = false;
                for (uint32_t j = 0; j < i; ++j) {
                    if (slots[j] == slot) {
                        reserved = true;
                        break;
                    }
                }
                if (!reserved && chosen == smrCount) {
                    chosen = slot;   // free or FAULT: reusable
                }
            }
        }
        if (chosen == smrCount) {
            return -1;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (slots[j] == chosen) {
                return -1;
            }
        }
        slots[i] = chosen;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = slots[i];
        const uint32_t s2cr = 0xc00u + 4u * slot;
        const uint32_t smr = 0x800u + 4u * slot;
        uint32_t value = 0;
        if (tb710fu_gpu_write32(smmu->window, s2cr, bank) != 0) {
            return -1;
        }
        tb710fu_gpu_barrier();
        if (tb710fu_gpu_read32(smmu->window, s2cr, &value) != 0 || (value & 0x300ffu) != bank) {
            IOLog("TB710FU GPU: smmu attach bank %u slot %u s2cr=0x%x(want 0x%x)\n", bank, slot, value, bank);
            return -1;
        }
        const uint32_t wantSmr = validSmr(smmu->id0) | sids[i];
        if (tb710fu_gpu_write32(smmu->window, smr, wantSmr) != 0) {
            return -1;
        }
        tb710fu_gpu_barrier();
        if (tb710fu_gpu_read32(smmu->window, smr, &value) != 0 ||
            (value & (validSmr(smmu->id0) | 0xffffu)) != wantSmr) {
            IOLog("TB710FU GPU: smmu attach bank %u slot %u smr=0x%x(want 0x%x)\n", bank, slot, value, wantSmr);
            return -1;
        }
        if (exids(smmu->id0)) {
            if (tb710fu_gpu_write32(smmu->window, s2cr, bank | validS2cr(smmu->id0)) != 0) {
                return -1;
            }
            tb710fu_gpu_barrier();
        }
    }
    tb710fu_gpu_barrier();
    return 0;
}

int tb710fu_gpu_smmu_unmap(Tb710fuGpuSmmu *smmu, uint32_t bank, uint64_t iova, uint64_t bytes)
{
    if (!smmu || !smmu->pool || bank > 1 || (iova & (kPageBytes - 1)) != 0 ||
        (bytes & (kPageBytes - 1)) != 0 || !bytes || iova + bytes > 0x100000000ull) {
        return -1;
    }
    if (smmu->rootTable[bank] >= smmu->tablesTotal) {
        return -1;
    }
    for (uint64_t offset = 0; offset < bytes; offset += kPageBytes) {
        const uint64_t address = iova + offset;
        uint8_t *root = tableAt(smmu, smmu->rootTable[bank]);
        const uint32_t i1 = (uint32_t)((address >> 30) & 3u);
        const uint64_t entry1 = reinterpret_cast<uint64_t *>(root)[i1];
        if ((entry1 & 3u) != 3u) {
            continue;
        }
        const uint32_t index2 = indexOfPa(smmu, entry1 & kDescriptorPaMask);
        if (index2 >= smmu->tablesTotal) {
            return -1;
        }
        uint8_t *l2 = tableAt(smmu, index2);
        const uint32_t i2 = (uint32_t)((address >> 21) & 511u);
        const uint64_t entry2 = reinterpret_cast<uint64_t *>(l2)[i2];
        if ((entry2 & 3u) != 3u) {
            continue;
        }
        const uint32_t index3 = indexOfPa(smmu, entry2 & kDescriptorPaMask);
        if (index3 >= smmu->tablesTotal) {
            return -1;
        }
        uint8_t *l3 = tableAt(smmu, index3);
        const uint32_t i3 = (uint32_t)((address >> 12) & 511u);
        reinterpret_cast<uint64_t *>(l3)[i3] = 0;
        cleanRange(l3, kPageBytes);
    }
    tb710fu_gpu_barrier();
    const uint32_t cb = smmu->bankOffset[bank];
    if (tb710fu_gpu_write32(smmu->window, cb + 0x618, 0) != 0 ||
        tb710fu_gpu_write32(smmu->window, cb + 0x7f0, 0xffffffffu) != 0) {
        return -1;
    }
    tb710fu_gpu_barrier();
    uint32_t sync = 0;
    for (unsigned i = 0; i < 100; ++i) {
        if (tb710fu_gpu_read32(smmu->window, cb + 0x7f4, &sync) != 0) {
            return -1;
        }
        if ((sync & 1u) == 0) {
            return 0;
        }
    }
    return -1;
}

uint32_t tb710fu_gpu_smmu_global_fault(const Tb710fuGpuSmmu *smmu)
{
    uint32_t gfsr = 0;
    if (!smmu || !smmu->window || tb710fu_gpu_read32(smmu->window, 0x48, &gfsr) != 0) {
        return 0xffffffffu;
    }
    return gfsr;
}

uint32_t tb710fu_gpu_smmu_bank_fault(const Tb710fuGpuSmmu *smmu, uint32_t bank)
{
    uint32_t fsr = 0;
    if (!smmu || !smmu->window || bank > 1 ||
        tb710fu_gpu_read32(smmu->window, smmu->bankOffset[bank] + 0x58, &fsr) != 0) {
        return 0xffffffffu;
    }
    return fsr;
}
