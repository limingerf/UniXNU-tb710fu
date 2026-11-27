/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB710FU touchscreen (Novatek NT36532 over QUPv3 GENI SPI) and PMIC keys.
 *
 * This file is the only part that touches IOKit. Every decision it makes is
 * implemented in the C layer next to it and host-tested:
 *   Tb710fuGeni.c        SE register model and FIFO transfer
 *   Tb710fuNvt.c         NT36532 framing, chip id, 60x40 decode
 *   Tb710fuTracker.c     stable contact identifiers and clamping
 *   Tb710fuHid.c         descriptors and report packing
 *   Tb710fuKeys.c        PON/GPIO key policy
 *   Tb710fuSpmi.c        arbiter v7 observer reads
 *
 * IOKit-side facts that are not negotiable on macOS 27.3 arm64e:
 *
 *   - IOHIDDevice belongs to a prelinked Apple kext. Naming it in C++ would
 *     create link symbols this fileset does not export, so the device is
 *     allocated by the live metaclass (`OSMetaClass::allocClassWithName`, which
 *     runs the class's own constructor and field initialisers) and its vtable
 *     is adopted: every entry is copied, re-signed for the new slot address
 *     with the diversity the shipped table carries, and exactly one slot
 *     (newReportDescriptor, 284) is replaced. The table and the named slots
 *     come from this board's prelinked kernel, see Tb710fuInputHidSlots.h.
 *   - the vptr itself is signed with ptrauth_key_cxx_vtable_pointer and the
 *     OSMetaClassBase diversity 0xcda1 blended with the object address, the
 *     same convention package/Tb710fuEthernet.cpp uses.
 *   - MMIO is mapped through IODeviceMemory and touched only through the
 *     aligned 32-bit helpers in Tb710fuMem.h: no memcpy/memset over a mapping
 *     (ESR 0x96000061 on SM8650) and no volatile++ arithmetic.
 */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
#include "Tb710fuGeni.h"
#include "Tb710fuHid.h"
#include "Tb710fuInput.h"
#include "Tb710fuInputConfig.h"
#include "Tb710fuInputHardware.h"
#include "Tb710fuInputHidSlots.h"
#include "Tb710fuKeys.h"
#include "Tb710fuMem.h"
#include "Tb710fuNvt.h"
#include "Tb710fuNvtFirmware.h"
#include "Tb710fuSpmi.h"
#include "Tb710fuTracker.h"
}

#if defined(__arm64e__)
#include <ptrauth.h>
#else
#error "TB710FU input targets arm64e: pointer authentication is part of the ABI"
#endif

#define TB710FU_LOG(fmt, ...) IOLog("TB710FU input: " fmt, ##__VA_ARGS__)

namespace {

/* ---- mapping ---- */

/* Plain data so the file has no static constructors: every global below is
 * trivially initialised and nothing runs before the service list calls in. */
struct Region {
    IOMemoryMap *keep;
    uintptr_t va;
};

bool map_region(uint64_t phys, uint64_t length, Region &region)
{
    IODeviceMemory *memory = IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOByteCount)length);

    if (!memory) return false;
    region.keep = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    region.va = region.keep ? (uintptr_t)region.keep->getVirtualAddress() : 0;
    return region.va != 0;
}

inline uint32_t rd32(const Region &region, uint32_t offset)
{
    return tb710fu_mmio_read32((const void *)region.va, offset);
}

inline void wr32(const Region &region, uint32_t offset, uint32_t value)
{
    tb710fu_mmio_write32((void *)region.va, offset, value);
    tb710fu_mmio_fence();
}

/* ---- state ---- */

Region gSe;
Region gWrap;
Region gSpmiCore;
Region gSpmiObs;
Region gSpmiCnfg;

struct tb710fu_geni_spi gGeni;
struct tb710fu_nvt_chip gChip;
struct tb710fu_tracker gTracker;
struct tb710fu_spmi gSpmi;
struct tb710fu_keys gKeys;
struct tb710fu_key_filter gKeyFilter;
struct tb710fu_input_options gOptions;

struct InputBuffers {
    struct tb710fu_nvt_core core;
    uint8_t scratch[TB710FU_NVT_READ_MAX];
    uint8_t rx[TB710FU_NVT_READ_MAX];
    uint8_t frame[TB710FU_NVT_READ_BYTES];
    uint64_t touchVtable[TB710FU_HID_VTABLE_SLOTS];
    uint64_t keysVtable[TB710FU_HID_VTABLE_SLOTS];
};
InputBuffers *gBuf = nullptr;

