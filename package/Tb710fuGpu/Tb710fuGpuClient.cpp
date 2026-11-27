// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU user client (milestone 2): NativeAdrenoABI v2 on the accelerator.
//
// The Metal plugin (NativeAdrenoMetal.bundle, and Turnip behind it) reaches this
// driver by opening the accelerator service with type 0x4147 and then talking
// the ABI in Tb710fuGpuNativeABI.h. The connection class keeps the name
// AGXDeviceUserClient because the original sandbox profiles of the processes
// that use Metal admit that user-client class and nothing else of this driver's;
// no Apple AGX kext is ever loaded on this board, so the name decides admission
// and nothing else (the reference driver's NativeAdrenoPublish.cpp does the
// same for the same reason).
//
// Two transports are served, exactly as the reference does:
//   - direct:  selector = NativeAdrenoMethod*, records are the exact wire
//              structs, sizes are checked here (IOConnectCallStructMethod).
//   - tunnel:  any admitted selector carrying {0x4e544e4c, method, payload};
//              method 0xffffffff only probes admission. The sandbox decides
//              which selector numbers reach the connection, so the plugin sends
//              everything through one it was granted.
//
// Milestone 2 has power, clocks, GMU clocks and firmware provenance, but no CP
// ring: BO creation, CPU mapping, metadata, queues, status and priority are
// real; anything that would need a GPU address (BoMap), a submission, a fence
// or the device facts table answers kIOReturnUnsupported and is counted, so the
// caller learns the ring is missing instead of receiving a made-up value.
#include "Tb710fuGpuClient.h"
#include "Tb710fuGpuCore.h"
#include "Tb710fuGpuNativeABI.h"
#include "Tb710fuGpu.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/libkern.h>
#include <string.h>

namespace {
const uint32_t kTunnelMagic = 0x4e544e4c;  /* 'NTNL' */
const uint32_t kProbeMethod = 0xffffffffu;
const uint64_t kMaxBoBytes = 128ull * 1024 * 1024;
const unsigned kMaxBo = 256;
const unsigned kMaxMetadata = 32;
const unsigned kMaxQueues = 32;
}

/* The connection class. The constructor receives the metaclass pointer from
 * OSDefineMetaClassAndStructors' static init, which the build defers to the
 * preModLoad window (Tb710fuGpuClient.cpp's __mod_init_func). */
class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)
public:
    struct Bo {
        IOMemoryDescriptor *memory;
        uint64_t handle;
        uint64_t bytes;
        uint64_t iova;
        uint32_t used;
        uint8_t mapped;
        uint8_t imported;
        uint8_t prepared;
        uint8_t readOnly;
    };
    struct Meta {
        uint64_t handle;
        uint32_t bytes;
        uint32_t used;
        char text[NativeAdrenoAbiMetadataBytes];
    };

    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
                      OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;

private:
    IOReturn run(uint32_t method, const uint8_t *in, uint32_t inSize,
                 uint8_t *out, uint32_t outSize);
    int slotFor(uint64_t handle) const;
    int freeSlot(void) const;
    void releaseBo(int slot);
    void releaseAll(void);
    IOReturn allocBo(const NativeAdrenoAbiBoAlloc &request, uint8_t *out);
    IOReturn freeBo(const NativeAdrenoAbiBoFree &request);
    IOReturn cpuMap(const NativeAdrenoAbiBoCpuMap &request, uint8_t *out);
    IOReturn setMetadata(const NativeAdrenoAbiMetadata &request);
    IOReturn getMetadata(const NativeAdrenoAbiMetadata &request, uint8_t *out);
    IOReturn createQueue(const NativeAdrenoAbiQueue &request, uint8_t *out);
    IOReturn mapBo(const NativeAdrenoAbiBoMap &request, uint8_t *out);
    IOReturn importBo(const NativeAdrenoAbiBoImport &request, uint8_t *out);
    IOReturn submit(const NativeAdrenoAbiSubmit &request, uint8_t *out);
    IOReturn waitFence(const NativeAdrenoAbiWaitFence &request);
    IOReturn deviceInfo(uint8_t *out);
    IOReturn timestamp(uint8_t *out);

    IOLock *fLock = nullptr;
    task_t fTask = nullptr;
    Bo fBo[kMaxBo] = {};
    Meta fMetadata[kMaxMetadata] = {};
    uint64_t fQueue[kMaxQueues] = {};
    uint64_t fSequence = 0;
    uint64_t fVaStart = 0;
    uint64_t fVaSize = 0;
    int32_t fVaSlot = -1;
    uint32_t fNotesPrinted = 0;
    bool ensureVaSlice(void);
    void releaseVaSlice(void);
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)

