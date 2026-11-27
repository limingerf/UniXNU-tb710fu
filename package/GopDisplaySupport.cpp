// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB710FU macOS 27 GopFramebuffer observation and surface hooks (pl21).
//
// NativeLegacyDisplay.cpp calls these as plain C hooks. On iPadOS they came from
// the reference device's probe unit and from the family's own helpers; the macOS 27
// family helpers are prelinked kext functions this board cannot bind by address, so
// the IOSurface accessors are decoded from the reviewed 26C5035r stubs (six leaf
// loads: descriptor 0x30, width 0x58, height 0x60, stride 0x68, format 0x80,
// planes 0x98).
//
// pl47 turns the IOAV, vsync, refresh and swap-info hooks from no-ops into calls
// into the *live* IOAVFamily / IOMobileFramebufferLegacy code. Every target is
// derived from the runtime image itself (live metaclass minus the fixture VA) and
// checked before use, so a fileset that moves the code degrades to the pl46
// down-grade instead of trapping.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSet.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <ptrauth.h>
#include "GopDisplayTraits.h"

namespace {
/* IOMobileFramebufferLegacy instance +0x2380 (26C5035r): the word
 * set_hotPlug_state_gated() stores and get_digital_out_state_gated() reads. */
constexpr unsigned kLegacyHotPlugState = 0x2380;

volatile uint64_t gFrames;
volatile uint64_t gTransfers;
volatile uint64_t gRetirements;
volatile uint64_t gVsyncSends;
volatile uint64_t gVsyncDelivered;
volatile uint64_t gHotplugSends;
volatile uint64_t gSwapInfoSends;
volatile uint64_t gSwapInfoDelivered;
volatile uint64_t gIoavRequests;
volatile uint64_t gIoavTimings;
volatile uint64_t gIoavColors;
volatile uint64_t gRejectedSurfaces;
volatile uint64_t gClientEntries;
volatile uint64_t gGuardHits;

/* pl52: one-shot latches. Every hook below fires hundreds of times per second
 * once the compositor is live, so each of the first occurrences prints once and
 * the totals are read back through the CDC "disp" verb instead. */
bool gClientLogged;
bool gClientObservedLogged;
bool gModeLogged[7];
bool gAcceptLogged;
bool gRejectLogged;
bool gSurfaceLogged;
bool gTransferOkLogged;
bool gTransferFailLogged;
bool gFrameLogged;
bool gRetireLogged;
uint32_t gSwapStagesSeen;

/* The swap lifecycle counters live in the generated NativeLegacyDisplay
 * translation unit (extern "C" globals, so the same link unit resolves them):
 *   native_legacy_swapstat[0..15]  validated, kicks, in-flight, not-ready,
 *                                  no-head, latch-busy, fence waits, copies,
 *                                  blank frames, flips, latches, retires,
 *                                  slow (us), longest (us), total (us), depth
 *   native_legacy_formats[0..9]    'BGRA', '&BGA', 'RGhA', refused, last refused
 *                                  format, direct flips, GPU 8-bit, GPU half,
 *                                  CPU 8-bit, CPU half
 *   native_legacy_direct[0..13]    flips, mapped, map failures, slow latches,
 *                                  longest (us), total (us), released, copies,
 *                                  boot flips, GPU waits, wait total (us),
 *                                  longest wait (us), waits >= 250 ms, >= 8 ms
 *   native_legacy_surfaces[0..13]  distinct surfaces, swaps checked, then
 *                                  allocation/segments/largest for the first four
 * The definitions live in the generated NativeLegacyDisplay translation unit and
 * are declared at file scope below, outside this unnamed namespace: an extern
 * declaration inside an unnamed namespace would acquire internal linkage. */

uintptr_t surfaceField(OSObject *surface, unsigned offset, unsigned width)
{
    uintptr_t value = 0;
    if (!surface) {
        return 0;
    }
    memcpy(&value, (const unsigned char *)surface + offset, width);
    return value;
}

/* ---- pl47: live IOAVFamily element construction ----------------------------
 *
 * NativeLegacyDisplay::publishDisplayAttributes() builds the timing and colour
 * elements macOS reads the display mode from, and publishes "DisplayAttributes"
 * only when both exist. pl46 left both hooks returning nullptr, which is exactly
 * the "display attributes not published" line on COM5.
 *
 * 26C5035r evidence (prelinked IOAVFamily, disassembled from the pl45b image;
 * the fixture addresses below are that image minus its 0xf00000000 image-base
 * difference from the reviewed ABI fixture):
 *
 *   IOAVVideoTimingElement::withData (0xfffffe000a7e8d68) allocates through
 *   gMetaClass.alloc and then calls initWithData through the *live* vtable:
 *     ldr x16,[x0]            ; object vptr, pacda/0xcda1
 *     add x16,x16,#0x170      ; address-point offset
 *     ldr x8,[x16]
 *     blraa x8,x16(0xf722)
 *   i.e. initWithData is virtual at address-point offset 0x170 with slot
 *   diversity 0xf722, so the live table of the allocated object is authoritative.
 *   setColorElements is virtual at address-point offset 0x1a0 with diversity
 *   0x828c (it retains the array into +0x70 and prunes the element IDs).
 *
 *   IOAVVideoColorElement::withCapabilities (0xfffffe000a7e80c0) is NOT virtual:
 *   the 61-word colour vtable (__ZTV21IOAVVideoColorElement .. +0x1e8) contains
 *   neither it nor its initWithData, and the function itself builds an
 *   IOAVVideoColorData on the stack and calls withData/initWithData directly.
 *   It is therefore called at its live text address, after checking the arm64e
 *   prologue, and the returned object's vptr is checked against the live colour
 *   table before it is handed to the display.
 *
 * The slide comes from the two IOAV metaclasses (their fixture addresses are in
 * __DATA, which XnuPrelink does not move): live - fixture must agree for both
 * classes and be page aligned. Nothing here is called unless that holds. */
constexpr uintptr_t kIoavTimingMetaClassFixture = 0xfffffe000cc1c3d8ull;
constexpr uintptr_t kIoavColorMetaClassFixture = 0xfffffe000cc1c3b0ull;
constexpr uintptr_t kIoavTimingVtableFixture = 0xfffffe00084fd1c0ull;
constexpr uintptr_t kIoavColorVtableFixture = 0xfffffe00084fcec0ull;
constexpr uintptr_t kIoavColorFactoryFixture = 0xfffffe000a7e80c0ull;
constexpr uintptr_t kIoavTimingInitWithDataSlot = 0x170;
constexpr uint16_t kIoavTimingInitWithDataDiversity = 0xf722;
constexpr uintptr_t kIoavTimingSetColorElementsSlot = 0x1a0;
constexpr uint16_t kIoavTimingSetColorElementsDiversity = 0x828c;
constexpr uint16_t kMetaClassBaseDiversity = 0xcda1; /* "_ZTV15OSMetaClassBase" */

/* IOMobileFramebufferLegacy 26C5035r senders, same fixture base:
 *   swap_notify_dispatch_gated(self, timestamp, continuous host time, interval)
 *     sends the six-word display-refresh message to the clients queued at +0x2078;
 *     without it the render server never sees a vblank.
 *   setDisplayRefreshProperties(self) publishes IOMFBDisplayRefresh; without it
 *     WindowServer falls back to its own 1/60 s heartbeat.
 *   swap_info_notify_dispatch(self, SwapInfoBlob *) forwards swap statistics. */
constexpr uintptr_t kLegacySwapNotifyDispatchFixture = 0xfffffe000ac2e678ull;
constexpr uintptr_t kLegacyRefreshPropertiesFixture = 0xfffffe000ac329d0ull;
constexpr uintptr_t kLegacySwapInfoDispatchFixture = 0xfffffe000ac2e844ull;
constexpr unsigned kLegacyNotifyObjectOffset = 0x2108;

bool gIoavProbed;
bool gIoavReady;
bool gIoavSlideValid;
uintptr_t gIoavSlide;
bool gVsyncLogged;
bool gRefreshLogged;
bool gSwapInfoLogged;

const OSMetaClass *ioavMetaClass(const char *className)
{
    const OSSymbol *symbol = OSSymbol::withCString(className);
    if (!symbol) {
        return nullptr;
    }
    const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(symbol);
    symbol->release();
    return meta;
}

/* A PAC-stripped or plain live address of this image: canonical kernel VA. */
bool ioavCanonical(uintptr_t address)
{
    return (address >> 48) == 0xffffull && (address & 1u) == 0;
}

/* Every target below starts with `bti c` (0xd503245f) in 26C5035r. Requiring it
 * proves the computed address is a function entry and not a data word. */
bool ioavTextLooksLive(uintptr_t address)
{
    if (!ioavCanonical(address) || (address & 3u) != 0) {
        return false;
    }
    uint32_t word = 0;
    memcpy(&word, (const void *)address, sizeof(word));
    return word == 0xd503245fu;
}

bool ioavPrepare(void)
{
    if (gIoavProbed) {
        return gIoavReady;
    }
    gIoavProbed = true;
    OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOAVFamily");
    const OSMetaClass *timing = ioavMetaClass("IOAVVideoTimingElement");
    const OSMetaClass *color = ioavMetaClass("IOAVVideoColorElement");
    printf("TB710FU GopDisplay: IOAV load=%x meta timing=%p color=%p\n",
           loaded, (void *)timing, (void *)color);
    if (!timing || !color) {
        return false;
    }
    uintptr_t timingAddress = (uintptr_t)timing;
    uintptr_t colorAddress = (uintptr_t)color;
    if (!ioavCanonical(timingAddress) || !ioavCanonical(colorAddress)) {
        printf("TB710FU GopDisplay: IOAV metaclass outside the kernel image, attributes stay unpublished\n");
        return false;
    }
    uintptr_t timingSlide = timingAddress - kIoavTimingMetaClassFixture;
    uintptr_t colorSlide = colorAddress - kIoavColorMetaClassFixture;
    if (timingSlide != colorSlide || (timingSlide & 0x3fffull) != 0 ||
        timingSlide > 0x100000000000ull) {
        printf("TB710FU GopDisplay: IOAV slide mismatch %llx/%llx, attributes stay unpublished\n",
               (unsigned long long)timingSlide, (unsigned long long)colorSlide);
        return false;
    }
    gIoavSlide = timingSlide;
    gIoavSlideValid = true;
    gIoavReady = true;
    printf("TB710FU GopDisplay: IOAV slide=%llx\n", (unsigned long long)gIoavSlide);
    return true;
}

/* Authenticate the object's vptr exactly as the family does: ASDA key, address
 * discrimination on `this`, root discriminator 0xcda1. The strip first rejects a
 * word that is not a kernel pointer without authenticating it. */
uintptr_t ioavObjectVtable(OSObject *object)
{
    uintptr_t raw = 0;
    memcpy(&raw, (const void *)object, sizeof(raw));
    uintptr_t stripped = (uintptr_t)ptrauth_strip((void *)raw, ptrauth_key_cxx_vtable_pointer);
    if (!ioavCanonical(stripped) || (stripped & 7u) != 0) {
        return 0;
    }
    uintptr_t vtable = (uintptr_t)ptrauth_auth_data((void *)raw, ptrauth_key_cxx_vtable_pointer,
                                                    ptrauth_blend_discriminator(object, kMetaClassBaseDiversity));
    /* AUTDA of a word signed with another modifier returns a corrupted pointer
     * rather than trapping; the authenticated value must equal the stripped one. */
    if (vtable != stripped) {
        return 0;
    }
    return vtable;
}

/* One live virtual entry: signed with the slot's own address + diversity, then
 * re-signed for this call (the pattern proven in pl45b). */
template <typename Fn>
Fn ioavEntry(uintptr_t vtable, uintptr_t offset, uint16_t diversity)
{
    const volatile uint64_t *slot = (const volatile uint64_t *)(vtable + offset);
    uintptr_t signedEntry = (uintptr_t)slot[0];
    uintptr_t target = (uintptr_t)ptrauth_strip((void *)signedEntry, ptrauth_key_function_pointer);
    if (!ioavCanonical(target) || (target & 3u) != 0) {
        return nullptr;
    }
    void *entry = ptrauth_auth_and_resign((void *)signedEntry, ptrauth_key_function_pointer,
                                          ptrauth_blend_discriminator((void *)(uintptr_t)slot, diversity),
                                          ptrauth_key_function_pointer, 0);
    if ((uintptr_t)ptrauth_strip(entry, ptrauth_key_function_pointer) != target) {
        return nullptr;
    }
    return (Fn)(uintptr_t)entry;
}

template <typename Fn>
Fn legacyFamilyFunction(uintptr_t fixtureAddress)
{
    if (!ioavPrepare() || !gIoavSlideValid) {
        return nullptr;
    }
    uintptr_t address = fixtureAddress + gIoavSlide;
    if (!ioavTextLooksLive(address)) {
        return nullptr;
    }
    return reinterpret_cast<Fn>(
        ptrauth_sign_unauthenticated(reinterpret_cast<void *>(address),
                                     ptrauth_key_function_pointer, 0));
}
}  // namespace