::IOService *gTouchDevice = nullptr;
::IOService *gKeysDevice = nullptr;
::IOService *gHidResource = nullptr;

bool gTouchVtableReady = false;
bool gKeysVtableReady = false;

thread_t gTouchThread = nullptr;
thread_t gKeysThread = nullptr;
int gStop = 0;

IOBufferMemoryDescriptor *gTouchReport = nullptr;
IOBufferMemoryDescriptor *gKeysReport = nullptr;
uint8_t gLastTouch[TB710FU_HID_TOUCH_BYTES];
uint8_t gLastKeys[TB710FU_HID_KEY_BYTES];

/* ---- GENI callbacks ---- */

uint32_t geni_read(void *, uint32_t offset)
{
    return rd32(gSe, offset);
}

void geni_write(void *, uint32_t offset, uint32_t value)
{
    wr32(gSe, offset, value);
}

uint32_t geni_read_wrapper(void *, uint32_t offset)
{
    return rd32(gWrap, offset);
}

void geni_delay_us(void *, unsigned microseconds)
{
    IODelay(microseconds);
}

/* ---- SPMI callbacks: `window` is the Region the probe registered ---- */

uint32_t spmi_read(void *, void *window, uint32_t offset)
{
    return rd32(*(const Region *)window, offset);
}

void spmi_write(void *, void *window, uint32_t offset, uint32_t value)
{
    wr32(*(const Region *)window, offset, value);
}

void spmi_barrier(void *)
{
    tb710fu_mmio_fence();
}

/* ---- NT36532 bus callback: one GENI SPI transaction, mode 0, CS0 ---- */

int nvt_bus_xfer(void *, const uint8_t *tx, uint8_t *rx, size_t len)
{
    return tb710fu_geni_transfer(&gGeni, tx, rx, len, 0) == TB710FU_GENI_OK ? 0 : -1;
}

/* ---- HID adoption ---- */

constexpr uint16_t kMetaClassBaseDiversity = 0xcda1; /* "_ZTV15OSMetaClassBase" */

bool is_kernel_pointer(const void *pointer)
{
    const uintptr_t value = (uintptr_t)pointer;

    /* Kernel text and data are canonical addresses above 0xfffff00000000000;
     * an authentication failure leaves a poisoned pointer outside that range. */
    return value >= (uintptr_t)0xfffff00000000000ULL && value != (uintptr_t)~0ULL;
}

IOReturn hid_new_report_descriptor(const void *self, IOMemoryDescriptor **out);

bool adopt_vtable(::IOService *device, uint64_t *table, bool touch)
{
    /* The live object's vptr is signed with the C++ vtable key; strip it before
     * treating the result as a table of function pointers. */
    void *live = ptrauth_strip(*(void **)(void *)device, ptrauth_key_cxx_vtable_pointer);
    unsigned index;

    if (!live || !is_kernel_pointer(live)) {
        TB710FU_LOG("hid: live vtable pointer unusable\n");
        return false;
    }
    for (index = 0u; index < TB710FU_HID_VTABLE_SLOTS; ++index) {
        const uint64_t entry = ((const uint64_t *)live)[index];
        const void *raw = ptrauth_strip((const void *)(uintptr_t)entry,
                                        ptrauth_key_function_pointer);

        if (!entry || !is_kernel_pointer(raw)) {
            TB710FU_LOG("hid: slot %u does not strip to kernel text\n", index);
            return false;
        }
        table[index] = (uint64_t)(uintptr_t)ptrauth_sign_unauthenticated(
            (void *)raw, ptrauth_key_function_pointer,
            ptrauth_blend_discriminator((void *)&table[index],
                                        tb710fu_hid_slot_diversity[index]));
    }
    {
        /* The family would otherwise dispatch into __cxa_pure_virtual: give
         * newReportDescriptor an implementation that selects the descriptor
         * by the device instance this probe started. */
        const unsigned slot = TB710FU_HID_SLOT_NEW_REPORT_DESC;

        table[slot] = (uint64_t)(uintptr_t)ptrauth_sign_unauthenticated(
            (void *)&hid_new_report_descriptor, ptrauth_key_function_pointer,
            ptrauth_blend_discriminator((void *)&table[slot],
                                        tb710fu_hid_slot_diversity[slot]));
    }
    {
        void *vptr = ptrauth_sign_unauthenticated(
            (void *)table, ptrauth_key_cxx_vtable_pointer,
            ptrauth_blend_discriminator((void *)device, kMetaClassBaseDiversity));

        *(void **)(void *)device = vptr;
    }
    if (touch) {
        gTouchVtableReady = true;
    } else {
        gKeysVtableReady = true;
    }
    return true;
}