namespace {
/* Exact wire record sizes: a direct call must pass them, a tunnelled one is
 * checked by run() after the eight-byte header is removed. */
void expectedSizes(uint32_t method, uint32_t *in, uint32_t *out)
{
    switch (method) {
    case NativeAdrenoMethodQuery:          *in = 0; *out = sizeof(NativeAdrenoAbiParams); break;
    case NativeAdrenoMethodBoAlloc:        *in = sizeof(NativeAdrenoAbiBoAlloc); *out = sizeof(NativeAdrenoAbiBoAllocReply); break;
    case NativeAdrenoMethodBoMap:          *in = sizeof(NativeAdrenoAbiBoMap); *out = sizeof(NativeAdrenoAbiBoMapReply); break;
    case NativeAdrenoMethodBoFree:         *in = sizeof(NativeAdrenoAbiBoFree); *out = 0; break;
    case NativeAdrenoMethodSubmit:         *in = sizeof(NativeAdrenoAbiSubmit); *out = sizeof(NativeAdrenoAbiSubmitReply); break;
    case NativeAdrenoMethodWaitFence:      *in = sizeof(NativeAdrenoAbiWaitFence); *out = 0; break;
    case NativeAdrenoMethodGetTimestamp:   *in = 0; *out = sizeof(NativeAdrenoAbiTimestamp); break;
    case NativeAdrenoMethodGetStatus:      *in = 0; *out = sizeof(NativeAdrenoAbiStatus); break;
    case NativeAdrenoMethodSetMetadata:    *in = sizeof(NativeAdrenoAbiMetadata); *out = 0; break;
    case NativeAdrenoMethodGetMetadata:    *in = sizeof(NativeAdrenoAbiMetadata); *out = sizeof(NativeAdrenoAbiMetadata); break;
    case NativeAdrenoMethodCreateQueue:    *in = sizeof(NativeAdrenoAbiQueue); *out = sizeof(NativeAdrenoAbiQueueReply); break;
    case NativeAdrenoMethodCloseQueue:     *in = sizeof(NativeAdrenoAbiQueue); *out = 0; break;
    case NativeAdrenoMethodBoCpuMap:       *in = sizeof(NativeAdrenoAbiBoCpuMap); *out = sizeof(NativeAdrenoAbiBoCpuMapReply); break;
    case NativeAdrenoMethodBoCpuUnmap:     *in = sizeof(NativeAdrenoAbiBoCpuUnmap); *out = 0; break;
    case NativeAdrenoMethodGetDeviceInfo:  *in = 0; *out = sizeof(NativeAdrenoAbiDeviceInfo); break;
    case NativeAdrenoMethodBoImport:       *in = sizeof(NativeAdrenoAbiBoImport); *out = sizeof(NativeAdrenoAbiBoAllocReply); break;
    case NativeAdrenoMethodSetPriority:    *in = sizeof(NativeAdrenoAbiPriority); *out = 0; break;
    case NativeAdrenoMethodNote:           *in = sizeof(NativeAdrenoAbiNote); *out = 0; break;
    default:                               *in = 0; *out = 0; break;
    }
}

bool plausible(const void *pointer, uint32_t bytes)
{
    return bytes == 0 || pointer != nullptr;
}

void copyOut(uint8_t *out, uint32_t outSize, const void *reply, uint32_t bytes)
{
    if (bytes && out && outSize >= bytes) {
        memcpy(out, reply, bytes);
    }
}

volatile uint32_t gClientVaBitmap = 0;
const int32_t kMaxVaSlots = 23; /* slot 0: [0x10000000,+512M), slots 1..8: [0x30000000,+64M), slots 9..22: [0x80000000,+128M) (skips kernel 0x70000000) */
}