/* Swap lifecycle counters owned by the generated NativeLegacyDisplay object.
 * extern "C" at file scope, because the definitions are extern "C" globals in
 * that translation unit. */
extern "C" {
extern volatile uint32_t native_legacy_swapstat[16];
extern volatile uint32_t native_legacy_formats[10];
extern volatile uint32_t native_legacy_direct[14];
extern volatile uint32_t native_legacy_surfaces[14];
}

extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *object, void *returnAddress)
{
    gGuardHits = gGuardHits + 1;
    panic("TB710FU GopDisplay: unowned IOMobileFramebufferLegacy slot %u entered (object %p, caller %p); "
          "the live vtable was not adopted", slot, object, returnAddress);
}

extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *surface)
{
    return (IOMemoryDescriptor *)surfaceField(surface, TB710FU_GOP_SURFACE_DESCRIPTOR,
                                              TB710FU_GOP_SURFACE_DESCRIPTOR_WIDTH);
}

extern "C" uint64_t native_legacy_surface_width(OSObject *surface)
{
    return surfaceField(surface, TB710FU_GOP_SURFACE_WIDTH, TB710FU_GOP_SURFACE_WIDTH_WIDTH);
}

extern "C" uint64_t native_legacy_surface_height(OSObject *surface)
{
    return surfaceField(surface, TB710FU_GOP_SURFACE_HEIGHT, TB710FU_GOP_SURFACE_HEIGHT_WIDTH);
}