typedef IOReturn (*HidReportFunction)(::IOService *, IOMemoryDescriptor *, uint32_t, uint32_t);

IOReturn submit_report(::IOService *device, uint64_t *table, IOBufferMemoryDescriptor *report,
                       const uint8_t *bytes, unsigned length)
{
    const unsigned slot = TB710FU_HID_SLOT_HANDLE_REPORT;
    void *function;

    if (!device || !table || !report || !bytes || length == 0u) return kIOReturnBadArgument;
    report->setLength(length);
    if (report->writeBytes(0, bytes, length) != length) return kIOReturnNoSpace;
    function = ptrauth_auth_function((void *)(uintptr_t)table[slot], ptrauth_key_function_pointer,
                                     ptrauth_blend_discriminator((void *)&table[slot],
                                                                 tb710fu_hid_slot_diversity[slot]));
    if (!function) return kIOReturnNotPermitted;
    return ((HidReportFunction)function)(device, report, 0u /* kIOHIDReportTypeInput */, 0u);
}

/* The family calls this through the adopted table; `self` is the IOHIDDevice
 * instance, which is how the two descriptors are told apart. */
IOReturn hid_new_report_descriptor(const void *self, IOMemoryDescriptor **out)
{
    const uint8_t *descriptor;
    unsigned length;

    if (!out) return kIOReturnBadArgument;
    if ((const void *)self == (const void *)gTouchDevice) {
        descriptor = tb710fu_hid_touch_descriptor;
        length = tb710fu_hid_touch_descriptor_bytes;
    } else if ((const void *)self == (const void *)gKeysDevice) {
        descriptor = tb710fu_hid_keys_descriptor;
        length = tb710fu_hid_keys_descriptor_bytes;
    } else {
        return kIOReturnNotAttached;
    }
    *out = IOBufferMemoryDescriptor::withBytes(descriptor, length, kIODirectionNone);
    return *out ? kIOReturnSuccess : kIOReturnNoMemory;
}

/* ---- HID device construction ---- */

::IOService *create_hid_device(const char *product, unsigned usage_page, unsigned usage,
                               unsigned vendor, unsigned product_id, uint64_t *table, bool touch)
{
    ::IOService *device = (::IOService *)OSMetaClass::allocClassWithName("IOHIDDevice");
    const OSMetaClass *meta;

    if (!device) {
        TB710FU_LOG("hid: IOHIDDevice allocation failed (%s)\n", product);
        return nullptr;
    }
    meta = device->getMetaClass();
    if (!meta || meta->getClassSize() != TB710FU_HID_INSTANCE_BYTES) {
        TB710FU_LOG("hid: IOHIDDevice extent is not 0x%x (%s)\n", TB710FU_HID_INSTANCE_BYTES,
                    product);
        device->release();
        return nullptr;
    }
    if (!adopt_vtable(device, table, touch)) {
        device->release();
        return nullptr;
    }
    if (!device->init()) {
        TB710FU_LOG("hid: init failed (%s)\n", product);
        device->release();
        return nullptr;
    }
    /* Property keys are the literal HID property names; the family's own
     * constants live in headers this fileset does not export. */
    device->setProperty("Product", product);
    device->setProperty("VendorID", vendor, 32);
    device->setProperty("ProductID", product_id, 32);
    device->setProperty("VersionNumber", 0x0100, 32);
    device->setProperty("Transport", "SPI");
    device->setProperty("PrimaryUsagePage", usage_page, 32);
    device->setProperty("PrimaryUsage", usage, 32);
    device->setProperty("Built-In", kOSBooleanTrue);
    device->setProperty("DisplayIntegrated", kOSBooleanTrue);
    /* The generic event personality: without it the family creates the
     * interface but no event service, and nothing reaches backboardd. */
    device->setProperty("HIDDefaultBehavior", kOSBooleanTrue);
    return device;
}

