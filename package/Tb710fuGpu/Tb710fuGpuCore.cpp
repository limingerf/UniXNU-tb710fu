// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Milestone 3 GPU core (see Tb710fuGpuCore.h).
 *
 * Sequencing, register offsets and message formats are taken from the mainline
 * Linux sm8650 Adreno 750 driver on disk:
 *   - a6xx_gmu.c  a6xx_gmu_fw_start / a6xx_gmu_start / a6xx_gmu_rpmh_init /
 *                 a6xx_gmu_power_config / a6xx_hfi_start / a6xx_hfi_send_msg /
 *                 a6xx_gmu_set_oob / a6xx_gmu_set_freq
 *   - a6xx_hfi.c  perf / bw table builders, GX_BW_PERF_VOTE
 *   - a6xx_gpu.c  a7xx_cp_init, UCHE_GMEM_RANGE, SECVID_TSB
 *   - a6xx_gmu.xml / a6xx.xml / a6xx_hfi.h for every register and layout
 * plus the hardware-proven UniXNU Adreno740 ring/fence/CP-init and ArmSmmu4K.
 *
 * A750 is ADRENO_7XX_GEN3, not a740-family, so this uses the generic RSCC
 * SEQ_MEM_0_DRV0 offset (0x600), the 1 MiB GMEM VA base, and the
 * 0x80000000 hidden-TCS data value. Those choices are recorded in
 * docs/GPU_METAL_M3.md; nothing here guesses at a register value.
 *
 * HFI/GMU/RING buffers are ordinary cached kernel memory reached by the GMU
 * and CP through SMMU non-coherent walks, so every handoff is bracketed by
 * explicit dc cvac (producer) / dc ivac (consumer) on the CPU side. No MMIO
 * region is ever touched with memcpy/bzero/memset.
 */
#include "Tb710fuGpuCore.h"
#include "Tb710fuGpuFirmware.h"
#include "Tb710fuGpuHw.h"
#include "Tb710fuGpuSmmu.h"
#include "../Tb710fuAdsp/QcomRpmh.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/libkern.h>
#include <string.h>