extern "C" uint64_t native_legacy_surface_stride(OSObject *surface)
{
    uint64_t stride = surfaceField(surface, TB710FU_GOP_SURFACE_STRIDE, TB710FU_GOP_SURFACE_STRIDE_WIDTH);
    uint32_t planeCount = (uint32_t)surfaceField(surface, TB710FU_GOP_SURFACE_PLANES, TB710FU_GOP_SURFACE_PLANES_WIDTH);
    if (surface && planeCount >= 1) {
        /* 26C5035r IOSurface plane table at +0x180 (count at +0x188), 56-byte
         * entries. The entry layout was resolved symbol-for-symbol from the
         * pl92 prelinked kernel's IOSurface accessors (each loads one entry
         * word and falls back to the legacy surface field for plane 0):
         *   +0x00 getPlaneWidth        +0x08 getPlaneHeight
         *   +0x10 getPlaneBase         <- kIOSurfacePlaneBase, the baseOffset
         *   +0x18 getPlaneOffset       <- kIOSurfacePlaneOffset (secondary)
         *   +0x20 getPlaneBytesPerRow  <- the real row stride (12800)
         *   +0x28 getPlaneSize         <- kIOSurfacePlaneSize, planeAllocSize
         *   +0x30 getPlaneBytesPerElement (u16)
         * pl93-5g-srgb logged plane0_offset=26279936 because +0x28 was read as
         * the offset; that word is getPlaneSize (the allocation size), NOT the
         * baseOffset. The baseOffset is +0x10 (getPlaneBase). +0x68 on the
         * surface itself holds the pixel width (3200), so the plane table's
         * +0x20 (12800) is the stride scanout must use. */
        uintptr_t planes = surfaceField(surface, 0x180u, 8u);
        uint32_t allocCount = (uint32_t)surfaceField(surface, 0x188u, 4u);
        if (planes && (planes >> 48) == 0xffffull && allocCount >= 1) {
            const unsigned char *plane0 = (const unsigned char *)planes;
            uint64_t planeStride = 0;   /* +0x20 getPlaneBytesPerRow */
            uint64_t planeBase = 0;     /* +0x10 getPlaneBase == baseOffset */
            memcpy(&planeStride, plane0 + 0x20u, sizeof(planeStride));
            memcpy(&planeBase, plane0 + 0x10u, sizeof(planeBase));
            static bool logged = false;
            if (!logged) {
                logged = true;
                /* One-shot dump of every 64-bit word of plane[0] so the exact
                 * baseOffset is confirmed on hardware, not inferred. */
                uint64_t w[7] = {};
                for (unsigned i = 0; i < 7; ++i) {
                    memcpy(&w[i], plane0 + i * 8u, sizeof(w[i]));
                }
                printf("TB710FU GopDisplay: plane0 words +00=%llu +08=%llu +10=%llu "
                       "+18=%llu +20=%llu +28=%llu +30=%llu\n",
                       (unsigned long long)w[0], (unsigned long long)w[1],
                       (unsigned long long)w[2], (unsigned long long)w[3],
                       (unsigned long long)w[4], (unsigned long long)w[5],
                       (unsigned long long)w[6]);
                printf("TB710FU GopDisplay: planar stride raw=%llu plane0_stride=%llu "
                       "plane0_baseOffset=%llu plane0_size=%llu\n",
                       (unsigned long long)stride, (unsigned long long)planeStride,
                       (unsigned long long)planeBase, (unsigned long long)w[5]);
            }
            if (planeStride > stride) {
                stride = planeStride;
            }
        }
    }
    return stride;
}