bool AGXDeviceUserClient::ensureVaSlice(void)
{
    if (fVaSlot >= 0) {
        return true;
    }
    for (int32_t s = 0; s < kMaxVaSlots; ++s) {
        const uint32_t bit = 1u << static_cast<uint32_t>(s);
        if ((__atomic_fetch_or(&gClientVaBitmap, bit, __ATOMIC_ACQ_REL) & bit) == 0) {
            fVaSlot = s;
            if (s == 0) {
                fVaStart = 0x10000000ull;
                fVaSize = 0x20000000ull;
            } else if (s <= 8) {
                fVaStart = 0x30000000ull + static_cast<uint64_t>(s - 1) * 0x04000000ull;
                fVaSize = 0x04000000ull;
            } else {
                fVaStart = 0x80000000ull + static_cast<uint64_t>(s - 9) * 0x08000000ull;
                fVaSize = 0x08000000ull;
            }
            return true;
        }
    }
    return false;
}

void AGXDeviceUserClient::releaseVaSlice(void)
{
    if (fVaSlot >= 0) {
        const uint32_t bit = 1u << static_cast<uint32_t>(fVaSlot);
        __atomic_fetch_and(&gClientVaBitmap, ~bit, __ATOMIC_ACQ_REL);
        fVaSlot = -1;
        fVaStart = 0;
        fVaSize = 0;
    }
}

bool AGXDeviceUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
                                       OSDictionary *properties)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
        return false;
    }
    fTask = owningTask;
    (void)type;
    fLock = IOLockAlloc();
    return fLock != nullptr;
}

void AGXDeviceUserClient::releaseBo(int slot)
{
    if (slot < 0 || slot >= (int)kMaxBo) {
        return;
    }
    if (fBo[slot].mapped && fBo[slot].iova) {
        tb710fu_gpu_core_unmap_bo(fBo[slot].iova, fBo[slot].bytes);
    }
    if (fBo[slot].memory) {
        if (fBo[slot].imported && fBo[slot].prepared) {
            fBo[slot].memory->complete();
        }
        fBo[slot].memory->release();
    }
    memset(&fBo[slot], 0, sizeof(fBo[slot]));
}

void AGXDeviceUserClient::releaseAll(void)
{
    for (unsigned i = 0; i < kMaxBo; ++i) {
        releaseBo((int)i);
    }
    releaseVaSlice();
}

IOReturn AGXDeviceUserClient::clientClose(void)
{
    if (fLock) {
        IOLockLock(fLock);
    }
    releaseAll();
    if (fLock) {
        IOLockUnlock(fLock);
    }
    terminate();
    return kIOReturnSuccess;
}

void AGXDeviceUserClient::free(void)
{
    releaseAll();
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    IOUserClient::free();
}