namespace {

/* ---- small helpers ---------------------------------------------------- */

const uint32_t kPageBytes = 4096;
const uint32_t kSmmuPoolBytes = 2u * 1024u * 1024u;
IOLock *gCoreLock = nullptr;

const uint32_t kPdcEnableValue = 0x80000001u;
const uint32_t kGmuFenceRange = (1u << 31) | (0x32u << 18) | 0x8a0u;
const uint32_t kCm3Cfg = 0x4052u;
const uint32_t kMsmRbCntl = 0x20du;      /* 64 KiB ring, 32 B blocks (A740) */
const uint32_t kScratchFenceSeq = 1u;    /* the self-test fence */

const uint32_t kGmuAsid = 1;
const uint32_t kGpuAsid = 2;

const uint32_t kArcGfxLevels[] = {
    0, 52, 56, 60, 64, 80, 128, 144, 192, 224, 256, 320
};
const uint32_t kArcGfxFreqKhz[] = {
    0, 231000, 310000, 366000, 422000, 500000, 578000,
    629000, 680000, 720000, 770000, 834000
};
const uint32_t kArcGmuLevels[] = {0, 64, 128};
const uint32_t kArcGmuFreqKhz[] = {0, 260000, 625000};
/* dev_pm_opp bandwidth table after a6xx_gmu_build_bw_table(): an explicit
 * zero level followed by the distinct opp-peak-kBps corners of the A750 OPP
 * table. */
const uint32_t kArcBwKhz[] = {
    0, 2136718, 6074218, 8171875, 10687500, 12449218, 14398437
};

const uint32_t kHfiQueueWords = 1024;
const uint32_t kHfiTableDwords = 168;

void cpuClean(const void *base, uint32_t bytes)
{
    const uint8_t *at = static_cast<const uint8_t *>(base);
    const uint8_t *end = at + (bytes & ~63u);
    for (; at < end; at += 64) {
        __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void cpuInvalidate(const void *base, uint32_t bytes)
{
    const uint8_t *at = static_cast<const uint8_t *>(base);
    const uint8_t *end = at + (bytes & ~63u);
    for (; at < end; at += 64) {
        __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t ilog2u(uint32_t value)
{
    uint32_t log = 0;
    while (value >>= 1) {
        ++log;
    }
    return log;
}

uint32_t arcIndex(const QcomRpmhResource *resource, uint32_t level, bool clamp)
{
    if (!resource || !resource->level_count) {
        return 0;
    }
    for (uint32_t i = 0; i < resource->level_count; ++i) {
        if (resource->levels[i] >= level) {
            return i;
        }
    }
    return clamp ? resource->level_count - 1 : 0;
}

/* ---- one physically contiguous kernel buffer -------------------------- */

struct Buffer {
    IOBufferMemoryDescriptor *memory;
    uint8_t *cpu;
    addr64_t pa;
    uint32_t bytes;
};

int bufferAlloc(Buffer *buffer, uint32_t bytes)
{
    if (!buffer || bytes == 0) {
        return -1;
    }
    memset(buffer, 0, sizeof(*buffer));
    IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                           static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    buffer->memory = IOBufferMemoryDescriptor::withOptions(options, bytes, kPageBytes);
    if (!buffer->memory) {
        return -1;
    }
    buffer->cpu = static_cast<uint8_t *>(buffer->memory->getBytesNoCopy());
    IOByteCount length = 0;
    buffer->pa = buffer->memory->getPhysicalSegment(0, &length,
                                                    static_cast<IOOptionBits>(kIOMemoryMapperNone));
    if (!buffer->cpu || !buffer->pa || (buffer->pa & (kPageBytes - 1)) || length < bytes) {
        buffer->memory->release();
        memset(buffer, 0, sizeof(*buffer));
        return -1;
    }
    buffer->bytes = bytes;
    bzero(buffer->cpu, bytes);
    return 0;
}

__attribute__((unused)) void bufferFree(Buffer *buffer)
{
    if (buffer && buffer->memory) {
        buffer->memory->release();
        memset(buffer, 0, sizeof(*buffer));
    }
}

/* ---- state ------------------------------------------------------------ */

struct CoreState {
    bool windowsBound;
    Tb710fuGpuCoreWindows windows;
    Tb710fuGpuCoreReport *report;

    Tb710fuGpuSmmu smmu;
    bool smmuReady;

    Buffer icache;    /* 0x4000, 4 MiB */
    Buffer dummy;     /* 0x60000000, 16 KiB */
    Buffer log;       /* 0x60004000, 16 KiB */
    Buffer hfi;       /* 0x60008000, 16 KiB */
    Buffer debug;     /* 0x60400000, 28 KiB */

    Buffer ring;      /* 0x70000000, 64 KiB */
    Buffer fence;     /* 0x70010000, 4 KiB */
    Buffer pwrup;     /* 0x70011000, 4 KiB */
    Buffer sqe;       /* 0x70020000, 0x16000 */
    Buffer aqe;       /* 0x70030000, 0x9000 */

    uint32_t hfiSeq;
    uint32_t hfiError;
    uint32_t ringWptr;   /* dwords the CP has not consumed yet, from our view */
    uint32_t ringSeq;    /* last fence sequence emitted */
    bool ringReady;

    uint32_t gxPowerState;
    uint32_t rbbmStatus;
    uint32_t chipId;
    uint64_t gmemBase;
    uint32_t gmemBytes;
    uint32_t lastFence;

    Buffer cmdDb;
};

CoreState gCore;

int issueFenceSelfTest(uint32_t *observed);
int writeFirmwareToBuffer(const char *name, uint64_t expectedBytes, const char *sha,
                          Buffer *buffer);

/* ---- register helpers ------------------------------------------------- */

int gmuWrite(uint32_t offset, uint32_t value)
{
    return tb710fu_gpu_write32(gCore.windows.gmu, offset, value);
}

int gmuRead(uint32_t offset, uint32_t *value)
{
    return tb710fu_gpu_read32(gCore.windows.gmu, offset, value);
}

int gpuWrite(uint32_t offset, uint32_t value)
{
    return tb710fu_gpu_write32(gCore.windows.gpu, offset, value);
}

int gpuWrite64(uint32_t offset, uint64_t value)
{
    if (gpuWrite(offset, static_cast<uint32_t>(value)) != 0) {
        return -1;
    }
    return gpuWrite(offset + 4u, static_cast<uint32_t>(value >> 32));
}

int gpuRead(uint32_t offset, uint32_t *value)
{
    return tb710fu_gpu_read32(gCore.windows.gpu, offset, value);
}

int rsccWrite(uint32_t offset, uint32_t value)
{
    return tb710fu_gpu_write32(gCore.windows.rscc, offset, value);
}

int pdcWrite(uint32_t offset, uint32_t value)
{
    return tb710fu_gpu_write32(gCore.windows.pdc, offset, value);
}

/* Polls `read` until (value & mask) == want; 0 on success, -1 on timeout. */
template <typename ReadFn>
int pollRegister(ReadFn read, uint32_t mask, uint32_t want, uint32_t tries, uint32_t delayUs,
                 uint32_t *last)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < tries; ++i) {
        if (read(&value) != 0) {
            return -1;
        }
        if ((value & mask) == want) {
            if (last) {
                *last = value;
            }
            return 0;
        }
        tb710fu_gpu_delay_us(delayUs);
    }
    if (last) {
        *last = value;
    }
    return -1;
}

/* ---- SMMU mapping of one buffer --------------------------------------- */

int mapBuffer(uint32_t bank, uint64_t iova, const Buffer &buffer, uint32_t flags)
{
    return tb710fu_gpu_smmu_map(&gCore.smmu, bank, iova, buffer.memory, buffer.bytes, flags);
}

/* ---- GMU firmware container (a6xx_gmu.c struct block_header) ---------- */

struct FwBlockHeader {
    uint32_t addr;
    uint32_t size;
    uint32_t type;
    uint32_t value;
};

int gmuFirmwareLoad(void *image, uint64_t bytes)
{
    const uint8_t *data = static_cast<const uint8_t *>(image);
    uint64_t at = 0;
    bool wroteItcm = false;
    while (at + sizeof(FwBlockHeader) <= bytes) {
        FwBlockHeader header;
        memcpy(&header, data + at, sizeof(header));
        const uint64_t next = at + sizeof(FwBlockHeader) + header.size;
        if (next > bytes) {
            return -1;
        }
        if (header.size == 0) {
            at = next;
            continue;
        }
        const uint8_t *payload = data + at + sizeof(FwBlockHeader);
        if (header.addr < 0x4000u) {
            /* ITCM: 16 KiB at GMU_CM3_ITCM_START, offsets in bytes. */
            if (header.addr + header.size > 0x4000u) {
                return -1;
            }
            if (tb710fu_gpu_mmio_copy32(gCore.windows.gmu, TBGPU_GMU_ITCM_START + header.addr,
                                        payload, header.size) != 0) {
                return -1;
            }
            wroteItcm = true;
        } else if (header.addr >= 0x10004000u && header.addr + header.size <= 0x10008000u) {
            /* DTCM: 16 KiB at GMU_CM3_DTCM_START. */
            const uint32_t offset = TBGPU_GMU_DTCM_START + (header.addr - 0x10004000u);
            if (tb710fu_gpu_mmio_copy32(gCore.windows.gmu, offset, payload, header.size) != 0) {
                return -1;
            }
        } else if (header.addr >= TBGPU_GMU_ICACHE_IOVA &&
                   header.addr + header.size <= TBGPU_GMU_ICACHE_IOVA + gCore.icache.bytes) {
            memcpy(gCore.icache.cpu + (header.addr - TBGPU_GMU_ICACHE_IOVA), payload, header.size);
        } else {
            /* gen70900 also lists prealloc requests (type 1/7) with size 0;
             * a non-empty block outside the mapped apertures is a real error. */
            return -1;
        }
        at = next;
    }
    if (!wroteItcm) {
        return -1;
    }
    cpuClean(gCore.icache.cpu, gCore.icache.bytes);
    return 0;
}

/* ---- HFI -------------------------------------------------------------- */

uint32_t *hfiTable()
{
    return reinterpret_cast<uint32_t *>(gCore.hfi.cpu);
}

uint32_t *hfiQueue(uint32_t queue)
{
    return reinterpret_cast<uint32_t *>(gCore.hfi.cpu + (queue + 1u) * kPageBytes);
}

void hfiQueueInit()
{
    uint32_t *table = hfiTable();
    bzero(table, kHfiTableDwords * sizeof(uint32_t));
    table[0] = 0;
    table[1] = 168;
    table[2] = 6;
    table[3] = 12;
    table[4] = 3;
    table[5] = 3;
    for (uint32_t q = 0; q < 3; ++q) {
        const uint32_t at = 6 + 12 * q;
        table[at + 0] = 1;      /* status: ready */
        table[at + 1] = static_cast<uint32_t>(TBGPU_GMU_HFI_IOVA + (q + 1) * kPageBytes);
        table[at + 2] = static_cast<uint32_t>(TBGPU_HFI_DEVICE_QUEUE_TYPE | q);
        table[at + 3] = kHfiQueueWords;
        table[at + 4] = 0;      /* message size: variable */
        table[at + 5] = 0;      /* dropped */
        table[at + 6] = 1;      /* rx watermark */
        table[at + 7] = 1;      /* tx watermark */
        table[at + 8] = 1;      /* rx requested */
        table[at + 9] = 0;      /* tx requested */
        table[at + 10] = 0;     /* read index */
        table[at + 11] = 0;     /* write index */
    }
    cpuClean(gCore.hfi.cpu, 4u * kPageBytes);
    tb710fu_gpu_barrier();
}

int hfiSend(uint32_t messageId, uint32_t dwords, const uint32_t *payload)
{
    if (!gCore.hfi.cpu || dwords > kHfiQueueWords - 2) {
        return -1;
    }
    uint32_t *table = hfiTable();
    const uint32_t at = 6;   /* queue 0: commands */
    uint32_t write = table[at + 11];
    uint32_t seq = gCore.hfiSeq + 1;
    if (seq == 0 || seq > 0xfff) {
        seq = 1;
    }
    gCore.hfiSeq = seq;
    const uint32_t header = (seq << 20) | (0u << 16) |
                            (((dwords + 1) & 0xffu) << 8) | (messageId & 0xffu);
    uint32_t *queue = hfiQueue(0);
    const uint32_t total = dwords + 1;
    const uint32_t cmdRead = table[at + 10];
    if (((cmdRead - write - 1u + kHfiQueueWords) % kHfiQueueWords) < total + 3u) {
        return -1;
    }
    queue[write] = header;
    for (uint32_t i = 0; i < dwords; ++i) {
        queue[(write + 1 + i) % kHfiQueueWords] = payload[i];
    }
    write = (write + total) % kHfiQueueWords;
    /* Non-legacy HFI pads every message to a 4-dword boundary with the
     * 0xfafafafa cook pattern (a6xx_hfi_queue_write). */
    while ((write & 3u) != 0) {
        queue[write] = 0xfafafafau;
        write = (write + 1) % kHfiQueueWords;
    }
    cpuClean(gCore.hfi.cpu + kPageBytes, kPageBytes);
    table[at + 11] = write;
    cpuClean(gCore.hfi.cpu, kPageBytes);
    tb710fu_gpu_barrier();
    if (gmuWrite(TBGPU_GMU_HOST2GMU_INTR_SET, 1u << 0) != 0) {
        return -1;
    }

    /* Wait for the matching ACK on queue 1; match by echoed sequence number
     * just like AdrenoHfi.c (A7xx GMU ACKs carry type=1 / echoed seq rather
     * than always id==126). */
    const uint32_t ackAt = 6 + 12;
    uint32_t *ackQueue = hfiQueue(1);
    uint32_t lastRespHdr = 0, lastRespRet = 0, lastRespErr = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
        cpuInvalidate(gCore.hfi.cpu, kPageBytes);
        cpuInvalidate(gCore.hfi.cpu + 2u * kPageBytes, kPageBytes);
        uint32_t *t = hfiTable();
        const uint32_t read = t[ackAt + 10];
        const uint32_t ackWrite = t[ackAt + 11];
        if (read != ackWrite) {
            const uint32_t response = ackQueue[read];
            const uint32_t id = response & 0xffu;
            const uint32_t words = (response >> 8) & 0xffu;
            const uint32_t retHeader = ackQueue[(read + 1) % kHfiQueueWords];
            const uint32_t ackError = ackQueue[(read + 2) % kHfiQueueWords];
            lastRespHdr = response;
            lastRespRet = retHeader;
            lastRespErr = ackError;
            if (id == TBGPU_HFI_MSG_ERROR) {
                gCore.hfiError = retHeader;
                gCore.report->hfiError = gCore.hfiError;
                const uint32_t advance = (words < 2u) ? 2u : words;
                const uint32_t next = (read + advance + 3u) & ~3u;
                t[ackAt + 10] = next % kHfiQueueWords;
                cpuClean(gCore.hfi.cpu, kPageBytes);
                gmuWrite(TBGPU_GMU_GMU2HOST_INTR_CLR, 1u);
                IOLog("TB710FU GPU: HFI F2H_ERROR msg=%u seq=%u hdr=0x%x code=0x%x\n",
                      messageId, seq, response, retHeader);
                continue;
            }
            const uint32_t ackSeq = (retHeader >> 20) & 0xfffu;
            if (words >= 3u && (id == TBGPU_HFI_MSG_ACK || ackSeq == seq)) {
                const uint32_t advance = (words < 3u) ? 3u : words;
                const uint32_t next = (read + advance + 3u) & ~3u;
                t[ackAt + 10] = next % kHfiQueueWords;
                cpuClean(gCore.hfi.cpu, kPageBytes);
                if (ackError) {
                    gCore.hfiError = ackError;
                    gCore.report->hfiError = ackError;
                    gmuWrite(TBGPU_GMU_GMU2HOST_INTR_CLR, 1u);
                    IOLog("TB710FU GPU: HFI ACK error msg=%u seq=%u hdr=0x%x ret=0x%x err=0x%x\n",
                          messageId, seq, response, retHeader, ackError);
                    return -1;
                }
                if (ackSeq == seq) {
                    gCore.report->hfiSeq = seq;
                    gmuWrite(TBGPU_GMU_GMU2HOST_INTR_CLR, 1u);
                    return 0;
                }
                continue;
            }
            /* Unknown response type: drop it and keep waiting. */
            const uint32_t advance = (words < 2u) ? 2u : words;
            const uint32_t next = (read + advance + 3u) & ~3u;
            t[ackAt + 10] = next % kHfiQueueWords;
            cpuClean(gCore.hfi.cpu, kPageBytes);
        }
        tb710fu_gpu_delay_us(100);
    }
    uint32_t gmuIntr = 0;
    gmuRead(TBGPU_GMU_GMU2HOST_INTR_INFO, &gmuIntr);
    cpuInvalidate(gCore.hfi.cpu, kPageBytes);
    uint32_t *t = hfiTable();
    IOLog("TB710FU GPU: HFI timeout msg=%u seq=%u cmd=%u/%u ack=%u/%u intr=0x%x gfsr=0x%x cb1fsr=0x%x last=0x%x/0x%x/0x%x\n",
          messageId, seq, t[at + 10], t[at + 11], t[ackAt + 10], t[ackAt + 11], gmuIntr,
          tb710fu_gpu_smmu_global_fault(&gCore.smmu), tb710fu_gpu_smmu_bank_fault(&gCore.smmu, 1),
          lastRespHdr, lastRespRet, lastRespErr);
    gCore.hfiError = 0xffffffffu;
    gCore.report->hfiError = gCore.hfiError;
    return -1;
}

/* ---- command DB and ARC votes ----------------------------------------- */

struct ArcSet {
    QcomRpmhResource gfx;
    QcomRpmhResource gmxc;
    QcomRpmhResource mx;
    QcomRpmhResource cx;
    QcomRpmhResource sh0;
    QcomRpmhResource mc0;
    QcomRpmhResource acv;
    QcomRpmhResource cn0;
    bool usable;
};

ArcSet gArcs;
bool gArcLoaded;

uint32_t arcVote(const QcomRpmhResource *primary, const QcomRpmhResource *secondary, uint32_t level)
{
    const uint32_t pindex = arcIndex(primary, level, false);
    if (!primary->level_count) {
        return 0;
    }
    const uint32_t sindex = arcIndex(secondary, primary->levels[pindex], true);
    return ((uint32_t)primary->levels[pindex] << 16) | ((sindex & 0xffu) << 8) | (pindex & 0xffu);
}

int cmdDbLoad(ArcSet *arcs)
{
    memset(arcs, 0, sizeof(*arcs));
    Tb710fuGpuWindow dbWindow = {};
    if (tb710fu_gpu_window_map(&dbWindow, 0x81c60000ull, 0x20000ull, kIOMapInhibitCache) != 0) {
        return -1;
    }
    if (bufferAlloc(&gCore.cmdDb, 0x20000) != 0) {
        tb710fu_gpu_window_unmap(&dbWindow);
        return -1;
    }
    for (uint32_t at = 0; at < 0x20000; at += 4) {
        uint32_t value = 0;
        if (tb710fu_gpu_read32(&dbWindow, at, &value) != 0) {
            tb710fu_gpu_window_unmap(&dbWindow);
            return -1;
        }
        memcpy(gCore.cmdDb.cpu + at, &value, sizeof(value));
    }
    tb710fu_gpu_window_unmap(&dbWindow);

    const uint8_t *db = gCore.cmdDb.cpu;
    arcs->usable = true;
    if (qcom_cmd_db_find(db, gCore.cmdDb.bytes, "gfx.lvl", &arcs->gfx) != QcomRpmhOk ||
        qcom_cmd_db_find(db, gCore.cmdDb.bytes, "cx.lvl", &arcs->cx) != QcomRpmhOk ||
        qcom_cmd_db_find(db, gCore.cmdDb.bytes, "mx.lvl", &arcs->mx) != QcomRpmhOk) {
        arcs->usable = false;
    }
    if (qcom_cmd_db_find(db, gCore.cmdDb.bytes, "gmxc.lvl", &arcs->gmxc) != QcomRpmhOk) {
        arcs->gmxc = arcs->mx;
    }
    const struct {
        const char *name;
        QcomRpmhResource *resource;
    } bandwidth[] = {
        {"SH0", &arcs->sh0}, {"MC0", &arcs->mc0}, {"ACV", &arcs->acv}, {"CN0", &arcs->cn0},
    };
    for (unsigned i = 0; i < sizeof(bandwidth) / sizeof(bandwidth[0]); ++i) {
        qcom_cmd_db_find(db, gCore.cmdDb.bytes, bandwidth[i].name, bandwidth[i].resource);
    }
    return 0;
}

/* ---- message payloads ------------------------------------------------- */

int sendPerfTable()
{
    /* struct a6xx_hfi_msg_perf_table is a fixed 59-dword image: 3 header
     * dwords, 16 gx slots and 4 cx slots. Only 12 gx / 3 cx are populated. */
    uint32_t table[59] = {};
    table[1] = 12;            /* num_gpu_levels: off + 11 A750 OPPs */
    table[2] = 3;             /* num_gmu_levels: off + 2 GMU OPPs */
    for (uint32_t i = 0; i < 12; ++i) {
        table[3 + 3 * i + 0] = 0;      /* filled below */
        table[3 + 3 * i + 1] = 0xffffffffu;  /* acd */
        table[3 + 3 * i + 2] = kArcGfxFreqKhz[i];
    }
    for (uint32_t i = 0; i < 3; ++i) {
        table[3 + 48 + 2 * i + 0] = 0;
        table[3 + 48 + 2 * i + 1] = kArcGmuFreqKhz[i];
    }

    ArcSet &arcs = gArcs;
    if (!gArcLoaded) {
        cmdDbLoad(&gArcs);   /* a missing command DB leaves usable=false */
        gArcLoaded = true;
    }
    if (arcs.usable) {
        for (uint32_t i = 0; i < 12; ++i) {
            table[3 + 3 * i] = arcVote(&arcs.gfx, &arcs.gmxc, kArcGfxLevels[i]);
        }
        for (uint32_t i = 0; i < 3; ++i) {
            table[3 + 48 + 2 * i] = arcVote(&arcs.cx, &arcs.mx, kArcGmuLevels[i]);
        }
    }
    return hfiSend(TBGPU_HFI_MSG_PERF_TABLE, 58, table + 1);
}

/* The command DB's BCM auxiliary record is { u32 unit; u16 width; u8 vcd;
 * u8 reserved; } (a6xx_gmu_rpmh_bw_votes_init). The shared QcomRpmh reader
 * only returns arc levels, so read this record from our own copy here. */
struct CmdDbBcm {
    uint32_t address;
    uint32_t unit;
    uint16_t width;
    uint8_t vcd;
    bool valid;
};

uint16_t cmdDbLe16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t cmdDbLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(cmdDbLe16(p)) | (static_cast<uint32_t>(cmdDbLe16(p + 2)) << 16);
}

bool cmdDbBcm(const char *name, CmdDbBcm *out)
{
    if (!gCore.cmdDb.cpu || gCore.cmdDb.bytes < 144 || !name || !out) {
        return false;
    }
    const uint8_t *db = gCore.cmdDb.cpu;
    const size_t bytes = gCore.cmdDb.bytes;
    if (cmdDbLe32(db + 4) != QCOM_CMD_DB_MAGIC) {
        return false;
    }
    char id[8] = {0};
    for (unsigned i = 0; i < 8 && name[i]; ++i) {
        id[i] = name[i];
    }
    for (unsigned slaveIndex = 0; slaveIndex < 8; ++slaveIndex) {
        const uint8_t *slave = db + 8 + 16 * slaveIndex;
        if (!cmdDbLe16(slave)) {
            break;
        }
        const size_t heads = 144u + cmdDbLe16(slave + 2);
        const size_t data = 144u + cmdDbLe16(slave + 4);
        const size_t count = cmdDbLe16(slave + 6);
        if (heads > bytes || count * 24u > bytes - heads) {
            return false;
        }
        for (size_t e = 0; e < count; ++e) {
            const uint8_t *entry = db + heads + 24 * e;
            bool same = true;
            for (unsigned i = 0; i < 8 && same; ++i) {
                same = entry[i] == static_cast<uint8_t>(id[i]);
            }
            if (!same) {
                continue;
            }
            const size_t length = cmdDbLe16(entry + 20);
            const size_t at = data + cmdDbLe16(entry + 22);
            if (length < 8 || at > bytes || length > bytes - at) {
                return false;
            }
            out->address = cmdDbLe32(entry + 16);
            out->unit = cmdDbLe32(db + at);
            out->width = cmdDbLe16(db + at + 4);
            out->vcd = db[at + 6];
            /* ACV is fixed and legitimately has width 0; SH0/MC0 callers
             * reject a zero width themselves. */
            out->valid = out->address != 0 && out->unit != 0;
            return out->valid;
        }
    }
    return false;
}

uint32_t bcmCmd(bool commit, bool valid, uint32_t voteX, uint32_t voteY)
{
    return (commit ? (1u << 30) : 0u) | (valid ? (1u << 29) : 0u) |
           ((voteX & 0x3fffu) << 14) | (voteY & 0x3fffu);
}

int sendBwTable()
{
    /* 1 header + 5 counts/masks + 6 CNOC addrs + 2*6 CNOC data
     * + 8 DDR addrs + 16*8 DDR data = 160 dwords. */
    uint32_t table[160] = {};
    if (!gArcLoaded) {
        cmdDbLoad(&gArcs);
        gArcLoaded = true;
    }
    ArcSet &arcs = gArcs;
    CmdDbBcm sh0 = {}, mc0 = {}, acv = {};
    if (!arcs.usable || !cmdDbBcm("SH0", &sh0) || !cmdDbBcm("MC0", &mc0) ||
        !cmdDbBcm("ACV", &acv) || !arcs.cn0.address || !sh0.width || !mc0.width) {
        return -1;
    }

    table[0] = 0;
    table[1] = 7;             /* bw_level_num = nr_gpu_bws */
    table[2] = 1;             /* cnoc_cmds_num: CN0 */
    table[3] = 3;             /* ddr_cmds_num: SH0, MC0, ACV */
    table[6] = arcs.cn0.address;
    table[12] = bcmCmd(true, false, 0, 0);
    table[18] = bcmCmd(true, true, 0, 1u << 0);
    table[24] = sh0.address;
    table[25] = mc0.address;
    table[26] = acv.address;

    /* BCM commit semantics: a command commits when the next BCM lives in a
     * different VCD; the last command always commits. */
    const bool sh0Commit = mc0.vcd != sh0.vcd;
    const bool mc0Commit = acv.vcd != mc0.vcd;
    for (unsigned level = 0; level < 7; ++level) {
        const uint32_t bw = kArcBwKhz[level];
        uint32_t *row = &table[32 + 8 * level];
        if (bw == 0) {
            row[0] = bcmCmd(sh0Commit, false, 0, 0);
            row[1] = bcmCmd(mc0Commit, false, 0, 0);
            row[2] = bcmCmd(true, false, 0, 0);
            continue;
        }
        for (unsigned cmd = 0; cmd < 2; ++cmd) {
            const CmdDbBcm &bcm = cmd == 0 ? sh0 : mc0;
            const uint16_t buswidth = cmd == 0 ? 16 : 4;
            uint64_t peak = static_cast<uint64_t>(bw) * bcm.width;
            peak /= buswidth;
            peak *= 1000;
            peak /= bcm.unit;
            if (peak > 0x3fffu) {
                peak = 0x3fffu;
            }
            if (peak == 0) {
                peak = 1;
            }
            const uint32_t vote = static_cast<uint32_t>(peak);
            row[cmd] = bcmCmd(cmd == 0 ? sh0Commit : mc0Commit, true, vote, vote);
        }
        /* ACV is fixed: A7xx votes perfmode only at/above perfmode_bw. */
        const uint32_t perfmode = (bw >= 10687500u) ? (1u << 2) : 0u;
        row[2] = bcmCmd(true, true, 0, perfmode);
    }

    uint32_t ddrWait = 0;
    for (unsigned cmd = 0; cmd < 3; ++cmd) {
        if (table[32 + cmd] & (1u << 30)) {
            ddrWait |= 1u << cmd;
        }
    }
    table[4] = 1;             /* CN0 row 0 carries commit */
    table[5] = ddrWait;
    return hfiSend(TBGPU_HFI_MSG_BW_TABLE, 159, table + 1);
}

int sendCoreFwStart()
{
    const uint32_t payload[1] = {0};   /* handle = 0 */
    return hfiSend(TBGPU_HFI_MSG_CORE_FW_START, 1, payload);
}

int sendStart()
{
    /* The v1 START message is the HFI header alone. */
    return hfiSend(TBGPU_HFI_MSG_START, 0, nullptr);
}

int sendIfpcFeature()
{
    /* A750 carries ADRENO_QUIRK_IFPC, so Linux enables HFI_FEATURE_IFPC
     * with IFPC_LONG_HYST (0x1680) before CORE_FW_START. */
    const uint32_t payload[3] = {9u /* HFI_FEATURE_IFPC */, 1u, 0x1680u};
    return hfiSend(TBGPU_HFI_MSG_FEATURE_CTRL, 3, payload);
}

uint32_t a750InitialBwVote()
{
    /* a6xx_gmu_set_freq(): the highest bandwidth corner, plus A750's 25%
     * AB (advertised bandwidth) vote in bits 31:16 and AB_VOTE_ENABLE in
     * bit 8. */
    const uint32_t maxBw = kArcBwKhz[6];
    const uint32_t level = 6;
    uint64_t tmp = static_cast<uint64_t>(maxBw) * 25u;
    tmp /= 100u;
    tmp *= 0xfffeu;
    tmp /= maxBw;
    if (tmp < 1) {
        tmp = 1;
    }
    if (tmp > 0xfffeu) {
        tmp = 0xfffeu;
    }
    return level | (static_cast<uint32_t>(tmp) << 16) | (1u << 8);
}

int sendGxVote(uint32_t freqIndex, uint32_t bwIndex)
{
    const uint32_t payload[3] = {1u /* blocking ack */, freqIndex, bwIndex};
    return hfiSend(TBGPU_HFI_MSG_GX_BW_VOTE, 3, payload);
}

/* ---- OOB handshake ---------------------------------------------------- */

int gmuSetOob(uint32_t requestBit, uint32_t ackBit)
{
    if (gmuWrite(TBGPU_GMU_HOST2GMU_INTR_SET, 1u << requestBit) != 0) {
        return -1;
    }
    uint32_t value = 0;
    int status = pollRegister(
        [&](uint32_t *out) { return gmuRead(TBGPU_GMU_GMU2HOST_INTR_INFO, out); },
        1u << ackBit, 1u << ackBit, 200, 100, &value);
    /* a6xx_gmu_set_oob() always clears the acknowledge interrupt. */
    gmuWrite(TBGPU_GMU_GMU2HOST_INTR_CLR, 1u << ackBit);
    return status;
}

/* ---- RSCC/PDC init (a6xx_gmu_rpmh_init, A7xx: PDC sequence lives in AOP) */

int rpmhInit()
{
    if (rsccWrite(TBGPU_RSCC_STATUS0, 1u << 24) != 0) {          /* disable SDE clock gating */
        return -1;
    }
    if (rsccWrite(TBGPU_RSCC_SLAVE_ID, 1) != 0 ||
        rsccWrite(TBGPU_RSCC_HIDDEN_TCS_CMD0_DATA, 0) != 0 ||
        rsccWrite(TBGPU_RSCC_HIDDEN_TCS_CMD0_ADDR, 0) != 0 ||
        rsccWrite(TBGPU_RSCC_HIDDEN_TCS_CMD0_DATA + 8, 0) != 0 ||
        rsccWrite(TBGPU_RSCC_HIDDEN_TCS_CMD0_ADDR + 8, 0) != 0 ||
        rsccWrite(TBGPU_RSCC_HIDDEN_TCS_CMD0_DATA + 16, 0x80000000u) != 0 ||
        rsccWrite(TBGPU_RSCC_HIDDEN_TCS_CMD0_ADDR + 16, 0) != 0 ||
        rsccWrite(TBGPU_RSCC_OVERRIDE_START, 0) != 0 ||
        rsccWrite(TBGPU_RSCC_PDC_SEQ_START, 0x4520) != 0 ||
        rsccWrite(TBGPU_RSCC_MATCH_LO, 0x4510) != 0 ||
        rsccWrite(TBGPU_RSCC_MATCH_HI, 0x4514) != 0) {
        return -1;
    }

    /* A750 is ADRENO_7XX_GEN3, not a740-family: generic SEQ_MEM_0_DRV0 (0x600). */
    const uint32_t uc[5] = {0xeaaae5a0u, 0xe1a1ebabu, 0xa2e0a581u, 0xecac82e2u, 0x0020edadu};
    for (uint32_t i = 0; i < 5; ++i) {
        if (rsccWrite(TBGPU_RSCC_SEQ_MEM_0_GENERIC + 4 * i, uc[i]) != 0) {
            return -1;
        }
    }

    if (pdcWrite(TBGPU_PDC_GPU_SEQ_START_ADDR, 0) != 0 ||
        pdcWrite(TBGPU_PDC_GPU_ENABLE_PDC, kPdcEnableValue) != 0) {
        return -1;
    }
    tb710fu_gpu_barrier();
    return 0;
}

/* ---- stage 7 ---------------------------------------------------------- */

int stage7(const Tb710fuGpuCoreWindows *windows)
{
    Tb710fuGpuCoreReport *report = gCore.report;

    /* 7.1 GPU SMMU: pool, bank 1 (GMU SID 5). */
    int status = tb710fu_gpu_smmu_init(&gCore.smmu, windows->smmu, kSmmuPoolBytes);
    if (status != 0) {
        return -2;
    }
    gCore.smmuReady = true;
    report->smmuId0 = gCore.smmu.id0;
    report->smmuId1 = gCore.smmu.id1;
    report->smmuId2 = gCore.smmu.id2;
    if (tb710fu_gpu_smmu_begin(&gCore.smmu, 1) != 0) {
        return -3;
    }

    uint32_t flags = Tb710fuGpuMapRead | Tb710fuGpuMapWrite | Tb710fuGpuMapWriteback;
    if (mapBuffer(1, TBGPU_GMU_ICACHE_IOVA, gCore.icache, flags) != 0 ||
        mapBuffer(1, TBGPU_GMU_DUMMY_IOVA, gCore.dummy, flags) != 0 ||
        mapBuffer(1, TBGPU_GMU_LOG_IOVA, gCore.log, flags) != 0 ||
        mapBuffer(1, TBGPU_GMU_HFI_IOVA, gCore.hfi, flags) != 0 ||
        mapBuffer(1, TBGPU_GMU_DEBUG_IOVA, gCore.debug, flags) != 0) {
        return -4;
    }
    report->smmuTables = gCore.smmu.tablesUsed;
    if (tb710fu_gpu_smmu_program(&gCore.smmu, 1, kGmuAsid) != 0) {
        return -5;
    }
    const uint32_t gmuSid = 5;
    if (tb710fu_gpu_smmu_attach(&gCore.smmu, 1, &gmuSid, 1) != 0) {
        return -6;
    }
    report->stage7Step = 3;

    /* 7.2 SM8650 RPMh/RSC/PDC programming. */
    if (rpmhInit() != 0) {
        return -7;
    }
    report->stage7Step = 4;

    /* 7.3 FAL veto and TCM retention. */
    if (gmuWrite(TBGPU_GMU_CX_FALNEXT_INTF, 1) != 0 ||
        gmuWrite(TBGPU_GMU_CX_FAL_INTF, 1) != 0 ||
        tb710fu_gpu_write32(windows->cxMisc, TBGPU_CX_MISC_TCM_RET_CNTL, 1) != 0) {
        return -8;
    }
    report->stage7Step = 5;

    /* 7.4 Load gmu_gen70900.bin into ITCM/DTCM/icache and clean it. */
    void *image = nullptr;
    uint64_t imageBytes = 0;
    if (tb710fu_gpu_firmware_load("gmu_gen70900.bin", 78588,
            "9685f62c42543befcaee68d25355e16bf55cdb0d573bddcc9f6633f59eec3f72",
            &image, &imageBytes) != 0) {
        return -9;
    }
    if (gmuFirmwareLoad(image, imageBytes) != 0) {
        tb710fu_gpu_firmware_free(image, imageBytes);
        return -10;
    }
    tb710fu_gpu_firmware_free(image, imageBytes);
    report->stage7Step = 6;

    /* The queue table and its three ring buffers must already be coherent
     * before CM3 is released: the firmware reads QTBL_ADDR during boot. */
    hfiQueueInit();

    uint32_t version = 0;
    if (gmuRead(TBGPU_GMU_FW_VERSION, &version) != 0) {
        return -11;
    }
    report->gmuVersion = version;

    /* 7.5 Boot registers, then release CM3. */
    const uint32_t chipId = TBGPU_GMU_CHIPID_A750;
    if (gmuWrite(TBGPU_GMU_CM3_FW_INIT_RESULT, 0) != 0 ||
        gmuWrite(TBGPU_GMU_CM3_BOOT_CONFIG, 0x02) != 0 ||
        gmuWrite(TBGPU_GMU_HFI_QTBL_ADDR, static_cast<uint32_t>(TBGPU_GMU_HFI_IOVA)) != 0 ||
        gmuWrite(TBGPU_GMU_HFI_QTBL_INFO, 1) != 0 ||
        gmuWrite(TBGPU_GMU_AO_AHB_FENCE_RANGE, kGmuFenceRange) != 0 ||
        gmuWrite(TBGPU_GMU_CM3_CFG, kCm3Cfg) != 0 ||
        gmuWrite(TBGPU_GMU_GENERAL_10, chipId) != 0 ||
        gmuWrite(TBGPU_GMU_GENERAL_8,
                 static_cast<uint32_t>(TBGPU_GMU_LOG_IOVA) |
                 ((gCore.log.bytes / kPageBytes - 1u) & 0xffu)) != 0) {
        return -12;
    }
    report->stage7Step = 7;

    for (uint32_t i = 0; i < 4; ++i) {
        if (gpuWrite(TBGPU_GBIF_QSB_SIDE0 + 4 * i, 0x00071620u) != 0) {
            return -13;
        }
    }
    if (gmuWrite(TBGPU_GMU_SYS_BUS_CONFIG, 1) != 0 ||
        gmuWrite(TBGPU_GMU_ICACHE_CONFIG, 1) != 0 ||
        gmuWrite(TBGPU_GMU_DCACHE_CONFIG, 1) != 0) {
        return -14;
    }
    report->stage7Step = 8;

    if (gmuWrite(TBGPU_GMU_CM3_SYSRESET, 1) != 0 ||
        gmuWrite(TBGPU_GMU_GENERAL_9, 0) != 0 ||
        gmuWrite(TBGPU_GMU_CM3_SYSRESET, 0) != 0) {
        return -15;
    }
    const uint32_t mask = (version <= 0x20010004u) ? 0xffffffffu : 0x1ffu;
    const uint32_t reset = (version <= 0x20010004u) ? 0xbabefaceu : 0x100u;
    uint32_t result = 0;
    if (pollRegister([&](uint32_t *out) { return gmuRead(TBGPU_GMU_CM3_FW_INIT_RESULT, out); },
                     mask, reset, 1000, 100, &result) != 0) {
        report->gmuBoot = result;
        return -16;
    }
    report->gmuBoot = result;
    gCore.chipId = chipId;
    report->stage7Step = 9;

    /* 7.6 HFI engine: the queue table itself was published before CM3 boot. */
    if (gmuWrite(TBGPU_GMU_HFI_CTRL_INIT, 1) != 0) {
        return -17;
    }
    uint32_t hfiStatus = 0;
    if (pollRegister([&](uint32_t *out) { return gmuRead(TBGPU_GMU_HFI_CTRL_STATUS, out); },
                     1, 1, 100, 100, &hfiStatus) != 0) {
        return -18;
    }
    report->stage7Step = 10;

    /* 7.7 A7xx non-legacy HFI: perf -> bw -> IFPC feature -> CORE_FW_START
     * -> START (a6xx_hfi_start, non-legacy path). */
    if (sendPerfTable() != 0) {
        return -19;
    }
    if (sendBwTable() != 0) {
        return -20;
    }
    if (sendIfpcFeature() != 0) {
        return -21;
    }
    if (sendCoreFwStart() != 0) {
        return -22;
    }
    if (sendStart() != 0) {
        return -23;
    }
    report->stage7Step = 12;
    report->gmuChipId = chipId;
    return 0;
}

/* ---- stage 8 ---------------------------------------------------------- */

int stage8()
{
    Tb710fuGpuCoreReport *report = gCore.report;

    /* 8.1 GX vote and the GPU_SET OOB handshake that keeps GX up. */
    /* Boot GX at mid-low OPP (index 3 = 366 MHz) to avoid excessive boot current draw. */
    if (sendGxVote(3, a750InitialBwVote()) != 0) {
        return -1;
    }
    const int oobStatus = gmuSetOob(30, 31);   /* GMU_OOB_GPU_SET new bits: set 30, ack 31 */
    uint32_t sptprac = 0;
    const int powerStatus = pollRegister(
        [&](uint32_t *out) { return gmuRead(TBGPU_GMU_SPTPRAC_PWR_CLK_STATUS, out); },
        3u, 0u, 100, 100, &sptprac);
    if (powerStatus != 0) {
        report->gxPowerState = sptprac;
        gCore.gxPowerState = sptprac;
        return oobStatus != 0 ? -2 : -3;
    }
    gCore.gxPowerState = sptprac;
    report->gxPower = 1;
    report->gxPowerState = sptprac;
    report->stage8Step = 1;

    /* 8.1b Clear GBIF halt in case GX was not collapsed, exactly as a7xx
     * hw_init does before it touches the ring (read back each write so a
     * posted write cannot be reordered behind the SMMU programming). */
    uint32_t halt = 0;
    if (gpuWrite(TBGPU_GBIF_HALT, 0) != 0 || gpuRead(TBGPU_GBIF_HALT, &halt) != 0 ||
        gpuWrite(TBGPU_RBBM_GBIF_HALT, 0) != 0 || gpuRead(TBGPU_RBBM_GBIF_HALT, &halt) != 0 ||
        gpuWrite(TBGPU_RBBM_GBIF_CLIENT_QOS_CNTL, 0x02120212u) != 0 ||
        gpuWrite(TBGPU_UCHE_GBIF_GX_CONFIG, 0x10240e0u) != 0 ||
        gpuWrite(TBGPU_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffffu) != 0 ||
        gpuWrite(TBGPU_SECVID_TSB_CNTL, 0) != 0) {
        return -40;
    }

    /* 8.2 GPU SMMU bank 0 (SID 0 and 1). */
    if (tb710fu_gpu_smmu_begin(&gCore.smmu, 0) != 0) {
        return -4;
    }
    uint32_t flags = Tb710fuGpuMapRead | Tb710fuGpuMapWrite | Tb710fuGpuMapWriteback;
    if (mapBuffer(0, TBGPU_RING_IOVA, gCore.ring, flags) != 0 ||
        mapBuffer(0, TBGPU_FENCE_IOVA, gCore.fence, flags) != 0 ||
        mapBuffer(0, TBGPU_PWRUP_IOVA, gCore.pwrup, flags) != 0 ||
        mapBuffer(0, TBGPU_SQE_IOVA, gCore.sqe, flags) != 0 ||
        mapBuffer(0, TBGPU_AQE_IOVA, gCore.aqe, flags) != 0) {
        return -5;
    }
    if (tb710fu_gpu_smmu_program(&gCore.smmu, 0, kGpuAsid) != 0) {
        return -6;
    }
    const uint32_t gpuSids[2] = {0, 1};
    if (tb710fu_gpu_smmu_attach(&gCore.smmu, 0, gpuSids, 2) != 0) {
        return -7;
    }
    report->smmuTables = gCore.smmu.tablesUsed;
    report->stage8Step = 2;

    uint32_t rbbm = 0;
    if (gpuRead(TBGPU_RBBM_STATUS_BYTES, &rbbm) != 0) {
        return -8;
    }
    gCore.rbbmStatus = rbbm;
    report->rbbmStatus = rbbm;

    /* 8.3 UCHE/GMEM range, A750 UCHE/UBWC/APRIV/CP prep, and secure-video door. */
    const uint64_t ucheTrapBase = 0x0001fffffffff000ull;
    if (gpuWrite64(TBGPU_UCHE_GMEM_RANGE_MIN, TBGPU_GMEM_BASE) != 0 ||
        gpuWrite64(TBGPU_UCHE_GMEM_RANGE_MAX, TBGPU_GMEM_BASE + TBGPU_GMEM_BYTES - 1) != 0 ||
        gpuWrite64(0x00e09u * 4u, ucheTrapBase) != 0 ||               /* UCHE_TRAP_BASE */
        gpuWrite64(0x00e07u * 4u, ucheTrapBase) != 0 ||               /* UCHE_WRITE_THRU_BASE */
        gpuWrite(TBGPU_UCHE_CACHE_WAYS, 1u << 23) != 0 ||
        gpuWrite(0x00e19u * 4u, (1u << 7) | 1u) != 0 ||               /* UCHE_CLIENT_PF */
        gpuWrite(0x00e3cu * 4u, 0x0006690eu) != 0) {                  /* UCHE_CMDQ_CONFIG (A7xx) */
        return -9;
    }
    gpuWrite(0x00500u * 4u, 1u);       /* RBBM_PERFCTR_CNTL: enable counters */
    gpuWrite(0x008d0u * 4u, 0u);       /* CP_PERFCTR_CP_SEL(0): PERF_CP_ALWAYS_COUNT */
    gpuWrite(0x0098du * 4u, 1u);       /* CP_AHB_CNTL: ERROR on default slave */
    gpuWrite(0x0084fu * 4u, 0u);       /* CP_PROTECT_CNTL: off during bring-up */
    gpuWrite(0x00844u * 4u, 0x6fu);    /* CP_APRIV_CNTL: A7XX_BR_APRIVMASK */
    gpuWrite(0x00ad0u * 4u, 0x6au);    /* CP_BV_APRIV_CNTL: A7XX_APRIV_MASK */
    gpuWrite(0x00b31u * 4u, 0x6au);    /* CP_LPAC_APRIV_CNTL: A7XX_APRIV_MASK */
    gpuWrite(0x00a00u * 4u, 0u);       /* CP_APERTURE_CNTL_HOST */
    /* A750 UBWC 4.0 / hbb=16 / amsbc=1 NC_MODE_CNTL & A750 ECO bits (a6xx_gpu.c) */
    gpuWrite(0x08e08u * 4u, 0x816u);   /* RB_NC_MODE_CNTL */
    gpuWrite(0x0b604u * 4u, 0x6u);     /* TPL1_NC_MODE_CNTL */
    gpuWrite(0x0ae02u * 4u, 0x26u);    /* SP_NC_MODE_CNTL */
    uint32_t rbCmpEco = 0;
    if (gpuRead(0x08e28u * 4u, &rbCmpEco) == 0) {
        gpuWrite(0x08e28u * 4u, rbCmpEco | (1u << 19)); /* RB_CMP_DBG_ECO_CNTL (A750) */
    }
    gpuWrite(0x0b602u * 4u, 0xc0700u); /* TPL1_DBG_ECO_CNTL1 (A750) */
    gpuWrite(TBGPU_SECVID_TSB_BASE, 0);
    gpuWrite(TBGPU_SECVID_TSB_BASE + 4, 0);
    gpuWrite(TBGPU_SECVID_TSB_SIZE, 0);
    gpuWrite(TBGPU_SECVID_TSB_CNTL, 0);
    cpuClean(gCore.pwrup.cpu, gCore.pwrup.bytes);
    cpuClean(gCore.fence.cpu, gCore.fence.bytes);
    report->stage8Step = 3;

    /* 8.4 SQE/AQE microcode. */
    if (writeFirmwareToBuffer("gen70900_sqe.fw", 85364,
            "f756bafe5f69a87161c7c53385386374a791861a574c244e845baba677993842",
            &gCore.sqe) != 0 ||
        writeFirmwareToBuffer("gen70900_aqe.fw", 33176,
            "25237c161d727b8ea92b4beb837a31651b44ea78bfc34d95672af8f346a45825",
            &gCore.aqe) != 0) {
        return -10;
    }
    if (gpuWrite64(TBGPU_CP_SQE_INSTR_BASE, TBGPU_SQE_IOVA) != 0 ||
        gpuWrite64(TBGPU_CP_AQE_INSTR_BASE_0, TBGPU_AQE_IOVA) != 0) {
        return -11;
    }
    report->stage8Step = 4;

    /* 8.5 RB0 + BV RPTR shadow. */
    const uint32_t ringControl = static_cast<uint32_t>(ilog2u(gCore.ring.bytes / 8) & 0x3f) |
                                 (ilog2u(32 / 8) << 8);
    if (gpuWrite64(TBGPU_CP_RB_BASE, TBGPU_RING_IOVA) != 0 ||
        gpuWrite(TBGPU_CP_RB_CNTL, ringControl != 0 ? ringControl : kMsmRbCntl) != 0 ||
        gpuWrite64(TBGPU_CP_RB_RPTR_ADDR, TBGPU_FENCE_IOVA) != 0 ||
        gpuWrite64(0x00a98u * 4u, TBGPU_FENCE_IOVA + 8u) != 0 ||
        gpuWrite(TBGPU_CP_RB_WPTR, 0) != 0) {
        return -12;
    }
    /* Start the SQE only after RB0 and the microcode base are in place. */
    if (gpuWrite(TBGPU_CP_SQE_CNTL, 1) != 0) {
        return -12;
    }
    report->stage8Step = 5;

    /* 8.6 CP init + a real PM4 fence; ringReady only when RAM shows it. */
    uint32_t fence = 0;
    if (issueFenceSelfTest(&fence) != 0) {
        report->fenceValue = fence;
        return -13;
    }
    gCore.ringReady = true;
    gCore.ringSeq = kScratchFenceSeq;
    gCore.lastFence = kScratchFenceSeq;
    report->cpStep = 3;
    report->fenceValue = fence;
    report->ringReady = 1;
    report->lastFence = gCore.ringSeq;
    report->stage8Step = 7;
    /* Keep the GPU_SET request asserted: the GMU holds GX on while the ring and
     * the user client are live. It is only cleared on suspend (not in M3). */
    return 0;
}

/* ---- ring ------------------------------------------------------------- */

struct RingWriter {
    uint32_t *words;
    uint32_t wordsCapacity;
    uint32_t dwords;
};

uint32_t pm4OddParity(uint32_t val)
{
    val ^= val >> 16;
    val ^= val >> 8;
    val ^= val >> 4;
    return (0x9669u >> (val & 0xfu)) & 1u;
}

uint32_t type7Header(uint32_t opcode, uint32_t count)
{
    const uint32_t c = count & 0x3fffu;
    const uint32_t op = opcode & 0x7fu;
    return 0x70000000u | c | (pm4OddParity(c) << 15) |
           (op << 16) | (pm4OddParity(op) << 23);
}

uint32_t type4Header(uint32_t reg, uint32_t count)
{
    const uint32_t c = count & 0x7fu;
    const uint32_t r = reg & 0x3ffffu;
    return 0x40000000u | c | (pm4OddParity(c) << 7) |
           (r << 8) | (pm4OddParity(r) << 27);
}

/* Appends one TYPE7 packet; pads with CP_NOP if the packet would straddle the
 * end so every packet stays inside the ring. */
int emit7(RingWriter *writer, uint32_t opcode, const uint32_t *payload, uint32_t count)
{
    const uint32_t total = count + 1;
    if (writer->dwords + total > writer->wordsCapacity) {
        return -1;
    }
    uint32_t *at = writer->words + writer->dwords;
    at[0] = type7Header(opcode, count);
    for (uint32_t i = 0; i < count; ++i) {
        at[1 + i] = payload[i];
    }
    writer->dwords += total;
    return 0;
}

int emit4(RingWriter *writer, uint32_t reg, const uint32_t *payload, uint32_t count)
{
    const uint32_t total = count + 1;
    if (writer->dwords + total > writer->wordsCapacity) {
        return -1;
    }
    uint32_t *at = writer->words + writer->dwords;
    at[0] = type4Header(reg, count);
    for (uint32_t i = 0; i < count; ++i) {
        at[1 + i] = payload[i];
    }
    writer->dwords += total;
    return 0;
}

uint32_t ringRptr()
{
    uint32_t rptr = 0;
    gpuRead(TBGPU_CP_RB_RPTR, &rptr);
    return rptr;
}

/* Pushes the new write pointer only after the ring bytes are at coherency.
 * A WPTR write that lands while GX is not ready is dropped and flagged in
 * GMU_AHB_FENCE_STATUS bit0 (the A740 board logs the same behaviour); clear
 * the flag and give the write again, at most 200 times. */
int ringKick(uint32_t wptr)
{
    cpuClean(gCore.ring.cpu, gCore.ring.bytes);
    tb710fu_gpu_barrier();
    if (gpuWrite(TBGPU_CP_RB_WPTR, wptr) != 0) {
        return -1;
    }
    for (uint32_t tries = 0; tries < 200; ++tries) {
        uint32_t status = 0;
        if (gmuRead(TBGPU_GMU_AHB_FENCE_STATUS, &status) != 0) {
            return 0;   /* no fence-status window: the fence poll decides */
        }
        if ((status & 1u) == 0) {
            return 0;
        }
        gmuWrite(TBGPU_GMU_AHB_FENCE_STATUS_CLR, 0x7u);
        if (gpuWrite(TBGPU_CP_RB_WPTR, wptr) != 0) {
            return -1;
        }
    }
    return 0;
}

/* A CP_SCRATCH(2)=seqno + CP_EVENT_WRITE7(CACHE_CLEAN, IRQ) fence writing to
 * fence_iova+4 (a msm_rbmemptrs.fence slot). */
int emitFence(RingWriter *writer, uint32_t seqno)
{
    const uint32_t scratch[1] = {seqno};
    if (emit4(writer, TBGPU_CP_SCRATCH(2) / 4u, scratch, 1) != 0) {
        return -1;
    }
    const uint64_t at = TBGPU_FENCE_IOVA + 4;
    const uint32_t packet[4] = {0x31u | (1u << 27) | (1u << 31), static_cast<uint32_t>(at),
                                static_cast<uint32_t>(at >> 32), seqno};
    return emit7(writer, 0x46, packet, 4);
}

int issueFenceSelfTest(uint32_t *observed)
{
    *observed = 0;
    RingWriter writer = {reinterpret_cast<uint32_t *>(gCore.ring.cpu), gCore.ring.bytes / 4, 0};
    const uint32_t thread[1] = {1u << 27};   /* CP_THREAD_CONTROL: concurrent binning off */
    if (emit7(&writer, 0x17, thread, 1) != 0) {
        return -1;
    }
    const uint32_t meInit[7] = {
        (1u << 0) | (1u << 1) | (1u << 3) | (1u << 6) | (1u << 8),
        0x00000003u, 0x20000000u, 0x00000002u,
        static_cast<uint32_t>(TBGPU_PWRUP_IOVA), static_cast<uint32_t>(TBGPU_PWRUP_IOVA >> 32),
        1u << 31
    };
    if (emit7(&writer, 0x48, meInit, 7) != 0) {
        return -1;
    }

    /* 8.6a Like a7xx_cp_init + a6xx_idle: let the CP consume THREAD_CONTROL
     * and ME_INIT first, so a stall is reported at this step and not as an
     * anonymous missing fence. */
    gCore.ringWptr = writer.dwords;
    if (ringKick(writer.dwords) != 0) {
        return -1;
    }
    bool fetched = false;
    uint32_t rptr = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        rptr = ringRptr();
        if (rptr == writer.dwords) {
            fetched = true;
            break;
        }
        tb710fu_gpu_delay_us(100);
    }
    gCore.report->cpStep = fetched ? 1u : 0u;
    if (!fetched) {
        uint32_t st = 0, st3 = 0, int0 = 0, hwf = 0, cpi = 0, sqec = 0, rbBase = 0, rbCntl = 0, wptrRd = 0;
        uint32_t afc = 0, afs = 0;
        uint64_t cb0Far = 0;
        gpuRead(TBGPU_RBBM_STATUS_BYTES, &st);
        gpuRead(0x00213u * 4u, &st3);
        gpuRead(0x00201u * 4u, &int0);
        gpuRead(0x00821u * 4u, &hwf);
        gpuRead(0x00823u * 4u, &cpi);
        gpuRead(TBGPU_CP_SQE_CNTL, &sqec);
        gpuRead(TBGPU_CP_RB_BASE, &rbBase);
        gpuRead(TBGPU_CP_RB_CNTL, &rbCntl);
        gpuRead(TBGPU_CP_RB_WPTR, &wptrRd);
        gmuRead(TBGPU_GMU_AO_AHB_FENCE_CTRL, &afc);
        gmuRead(TBGPU_GMU_AHB_FENCE_STATUS, &afs);
        tb710fu_gpu_read64(gCore.smmu.window, gCore.smmu.bankOffset[0] + 0x60u, &cb0Far);
        IOLog("TB710FU GPU: CP stall rptr=%u wptr=%u(rd=%u) rbbm=0x%x/0x%x int0=0x%x hwf=0x%x cpi=0x%x sqec=0x%x rb=0x%x/0x%x ahb=0x%x/0x%x gfsr=0x%x cb0fsr=0x%x far=0x%llx\n",
              rptr, writer.dwords, wptrRd, st, st3, int0, hwf, cpi, sqec, rbBase, rbCntl, afc, afs,
              tb710fu_gpu_smmu_global_fault(&gCore.smmu), tb710fu_gpu_smmu_bank_fault(&gCore.smmu, 0),
              (unsigned long long)cb0Far);
        return -1;
    }