extern "C" uint64_t native_legacy_surface_allocation(OSObject *surface)
{
    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    return memory ? (uint64_t)memory->getLength() : 0;
}

extern "C" uint32_t native_legacy_surface_format(OSObject *surface)
{
    return (uint32_t)surfaceField(surface, TB710FU_GOP_SURFACE_FORMAT, TB710FU_GOP_SURFACE_FORMAT_WIDTH);
}

extern "C" uint32_t native_legacy_surface_planes(OSObject *surface)
{
    return (uint32_t)surfaceField(surface, TB710FU_GOP_SURFACE_PLANES, TB710FU_GOP_SURFACE_PLANES_WIDTH);
}

extern "C" bool native_legacy_gop_mapping_observed(IOService *display, IOMemoryMap *mapping,
                                                   uint32_t width, uint32_t height)
{
    if (!display || !mapping || !width || !height) {
        return false;
    }
    return mapping->getVirtualAddress() != 0 && mapping->getLength() != 0;
}

extern "C" void native_legacy_registration_observed(IOService *display)
{
    if (display) {
        printf("TB710FU GopDisplay: registered %s\n", display->getName());
    }
}

extern "C" void native_legacy_swap_stage(unsigned stage)
{
    /* pl52: the display raises stage 1 on the flip and further stages on later
     * paths; recording which ones were reached separates "the swap never got
     * here" from "the swap got here and the scanout did not follow". */
    if (stage < 32 && !(gSwapStagesSeen & (1u << stage))) {
        gSwapStagesSeen |= 1u << stage;
        printf("TB710FU GopDisplay: swap stage %u reached\n", stage);
    }
}