::IOService *wait_for_hid_resource(unsigned timeout_ms)
{
    unsigned waited = 0u;

    while (waited < timeout_ms && !gStop) {
        OSDictionary *matching = ::IOService::serviceMatching("IOHIDResource");
        ::IOService *resource = matching
                                    ? ::IOService::waitForMatchingService(matching, 1000000000ULL)
                                    : nullptr;

        if (matching) matching->release();
        if (resource) return resource;
        waited += 1000u;
        IOSleep(1000);
    }
    return nullptr;
}

bool start_hid_device(::IOService *device, const char *product)
{
    if (!device) return false;
    if (!gHidResource) {
        TB710FU_LOG("hid: no IOHIDResource provider (%s)\n", product);
        return false;
    }
    if (!device->attach(gHidResource)) {
        TB710FU_LOG("hid: attach failed (%s)\n", product);
        return false;
    }
    if (!device->start(gHidResource)) {
        TB710FU_LOG("hid: start failed (%s)\n", product);
        device->detach(gHidResource);
        return false;
    }
    device->registerService();
    /* backboardd reads Built-In, DisplayIntegrated and the usage pair from the
     * event service it opens, which is the device's parent after start. */
    if (IORegistryEntry *service = device->getParentEntry(gIOServicePlane)) {
        service->setProperty("Built-In", kOSBooleanTrue);
        service->setProperty("DisplayIntegrated", kOSBooleanTrue);
        service->setProperty("PrimaryUsagePage", device->getProperty("PrimaryUsagePage"));
        service->setProperty("PrimaryUsage", device->getProperty("PrimaryUsage"));
    }
    TB710FU_LOG("%s started\n", product);
    return true;
}

const OSMetaClass *find_meta_class(const char *name)
{
    const OSSymbol *symbol = OSSymbol::withCString(name);
    const OSMetaClass *meta = nullptr;

    if (!symbol) return nullptr;
    meta = OSMetaClass::getMetaClassWithName(symbol);
    symbol->release();
    return meta;
}

bool ensure_hid_family(void)
{
    const OSMetaClass *meta = find_meta_class("IOHIDDevice");

    /* IOHIDFamily is prelinked in this fileset; loading it explicitly is a
     * no-op when present and makes an absent kext a clear log line instead of
     * a link-time surprise. The class lookup below is the authority. */
    if (!meta) {
        (void)OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
        meta = find_meta_class("IOHIDDevice");
    }
    if (!meta) {
        TB710FU_LOG("hid: IOHIDDevice metaclass absent\n");
        return false;
    }
    if (meta->getClassSize() != TB710FU_HID_INSTANCE_BYTES) {
        TB710FU_LOG("hid: IOHIDDevice extent 0x%x, expected 0x%x\n",
                    (unsigned)meta->getClassSize(), TB710FU_HID_INSTANCE_BYTES);
        return false;
    }
    return true;
}

/* ---- touch path ---- */

void touch_loop(void);

void touch_thread_entry(void *, wait_result_t)
{
    touch_loop();
}

bool touch_prepare(void)
{
    if (!map_region(TB710FU_SE4_BASE, TB710FU_SE4_BYTES, gSe)) {
        TB710FU_LOG("touch: SE4 window did not map\n");
        return false;
    }
    if (!map_region(TB710FU_QUP_WRAP1_BASE, TB710FU_QUP_WRAP1_BYTES, gWrap)) {
        TB710FU_LOG("touch: QUP wrapper window did not map\n");
        return false;
    }
    gGeni.io.read = geni_read;
    gGeni.io.write = geni_write;
    gGeni.io.read_wrapper = geni_read_wrapper;
    gGeni.io.delay_us = geni_delay_us;
    gGeni.io.ctx = nullptr;
    gGeni.poll_only = gOptions.use_irq ? 0 : 1;
    gGeni.poll_limit = 200000u;
    gGeni.state = TB710FU_GENI_RESET;

    {
        int result = tb710fu_geni_read_info(&gGeni, gOptions.fuse_probe);

        /* A window that reads all-ones (or all-zero) is an SE the firmware has
         * not clocked; reporting it as "no device" keeps stage 1 read-only. */
        if (result != TB710FU_GENI_OK) {
            TB710FU_LOG("touch: SE4 identity failed (%d): window unclocked or not GENI\n",
                        result);
            return false;
        }
    }
    TB710FU_LOG("touch: SE4 geni-fw=0x%08x proto=%u qup-hw=0x%08x fifo=%u/%u status=0x%08x\n",
                gGeni.info.fw_revision, gGeni.info.protocol, gGeni.info.qup_hw_version,
                gGeni.info.tx_fifo_depth, gGeni.info.fifo_width_bits, gGeni.info.geni_status);
    if (gGeni.info.fuse_probed) {
        TB710FU_LOG("touch: fuse probe if_disable=0x%08x fifo_disabled=%u\n",
                    gGeni.info.if_disable, gGeni.info.fifo_disabled);
    }
    return true;
}