    /* 8.6b One real PM4 fence: CP_SCRATCH(2)=seqno, then CP_EVENT_WRITE7
     * CACHE_CLEAN|IRQ writing seqno to fence_iova+4. ringReady is only set by
     * the caller when the fence word in RAM actually changes. */
    if (emitFence(&writer, kScratchFenceSeq) != 0) {
        return -1;
    }
    gCore.ringWptr = writer.dwords;
    if (ringKick(writer.dwords) != 0) {
        return -1;
    }

    for (uint32_t i = 0; i < 2000; ++i) {
        cpuInvalidate(gCore.fence.cpu, 64);
        memcpy(observed, gCore.fence.cpu + 4, sizeof(*observed));
        if (*observed >= kScratchFenceSeq) {
            gCore.report->cpStep = 2u;
            return 0;
        }
        tb710fu_gpu_delay_us(100);
    }
    gCore.report->cpStep = 2u;
    return -1;
}

/* Copies one verified Adreno microcode image (skipping the 4-byte container
 * header, matching Linux adreno_fw_create_bo) into a mapped buffer. */
int writeFirmwareToBuffer(const char *name, uint64_t expectedBytes, const char *sha,
                          Buffer *buffer)
{
    void *image = nullptr;
    uint64_t bytes = 0;
    if (tb710fu_gpu_firmware_load(name, expectedBytes, sha, &image, &bytes) != 0) {
        return -1;
    }
    if (bytes <= 4u || (bytes - 4u) > buffer->bytes) {
        tb710fu_gpu_firmware_free(image, bytes);
        return -1;
    }
    memcpy(buffer->cpu, static_cast<const uint8_t *>(image) + 4u, bytes - 4u);
    tb710fu_gpu_firmware_free(image, bytes);
    cpuClean(buffer->cpu, buffer->bytes);
    return 0;
}

}  // namespace