extern "C" void native_legacy_client_enter(task_t task, uint32_t type)
{
    gClientEntries = gClientEntries + 1;
    if (!gClientLogged) {
        gClientLogged = true;
        printf("TB710FU GopDisplay: first user client entry type=%u task=%p\n",
               (unsigned)type, (void *)task);
    }
}

extern "C" void native_legacy_client_observed(IOReturn status, IOUserClient *client)
{
    if (!gClientObservedLogged) {
        gClientObservedLogged = true;
        printf("TB710FU GopDisplay: first user client status=%x client=%p entries=%llu\n",
               (unsigned)status, (void *)client, (unsigned long long)gClientEntries);
    }
}

extern "C" void native_legacy_mode_observed(unsigned which, uint32_t first, uint32_t second,
                                            IOReturn status)
{
    /* The six macOS IOMFB display-client entry points, in the order
     * setDigitalOutMode, setDisplayDevice, setGammaTable, setColorRemapMode,
     * getColorRemapMode, swapSetColorMatrix. A first call that answers
     * kIOReturnUnsupported here is the reason WindowServer never presents. */
    if (which < 7 && !gModeLogged[which]) {
        gModeLogged[which] = true;
        printf("TB710FU GopDisplay: first mode call %u args=%08x/%08x status=%x\n",
               which, (unsigned)first, (unsigned)second, (unsigned)status);
    }
}

extern "C" void native_legacy_pm_observed(unsigned event, IOReturn status, unsigned long ordinal)
{
    (void)event;
    (void)status;
    (void)ordinal;
}

extern "C" void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx,
                                             uint64_t miny, uint64_t maxx, uint64_t maxy)
{
    gFrames = gFrames + 1;
    if (!gFrameLogged) {
        gFrameLogged = true;
        printf("TB710FU GopDisplay: first frame kind=%u count=%llu rect=%llu,%llu-%llu,%llu\n",
               (unsigned)kind, (unsigned long long)count, (unsigned long long)minx,
               (unsigned long long)miny, (unsigned long long)maxx, (unsigned long long)maxy);
    }
}

extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors, const uint64_t *counts, uint64_t other)
{
    (void)colors;
    (void)counts;
    (void)other;
}

extern "C" void native_legacy_swap_validation_observed(bool accepted, unsigned reason,
                                                       const void *request, const void *surface)
{
    /* Reasons: 1 gate off or no request, 2 no single surface, 3 layers/rects/
     * options/dimensions refused, 4 surface format/geometry/backing refused,
     * 5 accepted blank swap, 0 accepted. */
    if (accepted) {
        if (!gAcceptLogged) {
            gAcceptLogged = true;
            OSObject *object = (OSObject *)const_cast<void *>(surface);
            printf("TB710FU GopDisplay: first swap accepted reason=%u request=%p surface=%p "
                   "%llux%llu fmt=%08x planes=%u stride=%llu alloc=%llu\n",
                   reason, request, surface,
                   (unsigned long long)native_legacy_surface_width(object),
                   (unsigned long long)native_legacy_surface_height(object),
                   (unsigned)native_legacy_surface_format(object),
                   (unsigned)native_legacy_surface_planes(object),
                   (unsigned long long)native_legacy_surface_stride(object),
                   (unsigned long long)native_legacy_surface_allocation(object));
        }
    } else if (!gRejectLogged) {
        gRejectLogged = true;
        printf("TB710FU GopDisplay: first swap rejected reason=%u request=%p surface=%p\n",
               reason, request, surface);
    }
}

extern "C" void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format,
                                               uint32_t planes, uint64_t stride, uint64_t allocation,
                                               uint64_t length)
{
    gRejectedSurfaces = gRejectedSurfaces + 1;
    if (!gSurfaceLogged) {
        gSurfaceLogged = true;
        printf("TB710FU GopDisplay: first surface rejected %llux%llu fmt=%08x planes=%u "
               "stride=%llu alloc=%llu length=%llu\n",
               (unsigned long long)width, (unsigned long long)height, (unsigned)format,
               (unsigned)planes, (unsigned long long)stride,
               (unsigned long long)allocation, (unsigned long long)length);
    }
}

extern "C" bool native_legacy_prepare_scanout(void)
{
    // The console was handed over when the display started; nothing to reclaim here.
    return true;
}

