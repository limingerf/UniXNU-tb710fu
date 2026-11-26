// SPDX-License-Identifier: BSD-2-Clause-Patent
// Lenovo TB710FU (SM8650Q-AB) Bluetooth: Qualcomm WCN7850 "Hamilton" on QUP wrapper 2 serial engine 6
// (uart14, 0x00898000), over GENI HSUART with hardware RTS/CTS, plus the QCA patch/NVM loader.
//
// Hardware facts and their device-tree provenance are in Tb710fuBluetoothBoard.h (uart14 at
// sm8650.dtsi:2489, the QUP2 wrapper at 2026, TLMM at 6113, the BT pins at dts:1020-1032, the BT node at
// dts:1223-1239, the WCN7850 PMU at dts:254-271). GENI/UART register semantics and the FIFO-only
// programming order are in Tb710fuBluetoothGeni.h, the GCC RCG encoding in Tb710fuBluetoothClock.h,
// the TLMM bit layout in Tb710fuBluetoothPower.h, and the QCA file format in Tb710fuBluetoothQca.h.
// Every one of those files cites its own source; none of them was copied from the SM8550 port.
//
// Staged bring-up, each level gated by the "bt" boot argument (absent or 0: this file does nothing):
//   bt=1  map GENI/GCC/wrapper/TLMM, WCN power-on (BT_EN pulse, SW_CTRL sampled), TLMM pinmux, SE
//         identification registers (read-only), GCC branch votes + RCG at 115200, FIFO UART configured,
//         hardware flow control on.
//   bt=2  HCI Reset (best effort, see stageResetAndVersion) and the QCA ROM version read, then the
//         baud switch to max-speed (3.2 Mbaud, QCA code 17) with clock, divider and flow control
//         re-programmed around it.
//   bt=3  hmtbtfw20.tlv patch download (243-byte segments, vendor 0xFC00/0x1E), board id, hmtnv20.bin
//         with its HCI/deep-sleep tags rewritten, SoC logging off, the authoritative HCI Reset, build
//         info, Read_BD_ADDR (and an optional write from "tb710fu-bt-bdaddr").
//   bt=4  publish the IOService with an IOUserClient for HCI traffic. No poll thread runs: a transaction
//         is driven synchronously by whoever calls in, under one lock.
// Sub-arguments: tb710fu-bt-log=0 quiets the evidence lines, tb710fu-bt-fw=<dir> moves the VFS firmware
// fallback, tb710fu-bt-speed=<baud> overrides the device tree's 3.2 Mbaud (it must be in the clock table),
// tb710fu-bt-bdaddr=AA:BB:CC:DD:EE:FF sets the address when the module reports a blank one.
//
// What is deliberately absent: no GIC interrupt is requested (SE_IRQ_EN stays cleared at the SE and M/S
// status is polled), no IOBluetoothFamily symbol is referenced (that family's 26C5035r vtable layout is
// not measured on this board, so adopting it would be a guess), and no PMIC/RPMh register is written
// (the WCN rails are PMU LDOs behind the WCN7850 PMU interface; a provider can install itself through
// tb710fu_bluetooth_set_rail_provider before this driver starts).
//
// A successful bt=1..3 is a driven UART and a loaded controller, not a Bluetooth stack: no host
// controller is attached, and nothing here has run on hardware yet, which is why every level is
// separate and off by default.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>

#include "Tb710fuBluetoothBoard.h"
#include "Tb710fuBluetoothClock.h"
#include "Tb710fuBluetoothFirmware.h"
#include "Tb710fuBluetoothGeni.h"
#include "Tb710fuBluetoothHci.h"
#include "Tb710fuBluetoothPower.h"
#include "Tb710fuBluetoothQca.h"

/* Evidence, for a later ioreg/COM5 read: [0] stage (negative = the stage that failed), [1] last error,
 * [2] QUP2 wrapper hardware version, [3] SE protocol id, [4] tx FIFO words | rx FIFO words << 16,
 * [5] GCC RCG command word, [6] GCC RCG cfg, [7] GCC branch register, [8] baud, [9] clock divider,
 * [10] ROM product id, [11] ROM version, [12] patch file product id, [13] patch file patch version,
 * [14] patch segments, [15] NVM segments, [16] board id, [17] NVM tags, [18] BD_ADDR word 0,
 * [19] BD_ADDR word 1, [20] CTS, [21] flow control, [22] firmware patch bytes, [23] firmware nvm bytes */
extern "C" {
volatile int32_t native_tb710fu_bt_stats[24];
char native_tb710fu_bt_text[96];
}

namespace {

constexpr uint32_t kHwVerReg = 0x4; /* QUPV3_HW_VER_REG in qcom-geni-se.c */
constexpr uint32_t kMaxBaudPlan = 4000000u;
constexpr uint32_t kHciEventCap = 260u; /* largest HCI event on this transport */
constexpr uint32_t kRingBytes = 4096u;

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
};