/* ---- public API ------------------------------------------------------- */

int tb710fu_gpu_core_start(const Tb710fuGpuCoreWindows *windows, uint32_t level,
                           Tb710fuGpuCoreReport *report)
{
    if (!windows || !report || !windows->gpu || !windows->gmu || !windows->rscc ||
        !windows->pdc || !windows->smmu || !windows->cxMisc) {
        return -1;
    }
    bzero(report, sizeof(*report));
    if (!gCoreLock) {
        gCoreLock = IOLockAlloc();
    }
    gCore.report = report;
    gCore.windows = *windows;
    gCore.windowsBound = true;
    gCore.gmemBase = TBGPU_GMEM_BASE;
    gCore.gmemBytes = static_cast<uint32_t>(TBGPU_GMEM_BYTES);
    gCore.chipId = TBGPU_CHIPID_A750;
    report->chipId = gCore.chipId;
    report->gmemBytes = gCore.gmemBytes;
    report->gmemBase = gCore.gmemBase;

    /* Kernel-owned buffers. All are physically contiguous and page aligned. */
    if (bufferAlloc(&gCore.icache, static_cast<uint32_t>(TBGPU_GMU_ICACHE_BYTES)) != 0 ||
        bufferAlloc(&gCore.dummy, TBGPU_GMU_SMALL_BYTES) != 0 ||
        bufferAlloc(&gCore.log, TBGPU_GMU_SMALL_BYTES) != 0 ||
        bufferAlloc(&gCore.hfi, TBGPU_GMU_SMALL_BYTES) != 0 ||
        bufferAlloc(&gCore.debug, TBGPU_GMU_DEBUG_BYTES) != 0 ||
        bufferAlloc(&gCore.ring, TBGPU_RING_BYTES) != 0 ||
        bufferAlloc(&gCore.fence, 0x1000) != 0 ||
        bufferAlloc(&gCore.pwrup, 0x1000) != 0 ||
        bufferAlloc(&gCore.sqe, static_cast<uint32_t>(TBGPU_SQE_BYTES)) != 0 ||
        bufferAlloc(&gCore.aqe, static_cast<uint32_t>(TBGPU_AQE_BYTES)) != 0) {
        return -2;
    }

    int status = stage7(windows);
    if (status != 0) {
        report->hfiError = gCore.hfiError;
        return status;
    }
    if (level < Tb710fuGpuStageGx) {
        return 0;
    }
    status = stage8();
    if (status != 0) {
        report->smmuFault = tb710fu_gpu_smmu_global_fault(&gCore.smmu);
        return status;
    }
    report->smmuFault = tb710fu_gpu_smmu_global_fault(&gCore.smmu);
    report->smmuTables = gCore.smmu.tablesUsed;
    return 0;
}