extern "C" void native_legacy_transfer_observed(IOReturn status, const void *request, uint64_t bytes)
{
    if (status == kIOReturnSuccess) {
        gTransfers = gTransfers + 1;
        if (!gTransferOkLogged) {
            gTransferOkLogged = true;
            printf("TB710FU GopDisplay: first transfer ok bytes=%llu request=%p transfers=%llu\n",
                   (unsigned long long)bytes, request, (unsigned long long)gTransfers);
        }
    } else if (!gTransferFailLogged) {
        gTransferFailLogged = true;
        printf("TB710FU GopDisplay: first transfer failed status=%x request=%p\n",
               (unsigned)status, request);
    }
}

extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes)
{
    (void)ticks;
    (void)bytes;
}

extern "C" void native_legacy_retirement_observed(IOService *display)
{
    gRetirements = gRetirements + 1;
    if (!gRetireLogged) {
        gRetireLogged = true;
        printf("TB710FU GopDisplay: first retirement display=%p retires=%llu\n",
               (void *)display, (unsigned long long)gRetirements);
    }
}

extern "C" void tb710fu_gop_display_report(void)
{
    /* pl52: the CDC "disp" verb. Everything the pl51 log could not show: how many
     * clients arrived, how many frames reached the scanout, and - the decisive
     * one - where each submitted swap stopped inside the lifecycle below. */
    printf("TB710FU GopDisplay: disp clients=%llu frames=%llu transfers=%llu retire=%llu "
           "vsync_send=%llu vsync_deliv=%llu rejected=%llu hotplug=%llu swapinfo=%llu/%llu "
           "guards=%llu\n",
           (unsigned long long)gClientEntries, (unsigned long long)gFrames,
           (unsigned long long)gTransfers, (unsigned long long)gRetirements,
           (unsigned long long)gVsyncSends, (unsigned long long)gVsyncDelivered,
           (unsigned long long)gRejectedSurfaces, (unsigned long long)gHotplugSends,
           (unsigned long long)gSwapInfoSends, (unsigned long long)gSwapInfoDelivered,
           (unsigned long long)gGuardHits);
    printf("TB710FU GopDisplay: swap validated=%u kicks=%u inflight=%u notready=%u nohead=%u "
           "latchbusy=%u fence=%u copy=%u blank=%u flips=%u latches=%u retires=%u slow=%u "
           "maxus=%u totalus=%u depth=%u\n",
           native_legacy_swapstat[0], native_legacy_swapstat[1], native_legacy_swapstat[2],
           native_legacy_swapstat[3], native_legacy_swapstat[4], native_legacy_swapstat[5],
           native_legacy_swapstat[6], native_legacy_swapstat[7], native_legacy_swapstat[8],
           native_legacy_swapstat[9], native_legacy_swapstat[10], native_legacy_swapstat[11],
           native_legacy_swapstat[12], native_legacy_swapstat[13], native_legacy_swapstat[14],
           native_legacy_swapstat[15]);
    printf("TB710FU GopDisplay: formats bgra=%u abgr=%u half=%u refused=%u lastfmt=%08x "
           "direct=%u gpu8=%u gpu16=%u cpu8=%u cpu16=%u\n",
           native_legacy_formats[0], native_legacy_formats[1], native_legacy_formats[2],
           native_legacy_formats[3], native_legacy_formats[4], native_legacy_formats[5],
           native_legacy_formats[6], native_legacy_formats[7], native_legacy_formats[8],
           native_legacy_formats[9]);
    printf("TB710FU GopDisplay: scanout flips=%u mapped=%u mapfail=%u released=%u copies=%u "
           "bootflips=%u gpuwait=%u waitus=%u maxwait=%u long250=%u miss8=%u "
           "surfaces=%u checked=%u\n",
           native_legacy_direct[0], native_legacy_direct[1], native_legacy_direct[2],
           native_legacy_direct[6], native_legacy_direct[7], native_legacy_direct[8],
           native_legacy_direct[9], native_legacy_direct[5], native_legacy_direct[11],
           native_legacy_direct[12], native_legacy_direct[13],
           native_legacy_surfaces[0], native_legacy_surfaces[1]);
}

extern "C" bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride,
                                        uint32_t width, uint32_t height, uint32_t format)
{
    // The Adreno presenter (NativeGOPPresenter) is not part of this archive: the
    // backend keeps the CPU copy path and never claims a GPU fence.
    (void)display;
    (void)memory;
    (void)stride;
    (void)width;
    (void)height;
    (void)format;
    return false;
}

extern "C" void native_legacy_gpu_fallback_observed(int status)
{
    (void)status;
}

