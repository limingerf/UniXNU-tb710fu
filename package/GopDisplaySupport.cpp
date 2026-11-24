// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB710FU macOS 27 GopFramebuffer observation and surface hooks (pl21).
//
// NativeLegacyDisplay.cpp calls these as plain C hooks. On iPadOS they came from
// the reference device's probe unit and from the family's own helpers; the macOS 27
// family helpers are prelinked kext functions this board cannot bind by address, so
// the IOSurface accessors are decoded from the reviewed 26C5035r stubs (six leaf
// loads: descriptor 0x30, width 0x58, height 0x60, stride 0x68, format 0x80,
// planes 0x98) and the IOAV, hotplug, vsync and swap-info senders are explicit
// downgrades: they report and do nothing, which leaves the GOP scanout copy working
// and only drops the display-mode metadata and vblank signalling.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSet.h>
#include <pexpert/pexpert.h>
#include "GopDisplayTraits.h"

namespace {
volatile uint64_t gFrames;
volatile uint64_t gTransfers;
volatile uint64_t gRetirements;
volatile uint64_t gVsyncSends;
volatile uint64_t gHotplugSends;
volatile uint64_t gSwapInfoSends;
volatile uint64_t gIoavRequests;
volatile uint64_t gRejectedSurfaces;
volatile uint64_t gClientEntries;
volatile uint64_t gGuardHits;

uintptr_t surfaceField(OSObject *surface, unsigned offset, unsigned width)
{
    uintptr_t value = 0;
    if (!surface) {
        return 0;
    }
    memcpy(&value, (const unsigned char *)surface + offset, width);
    return value;
}
}  // namespace

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
    return surfaceField(surface, TB710FU_GOP_SURFACE_STRIDE, TB710FU_GOP_SURFACE_STRIDE_WIDTH);
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
    (void)stage;
}

extern "C" void native_legacy_client_enter(task_t task, uint32_t type)
{
    (void)task;
    (void)type;
    gClientEntries = gClientEntries + 1;
}

extern "C" void native_legacy_client_observed(IOReturn status, IOUserClient *client)
{
    (void)status;
    (void)client;
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
    (void)kind;
    (void)count;
    (void)minx;
    (void)miny;
    (void)maxx;
    (void)maxy;
    gFrames = gFrames + 1;
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
    (void)accepted;
    (void)reason;
    (void)request;
    (void)surface;
}

extern "C" void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format,
                                               uint32_t planes, uint64_t stride, uint64_t allocation,
                                               uint64_t length)
{
    (void)width;
    (void)height;
    (void)format;
    (void)planes;
    (void)stride;
    (void)allocation;
    (void)length;
    gRejectedSurfaces = gRejectedSurfaces + 1;
}

extern "C" bool native_legacy_prepare_scanout(void)
{
    // The console was handed over when the display started; nothing to reclaim here.
    return true;
}

extern "C" void native_legacy_transfer_observed(IOReturn status, const void *request, uint64_t bytes)
{
    (void)request;
    (void)bytes;
    if (status == kIOReturnSuccess) {
        gTransfers = gTransfers + 1;
    }
}

extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes)
{
    (void)ticks;
    (void)bytes;
}

extern "C" void native_legacy_retirement_observed(IOService *display)
{
    (void)display;
    gRetirements = gRetirements + 1;
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
    // Downgrade: no IOMobileFramebuffer vsync message is published without the
    // family's own sender. The refresh tick still retires swaps.
    (void)display;
    (void)timestamp;
    (void)continuousTime;
    (void)interval;
    gVsyncSends = gVsyncSends + 1;
}

extern "C" bool native_legacy_set_refresh_properties(void *display)
{
    (void)display;
    return false;
}

extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz)
{
    (void)event;
    (void)hz;
}

extern "C" void native_legacy_swap_info_dispatch(void *display, void *blob)
{
    // Downgrade: the family's swap-info sender is not bound, so the compositor gets
    // no swap statistics; the swap itself still completes through the family queue.
    (void)display;
    (void)blob;
    gSwapInfoSends = gSwapInfoSends + 1;
}

extern "C" void native_legacy_hotplug_notify(void *display, uint64_t state)
{
    // Downgrade: the family's hotPlug_notify is not bound. Log the intent so the
    // boot trace shows whether the backend reached the connected state.
    (void)display;
    gHotplugSends = gHotplugSends + 1;
    printf("TB710FU GopDisplay: hotplug state %llu not published to the family\n",
           (unsigned long long)state);
}

extern "C" OSObject *native_legacy_ioav_timing(const void *data, uint32_t standardType, uint32_t timingStandard)
{
    // Downgrade: IOAVVideoTimingElement::withData is an IOAVFamily kext symbol this
    // board cannot bind. The display publishes its size and attributes without a
    // timing element, so CoreDisplay falls back to the GOP mode.
    (void)data;
    (void)standardType;
    (void)timingStandard;
    gIoavRequests = gIoavRequests + 1;
    return nullptr;
}

extern "C" void native_legacy_ioav_timing_colors(OSObject *timing, OSArray *colors)
{
    (void)timing;
    (void)colors;
}

extern "C" OSObject *native_legacy_ioav_color(uint32_t standardType, uint32_t depth, uint32_t encoding,
                                              uint32_t range, uint32_t eotf, uint32_t colorimetry, bool wide,
                                              const OSDictionary *primaries, const OSDictionary *gamma,
                                              const OSDictionary *matrix, const OSSet *capabilities)
{
    (void)standardType;
    (void)depth;
    (void)encoding;
    (void)range;
    (void)eotf;
    (void)colorimetry;
    (void)wide;
    (void)primaries;
    (void)gamma;
    (void)matrix;
    (void)capabilities;
    gIoavRequests = gIoavRequests + 1;
    return nullptr;
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