int AGXDeviceUserClient::slotFor(uint64_t handle) const
{
    if (!handle) {
        return -1;
    }
    for (unsigned i = 0; i < kMaxBo; ++i) {
        if (fBo[i].used && fBo[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

int AGXDeviceUserClient::freeSlot(void) const
{
    for (unsigned i = 0; i < kMaxBo; ++i) {
        if (!fBo[i].used) {
            return (int)i;
        }
    }
    return -1;
}

/* The BO is a kernel-user-shared descriptor: the GPU side has no address for it
 * in milestone 2, so the descriptor's only job is the CPU lease through
 * clientMemoryForType. Requested alignment above a page is refused here rather
 * than promised. */
IOReturn AGXDeviceUserClient::allocBo(const NativeAdrenoAbiBoAlloc &request, uint8_t *out)
{
    if (request.bytes == 0 || request.bytes > kMaxBoBytes || (request.bytes & 0xfffu) != 0 ||
        (request.align_log2 != 0 && (request.align_log2 < 12 || request.align_log2 > 24)) ||
        request.contiguous > 1) {
        return kIOReturnBadArgument;
    }
    const int slot = freeSlot();
    if (slot < 0) {
        return kIOReturnNoResources;
    }
    IOOptionBits options = static_cast<IOOptionBits>(kIOMemoryKernelUserShared) |
                           static_cast<IOOptionBits>(kIODirectionInOut) |
                           static_cast<IOOptionBits>(kIOMapWriteCombineCache);
    if (request.contiguous) {
        options |= kIOMemoryPhysicallyContiguous;
    }
    IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::withOptions(
        options, (vm_size_t)request.bytes, 1);
    if (!memory) {
        return kIOReturnNoMemory;
    }
    if (void *base = memory->getBytesNoCopy()) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(base) & ~63ull;
        const uintptr_t end = (reinterpret_cast<uintptr_t>(base) + (size_t)request.bytes + 63ull) & ~63ull;
        for (uintptr_t a = start; a < end; a += 64) {
            __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint64_t handle = ((fSequence + 1) << 12) | (uint64_t)(slot + 1);
    fSequence = fSequence + 1;
    fBo[slot].memory = memory;
    fBo[slot].handle = handle;
    fBo[slot].bytes = request.bytes;
    fBo[slot].used = 1;
    NativeAdrenoAbiBoAllocReply reply = {};
    reply.handle = handle;
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    tb710fu_gpu_stats[TB710FU_GPU_BO_ALLOCS] = tb710fu_gpu_stats[TB710FU_GPU_BO_ALLOCS] + 1;
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::freeBo(const NativeAdrenoAbiBoFree &request)
{
    const int slot = slotFor(request.handle);
    if (slot < 0) {
        return kIOReturnBadArgument;
    }
    releaseBo(slot);
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::cpuMap(const NativeAdrenoAbiBoCpuMap &request, uint8_t *out)
{
    const int slot = slotFor(request.handle);
    if (slot < 0) {
        return kIOReturnBadArgument;
    }
    if (fBo[slot].imported) {
        return kIOReturnUnsupported;
    }
    NativeAdrenoAbiBoCpuMapReply reply = {};
    reply.token = (uint32_t)(slot + 1);
    reply.bytes = fBo[slot].bytes;
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::setMetadata(const NativeAdrenoAbiMetadata &request)
{
    if (request.bytes > NativeAdrenoAbiMetadataBytes) {
        return kIOReturnBadArgument;
    }
    int slot = -1;
    for (unsigned i = 0; i < kMaxMetadata; ++i) {
        if (fMetadata[i].used && fMetadata[i].handle == request.handle) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        for (unsigned i = 0; i < kMaxMetadata; ++i) {
            if (!fMetadata[i].used) {
                slot = (int)i;
                break;
            }
        }
    }
    if (slot < 0) {
        return kIOReturnNoResources;
    }
    fMetadata[slot].used = 1;
    fMetadata[slot].handle = request.handle;
    fMetadata[slot].bytes = request.bytes;
    memcpy(fMetadata[slot].text, request.text, request.bytes);
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::getMetadata(const NativeAdrenoAbiMetadata &request, uint8_t *out)
{
    NativeAdrenoAbiMetadata reply = {};
    reply.handle = request.handle;
    for (unsigned i = 0; i < kMaxMetadata; ++i) {
        if (fMetadata[i].used && fMetadata[i].handle == request.handle) {
            const uint32_t bytes = fMetadata[i].bytes < request.bytes ? fMetadata[i].bytes : request.bytes;
            reply.bytes = bytes;
            memcpy(reply.text, fMetadata[i].text, bytes);
            break;
        }
    }
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::createQueue(const NativeAdrenoAbiQueue &request, uint8_t *out)
{
    if (request.queue != 0) {
        return kIOReturnBadArgument;
    }
    unsigned slot = kMaxQueues;
    for (unsigned i = 0; i < kMaxQueues; ++i) {
        if (!fQueue[i]) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxQueues) {
        return kIOReturnNoResources;
    }
    fSequence = fSequence + 1;
    const uint64_t id = (fSequence << 8) | 0x51u;
    fQueue[slot] = id;
    NativeAdrenoAbiQueueReply reply = {};
    reply.queue = id;
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::mapBo(const NativeAdrenoAbiBoMap &request, uint8_t *out)
{
    if (request.reserved || (request.permissions & ~3u) || !(request.permissions & 1u) ||
        request.writeback > 1 || request.shareability < 2 || request.shareability > 3 ||
        (request.iova & 0xfffull) != 0) {
        return kIOReturnBadArgument;
    }
    const int slot = slotFor(request.handle);
    if (slot < 0) {
        return kIOReturnBadArgument;
    }
    if (!tb710fu_gpu_core_ring_ready()) {
        return kIOReturnNotReady;
    }
    if (fBo[slot].mapped) {
        return kIOReturnBusy;
    }
    if (!ensureVaSlice()) {
        return kIOReturnNoResources;
    }
    if (request.iova < fVaStart || request.iova + fBo[slot].bytes > fVaStart + fVaSize ||
        request.iova + fBo[slot].bytes < request.iova) {
        return kIOReturnBadArgument;
    }
    uint32_t permissions = request.permissions;
    if (fBo[slot].readOnly) {
        permissions &= ~2u;
    }
    IOReturn result = (IOReturn)tb710fu_gpu_core_map_bo(fBo[slot].memory, request.iova,
                                                        fBo[slot].bytes, permissions,
                                                        request.writeback, request.shareability);
    if (result != kIOReturnSuccess) {
        return result;
    }
    fBo[slot].iova = request.iova;
    fBo[slot].mapped = 1;
    NativeAdrenoAbiBoMapReply reply = {};
    reply.iova = request.iova;
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::importBo(const NativeAdrenoAbiBoImport &request, uint8_t *out)
{
    if (!fTask || request.bytes == 0 || request.bytes > kMaxBoBytes ||
        (request.address & 0xfffull) != 0 || (request.bytes & 0xfffull) != 0) {
        return kIOReturnBadArgument;
    }
    const int slot = freeSlot();
    if (slot < 0) {
        return kIOReturnNoResources;
    }
    const IOOptionBits base = static_cast<IOOptionBits>(kIODirectionInOut) |
                              static_cast<IOOptionBits>(kIOMemoryMapperNone);
    const IOOptionBits inoutPersistent =
        base | static_cast<IOOptionBits>(kIOMemoryPersistent);
    const IOOptionBits outBase = static_cast<IOOptionBits>(kIODirectionOut) |
                                 static_cast<IOOptionBits>(kIOMemoryMapperNone);
    const IOOptionBits outPersistent =
        outBase | static_cast<IOOptionBits>(kIOMemoryPersistent);
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
        request.address, request.bytes, inoutPersistent, fTask);
    if (!memory) {
        memory = IOMemoryDescriptor::withAddressRange(request.address, request.bytes, base, fTask);
    }
    if (!memory) {
        return kIOReturnNoMemory;
    }
    bool readOnly = false;
    IOReturn status = memory->prepare(static_cast<IODirection>(kIODirectionInOut));
    if (status != kIOReturnSuccess) {
        memory->release();
        memory = IOMemoryDescriptor::withAddressRange(request.address, request.bytes,
                                                      outPersistent, fTask);
        if (!memory) {
            memory = IOMemoryDescriptor::withAddressRange(request.address, request.bytes,
                                                          outBase, fTask);
        }
        if (!memory) {
            return kIOReturnNoMemory;
        }
        status = memory->prepare(static_cast<IODirection>(kIODirectionOut));
        if (status == kIOReturnSuccess) {
            readOnly = true;
        }
    }
    if (status != kIOReturnSuccess) {
        memory->release();
        return kIOReturnNoResources;
    }
    uint64_t handle = ((fSequence + 1) << 12) | (uint64_t)(slot + 1);
    fSequence = fSequence + 1;
    fBo[slot].memory = memory;
    fBo[slot].handle = handle;
    fBo[slot].bytes = request.bytes;
    fBo[slot].used = 1;
    fBo[slot].imported = 1;
    fBo[slot].prepared = 1;
    fBo[slot].readOnly = readOnly ? 1 : 0;
    NativeAdrenoAbiBoAllocReply reply = {};
    reply.handle = handle;
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    tb710fu_gpu_stats[TB710FU_GPU_BO_ALLOCS] = tb710fu_gpu_stats[TB710FU_GPU_BO_ALLOCS] + 1;
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::submit(const NativeAdrenoAbiSubmit &request, uint8_t *out)
{
    if (request.reserved || request.buffer_count == 0 ||
        request.buffer_count > NativeAdrenoAbiMaxBuffers) {
        return kIOReturnBadArgument;
    }
    if (!tb710fu_gpu_core_ring_ready()) {
        return kIOReturnNotReady;
    }
    bool queueFound = (request.queue == 0);
    for (unsigned i = 0; !queueFound && i < kMaxQueues; ++i) {
        if (fQueue[i] && fQueue[i] == request.queue) {
            queueFound = true;
            break;
        }
    }
    if (!queueFound) {
        return kIOReturnBadArgument;
    }

    uint64_t iovas[NativeAdrenoAbiMaxBuffers] = {};
    uint64_t bytes[NativeAdrenoAbiMaxBuffers] = {};
    for (uint32_t i = 0; i < request.buffer_count; ++i) {
        const NativeAdrenoAbiSubmitBuffer &buffer = request.buffers[i];
        const int slot = slotFor(buffer.handle);
        if (slot < 0 || !fBo[slot].mapped ||
            buffer.bytes == 0 || (buffer.bytes & 3ull) != 0 ||
            buffer.iova < fBo[slot].iova ||
            (buffer.iova - fBo[slot].iova) > fBo[slot].bytes ||
            buffer.bytes > fBo[slot].bytes - (buffer.iova - fBo[slot].iova)) {
            return kIOReturnBadArgument;
        }
        iovas[i] = buffer.iova;
        bytes[i] = buffer.bytes;
    }
    /* Clean CPU dcache on mapped owned BOs (Normal Cacheable user mappings ->
     * non-coherent A750 SMMU) so IBs, shaders, and descriptors are visible to GPU. */
    for (unsigned s = 0; s < kMaxBo; ++s) {
        if (!fBo[s].used || !fBo[s].mapped || fBo[s].imported || !fBo[s].memory) {
            continue;
        }
        IOBufferMemoryDescriptor *buf = OSDynamicCast(IOBufferMemoryDescriptor, fBo[s].memory);
        if (!buf) {
            continue;
        }
        const uint8_t *base = static_cast<const uint8_t *>(buf->getBytesNoCopy());
        const uint64_t len = fBo[s].bytes;
        if (base && len) {
            const uintptr_t start = reinterpret_cast<uintptr_t>(base) & ~63ull;
            const uintptr_t end = (reinterpret_cast<uintptr_t>(base) + len + 63ull) & ~63ull;
            for (uintptr_t a = start; a < end; a += 64) {
                __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
            }
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    uint64_t fence = 0;
    if (tb710fu_gpu_core_submit(iovas, bytes, request.buffer_count, request.wait_fence,
                                request.wait_timeout_ns, &fence,
                                reinterpret_cast<uint64_t>(this)) != 0) {
        return kIOReturnError;
    }
    NativeAdrenoAbiSubmitReply reply = {};
    reply.fence = fence;
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::waitFence(const NativeAdrenoAbiWaitFence &request)
{
    if (!request.fence) {
        return kIOReturnSuccess;
    }
    if (!tb710fu_gpu_core_ring_ready()) {
        return kIOReturnNotReady;
    }
    if (tb710fu_gpu_core_wait_fence(request.fence, request.timeout_ns, nullptr) != 0) {
        return kIOReturnTimeout;
    }
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::deviceInfo(uint8_t *out)
{
    Tb710fuGpuCoreDeviceInfo info = {};
    tb710fu_gpu_core_device_info(&info);
    NativeAdrenoAbiDeviceInfo reply = {};
    reply.chip_id = info.chipId;
    reply.gmem_bytes = info.gmemBytes;
    reply.gmem_base = info.gmemBase;
    reply.uche_trap_base = info.ucheTrapBase;
    if (!ensureVaSlice()) {
        return kIOReturnNoResources;
    }
    reply.va_start = fVaStart;
    reply.va_size = fVaSize;
    reply.highest_bank_bit = info.highestBankBit;
    reply.ubwc_swizzle = info.ubwcSwizzle;
    reply.macrotile_mode = info.macrotileMode;
    reply.ubwc_version = info.ubwcVersion;
    reply.flags = static_cast<uint32_t>(NativeAdrenoDeviceClientIova);
    static volatile uint32_t gInfoLogged = 0;
    if (__atomic_fetch_add(&gInfoLogged, 1u, __ATOMIC_RELAXED) < 4u) {
        IOLog("TB710FU GPU: client[%d] GetDeviceInfo chip=0x%x gmem=%u hbb=%u va=[0x%llx,+0x%llx)\n",
              fVaSlot, reply.chip_id, reply.gmem_bytes, reply.highest_bank_bit,
              (unsigned long long)reply.va_start, (unsigned long long)reply.va_size);
    }
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::timestamp(uint8_t *out)
{
    NativeAdrenoAbiTimestamp reply = {};
    reply.ticks = tb710fu_gpu_core_timestamp();
    copyOut(out, sizeof(reply), &reply, sizeof(reply));
    return kIOReturnSuccess;
}


IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                            IOExternalMethodDispatch *dispatch, OSObject *target,
                                            void *reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;
    if (!arguments) {
        return kIOReturnBadArgument;
    }
    tb710fu_gpu_stats[TB710FU_GPU_LAST_METHOD] = selector;

    const uint8_t *input = static_cast<const uint8_t *>(arguments->structureInput);
    uint8_t *output = static_cast<uint8_t *>(arguments->structureOutput);
    const uint32_t inputSize = arguments->structureInputSize;
    const uint32_t outputSize = arguments->structureOutputSize;

    uint32_t method = selector;
    uint32_t payload = 0;
    if (input && inputSize >= 8) {
        uint32_t header[2];
        memcpy(header, input, sizeof(header));
        if (header[0] == kTunnelMagic) {
            if (header[1] == kProbeMethod) {
                return kIOReturnSuccess;
            }
            method = header[1];
            payload = 8;
        }
    }
    IOReturn ret = run(method, input ? input + payload : nullptr, inputSize - payload,
                       output, outputSize);
    if (ret != kIOReturnSuccess) {
        static volatile uint32_t gErrLogs = 0;
        uint32_t n = __atomic_fetch_add(&gErrLogs, 1u, __ATOMIC_RELAXED);
        if (n < 16u) {
            IOLog("TB710FU GPU: client method=%u err=0x%x in=%u out=%u\n",
                  method, (unsigned)ret, inputSize - payload, outputSize);
        }
    } else if (method == NativeAdrenoMethodSubmit) {
        static volatile uint32_t gSubLogs = 0;
        uint32_t n = __atomic_fetch_add(&gSubLogs, 1u, __ATOMIC_RELAXED);
        if (n < 5u) {
            IOLog("TB710FU GPU: client Submit #%u ok\n", n + 1u);
        }
    }
    return ret;
}

IOReturn AGXDeviceUserClient::run(uint32_t method, const uint8_t *in, uint32_t inSize,
                                  uint8_t *out, uint32_t outSize)
{
    uint32_t wantIn = 0;
    uint32_t wantOut = 0;
    expectedSizes(method, &wantIn, &wantOut);
    if (method < NativeAdrenoMethodQuery || method > NativeAdrenoMethodNote) {
        return kIOReturnUnsupported;
    }
    if (inSize != wantIn || outSize != wantOut || !plausible(in, inSize) || !plausible(out, outSize)) {
        return kIOReturnBadArgument;
    }
    if (!fLock) {
        return kIOReturnNotReady;
    }

    IOReturn result = kIOReturnSuccess;
    IOLockLock(fLock);
    switch (method) {
    case NativeAdrenoMethodQuery: {
        NativeAdrenoAbiParams reply = {};
        reply.abi_version = NativeAdrenoAbiVersion;
        reply.input_bits = 32;  /* the SMMU walks 32-bit input addresses */
        reply.output_bits = 40; /* OAS from the SMMU ID registers */
        reply.flags = NativeAdrenoParamsHostImport;
        reply.max_bo_bytes = kMaxBoBytes;
        copyOut(out, outSize, &reply, sizeof(reply));
        break;
    }
    case NativeAdrenoMethodBoAlloc: {
        NativeAdrenoAbiBoAlloc request = {};
        memcpy(&request, in, sizeof(request));
        result = allocBo(request, out);
        break;
    }
    case NativeAdrenoMethodBoFree: {
        NativeAdrenoAbiBoFree request = {};
        memcpy(&request, in, sizeof(request));
        result = freeBo(request);
        break;
    }
    case NativeAdrenoMethodBoCpuMap: {
        NativeAdrenoAbiBoCpuMap request = {};
        memcpy(&request, in, sizeof(request));
        result = cpuMap(request, out);
        break;
    }
    case NativeAdrenoMethodBoCpuUnmap: {
        NativeAdrenoAbiBoCpuUnmap request = {};
        memcpy(&request, in, sizeof(request));
        if (slotFor(request.handle) < 0 || (uint32_t)(slotFor(request.handle) + 1) != request.token) {
            result = kIOReturnBadArgument;
        }
        break;
    }
    case NativeAdrenoMethodSetMetadata: {
        NativeAdrenoAbiMetadata request = {};
        memcpy(&request, in, sizeof(request));
        result = setMetadata(request);
        break;
    }
    case NativeAdrenoMethodGetMetadata: {
        NativeAdrenoAbiMetadata request = {};
        memcpy(&request, in, sizeof(request));
        result = getMetadata(request, out);
        break;
    }
    case NativeAdrenoMethodCreateQueue: {
        NativeAdrenoAbiQueue request = {};
        memcpy(&request, in, sizeof(request));
        result = createQueue(request, out);
        break;
    }
    case NativeAdrenoMethodCloseQueue: {
        NativeAdrenoAbiQueue request = {};
        memcpy(&request, in, sizeof(request));
        bool found = false;
        for (unsigned i = 0; i < kMaxQueues; ++i) {
            if (fQueue[i] && fQueue[i] == request.queue) {
                fQueue[i] = 0;
                found = true;
                break;
            }
        }
        result = found ? kIOReturnSuccess : kIOReturnBadArgument;
        break;
    }
    case NativeAdrenoMethodGetStatus: {
        /* No ring, so no fault and no suspend has ever been observed through it;
         * a nonzero value would be invented. */
        NativeAdrenoAbiStatus reply = {};
        copyOut(out, outSize, &reply, sizeof(reply));
        break;
    }
    case NativeAdrenoMethodSetPriority: {
        NativeAdrenoAbiPriority request = {};
        memcpy(&request, in, sizeof(request));
        if (request.foreground > 5 || request.background > 5 || request.foreground == 3) {
            result = kIOReturnBadArgument;
        } else {
            /* Recorded for the boot page; nothing schedules on it before a ring
             * exists, and pretending it changed GPU scheduling would be false. */
            tb710fu_gpu_stats[TB710FU_GPU_PRIORITY] =
                request.foreground | (request.background << 8);
        }
        break;
    }
    case NativeAdrenoMethodNote: {
        NativeAdrenoAbiNote request = {};
        memcpy(&request, in, sizeof(request));
        if (request.count > NativeAdrenoNoteWords ||
            (request.kind != NativeAdrenoNotePass && request.kind != NativeAdrenoNotePipeline)) {
            result = kIOReturnBadArgument;
        } else if (fNotesPrinted == 0 && request.count >= 2) {
            fNotesPrinted = 1;
            printf("TB710FU GPU: client note kind=%u count=%u first=%u,%u\n",
                   request.kind, request.count, request.words[0], request.words[1]);
        }
        break;
    }
    case NativeAdrenoMethodWaitFence: {
        NativeAdrenoAbiWaitFence request = {};
        memcpy(&request, in, sizeof(request));
        result = waitFence(request);
        break;
    }
    case NativeAdrenoMethodBoMap: {
        NativeAdrenoAbiBoMap request = {};
        memcpy(&request, in, sizeof(request));
        result = mapBo(request, out);
        break;
    }
    case NativeAdrenoMethodBoImport: {
        NativeAdrenoAbiBoImport request = {};
        memcpy(&request, in, sizeof(request));
        result = importBo(request, out);
        break;
    }
    case NativeAdrenoMethodSubmit: {
        NativeAdrenoAbiSubmit request = {};
        memcpy(&request, in, sizeof(request));
        result = submit(request, out);
        break;
    }
    case NativeAdrenoMethodGetTimestamp:
        result = timestamp(out);
        break;
    case NativeAdrenoMethodGetDeviceInfo:
        result = deviceInfo(out);
        break;
    default:
        result = kIOReturnUnsupported;
        break;
    }
    IOLockUnlock(fLock);
    return result;
}

IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                   IOMemoryDescriptor **memory)
{
    if (memory) {
        *memory = nullptr;
    }
    if (options) {
        *options = kIOMapAnywhere;
    }
    if (!memory || type == 0 || type > kMaxBo || !fLock) {
        return kIOReturnUnsupported;
    }
    const unsigned slot = type - 1;
    IOLockLock(fLock);
    IOMemoryDescriptor *descriptor =
        (fBo[slot].used && fBo[slot].memory && !fBo[slot].imported) ? fBo[slot].memory : nullptr;
    if (descriptor) {
        descriptor->retain();
        *memory = descriptor;
    }
    IOLockUnlock(fLock);
    return descriptor ? kIOReturnSuccess : kIOReturnUnsupported;
}

extern "C" IOUserClient *tb710fu_gpu_client_create(task_t owningTask, void *securityID,
                                                    UInt32 type, OSDictionary *properties)
{
    AGXDeviceUserClient *client = new AGXDeviceUserClient;
    if (!client) {
        return nullptr;
    }
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return nullptr;
    }
    tb710fu_gpu_stats[TB710FU_GPU_CLIENTS] = tb710fu_gpu_stats[TB710FU_GPU_CLIENTS] + 1;
    return client;
}