extern "C" void native_legacy_vsync_notify(void *display, uint64_t timestamp, uint64_t continuousTime,
                                           uint64_t interval)
{
    /* The family's own display-refresh sender. The render server paces its
     * display timer by these messages; pl46's no-op left it without a vblank. */
    gVsyncSends = gVsyncSends + 1;
    typedef void (*Fn)(void *, uint64_t, uint64_t, uint64_t);
    Fn send = legacyFamilyFunction<Fn>(kLegacySwapNotifyDispatchFixture);
    if (!display || !send) {
        if (!gVsyncLogged) {
            gVsyncLogged = true;
            printf("TB710FU GopDisplay: vsync sender unavailable (display=%p slide=%llx), clients get no vblank\n",
                   (void *)display, (unsigned long long)gIoavSlide);
        }
        return;
    }
    if (!gVsyncLogged) {
        gVsyncLogged = true;
        printf("TB710FU GopDisplay: vsync sender live (fixture %llx + slide %llx)\n",
               (unsigned long long)kLegacySwapNotifyDispatchFixture, (unsigned long long)gIoavSlide);
    }
    send(display, timestamp, continuousTime, interval);
    gVsyncDelivered = gVsyncDelivered + 1;
}

extern "C" bool native_legacy_set_refresh_properties(void *display)
{
    /* Publishes IOMFBDisplayRefresh through slot-301 refresh words. Nothing in
     * the family calls it; without it the render server keeps its own 1/60 s
     * heartbeat, so this is pacing fidelity rather than a hard blocker. */
    typedef bool (*Fn)(void *);
    Fn publish = legacyFamilyFunction<Fn>(kLegacyRefreshPropertiesFixture);
    if (!display || !publish) {
        if (!gRefreshLogged) {
            gRefreshLogged = true;
            printf("TB710FU GopDisplay: refresh publisher unavailable (display=%p), render server keeps 1/60 s\n",
                   (void *)display);
        }
        return false;
    }
    bool ok = publish(display);
    if (!gRefreshLogged) {
        gRefreshLogged = true;
        printf("TB710FU GopDisplay: IOMFBDisplayRefresh published=%d\n", ok ? 1 : 0);
    }
    return ok;
}

extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz)
{
    (void)event;
    (void)hz;
}

extern "C" void native_legacy_swap_info_dispatch(void *display, void *blob)
{
    /* The family's swap-statistics sender. It dereferences the notification
     * object at instance +0x2108 without a null check, so that pointer is tested
     * here first; a display that never started leaves it null and is skipped. */
    gSwapInfoSends = gSwapInfoSends + 1;
    typedef void (*Fn)(void *, void *);
    Fn dispatch = legacyFamilyFunction<Fn>(kLegacySwapInfoDispatchFixture);
    if (!display || !blob || !dispatch) {
        if (!gSwapInfoLogged) {
            gSwapInfoLogged = true;
            printf("TB710FU GopDisplay: swap-info sender unavailable (display=%p blob=%p)\n",
                   (void *)display, blob);
        }
        return;
    }
    uintptr_t notification = 0;
    memcpy(&notification, (const unsigned char *)display + kLegacyNotifyObjectOffset, sizeof(notification));
    if (!notification) {
        if (!gSwapInfoLogged) {
            gSwapInfoLogged = true;
            printf("TB710FU GopDisplay: swap-info sender has no notification object at +%x, skipped\n",
                   kLegacyNotifyObjectOffset);
        }
        return;
    }
    if (!gSwapInfoLogged) {
        gSwapInfoLogged = true;
        printf("TB710FU GopDisplay: swap-info sender live (fixture %llx + slide %llx)\n",
               (unsigned long long)kLegacySwapInfoDispatchFixture, (unsigned long long)gIoavSlide);
    }
    dispatch(display, blob);
    gSwapInfoDelivered = gSwapInfoDelivered + 1;
}

extern "C" void native_legacy_hotplug_notify(void *display, uint64_t state)
{
    /* The family's hotPlug_notify is non-virtual and has no adopted vtable slot;
     * this backend publishes the state directly instead. What the panel
     * connection carries is one word - the gated setter
     * (__ZN25IOMobileFramebufferLegacy23set_hotPlug_state_gatedEj, 26C5035r)
     * stores state at instance +0x2380 and get_digital_out_state_gated() reads
     * it back for the user client - so writing it here keeps the connected
     * answer WindowServer reads at boot. Only the workloop notification push
     * that hotPlug_notify's runAction performs is still missing. */
    gHotplugSends = gHotplugSends + 1;
    if (display) {
        uint32_t value = (uint32_t)state;
        memcpy((unsigned char *)display + kLegacyHotPlugState, &value, sizeof(value));
    }
    printf("TB710FU GopDisplay: hotplug state %llu published to the family\n",
           (unsigned long long)state);
}