bool touch_engine(void)
{
    int result = tb710fu_geni_init(&gGeni);

    if (result != TB710FU_GENI_OK) {
        TB710FU_LOG("touch: GENI init failed (%d) state=%s\n", result,
                    tb710fu_geni_state_name(gGeni.state));
        return false;
    }
    result = tb710fu_geni_setup(&gGeni, TB710FU_SPI_MODE_CPOL | TB710FU_SPI_MODE_CPHA,
                                TB710FU_TS_CS);
    if (result != TB710FU_GENI_OK) {
        TB710FU_LOG("touch: GENI setup failed (%d)\n", result);
        return false;
    }
    /* The SE parent is the 19.2 MHz TCXO: 9.6 MHz divides it by two exactly,
     * and unlike GPLL0 it is guaranteed on for a QUP the firmware handed over. */
    result = tb710fu_geni_set_clock(&gGeni, TB710FU_TOUCH_SPI_HZ, 19200000u, 0u);
    if (result != TB710FU_GENI_OK) {
        TB710FU_LOG("touch: clock programming failed (%d)\n", result);
        return false;
    }
    TB710FU_LOG("touch: engine idle, spi=%u Hz\n", gGeni.cur_speed_hz);
    return true;
}

void touch_submit(const struct tb710fu_track_point *points, int count)
{
    struct tb710fu_hid_contact contacts[TB710FU_HID_TOUCH_MAX_CONTACTS];
    uint8_t report[TB710FU_HID_TOUCH_BYTES];
    unsigned length;
    int i;

    if (!gTouchReport || !gTouchVtableReady || !gTouchDevice) return;
    for (i = 0; i < (int)TB710FU_HID_TOUCH_MAX_CONTACTS; ++i) {
        contacts[i].id = 0u;
        contacts[i].active = 0;
        contacts[i].x = 0u;
        contacts[i].y = 0u;
    }
    for (i = 0; i < count && i < (int)TB710FU_HID_TOUCH_MAX_CONTACTS; ++i) {
        const int32_t x = points[i].x < 0 ? 0 : points[i].x;
        const int32_t y = points[i].y < 0 ? 0 : points[i].y;

        contacts[i].id = points[i].id & 0x7Fu;
        contacts[i].active = 1;
        contacts[i].x = (uint16_t)(x > (int32_t)TB710FU_HID_TOUCH_LOGICAL_X
                                       ? TB710FU_HID_TOUCH_LOGICAL_X
                                       : x);
        contacts[i].y = (uint16_t)(y > (int32_t)TB710FU_HID_TOUCH_LOGICAL_Y
                                       ? TB710FU_HID_TOUCH_LOGICAL_Y
                                       : y);
    }
    length = tb710fu_hid_touch_report(contacts, (unsigned)(count > 0 ? count : 0), report,
                                      sizeof(report));
    if (length == 0u) return;
    if (memcmp(report, gLastTouch, length) == 0) return;
    memcpy(gLastTouch, report, length);
    if (submit_report(gTouchDevice, gBuf->touchVtable, gTouchReport, report, length) != kIOReturnSuccess)
        TB710FU_LOG("touch: report rejected\n");
}