bool tb710fu_gpu_core_ring_ready(void)
{
    return gCore.ringReady;
}

int tb710fu_gpu_core_map_bo(IOMemoryDescriptor *memory, uint64_t iova, uint64_t bytes,
                            uint32_t permissions, uint32_t writeback, uint32_t shareability)
{
    if (!gCore.smmuReady || !gCore.windowsBound || !gCore.report || !memory || !bytes) {
        return -1;
    }
    uint32_t flags = Tb710fuGpuMapRead;
    if ((permissions & 2u) != 0) {   /* ABI Write */
        flags |= Tb710fuGpuMapWrite;
    }
    if (writeback) {
        flags |= Tb710fuGpuMapWriteback;
    }
    flags |= (shareability & 3u) << Tb710fuGpuMapShareShift;
    if (iova < TBGPU_CLIENT_VA_START || iova + bytes > TBGPU_CLIENT_VA_START + TBGPU_CLIENT_VA_SIZE) {
        return -1;
    }
    if (gCoreLock) {
        IOLockLock(gCoreLock);
    }
    int rc = tb710fu_gpu_smmu_map(&gCore.smmu, 0, iova, memory, bytes, flags);
    if (rc == 0) {
        gCore.report->boMapped++;
    }
    if (gCoreLock) {
        IOLockUnlock(gCoreLock);
    }
    return rc == 0 ? 0 : -1;
}