extern "C" OSObject *native_legacy_ioav_timing(const void *data, uint32_t standardType, uint32_t timingStandard)
{
    gIoavRequests = gIoavRequests + 1;
    if (!data) {
        printf("TB710FU GopDisplay: IOAV timing called without data\n");
        return nullptr;
    }
    if (!ioavPrepare()) {
        return nullptr;
    }
    /* withData's own sequence: allocate through the class, then initWithData
     * through the object's live vtable (slot 0x170, diversity 0xf722). */
    OSObject *timing = OSMetaClass::allocClassWithName("IOAVVideoTimingElement");
    if (!timing) {
        printf("TB710FU GopDisplay: IOAV timing allocation failed\n");
        return nullptr;
    }
    uintptr_t vtable = ioavObjectVtable(timing);
    uintptr_t expected = kIoavTimingVtableFixture + 16 + gIoavSlide;
    if (vtable != expected) {
        printf("TB710FU GopDisplay: IOAV timing vtable %p != %p, element rejected\n",
               (void *)vtable, (void *)expected);
        timing->release();
        return nullptr;
    }
    typedef bool (*Fn)(void *, const void *, uint32_t, uint32_t);
    Fn initWithData = ioavEntry<Fn>(vtable, kIoavTimingInitWithDataSlot,
                                    kIoavTimingInitWithDataDiversity);
    bool built = initWithData ? initWithData(timing, data, standardType, timingStandard) : false;
    printf("TB710FU GopDisplay: IOAV timing=%p vtable=%p initWithData=%d%s\n",
           (void *)timing, (void *)vtable, built ? 1 : 0,
           initWithData ? "" : " (slot rejected)");
    if (!built) {
        timing->release();
        return nullptr;
    }
    gIoavTimings = gIoavTimings + 1;
    return timing;
}

extern "C" void native_legacy_ioav_timing_colors(OSObject *timing, OSArray *colors)
{
    if (!timing || !colors) {
        printf("TB710FU GopDisplay: IOAV setColorElements skipped (timing=%p colors=%p)\n",
               (void *)timing, (void *)colors);
        return;
    }
    uintptr_t vtable = ioavObjectVtable(timing);
    typedef void (*Fn)(void *, OSArray *);
    Fn setColorElements = vtable ? ioavEntry<Fn>(vtable, kIoavTimingSetColorElementsSlot,
                                                 kIoavTimingSetColorElementsDiversity) : nullptr;
    if (!setColorElements) {
        printf("TB710FU GopDisplay: IOAV setColorElements unavailable (vtable=%p)\n", (void *)vtable);
        return;
    }
    setColorElements(timing, colors);
    printf("TB710FU GopDisplay: IOAV setColorElements done\n");
}

extern "C" OSObject *native_legacy_ioav_color(uint32_t standardType, uint32_t depth, uint32_t encoding,
                                              uint32_t range, uint32_t eotf, uint32_t colorimetry, bool wide,
                                              const OSDictionary *primaries, const OSDictionary *gamma,
                                              const OSDictionary *matrix, const OSSet *capabilities)
{
    gIoavRequests = gIoavRequests + 1;
    if (!ioavPrepare()) {
        return nullptr;
    }
    if (!gIoavSlideValid) {
        printf("TB710FU GopDisplay: IOAV color skipped, slide unresolved\n");
        return nullptr;
    }
    /* withCapabilities is not virtual: no colour-vtable entry exists for it, so
     * it is called at fixture + slide after the prologue check. */
    uintptr_t address = kIoavColorFactoryFixture + gIoavSlide;
    if (!ioavTextLooksLive(address)) {
        printf("TB710FU GopDisplay: IOAV color factory not at %p, color stays unpublished\n",
               (void *)address);
        return nullptr;
    }
    typedef OSObject *(*Fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool,
                            const OSDictionary *, const OSDictionary *, const OSDictionary *, const OSSet *);
    Fn withCapabilities = reinterpret_cast<Fn>(
        ptrauth_sign_unauthenticated(reinterpret_cast<void *>(address),
                                     ptrauth_key_function_pointer, 0));
    OSObject *color = withCapabilities(standardType, depth, encoding, range, eotf, colorimetry, wide,
                                       primaries, gamma, matrix, capabilities);
    uintptr_t vtable = color ? ioavObjectVtable(color) : 0;
    uintptr_t expected = kIoavColorVtableFixture + 16 + gIoavSlide;
    printf("TB710FU GopDisplay: IOAV color=%p vtable=%p expected=%p\n",
           (void *)color, (void *)vtable, (void *)expected);
    if (!color || vtable != expected) {
        if (color) {
            color->release();
        }
        return nullptr;
    }
    gIoavColors = gIoavColors + 1;
    return color;
}

// Optional GPU/DPU weak_import hooks from NativeLegacyDisplay.cpp.
// XNU static link requires definitions when Adreno/DPU providers are not linked;
// returning false/0 cleanly keeps GopFramebuffer in GOP CPU-present scanout mode.
extern "C" bool native_adreno_fences(uint32_t *submitted, uint32_t *retired)
{
    (void)submitted;
    (void)retired;
    return false;
}

extern "C" bool native_adreno_fences_detail(uint32_t *a, uint32_t *b, uint32_t *c)
{
    (void)a;
    (void)b;
    (void)c;
    return false;
}

extern "C" int native_gpu_trace_enabled(void)
{
    return 0;
}

extern "C" void native_gpu_trace_record(uint32_t event, uint32_t pid, const uint64_t *values, uint32_t count)
{
    (void)event;
    (void)pid;
    (void)values;
    (void)count;
}