bool mapWindow(Window &w, uint64_t base, uint64_t size)
{
    IODeviceMemory *memory = IODeviceMemory::withRange((IOPhysicalAddress)base, (IOByteCount)size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

void unmapWindow(Window &w)
{
    if (w.map) w.map->release();
    w.map = nullptr;
    w.va = nullptr;
}

/* Every MMIO access in this file is one aligned 32-bit volatile load or store. memcpy, bzero and memset
 * never see a Window: the firmware buffers that get copied are IOMallocData pointers, ordinary memory. */
inline uint32_t rd32(const Window &w, uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(w.va + offset);
}

inline void wr32(const Window &w, uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(w.va + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

inline void barrierSy() { __asm__ volatile("dsb sy" ::: "memory"); }

Window gSe, gGcc, gWrapper, gTlmm;
bool gMapped;
TbBtGeni gGeni;
TbBtH4Parser gParser;
IOLock *gLock;
bool gReady;
TbBtPowerState gPower;
TbBtClockPlan gPlan;
TbBtQcaStats gQcaStats;
uint8_t gBdAddr[6];
char gBuildLabel[64];
uint32_t gBuildLabelLen;

struct Config {
    uint32_t level;
    uint32_t log;
    uint32_t speed;
    char fwDir[160];
    uint8_t bdAddr[6];
    bool bdAddrSet;
};
Config gCfg;

#define btprintf(...)                       \
    do {                                    \
        if (gCfg.log) printf("TB710FU BT: " __VA_ARGS__); \
    } while (0)

uint64_t nowNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

void delayUs(uint32_t us) { IODelay(us); }
void delayMs(uint32_t ms) { IOSleep(ms); }

/* ---- op tables handed to the portable cores ---------------------------------------------------- */

uint32_t geniRead(void *, uint32_t offset) { return rd32(gSe, offset); }
void geniWrite(void *, uint32_t offset, uint32_t value) { wr32(gSe, offset, value); }
void geniBarrier(void *) { barrierSy(); }
void geniDelay(void *, uint32_t us) { delayUs(us); }
uint64_t geniNow(void *) { return nowNs(); }
void geniNote(void *, const char *what, uint32_t a, uint32_t b) { btprintf("%s %08x %08x\n", what, a, b); }

const TbBtGeniOps kGeniOps = {geniRead, geniWrite, geniBarrier, geniDelay, geniNow, geniNote, nullptr};

/* Tb710fuBluetoothClock.h uses offsets inside the GCC region (cmd_rcgr 0x1e760 of a 0x1f4200 block),
 * so the window starts at TB_BT_GCC_PHYS and the offsets are passed through unchanged. */
uint32_t clockRead(void *, uint32_t offset) { return rd32(gGcc, offset); }
void clockWrite(void *, uint32_t offset, uint32_t value) { wr32(gGcc, offset, value); }
void clockDelay(void *, uint32_t us) { delayUs(us); }
const TbBtClockIo kClockIo = {clockRead, clockWrite, clockDelay, nullptr};

uint32_t pinRead(void *, uint32_t offset) { return rd32(gTlmm, offset); }
void pinWrite(void *, uint32_t offset, uint32_t value) { wr32(gTlmm, offset, value); }
void pinDelay(void *, uint32_t ms) { delayMs(ms); }
const TbBtPowerIo kPowerIo = {pinRead, pinWrite, pinDelay, nullptr};

/* ---- event tap and receive ring ---------------------------------------------------------------- */

struct Tap {
    bool want;
    uint16_t opcode;
    bool got;
    int kind;
    uint8_t status;
    uint8_t data[64];
    uint32_t len;
};

Tap gTap;
uint8_t gRing[kRingBytes];
uint32_t gRingHead, gRingTail;
uint32_t gRingWritten;
uint32_t gRingDropped;

uint32_t ringUsed() { return (gRingHead >= gRingTail) ? (gRingHead - gRingTail) : (kRingBytes - gRingTail + gRingHead); }

void ringPush(uint8_t type, const uint8_t *body, uint32_t len)
{
    if (!len || len > TB_BT_H4_MAX_PACKET) return;
    const uint32_t record = 1u + len;
    if (record + ringUsed() >= kRingBytes) {
        gRingDropped++;
        return;
    }
    gRing[gRingHead] = type;
    gRingHead = (gRingHead + 1u) % kRingBytes;
    for (uint32_t i = 0; i < len; ++i) {
        gRing[gRingHead] = body[i];
        gRingHead = (gRingHead + 1u) % kRingBytes;
    }
    gRingWritten += record;
}

uint32_t ringPop(uint8_t *to, uint32_t cap)
{
    uint32_t got = 0;
    while (got < cap && ringUsed()) {
        to[got++] = gRing[gRingTail];
        gRingTail = (gRingTail + 1u) % kRingBytes;
    }
    return got;
}

void rxSink(void *, uint8_t type, const uint8_t *body, uint32_t len)
{
    ringPush(type, body, len);
    if (type != TB_BT_H4_EVT || len < 1u) return;
    if (body[0] == TB_BT_EVT_CMD_COMPLETE) {
        uint16_t op = 0;
        uint8_t status = 0;
        const uint8_t *params = nullptr;
        uint32_t plen = 0;
        if (!tb_bt_hci_parse_cmd_complete(body, len, &op, &status, &params, &plen)) return;
        if (gTap.want && !gTap.got && op == gTap.opcode) {
            gTap.kind = TB_BT_QCA_REPLY_CC;
            gTap.status = status;
            gTap.len = plen > 64u ? 64u : plen;
            for (uint32_t i = 0; i < gTap.len; ++i) gTap.data[i] = params[i];
            gTap.got = true;
        }
    } else if (body[0] == TB_BT_EVT_CMD_STATUS) {
        uint16_t op = 0;
        uint8_t status = 0;
        if (!tb_bt_hci_parse_cmd_status(body, len, &op, &status)) return;
        if (gTap.want && !gTap.got && op == gTap.opcode) {
            gTap.kind = TB_BT_QCA_REPLY_CS;
            gTap.status = status;
            gTap.len = 0;
            gTap.got = true;
        }
    } else if (body[0] == TB_BT_EVT_VENDOR && gTap.want && !gTap.got && len >= 3u) {
        gTap.kind = TB_BT_QCA_REPLY_VSE;
        gTap.status = body[2];
        gTap.len = (len - 3u) > 64u ? 64u : (len - 3u);
        for (uint32_t i = 0; i < gTap.len; ++i) gTap.data[i] = body[3u + i];
        gTap.got = true;
    }
}

/* Drains the FIFO into the parser. Returns bytes taken. */
uint32_t rxPump()
{
    uint32_t total = 0;
    for (unsigned round = 0; round < 8u; ++round) {
        uint8_t bytes[kHciEventCap];
        uint32_t got = 0;
        const int rc = tb_bt_geni_read(&gGeni, bytes, sizeof bytes, &got);
        if (got) {
            total += got;
            (void)tb_bt_h4_parser_feed(&gParser, bytes, got);
        }
        if (rc == TB_BT_GENI_OVERFLOW) native_tb710fu_bt_stats[1] = -TB_BT_GENI_OVERFLOW;
        if (!got) break;
    }
    return total;
}

void tapArm(uint16_t opcode)
{
    gTap.want = true;
    gTap.opcode = opcode;
    gTap.got = false;
    gTap.kind = TB_BT_QCA_REPLY_NONE;
    gTap.status = 0xffu;
    gTap.len = 0u;
}

int hciWait(uint32_t timeoutMs)
{
    const uint64_t deadline = nowNs() + (uint64_t)timeoutMs * 1000000ull;
    while (nowNs() < deadline) {
        rxPump();
        if (gTap.got) return 0;
        delayUs(50);
    }
    rxPump();
    return gTap.got ? 0 : -ETIMEDOUT;
}

int hciSend(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
    uint8_t packet[4u + 255u];
    const uint32_t len = tb_bt_hci_command_packet(opcode, params, plen, packet, sizeof packet);
    if (!len) return -EINVAL;
    const int wrote = tb_bt_geni_write(&gGeni, packet, len);
    if (wrote <= 0) return wrote ? wrote : -EIO;
    return 0;
}

/* One transaction at a time: the tap is the module-wide gTap, which rxSink fills. */
int hciTransact(uint16_t opcode, const uint8_t *params, uint8_t plen, uint32_t timeoutMs, Tap *snapshot)
{
    tapArm(opcode);
    const int rc = hciSend(opcode, params, plen);
    if (rc) return rc;
    const int waited = hciWait(timeoutMs);
    if (snapshot) *snapshot = gTap;
    return waited;
}

/* ---- QCA loader plumbing ------------------------------------------------------------------------ */

int qcaSend(void *, uint16_t opcode, const uint8_t *params, uint8_t plen) { return hciSend(opcode, params, plen); }

int qcaWait(void *, uint16_t opcode, TbBtQcaReply *out, uint32_t timeoutMs)
{
    tapArm(opcode);
    if (hciWait(timeoutMs) != 0 || !gTap.got) {
        out->kind = TB_BT_QCA_REPLY_TIMEOUT;
        out->len = 0u;
        return -1;
    }
    out->kind = gTap.kind;
    out->status = gTap.status;
    out->len = gTap.len;
    for (uint32_t i = 0; i < gTap.len; ++i) out->data[i] = gTap.data[i];
    return 0;
}

void qcaNote(void *, const char *what, uint32_t a, uint32_t b) { btprintf("qca %s %08x %08x\n", what, a, b); }
void qcaDelay(void *, uint32_t ms) { delayMs(ms); }

const TbBtQcaIo kQcaIo = {qcaSend, qcaWait, qcaDelay, qcaNote, nullptr};

/* ---- firmware ----------------------------------------------------------------------------------- */

uint8_t *loadFirmware(const char *name, uint32_t *size)
{
    *size = 0u;
    if (!tb710fu_fw_open_addon || !tb710fu_fw_open || !tb710fu_fw_read || !tb710fu_fw_close) {
        btprintf("no firmware reader in this kernel (the ADSP archive carries it)\n");
        return nullptr;
    }
    void *handle = nullptr;
    uint64_t bytes = 0;
    int rc = tb710fu_fw_open_addon(name, &handle, &bytes);
    if (rc || !handle) {
        char path[200];
        unsigned at = 0;
        for (unsigned i = 0; i < sizeof gCfg.fwDir - 1u && gCfg.fwDir[i] && at < sizeof path - 32u; ++i)
            path[at++] = gCfg.fwDir[i];
        path[at++] = '/';
        for (unsigned i = 0; name[i] && at < sizeof path - 1u; ++i) path[at++] = name[i];
        path[at] = 0;
        rc = tb710fu_fw_open(path, &handle, &bytes);
    }
    if (rc || !handle || !bytes || bytes > (64ull << 20)) {
        btprintf("firmware %s not found (addon and %s)\n", name, gCfg.fwDir);
        return nullptr;
    }
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocData((vm_size_t)bytes));
    if (!buffer) {
        tb710fu_fw_close(handle);
        return nullptr;
    }
    if (tb710fu_fw_read(handle, 0, buffer, (size_t)bytes) != 0) {
        IOFreeData(buffer, (vm_size_t)bytes);
        tb710fu_fw_close(handle);
        return nullptr;
    }
    tb710fu_fw_close(handle);
    *size = (uint32_t)bytes;
    return buffer;
}

/* ---- stages ------------------------------------------------------------------------------------- */

bool failStage(int stage, int error)
{
    native_tb710fu_bt_stats[1] = error;
    native_tb710fu_bt_stats[0] = -stage;
    btprintf("stage %d failed (%d)\n", stage, error);
    return false;
}

uint8_t baudCode(uint32_t baud)
{
    /* enum qca_baudrate in btqca.h, in declaration order; only the rates this board can clock. */
    switch (baud) {
    case 115200u: return 0u;
    case 921600u: return 10u;
    case 3000000u: return 14u;
    case 3200000u: return 17u;
    case 4000000u: return 15u;
    default: return 0xffu;
    }
}

bool stagePower()
{
    native_tb710fu_bt_stats[0] = 1;
    if (!mapWindow(gSe, TB_BT_SE_PHYS, TB_BT_SE_BYTES) ||
        !mapWindow(gGcc, TB_BT_GCC_PHYS, TB_BT_GCC_BYTES) ||
        !mapWindow(gWrapper, TB_BT_WRAPPER_PHYS, TB_BT_WRAPPER_BYTES) ||
        !mapWindow(gTlmm, TB_BT_TLMM_PHYS, TB_BT_TLMM_BYTES)) {
        return failStage(1, -ENODEV);
    }
    gMapped = true;
    const uint32_t hw = rd32(gWrapper, kHwVerReg);
    native_tb710fu_bt_stats[2] = (int32_t)hw;
    btprintf("QUP2 hw-version %08x (QUP >= 2.5 means oversampling 16)\n", (unsigned)hw);

    const TbBtRailOps *rails = tb710fu_bluetooth_rail_provider();
    if (tb_bt_power_on(&kPowerIo, rails, &gPower)) return failStage(1, -EIO);
    btprintf("power bt_en=%u sw_ctrl=%u rails=%u\n", (unsigned)gPower.bt_en_high,
             (unsigned)gPower.sw_ctrl_high, (unsigned)gPower.rail_provider);

    if (tb_bt_geni_probe(&gGeni, &kGeniOps)) return failStage(1, -EINVAL);
    native_tb710fu_bt_stats[3] = (int32_t)gGeni.proto;
    native_tb710fu_bt_stats[4] = (int32_t)(gGeni.tx_depth_words | (gGeni.rx_depth_words << 16));
    btprintf("se proto=%u tx=%u words width=%u rx=%u words\n", (unsigned)gGeni.proto,
             (unsigned)gGeni.tx_depth_words, (unsigned)gGeni.tx_width_bits, (unsigned)gGeni.rx_depth_words);
    if (gGeni.proto != 2u) {
        /* enum geni_se_protocol_type: UART is 2, 255 means no protocol firmware. Loading it needs
         * qcom_scm, which this kernel does not have; the boot chain has to have done that. */
        btprintf("SE does not carry the UART protocol firmware (%u); no SCM loader in this kernel\n",
                 (unsigned)gGeni.proto);
        return failStage(1, -EPROTONOSUPPORT);
    }

    if (tb_bt_clock_plan(TB_BT_INIT_SPEED, &gPlan)) return failStage(1, -EINVAL);
    TbBtClockEvidence evidence = {};
    if (!tb_bt_clock_vote(&kClockIo, true, &evidence)) return failStage(1, -EIO);
    if (tb_bt_clock_program(&kClockIo, &gPlan, &evidence)) return failStage(1, -ETIMEDOUT);
    native_tb710fu_bt_stats[5] = (int32_t)evidence.cmd;
    native_tb710fu_bt_stats[6] = (int32_t)evidence.cfg;
    native_tb710fu_bt_stats[7] = (int32_t)evidence.branch_after;
    btprintf("rcg cmd=%08x cfg=%08x m=%08x n=%08x d=%08x branch %08x->%08x\n", (unsigned)evidence.cmd,
             (unsigned)evidence.cfg, (unsigned)evidence.m, (unsigned)evidence.n, (unsigned)evidence.d,
             (unsigned)evidence.branch_before, (unsigned)evidence.branch_after);

    gGeni.baud = gPlan.baud;
    gGeni.clk_rate = gPlan.rate;
    gGeni.clk_div = gPlan.clk_div;
    if (tb_bt_geni_set_rate(&gGeni, gPlan.baud, gPlan.rate, gPlan.clk_div)) return failStage(1, -EINVAL);
    if (tb_bt_geni_config(&gGeni)) return failStage(1, -EIO);
    if (tb_bt_geni_set_flow(&gGeni, true)) return failStage(1, -EIO);
    native_tb710fu_bt_stats[8] = (int32_t)gGeni.baud;
    native_tb710fu_bt_stats[9] = (int32_t)gGeni.clk_div;
    native_tb710fu_bt_stats[20] = tb_bt_geni_cts(&gGeni) ? 1 : 0;
    native_tb710fu_bt_stats[21] = gGeni.flow ? 1 : 0;
    btprintf("uart %u baud, serial clock %u div %u, flow control on, cts=%u\n", (unsigned)gGeni.baud,
             (unsigned)gGeni.clk_rate, (unsigned)gGeni.clk_div, (unsigned)(native_tb710fu_bt_stats[20] != 0));
    return true;
}

bool stageResetAndVersion()
{
    native_tb710fu_bt_stats[0] = 2;
    /* btqca's flow relies on the reset after the NVM download, not before it. The task list asks this
     * stage for a Reset first, so it is sent best-effort: a ROM that ignores 0x0C03 before the patch
     * must not take the whole stage down with it. */
    Tap reset = {};
    const int resetRc = hciTransact(TB_BT_OP_RESET, nullptr, 0u, 1000u, &reset);
    btprintf("hci reset (rom) rc=%d status=%02x\n", resetRc, reset.got ? (unsigned)reset.status : 0xffu);

    TbBtQcaVersion version = {};
    if (tb_bt_qca_read_version(&kQcaIo, &version)) return failStage(2, -EIO);
    native_tb710fu_bt_stats[10] = (int32_t)version.product_id;
    native_tb710fu_bt_stats[11] = (int32_t)version.rom_version;
    btprintf("rom product=%08x rom_version=%04x patch=%04x soc_id=%08x (version at reply+%u)\n",
             (unsigned)version.product_id, (unsigned)version.rom_version, (unsigned)version.patch_version,
             (unsigned)version.soc_id, (unsigned)version.parse_offset);
    for (unsigned i = 0; i < version.raw_len && i < 6u; ++i)
        btprintf("version[%u]=%02x\n", i, (unsigned)version.raw[i]);

    if (!gCfg.speed || gCfg.speed == TB_BT_INIT_SPEED) return true;
    TbBtClockPlan fast = {};
    if (tb_bt_clock_plan(gCfg.speed, &fast)) {
        btprintf("speed %u is not in the clock table; staying at %u\n", (unsigned)gCfg.speed,
                 (unsigned)gGeni.baud);
        return true;
    }
    const uint8_t code = baudCode(fast.baud);
    if (code == 0xffu) return failStage(2, -EINVAL);

    /* hci_qca's qca_set_baudrate: flow control off, the vendor command at the old rate, then the host
     * side moves. Bytes still in the FIFO belong to the old rate. */
    if (tb_bt_geni_set_flow(&gGeni, false)) return failStage(2, -EIO);
    uint32_t drained = 0;
    for (unsigned i = 0; i < 4u; ++i) {
        uint8_t junk[64];
        uint32_t got = 0;
        (void)tb_bt_geni_read(&gGeni, junk, sizeof junk, &got);
        drained += got;
    }
    if (tb_bt_qca_set_baudrate(&kQcaIo, code)) return failStage(2, -EIO);
    TbBtClockEvidence evidence = {};
    if (tb_bt_clock_program(&kClockIo, &fast, &evidence)) return failStage(2, -ETIMEDOUT);
    if (tb_bt_geni_set_rate(&gGeni, fast.baud, fast.rate, fast.clk_div)) return failStage(2, -EINVAL);
    if (tb_bt_geni_set_flow(&gGeni, true)) return failStage(2, -EIO);
    gPlan = fast;
    native_tb710fu_bt_stats[8] = (int32_t)gGeni.baud;
    native_tb710fu_bt_stats[9] = (int32_t)gGeni.clk_div;
    native_tb710fu_bt_stats[6] = (int32_t)evidence.cfg;
    btprintf("switched to %u baud (qca code %u, serial clock %u div %u, drained %u)\n", (unsigned)gGeni.baud,
             (unsigned)code, (unsigned)gGeni.clk_rate, (unsigned)gGeni.clk_div, (unsigned)drained);
    return true;
}

bool stageFirmware()
{
    native_tb710fu_bt_stats[0] = 3;
    uint32_t patchSize = 0, nvmSize = 0;
    uint8_t *patch = loadFirmware(TB_BT_FW_PATCH_NAME, &patchSize);
    uint8_t *nvm = loadFirmware(TB_BT_FW_NVM_NAME, &nvmSize);
    if (!patch || !nvm) {
        if (patch) IOFreeData(patch, patchSize);
        if (nvm) IOFreeData(nvm, nvmSize);
        return failStage(3, -ENOENT);
    }
    native_tb710fu_bt_stats[22] = (int32_t)patchSize;
    native_tb710fu_bt_stats[23] = (int32_t)nvmSize;

    TbBtQcaTlv tlv = {};
    TbBtQcaPatch patchHdr = {};
    const int tlvRc = tb_bt_qca_tlv_parse(patch, patchSize, &tlv);
    if (tlvRc < 0 || tlv.type != TB_BT_QCA_TLV_PATCH ||
        tb_bt_qca_patch_parse(patch, patchSize, &patchHdr)) {
        IOFreeData(patch, patchSize);
        IOFreeData(nvm, nvmSize);
        return failStage(3, -EILSEQ);
    }
    if (tlvRc == 1)
        btprintf("patch TLV declares %u bytes after its 4-byte header but the file has %u\n",
                 (unsigned)tlv.length, (unsigned)patchSize);
    native_tb710fu_bt_stats[12] = (int32_t)patchHdr.product_id;
    native_tb710fu_bt_stats[13] = (int32_t)patchHdr.patch_version;
    btprintf("patch %s %u bytes type %u len %u product %04x patch %04x mode %u\n", TB_BT_FW_PATCH_NAME,
             (unsigned)patchSize, (unsigned)tlv.type, (unsigned)tlv.length, (unsigned)patchHdr.product_id,
             (unsigned)patchHdr.patch_version, (unsigned)patchHdr.download_mode);

    TbBtQcaNvm nvmInfo = {};
    const uint8_t code = baudCode(gGeni.baud);
    if (tb_bt_qca_nvm_prepare(nvm, nvmSize, code == 0xffu ? (uint8_t)TB_BT_QCA_BAUD_3200000 : code, &nvmInfo)) {
        IOFreeData(patch, patchSize);
        IOFreeData(nvm, nvmSize);
        return failStage(3, -EILSEQ);
    }
    native_tb710fu_bt_stats[17] = (int32_t)nvmInfo.tags;
    btprintf("nvm %s %u bytes tags %u (hci %u, deep-sleep %u, bdaddr %u) baud code %u\n", TB_BT_FW_NVM_NAME,
             (unsigned)nvmSize, (unsigned)nvmInfo.tags, (unsigned)nvmInfo.tag_hci,
             (unsigned)nvmInfo.tag_deep_sleep, (unsigned)nvmInfo.tag_bd_addr,
             (unsigned)nvmInfo.baud_code_written);

    if (tb_bt_qca_download_patch(&kQcaIo, patch, patchSize, &gQcaStats)) {
        IOFreeData(patch, patchSize);
        IOFreeData(nvm, nvmSize);
        return failStage(3, -EIO);
    }
    native_tb710fu_bt_stats[14] = (int32_t)gQcaStats.patch_segments;
    btprintf("patch: %u segments (%u acked)\n", (unsigned)gQcaStats.patch_segments,
             (unsigned)gQcaStats.patch_acked);

    delayMs(10); /* btqca gives the controller 10 ms to settle before asking for the board id */

    uint16_t boardId = 0;
    if (tb_bt_qca_read_board_id(&kQcaIo, &boardId)) {
        IOFreeData(patch, patchSize);
        IOFreeData(nvm, nvmSize);
        return failStage(3, -EIO);
    }
    native_tb710fu_bt_stats[16] = (int32_t)boardId;
    btprintf("board id %u\n", (unsigned)boardId);

    if (tb_bt_qca_download_nvm(&kQcaIo, nvm, nvmSize, &gQcaStats)) {
        IOFreeData(patch, patchSize);
        IOFreeData(nvm, nvmSize);
        return failStage(3, -EIO);
    }
    native_tb710fu_bt_stats[15] = (int32_t)gQcaStats.nvm_segments;
    btprintf("nvm: %u segments (%u acked)\n", (unsigned)gQcaStats.nvm_segments, (unsigned)gQcaStats.nvm_acked);
    IOFreeData(patch, patchSize);
    IOFreeData(nvm, nvmSize);

    if (tb_bt_qca_disable_logging(&kQcaIo)) return failStage(3, -EIO);

    /* The authoritative reset: btqca performs it after the NVM and before the build info. */
    Tap reset = {};
    if (hciTransact(TB_BT_OP_RESET, nullptr, 0u, 2000u, &reset) || !reset.got || reset.status != 0u)
        return failStage(3, -EIO);
    btprintf("hci reset ok\n");

    if (tb_bt_qca_read_build_info(&kQcaIo, (uint8_t *)gBuildLabel, (uint32_t)sizeof gBuildLabel,
                                  &gBuildLabelLen) == 0) {
        const uint32_t at = gBuildLabelLen < sizeof gBuildLabel ? gBuildLabelLen : (uint32_t)sizeof gBuildLabel - 1u;
        gBuildLabel[at] = 0;
        btprintf("build info: %s\n", gBuildLabel);
    } else {
        btprintf("build info unavailable (non-fatal)\n");
    }

    if (tb_bt_qca_read_bdaddr(&kQcaIo, gBdAddr)) return failStage(3, -EIO);
    native_tb710fu_bt_stats[18] = (int32_t)((uint32_t)gBdAddr[0] | ((uint32_t)gBdAddr[1] << 8) |
                                            ((uint32_t)gBdAddr[2] << 16) | ((uint32_t)gBdAddr[3] << 24));
    native_tb710fu_bt_stats[19] = (int32_t)((uint32_t)gBdAddr[4] | ((uint32_t)gBdAddr[5] << 8));
    btprintf("bd addr %02x:%02x:%02x:%02x:%02x:%02x\n", (unsigned)gBdAddr[0], (unsigned)gBdAddr[1],
             (unsigned)gBdAddr[2], (unsigned)gBdAddr[3], (unsigned)gBdAddr[4], (unsigned)gBdAddr[5]);

    if (gCfg.bdAddrSet) {
        bool allZero = true, allOnes = true;
        for (unsigned i = 0; i < 6u; ++i) {
            allZero = allZero && gBdAddr[i] == 0u;
            allOnes = allOnes && gBdAddr[i] == 0xffu;
        }
        if (!allZero && !allOnes) {
            btprintf("controller already has an address; the configured one is not written\n");
        } else if (tb_bt_qca_write_bdaddr(&kQcaIo, gCfg.bdAddr) == 0 &&
                   tb_bt_qca_read_bdaddr(&kQcaIo, gBdAddr) == 0) {
            btprintf("bd addr now %02x:%02x:%02x:%02x:%02x:%02x\n", (unsigned)gBdAddr[0], (unsigned)gBdAddr[1],
                     (unsigned)gBdAddr[2], (unsigned)gBdAddr[3], (unsigned)gBdAddr[4], (unsigned)gBdAddr[5]);
        } else {
            btprintf("bd addr write failed (non-fatal)\n");
        }
    }
    return true;
}

/* ---- configuration ----------------------------------------------------------------------------- */

int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseBdAddr(const char *text, uint8_t out[6])
{
    unsigned at = 0;
    for (unsigned i = 0; i < 6u; ++i) {
        while (text[at] == ' ' || text[at] == ':') at++;
        const int hi = hexNibble(text[at]);
        const int lo = hexNibble(text[at + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        at += 2;
        if (i < 5u && text[at] != ':') return false;
        if (text[at] == ':') at++;
    }
    return true;
}

void readConfig()
{
    uint32_t level = 0;
    gCfg.log = 1u;
    gCfg.speed = TB_BT_MAX_SPEED;
    gCfg.level = PE_parse_boot_argn("bt", &level, sizeof level) ? level : 0u;
    if (gCfg.level > 4u) gCfg.level = 4u;
    uint32_t value = 0;
    if (PE_parse_boot_argn("tb710fu-bt-log", &value, sizeof value)) gCfg.log = value;
    if (PE_parse_boot_argn("tb710fu-bt-speed", &value, sizeof value) && value >= 115200u &&
        value <= kMaxBaudPlan)
        gCfg.speed = value;
    char text[160] = {};
    if (PE_parse_boot_argn("tb710fu-bt-fw", text, sizeof text)) {
        unsigned at = 0;
        while (text[at] && at < sizeof gCfg.fwDir - 1u) {
            gCfg.fwDir[at] = text[at];
            at++;
        }
        gCfg.fwDir[at] = 0;
    } else {
        const char *fallback = TB_BT_FW_DIR;
        unsigned at = 0;
        while (fallback[at] && at < sizeof gCfg.fwDir - 1u) {
            gCfg.fwDir[at] = fallback[at];
            at++;
        }
        gCfg.fwDir[at] = 0;
    }
    char addr[32] = {};
    if (PE_parse_boot_argn("tb710fu-bt-bdaddr", addr, sizeof addr) && parseBdAddr(addr, gCfg.bdAddr))
        gCfg.bdAddrSet = true;
}

void setText(const char *text)
{
    unsigned at = 0;
    for (; text[at] && at < sizeof native_tb710fu_bt_text - 1u; ++at) native_tb710fu_bt_text[at] = text[at];
    native_tb710fu_bt_text[at] = 0;
}

void teardown()
{
    if (gLock) {
        IOLockFree(gLock);
        gLock = nullptr;
    }
    unmapWindow(gSe);
    unmapWindow(gGcc);
    unmapWindow(gWrapper);
    unmapWindow(gTlmm);
    gMapped = false;
    gReady = false;
}

}  // namespace

/* ---- IOKit service and user client --------------------------------------------------------------- */

class Tb710fuBluetooth : public IOService {
    OSDeclareDefaultStructors(Tb710fuBluetooth)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
};

class Tb710fuBluetoothClient : public IOUserClient {
    OSDeclareDefaultStructors(Tb710fuBluetoothClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    void free() override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;

    static IOReturn writeHci(OSObject *target, void *reference, IOExternalMethodArguments *arguments);
    static IOReturn readHci(OSObject *target, void *reference, IOExternalMethodArguments *arguments);
    static IOReturn status(OSObject *target, void *reference, IOExternalMethodArguments *arguments);
    static IOReturn power(OSObject *target, void *reference, IOExternalMethodArguments *arguments);
};

enum {
    kTbBtMethodWriteHci = 0,
    kTbBtMethodReadHci = 1,
    kTbBtMethodStatus = 2,
    kTbBtMethodPower = 3,
    kTbBtMethodCount = 4,
};

static const IOExternalMethodDispatch kTbBtDispatch[kTbBtMethodCount] = {
    /* write: one whole H4 command (260 bytes covers the largest command this transport allows);
     * out: {bytes written, event bytes appended to the ring, status byte, reply kind} */
    {(IOExternalMethodAction)&Tb710fuBluetoothClient::writeHci, 0, 260, 0, 16},
    /* read: scalar in = how many bytes the caller wants, scalar out = bytes returned, out struct = raw H4 */
    {(IOExternalMethodAction)&Tb710fuBluetoothClient::readHci, 1, 0, 1, 260},
    /* status: 24 words, see native_tb710fu_bt_stats for each slot */
    {(IOExternalMethodAction)&Tb710fuBluetoothClient::status, 0, 0, 0, 96},
    /* power: scalar in 0 = off, 1 = the bring-up power pulse again */
    {(IOExternalMethodAction)&Tb710fuBluetoothClient::power, 1, 0, 0, 0},
};

OSDefineMetaClassAndStructors(Tb710fuBluetooth, IOService)
OSDefineMetaClassAndStructors(Tb710fuBluetoothClient, IOUserClient)

bool Tb710fuBluetooth::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    setProperty("IOUserClientClass", "Tb710fuBluetoothClient");
    setProperty("tb710fu-bt-stage", (unsigned long long)native_tb710fu_bt_stats[0], 32);
    setProperty("tb710fu-bt-baud", (unsigned long long)gGeni.baud, 32);
    setProperty("tb710fu-bt-qup-hw-version", (unsigned long long)(uint32_t)native_tb710fu_bt_stats[2], 32);
    if (native_tb710fu_bt_stats[10])
        setProperty("tb710fu-bt-product-id", (unsigned long long)(uint32_t)native_tb710fu_bt_stats[10], 32);
    if (gBuildLabelLen) setProperty("tb710fu-bt-build-info", gBuildLabel);
    btprintf("service published (stage %d, %u baud)\n", native_tb710fu_bt_stats[0], (unsigned)gGeni.baud);
    return true;
}

void Tb710fuBluetooth::stop(IOService *provider)
{
    gReady = false;
    IOService::stop(provider);
}

void Tb710fuBluetooth::free()
{
    teardown();
    IOService::free();
}

IOReturn Tb710fuBluetooth::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                         OSDictionary *properties, IOUserClient **handler)
{
    if (type != 0u) return kIOReturnUnsupported;
    Tb710fuBluetoothClient *client = OSTypeAlloc(Tb710fuBluetoothClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

bool Tb710fuBluetoothClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (type != 0u) return false;
    return IOUserClient::initWithTask(owningTask, securityID, type);
}

bool Tb710fuBluetoothClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    if (!gReady) {
        btprintf("user client refused: the transport is not live (raise bt=)\n");
        return false;
    }
    return true;
}

void Tb710fuBluetoothClient::stop(IOService *provider) { IOUserClient::stop(provider); }
void Tb710fuBluetoothClient::free() { IOUserClient::free(); }

IOReturn Tb710fuBluetoothClient::clientClose()
{
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn Tb710fuBluetoothClient::clientDied() { return clientClose(); }

IOReturn Tb710fuBluetoothClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                                IOExternalMethodDispatch *dispatch, OSObject *target,
                                                void *reference)
{
    if (selector >= (uint32_t)kTbBtMethodCount) return kIOReturnUnsupported;
    dispatch = const_cast<IOExternalMethodDispatch *>(&kTbBtDispatch[selector]);
    if (!target) target = this;
    return IOUserClient::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn Tb710fuBluetoothClient::writeHci(OSObject *, void *, IOExternalMethodArguments *arguments)
{
    uint32_t result[4] = {0u, 0u, 0xffu, (uint32_t)TB_BT_QCA_REPLY_NONE};
    if (!arguments->structureInput || arguments->structureInputSize < 4u) return kIOReturnBadArgument;
    const uint8_t *packet = static_cast<const uint8_t *>(arguments->structureInput);
    if (packet[0] != TB_BT_H4_CMD) return kIOReturnUnsupported;
    const uint32_t plen = packet[3];
    const uint32_t total = 4u + plen;
    if (total > arguments->structureInputSize || total > kHciEventCap) return kIOReturnBadArgument;
    const uint16_t opcode = (uint16_t)(packet[1] | ((uint16_t)packet[2] << 8));
    if (!arguments->structureOutput || arguments->structureOutputSize < sizeof result) return kIOReturnNoSpace;

    IOLockLock(gLock);
    if (!gReady) {
        IOLockUnlock(gLock);
        return kIOReturnNotReady;
    }
    const uint32_t before = gRingWritten;
    const int wrote = tb_bt_geni_write(&gGeni, packet, total);
    if (wrote <= 0) {
        IOLockUnlock(gLock);
        uint32_t *out = static_cast<uint32_t *>(arguments->structureOutput);
        for (unsigned i = 0; i < 4u; ++i) out[i] = result[i];
        return kIOReturnIOError;
    }
    tapArm(opcode);
    (void)hciWait(500u);
    const Tap tap = gTap;
    result[0] = (uint32_t)wrote;
    result[1] = gRingWritten - before;
    result[2] = tap.got ? tap.status : 0xffu;
    result[3] = tap.got ? (uint32_t)tap.kind : (uint32_t)TB_BT_QCA_REPLY_NONE;
    IOLockUnlock(gLock);
    uint32_t *out = static_cast<uint32_t *>(arguments->structureOutput);
    for (unsigned i = 0; i < 4u; ++i) out[i] = result[i];
    return tap.got ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn Tb710fuBluetoothClient::readHci(OSObject *, void *, IOExternalMethodArguments *arguments)
{
    if (!arguments->structureOutput || !arguments->scalarOutput) return kIOReturnBadArgument;
    uint32_t want = arguments->scalarInputCount ? (uint32_t)arguments->scalarInput[0] : 0u;
    if (want > kHciEventCap) want = kHciEventCap;
    IOLockLock(gLock);
    const uint32_t got = gReady ? ringPop(static_cast<uint8_t *>(arguments->structureOutput), want) : 0u;
    IOLockUnlock(gLock);
    arguments->scalarOutput[0] = got;
    return kIOReturnSuccess;
}

IOReturn Tb710fuBluetoothClient::status(OSObject *, void *, IOExternalMethodArguments *arguments)
{
    if (!arguments->structureOutput || arguments->structureOutputSize < sizeof native_tb710fu_bt_stats)
        return kIOReturnBadArgument;
    uint32_t *out = static_cast<uint32_t *>(arguments->structureOutput);
    for (unsigned i = 0; i < sizeof native_tb710fu_bt_stats / sizeof(uint32_t); ++i)
        out[i] = (uint32_t)native_tb710fu_bt_stats[i];
    return kIOReturnSuccess;
}

IOReturn Tb710fuBluetoothClient::power(OSObject *, void *, IOExternalMethodArguments *arguments)
{
    if (!arguments->scalarInputCount) return kIOReturnBadArgument;
    const uint32_t on = (uint32_t)arguments->scalarInput[0];
    IOLockLock(gLock);
    const int rc = on ? tb_bt_power_on(&kPowerIo, tb710fu_bluetooth_rail_provider(), &gPower)
                      : tb_bt_power_off(&kPowerIo, tb710fu_bluetooth_rail_provider());
    IOLockUnlock(gLock);
    return rc == 0 ? kIOReturnSuccess : kIOReturnError;
}

/* ---- bring-up thread --------------------------------------------------------------------------- */

namespace {

bool bringUp()
{
    gLock = IOLockAlloc();
    if (!gLock) return false;
    tb_bt_h4_parser_init(&gParser, rxSink, nullptr);
    setText("mapping");
    if (!stagePower()) {
        setText(!gMapped ? "mapping" : (gGeni.proto == 2u ? "stage1" : "no uart se firmware"));
        tb_bt_power_off(&kPowerIo, tb710fu_bluetooth_rail_provider());
        teardown();
        return false;
    }
    if (gCfg.level < 2u) {
        setText("stage1 ok");
        teardown();
        return true;
    }
    if (!stageResetAndVersion()) {
        setText("stage2");
        tb_bt_power_off(&kPowerIo, tb710fu_bluetooth_rail_provider());
        teardown();
        return false;
    }
    if (gCfg.level < 3u) {
        setText("stage2 ok");
        teardown();
        return true;
    }
    if (!stageFirmware()) {
        setText("stage3");
        tb_bt_power_off(&kPowerIo, tb710fu_bluetooth_rail_provider());
        teardown();
        return false;
    }
    if (gCfg.level < 4u) {
        setText("stage3 ok");
        teardown();
        return true;
    }
    setText("stage4");
    native_tb710fu_bt_stats[0] = 4;
    gReady = true;
    return true;
}

void btWorker(void *, wait_result_t)
{
    if (!gCfg.level) {
        thread_terminate(current_thread());
        return;
    }
    if (!bringUp()) {
        btprintf("bring-up stopped at %s\n", native_tb710fu_bt_text);
        if (native_tb710fu_bt_stats[0] > 0) native_tb710fu_bt_stats[0] = -(int32_t)gCfg.level;
        thread_terminate(current_thread());
        return;
    }
    if (native_tb710fu_bt_stats[0] != 4) {
        btprintf("stage %d complete; stopping before the HCI service (bt=%u)\n",
                 native_tb710fu_bt_stats[0], (unsigned)gCfg.level);
        thread_terminate(current_thread());
        return;
    }

    IOService *resources = IOService::getResourceService();
    for (unsigned tries = 0; !resources && tries < 50u; ++tries) {
        IOSleep(100);
        resources = IOService::getResourceService();
    }
    if (!resources) {
        btprintf("IOResources unavailable\n");
        teardown();
        thread_terminate(current_thread());
        return;
    }
    Tb710fuBluetooth *service = OSTypeAlloc(Tb710fuBluetooth);
    if (!service || !service->init() || !service->attach(resources)) {
        if (service) service->release();
        btprintf("service did not attach\n");
        teardown();
        thread_terminate(current_thread());
        return;
    }
    service->registerService();
    service->release(); /* the registry attachment keeps it */
    btprintf("registered (bt=%u, %u baud, ring dropped %u)\n", (unsigned)gCfg.level, (unsigned)gGeni.baud,
             (unsigned)gRingDropped);
    thread_terminate(current_thread());
}

}  // namespace

extern "C" void tb710fu_bluetooth_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuBluetooth.cpp");

extern "C" void tb710fu_bluetooth_probe(void)
{
    readConfig();
    if (!gCfg.level) {
        printf("TB710FU BT: not requested (add bt=1..4)\n");
        return;
    }
    if (gCfg.speed && gCfg.speed != TB_BT_INIT_SPEED) {
        TbBtClockPlan fast = {};
        if (tb_bt_clock_plan(gCfg.speed, &fast)) {
            printf("TB710FU BT: bt-speed=%u has no exact clock plan; using 3200000\n", (unsigned)gCfg.speed);
            gCfg.speed = TB_BT_MAX_SPEED;
        }
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(btWorker, nullptr, &thread) != KERN_SUCCESS) {
        printf("TB710FU BT: worker spawn failed\n");
        return;
    }
    thread_deallocate(thread);
    printf("TB710FU BT: worker started (bt=%u max-speed=%u)\n", (unsigned)gCfg.level, (unsigned)gCfg.speed);
}