void touch_loop(void)
{
    unsigned frames = 0u;
    unsigned errors = 0u;

    while (!gStop) {
        struct tb710fu_nvt_contact blobs[TB710FU_NVT_MAX_CONTACTS];
        struct tb710fu_track_input tracked[TB710FU_NVT_MAX_CONTACTS];
        struct tb710fu_track_point points[TB710FU_TRACK_SLOTS];
        int blobs_count;
        int i;

        if (tb710fu_nvt_read_frame(&gChip, gBuf->frame) != TB710FU_NVT_OK) {
            errors++;
            if (errors == 1u || (errors % 100u) == 0u)
                TB710FU_LOG("touch: frame read failed (%u)\n", errors);
            (void)tb710fu_geni_recover(&gGeni);
            IOSleep(20);
            continue;
        }
        blobs_count = tb710fu_nvt_core_process(&gBuf->core, gBuf->frame, (int32_t)TB710FU_PANEL_WIDTH,
                                               (int32_t)TB710FU_PANEL_HEIGHT, blobs,
                                               (int)TB710FU_NVT_MAX_CONTACTS);
        if (blobs_count == TB710FU_NVT_ERR_FRAME) {
            errors++;
            if (errors == 1u || (errors % 100u) == 0u)
                TB710FU_LOG("touch: frame header invalid (%u)\n", errors);
            continue;
        }
        if (blobs_count < 0) continue;
        for (i = 0; i < blobs_count; ++i) {
            tracked[i].x = blobs[i].x;
            tracked[i].y = blobs[i].y;
            tracked[i].pressure = blobs[i].peak;
        }
        touch_submit(points,
                     tb710fu_tracker_update(&gTracker, tracked, blobs_count, points,
                                            (int)TB710FU_TRACK_SLOTS));

        frames++;
        if (gOptions.frame_limit && frames >= (unsigned)gOptions.frame_limit) {
            TB710FU_LOG("touch: frame limit %u reached, stopping the loop\n", frames);
            break;
        }
        if (gOptions.verbose) {
            struct tb710fu_nvt_peek peek;

            if (tb710fu_nvt_frame_parse(gBuf->frame, TB710FU_NVT_READ_BYTES, &peek) == 1)
                TB710FU_LOG("touch: frame %u counter=%u type=%u blobs=%d\n", frames,
                            peek.counter, peek.type, blobs_count);
        }
        IOSleep(8);
    }
    TB710FU_LOG("touch: loop ended\n");
    thread_terminate(current_thread());
}

void touch_start(void)
{
    struct tb710fu_nvt_bus bus;

    bus.xfer = nvt_bus_xfer;
    bus.delay_ms = nullptr;
    bus.ctx = nullptr;
    if (tb710fu_nvt_chip_init(&gChip, &bus, gBuf->scratch, sizeof(gBuf->scratch), gBuf->rx, sizeof(gBuf->rx)) !=
        TB710FU_NVT_OK) {
        TB710FU_LOG("touch: chip context rejected\n");
        return;
    }
    if (tb710fu_nvt_identify(&gChip, 0) != TB710FU_NVT_OK) {
        TB710FU_LOG("touch: NT36532 did not answer at 0x%06x (id %02x %02x %02x %02x %02x %02x)\n",
                    TB710FU_NVT_CHIP_VER_TRIM, gChip.id[0], gChip.id[1], gChip.id[2], gChip.id[3],
                    gChip.id[4], gChip.id[5]);
        return;
    }
    TB710FU_LOG("touch: NT36532 id %02x %02x %02x %02x %02x %02x buffer=0x%06x\n", gChip.id[0],
                gChip.id[1], gChip.id[2], gChip.id[3], gChip.id[4], gChip.id[5], gChip.event_buf);

    if (!ensure_hid_family()) return;
    gHidResource = wait_for_hid_resource(5000u);
    if (!gHidResource) {
        TB710FU_LOG("touch: IOHIDResource absent after 5 s\n");
        return;
    }
    gTouchDevice = create_hid_device("TB710FU Touchscreen", 0x0Du, 0x04u, 0x17EFu, 0x0001u,
                                     gBuf->touchVtable, true);
    if (!gTouchDevice) return;
    if (!start_hid_device(gTouchDevice, "TB710FU Touchscreen")) {
        gTouchDevice->release();
        gTouchDevice = nullptr;
        return;
    }
    gTouchReport = IOBufferMemoryDescriptor::withCapacity(TB710FU_HID_TOUCH_BYTES,
                                                        kIODirectionNone);
    if (!gTouchReport) {
        TB710FU_LOG("touch: report buffer allocation failed\n");
        return;
    }
    tb710fu_nvt_core_reset(&gBuf->core);
    tb710fu_tracker_init(&gTracker, (int32_t)TB710FU_HID_TOUCH_LOGICAL_X,
                         (int32_t)TB710FU_HID_TOUCH_LOGICAL_Y);
    memset(gLastTouch, 0, sizeof(gLastTouch));
    /* One empty report so the system knows the digitizer is live before the
     * first finger arrives. */
    touch_submit(nullptr, 0);

    if (kernel_thread_start(touch_thread_entry, nullptr, &gTouchThread) != KERN_SUCCESS) {
        TB710FU_LOG("touch: frame thread did not start\n");
        return;
    }
    /* Drop the creation reference; the running thread holds its own and ends
     * itself with thread_terminate(). */
    thread_deallocate(gTouchThread);
    gTouchThread = nullptr;
    TB710FU_LOG("touch: frame thread running\n");
}