int tb710fu_gpu_core_unmap_bo(uint64_t iova, uint64_t bytes)
{
    if (!gCore.smmuReady || !gCore.report || !iova || !bytes) {
        return -1;
    }
    if (gCoreLock) {
        IOLockLock(gCoreLock);
    }
    int rc = tb710fu_gpu_smmu_unmap(&gCore.smmu, 0, iova, bytes);
    if (rc == 0 && gCore.report->boMapped) {
        gCore.report->boMapped--;
    }
    if (gCoreLock) {
        IOLockUnlock(gCoreLock);
    }
    return rc == 0 ? 0 : -1;
}

int tb710fu_gpu_core_wait_fence(uint64_t fence, uint64_t timeoutNs, uint32_t *observed)
{
    if (observed) {
        *observed = 0;
    }
    if (!fence) {
        return 0;
    }
    uint64_t tries = timeoutNs / 100000ull;
    if (tries == 0) {
        tries = 1;   /* ABI: timeout 0 polls once */
    }
    uint32_t lastValue = 0;
    for (uint64_t i = 0; i < tries; ++i) {
        uint32_t value = 0;
        cpuInvalidate(gCore.fence.cpu, 64);
        memcpy(&value, gCore.fence.cpu + 4, sizeof(value));
        lastValue = value;
        if (observed) {
            *observed = value;
        }
        if (value >= fence) {
            return 0;
        }
        if (value > gCore.ringSeq) {
            return -1;
        }
        tb710fu_gpu_delay_us(100);
    }
    if (timeoutNs >= 10000000ull) {
        static volatile uint32_t gTimeoutLogged = 0;
        if (__atomic_fetch_add(&gTimeoutLogged, 1u, __ATOMIC_RELAXED) < 4u) {
            uint32_t rbbm = 0, cpInt = 0, rptr = 0, bvRptr = 0, hwf = 0, int0 = 0, bvVal = 0, cb0Fsr = 0, farLo = 0, farHi = 0;
            gpuRead(TBGPU_RBBM_STATUS_BYTES, &rbbm);
            gpuRead(0x0803u * 4u, &cpInt);
            gpuRead(TBGPU_CP_RB_RPTR, &rptr);
            gpuRead(0x0a66u * 4u, &bvRptr);
            gpuRead(0x0821u * 4u, &hwf);
            gpuRead(0x0201u * 4u, &int0);
            memcpy(&bvVal, gCore.fence.cpu + 12, sizeof(bvVal));
            if (gCore.windows.smmu && gCore.smmu.bankOffset[0]) {
                const uint32_t cb0 = gCore.smmu.bankOffset[0];
                tb710fu_gpu_read32(gCore.windows.smmu, cb0 + 0x58u, &cb0Fsr);
                tb710fu_gpu_read32(gCore.windows.smmu, cb0 + 0x60u, &farLo);
                tb710fu_gpu_read32(gCore.windows.smmu, cb0 + 0x64u, &farHi);
            }
            IOLog("TB710FU GPU: wait_fence timeout want=%llu obs=%u bv=%u rptr=0x%x bv_rptr=0x%x wptr=0x%x rbbm=0x%x int0=0x%x hwf=0x%x cp_int=0x%x cb0fsr=0x%x far=0x%llx\n",
                  (unsigned long long)fence, lastValue, bvVal, rptr, bvRptr, gCore.ringWptr,
                  rbbm, int0, hwf, cpInt, cb0Fsr, ((unsigned long long)farHi << 32) | farLo);
        }
    }
    return -1;
}