/* ---- key path ---- */

void keys_loop(void);

void keys_thread_entry(void *, wait_result_t)
{
    keys_loop();
}

void keys_submit(unsigned keys)
{
    uint8_t report[TB710FU_HID_KEY_BYTES];

    if (!gKeysReport || !gKeysVtableReady || !gKeysDevice) return;
    if (tb710fu_hid_key_report(keys, report, sizeof(report)) == 0u) return;
    if (memcmp(report, gLastKeys, sizeof(gLastKeys)) == 0) return;
    memcpy(gLastKeys, report, sizeof(gLastKeys));
    if (submit_report(gKeysDevice, gBuf->keysVtable, gKeysReport, report, sizeof(report)) !=
        kIOReturnSuccess)
        TB710FU_LOG("keys: report rejected\n");
}

void keys_loop(void)
{
    while (!gStop) {
        uint8_t pressed = 0u;

        if (tb710fu_keys_sample(&gSpmi, &gKeys, &pressed) == 0) {
            /* Two identical 10 ms samples cover the 15 ms debounce the board
             * DTS declares. Repeat is left to macOS: the HID event driver
             * generates it while a Consumer usage stays set. */
            (void)tb710fu_key_filter_step(&gKeyFilter, pressed, 0u, 0u);
            keys_submit(tb710fu_keys_hid_bits(gKeyFilter.stable));
        }
        IOSleep(TB710FU_KEY_POLL_MS);
    }
    TB710FU_LOG("keys: loop ended\n");
    thread_terminate(current_thread());
}

void keys_start(void)
{
    if (!map_region(TB710FU_SPMI_CORE_BASE, TB710FU_SPMI_CORE_BYTES, gSpmiCore) ||
        !map_region(TB710FU_SPMI_OBS_BASE, TB710FU_SPMI_OBS_BYTES, gSpmiObs) ||
        !map_region(TB710FU_SPMI_CNFG_BASE, TB710FU_SPMI_CNFG_BYTES, gSpmiCnfg)) {
        TB710FU_LOG("keys: SPMI windows did not map\n");
        return;
    }
    gSpmi.core = &gSpmiCore;
    gSpmi.obs = &gSpmiObs;
    gSpmi.cnfg = &gSpmiCnfg;
    gSpmi.ee = TB710FU_SPMI_EE;
    gSpmi.read32 = spmi_read;
    gSpmi.write32 = spmi_write;
    gSpmi.barrier = spmi_barrier;
    gSpmi.ctx = nullptr;

    {
        int result = tb710fu_keys_open(&gSpmi, &gKeys);

        if (result != 0) {
            TB710FU_LOG("keys: arbiter unusable (%d)\n", result);
            return;
        }
    }
    TB710FU_LOG("keys: PON apid=%d PM8550-GPIO6 apid=%d\n", gKeys.pon_apid, gKeys.gpio_apid);

    if (!ensure_hid_family()) return;
    if (!gHidResource) gHidResource = wait_for_hid_resource(5000u);
    if (!gHidResource) {
        TB710FU_LOG("keys: IOHIDResource absent\n");
        return;
    }
    gKeysDevice = create_hid_device("TB710FU Keys", 0x0Cu, 0x01u, 0x17EFu, 0x0002u, gBuf->keysVtable,
                                    false);
    if (!gKeysDevice) return;
    if (!start_hid_device(gKeysDevice, "TB710FU Keys")) {
        gKeysDevice->release();
        gKeysDevice = nullptr;
        return;
    }
    gKeysReport = IOBufferMemoryDescriptor::withCapacity(TB710FU_HID_KEY_BYTES, kIODirectionNone);
    if (!gKeysReport) {
        TB710FU_LOG("keys: report buffer allocation failed\n");
        return;
    }
    tb710fu_key_filter_reset(&gKeyFilter);
    memset(gLastKeys, 0, sizeof(gLastKeys));

    if (kernel_thread_start(keys_thread_entry, nullptr, &gKeysThread) != KERN_SUCCESS) {
        TB710FU_LOG("keys: poll thread did not start\n");
        return;
    }
    thread_deallocate(gKeysThread);
    gKeysThread = nullptr;
    TB710FU_LOG("keys: poll thread running\n");
}

} /* namespace */