int tb710fu_gpu_core_submit(const uint64_t *iovas, const uint64_t *bytes, uint32_t count,
                            uint64_t waitFence, uint64_t timeoutNs, uint64_t *fence)
{
    if (fence) {
        *fence = 0;
    }
    if (!gCore.ringReady || !iovas || !bytes || !count || count > 128) {
        return -1;
    }
    if (waitFence && tb710fu_gpu_core_wait_fence(waitFence, timeoutNs, nullptr) != 0) {
        return -1;
    }
    if (gCoreLock) {
        IOLockLock(gCoreLock);
    }

    /* A7xx framed submission (matching Linux a6xx_gpu.c a7xx_submit()):
     * prologue = 6 dwords, count * 4 IB dwords, framed fence = 31 dwords. */
    const uint32_t ringDwords = gCore.ring.bytes / 4;
    const uint32_t margin = 0x400;
    uint32_t at = gCore.ringWptr & (ringDwords - 1);
    const uint32_t rptr = ringRptr() & (ringDwords - 1);
    const uint32_t used = (gCore.ringWptr + ringDwords - rptr) & (ringDwords - 1);
    const uint32_t prologueDwords = 6u;
    const uint32_t framedFenceDwords = 31u;
    uint32_t needed = prologueDwords + count * 4u + framedFenceDwords;
    uint32_t pad = 0;
    if (at + needed > ringDwords) {
        pad = ringDwords - at;   /* CP_NOP fill to the end, then wrap to 0 */
        at = 0;
    }
    if (ringDwords - used < needed + pad + margin) {
        if (gCoreLock) {
            IOLockUnlock(gCoreLock);
        }
        return -1;
    }
    if (pad) {
        uint32_t *fill = reinterpret_cast<uint32_t *>(gCore.ring.cpu) + (gCore.ringWptr & (ringDwords - 1));
        for (uint32_t i = 0; i < pad; ++i) {
            fill[i] = type7Header(0x10, 0);   /* CP_NOP */
        }
    }

    RingWriter writer = {reinterpret_cast<uint32_t *>(gCore.ring.cpu), ringDwords, at};
    /* A7xx prologue (6 dwords): SYNC|BR, BOTH threads, SET_MARKER(0x101 IFPC off).
     * Omit CACHE_FLUSH7/CACHE_INVALIDATE7 (0x32/0x33) which stalled BR at rptr=0x13 on A750. */
    const uint32_t brSync = (1u << 31) | 1u;
    const uint32_t both = 3u;
    const uint32_t markerOn = 0x101u;
    if (emit7(&writer, 0x17, &brSync, 1) != 0 ||
        emit7(&writer, 0x17, &both, 1) != 0 ||
        emit7(&writer, 0x65, &markerOn, 1) != 0) {
        if (gCoreLock) {
            IOLockUnlock(gCoreLock);
        }
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes[i] == 0 || (bytes[i] & 3ull) || (iovas[i] & 3ull) ||
            (bytes[i] / 4ull) > 0xfffffull) {
            if (gCoreLock) {
                IOLockUnlock(gCoreLock);
            }
            return -1;
        }
        const uint32_t packet[3] = {static_cast<uint32_t>(iovas[i]),
                                    static_cast<uint32_t>(iovas[i] >> 32),
                                    static_cast<uint32_t>(bytes[i] / 4ull)};
        if (emit7(&writer, 0x3f, packet, 3) != 0) {   /* CP_INDIRECT_BUFFER */
            if (gCoreLock) {
                IOLockUnlock(gCoreLock);
            }
            return -1;
        }
    }
    const uint32_t seq = gCore.ringSeq + 1;
    /* A7xx framed fence (31 dwords): CP_SCRATCH(2), BR CCU depth/color inv,
     * BV timestamp @ +12, BR wait on BV timestamp, BR fence @ +4, BOTH,
     * SET_MARKER(0x100 IFPC on). */
    const uint32_t scratch[1] = {seq};
    const uint32_t br = 1u, bv = 2u, ccuDepth = 0x18u, ccuColor = 0x19u, markerOff = 0x100u;
    const uint64_t bvAt = TBGPU_FENCE_IOVA + 12u;
    const uint64_t fenceAt = TBGPU_FENCE_IOVA + 4u;
    const uint32_t bvWrite[4] = {0x31u | (1u << 27), static_cast<uint32_t>(bvAt),
                                 static_cast<uint32_t>(bvAt >> 32), seq};
    const uint32_t bvWait[4] = {0u, static_cast<uint32_t>(bvAt),
                                static_cast<uint32_t>(bvAt >> 32), seq};
    const uint32_t fenceWrite[4] = {0x31u | (1u << 27) | (1u << 31),
                                    static_cast<uint32_t>(fenceAt),
                                    static_cast<uint32_t>(fenceAt >> 32), seq};
    if (emit4(&writer, TBGPU_CP_SCRATCH(2) / 4u, scratch, 1) != 0 ||
        emit7(&writer, 0x17, &br, 1) != 0 ||
        emit7(&writer, 0x46, &ccuDepth, 1) != 0 ||
        emit7(&writer, 0x46, &ccuColor, 1) != 0 ||
        emit7(&writer, 0x17, &bv, 1) != 0 ||
        emit7(&writer, 0x46, bvWrite, 4) != 0 ||
        emit7(&writer, 0x17, &br, 1) != 0 ||
        emit7(&writer, 0x14, bvWait, 4) != 0 ||
        emit7(&writer, 0x46, fenceWrite, 4) != 0 ||
        emit7(&writer, 0x17, &both, 1) != 0 ||
        emit7(&writer, 0x65, &markerOff, 1) != 0) {
        if (gCoreLock) {
            IOLockUnlock(gCoreLock);
        }
        return -1;
    }
    if (writer.dwords != at + needed) {
        if (gCoreLock) {
            IOLockUnlock(gCoreLock);
        }
        return -1;
    }
    gCore.ringWptr = writer.dwords & (ringDwords - 1);
    gCore.ringSeq = seq;
    ringKick(gCore.ringWptr);
    gCore.report->submitCount++;
    if (gCoreLock) {
        IOLockUnlock(gCoreLock);
    }
    if (fence) {
        *fence = seq;
    }
    gCore.lastFence = seq;
    gCore.report->lastFence = seq;
    return 0;
}

uint64_t tb710fu_gpu_core_timestamp(void)
{
    uint64_t value = 0;
    if (!gCore.windows.gpu || tb710fu_gpu_read64(gCore.windows.gpu, TBGPU_CP_ALWAYS_ON_COUNTER,
                                                 &value) != 0) {
        return 0;
    }
    return value;
}

void tb710fu_gpu_core_device_info(Tb710fuGpuCoreDeviceInfo *info)
{
    if (!info) {
        return;
    }
    info->chipId = TBGPU_CHIPID_A750;
    info->gmemBytes = static_cast<uint32_t>(TBGPU_GMEM_BYTES);
    info->gmemBase = TBGPU_GMEM_BASE;
    info->ucheTrapBase = 0x0001fffffffff000ull;
    info->vaStart = TBGPU_CLIENT_VA_START;
    info->vaSize = TBGPU_CLIENT_VA_SIZE;
    info->highestBankBit = 16;
    info->ubwcSwizzle = 6;
    info->macrotileMode = 1;
    info->ubwcVersion = 4;
    info->flags = gCore.ringReady ? 1u : 0u;   /* bits: ringReady */
}