/* ---- entry point used by the boot-time service list ---- */

extern "C" int tb710fu_input_start(void)
{
    const char *boot;
    const char *error = nullptr;

    tb710fu_input_options_default(&gOptions);
    gStop = 0;
    boot = PE_boot_args();
    if (boot && boot[0]) {
        tb710fu_input_bootargs(boot, &gOptions, &error);
        if (error) TB710FU_LOG("boot args: %s\n", error);
    } else {
        /* No textual boot args exposed: read the two levels directly so the
         * subsystem still needs an explicit switch. */
        int touch = 0;
        int keys = 0;

        if (PE_parse_boot_argn("tb710fu-touch", &touch, sizeof(touch)) && touch >= 0 &&
            touch <= (int)TB710FU_TOUCH_FULL)
            gOptions.touch_stage = touch;
        if (PE_parse_boot_argn("tb710fu-keys", &keys, sizeof(keys)) && keys >= 0 &&
            keys <= (int)TB710FU_KEYS_ON)
            gOptions.keys_stage = keys;
    }

    TB710FU_LOG("options touch=%d keys=%d fuse=%d irq=%d verbose=%d frames=%d\n",
                gOptions.touch_stage, gOptions.keys_stage, gOptions.fuse_probe, gOptions.use_irq,
                gOptions.verbose, gOptions.frame_limit);

    if (gOptions.touch_stage == (int)TB710FU_TOUCH_OFF &&
        gOptions.keys_stage == (int)TB710FU_KEYS_OFF) {
        TB710FU_LOG("disabled (boot with tb710fu-touch=1..3 / tb710fu-keys=1)\n");
        return 0;
    }

    if (!gBuf) {
        gBuf = (InputBuffers *)IOMallocData(sizeof(InputBuffers));
        if (!gBuf) {
            TB710FU_LOG("input: failed to allocate InputBuffers (%zu bytes)\n", sizeof(InputBuffers));
            return -1;
        }
        memset(gBuf, 0, sizeof(InputBuffers));
    }

    if (gOptions.touch_stage != (int)TB710FU_TOUCH_OFF) {
        if (!touch_prepare()) {
            TB710FU_LOG("touch: stopped at stage %d (snapshot)\n", gOptions.touch_stage);
        } else if (gOptions.touch_stage == (int)TB710FU_TOUCH_SNAPSHOT) {
            TB710FU_LOG("touch: snapshot stage complete (no writes performed)\n");
        } else if (!touch_engine()) {
            TB710FU_LOG("touch: stopped at stage %d (engine)\n", gOptions.touch_stage);
        } else if (gOptions.touch_stage == (int)TB710FU_TOUCH_ENGINE) {
            TB710FU_LOG("touch: engine stage complete (no controller traffic)\n");
        } else {
            touch_start();
        }
    }

    if (gOptions.keys_stage == (int)TB710FU_KEYS_ON) keys_start();
    return 0;
}

/* Quiesce both workers. Terminal: the loops call thread_terminate() on exit,
 * so a stopped subsystem cannot be restarted in the same boot. */
extern "C" void tb710fu_input_stop(void)
{
    gStop = 1;
    TB710FU_LOG("stop requested\n");
}

/* Firmware image check. The blob is optional and never uploaded: when the
 * merge packs novatek/novatek_nt36532_fw.bin into an addon entry this
 * validates and fingerprints it. Without it the driver runs on the firmware
 * the controller already has, which is the safe default. */
extern "C" int tb710fu_input_firmware_check(const void *image, unsigned bytes)
{
    struct tb710fu_nvt_firmware firmware;
    int result = tb710fu_nvt_firmware_parse((const uint8_t *)image, bytes, &firmware);

    if (result != TB710FU_NVT_OK) {
        TB710FU_LOG("firmware: image rejected (%u bytes)\n", bytes);
        return -1;
    }
    TB710FU_LOG("firmware: ilm=0x%06x+%u sram=0x%06x dlm=0x%06x+%u sram=0x%06x crc=%s bytes=%u\n",
                firmware.ilm_address, firmware.ilm_bytes, firmware.ilm_sram,
                firmware.dlm_address, firmware.dlm_bytes, firmware.dlm_sram,
                firmware.dlm_crc_valid ? "ok" : "MISMATCH", firmware.image_bytes);
    return 0;
}
