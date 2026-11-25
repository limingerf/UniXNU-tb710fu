// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB710FU macOS 27 GopFramebuffer runtime binding (pl21).
//
// `IOMobileFramebufferLegacy` of 26C5035r is a prelinked kext class. XnuPrelink
// re-lays Apple kext text in this fileset, so no original vtable target or method
// address from the reviewed ABI fixture survives at its fixture address (verified:
// 0/483 entries). This file therefore resolves the class at boot and adopts its
// *live* vtable instead of linking any original symbol:
//
//   1. look the metaclass up by name and check the 0x23a0 instance extent;
//   2. derive the collection slide from the metaclass itself and validate the
//      vtable at the fixture offset by its 483 authenticated-pointer diversities;
//   3. on the first allocated display build a runtime table that is the live
//      original table with this backend's 25 legacy-method overrides and its
//      six base-slot overrides (start, stop, free, setPowerState,
//      newUserClient, registerService) copied in, and point the instance at
//      it. Every other guard slot then runs original family code.
//
// Nothing here is reachable unless the kernel is booted with tb710fu-gop=1.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSArray.h>
#include <libkern/OSKextLib.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <ptrauth.h>
#include <kern/debug.h>
#include "GopDisplayTraits.h"
#include "OriginalLegacyFramebufferABI.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" void tb710fu_gop_meta_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);

namespace {
volatile uint32_t gGopStarted = 0;
const OSMetaClass *gLegacyParent = nullptr;
uintptr_t gLegacyVtable = 0;
uint64_t *gLegacyTable = nullptr;

// An authenticated vtable entry is stripped of its signature and re-signed for
// this call site's own function type (arm64e kernel uses function-pointer type
// discrimination, not 0).
template <typename Fn>
Fn legacyEntry(unsigned slot)
{
    uint64_t raw = ((const volatile uint64_t *)gLegacyVtable)[slot];
    void *stripped = ptrauth_strip((void *)(uintptr_t)raw, ptrauth_key_function_pointer);
#if __has_feature(ptrauth_function_pointer_type_discrimination)
    return ptrauth_sign_unauthenticated((Fn)(uintptr_t)stripped, ptrauth_key_function_pointer,
                                        ptrauth_type_discriminator(Fn));
#else
    return ptrauth_sign_unauthenticated((Fn)(uintptr_t)stripped, ptrauth_key_function_pointer, 0);
#endif
}

/* Slots copied from this backend's own compiled table. The 25 legacy methods
 * are the family's virtuals it overrides; the six base slots are the
 * platform-subclass role - start() is the only caller of startDisplay() and
 * startHardware(), stop()/free() release the boot mapping, registerService()
 * publishes the layer count and refresh properties, setPowerState() drives the
 * logical PM path and newUserClient() forwards to the family.
 *
 * pl42 copied only the 25, so the live family entries stayed at those six
 * slots: display->start() ran Apple's IOMobileFramebufferLegacy::start()
 * instead of NativeLegacyDisplay::start(), startDisplay() was never reached and
 * the framebuffer was never mapped, stopped, freed or registered as this port
 * intends. Those slots carry their base method's own diversity (start is
 * 0x3c68 = ptrauth_string_discriminator("_ZN9IOService5startEPS_"), confirmed
 * at the call site in 26C5035r), so they are re-signed exactly like the legacy
 * slots. */
bool legacySlotIsOwned(unsigned slot)
{
    for (unsigned i = 0; i < TB710FU_GOP_OWNED_COUNT; i++) {
        if (tb710fu_gop_owned_slots[i] == slot) {
            return true;
        }
    }
    switch (slot) {
    case TB710FU_GOP_SLOT_START:
    case TB710FU_GOP_SLOT_STOP:
    case TB710FU_GOP_SLOT_FREE:
    case TB710FU_GOP_SLOT_SETPOWERSTATE:
    case TB710FU_GOP_SLOT_NEWUSERCLIENT:
    case TB710FU_GOP_SLOT_REGISTERSERVICE:
        return true;
    default:
        return false;
    }
}

bool legacyVtableLooksReviewed(uintptr_t vtable)
{
    if (!vtable || (vtable & 7u) != 0) {
        return false;
    }
    const volatile uint64_t *words = (const volatile uint64_t *)vtable;
    for (unsigned slot = 0; slot < TB710FU_GOP_SLOT_COUNT; slot++) {
        uint64_t word = words[slot];
        uintptr_t addr = (uintptr_t)ptrauth_strip((void *)(uintptr_t)word, ptrauth_key_function_pointer);
        if ((addr >> 48) != 0xffffu || (addr & 3u) != 0) {
            printf("TB710FU GopDisplay: bad vtable slot %u word=%llx addr=%llx\n",
                   slot, (unsigned long long)word, (unsigned long long)addr);
            return false;
        }
    }
    return true;
}
}  // namespace

extern "C" const OSMetaClass *native_tb710fu_legacy_parent(void)
{
    if (!gLegacyParent) {
        panic("TB710FU GopDisplay: IOMobileFramebufferLegacy parent not resolved before NativeLegacyDisplay metaclass init");
    }
    return gLegacyParent;
}

extern "C" void native_tb710fu_gop_adopt(void *object)
{
    if (!object || !gLegacyVtable) {
        return;
    }
    uintptr_t rawStatic = *(uintptr_t *)object;
    uintptr_t staticTable = (uintptr_t)ptrauth_strip((void *)rawStatic, ptrauth_key_cxx_vtable_pointer);
    if (!staticTable) {
        return;
    }
    if (!gLegacyTable) {
        uint64_t *table = (uint64_t *)IOMallocZero(sizeof(uint64_t) * TB710FU_GOP_SLOT_COUNT);
        if (!table) {
            printf("TB710FU GopDisplay: no memory for the adopted vtable, display left inert\n");
            return;
        }
        const uint64_t *origWords = (const uint64_t *)gLegacyVtable;
        const uint64_t *ownedWords = (const uint64_t *)staticTable;
        /* Every vtable call site builds modifier = slot_address | disc<<48 and
         * branches with BRAA/BRAAZ (verified in 26C5035r: IOService::start
         * tail-calls its base slot with "mov x17,x9; movk x17,#0x3c68,lsl#48;
         * blraa x8,x17"). The signature therefore depends on the slot address,
         * so the live words cannot simply be copied into this table - each
         * entry is stripped and re-signed against its new slot. The 16-bit
         * discriminator is the fixture's per-slot type discriminator (479/479
         * live entries match it). */
        for (unsigned slot = 0; slot < TB710FU_GOP_SLOT_COUNT; slot++) {
            uint64_t src = legacySlotIsOwned(slot) ? ownedWords[slot] : origWords[slot];
            void *stripped = ptrauth_strip((void *)(uintptr_t)src, ptrauth_key_function_pointer);
            uint64_t disc = ptrauth_blend_discriminator(&table[slot], tb710fu_gop_slot_diversity[slot]);
            table[slot] = (uint64_t)(uintptr_t)ptrauth_sign_unauthenticated(stripped, ptrauth_key_function_pointer, disc);
        }
        gLegacyTable = table;
        printf("TB710FU GopDisplay: adopted %u legacy + 6 base overrides (resigned at %p)\n",
               TB710FU_GOP_OWNED_COUNT, (void *)table);
    }
    // arm64e XNU signs OSObject/IOService/IOMobileFramebufferLegacy vptrs with
    // ASDA, address discrimination on `this`, and root type discriminator 0xcda1
    // (ptrauth_string_discriminator("_ZTV15OSMetaClassBase")), confirmed in the
    // 26C5035r kernelcache: "movk x17,#0xcda1,lsl#48; autda x16,x17".
    uint64_t vdisc = ptrauth_blend_discriminator(object, 0xcda1u);
    *(uintptr_t *)object = (uintptr_t)ptrauth_sign_unauthenticated(gLegacyTable, ptrauth_key_cxx_vtable_pointer, vdisc);
}

// Qualified base calls in NativeLegacyDisplay.cpp are routed here: the live
// original slot, never a linked symbol.
extern "C" void native_legacy_parent_free(void *display)
{
    typedef void (*Fn)(void *);
    legacyEntry<Fn>(TB710FU_GOP_SLOT_FREE)(display);
}

extern "C" bool native_legacy_parent_start(void *display, IOService *provider)
{
    typedef bool (*Fn)(void *, IOService *);
    return legacyEntry<Fn>(TB710FU_GOP_SLOT_START)(display, provider);
}

extern "C" void native_legacy_parent_stop(void *display, IOService *provider)
{
    typedef void (*Fn)(void *, IOService *);
    legacyEntry<Fn>(TB710FU_GOP_SLOT_STOP)(display, provider);
}

extern "C" IOReturn native_legacy_parent_set_power_state(void *display, unsigned long ordinal, IOService *device)
{
    typedef IOReturn (*Fn)(void *, unsigned long, IOService *);
    return legacyEntry<Fn>(TB710FU_GOP_SLOT_SETPOWERSTATE)(display, ordinal, device);
}

extern "C" IOReturn native_legacy_parent_new_user_client(void *display, task_t task, void *security,
                                                         UInt32 type, IOUserClient **client)
{
    typedef IOReturn (*Fn)(void *, task_t, void *, UInt32, IOUserClient **);
    return legacyEntry<Fn>(TB710FU_GOP_SLOT_NEWUSERCLIENT)(display, task, security, type, client);
}

extern "C" bool native_legacy_parent_refresh_attributes(void *display, uint64_t *words, bool fixed)
{
    typedef bool (*Fn)(void *, uint64_t *, bool);
    return legacyEntry<Fn>(423)(display, words, fixed);
}

extern "C" void native_legacy_parent_complete_current_swap(void *display)
{
    typedef void (*Fn)(void *);
    legacyEntry<Fn>(416)(display);
}

extern "C" void native_legacy_parent_display_dimensions(void *display, uint32_t *width, uint32_t *height)
{
    typedef void (*Fn)(void *, uint32_t *, uint32_t *);
    legacyEntry<Fn>(365)(display, width, height);
}

extern "C" int native_legacy_parent_apply_state_request(void *display, OriginalLegacyStateRequest *request, bool powered)
{
    typedef int (*Fn)(void *, OriginalLegacyStateRequest *, bool);
    return legacyEntry<Fn>(479)(display, request, powered);
}

extern "C" void native_legacy_parent_create_default_surface(void *display)
{
    typedef void (*Fn)(void *);
    legacyEntry<Fn>(420)(display);
}

/* The generic provider (IOResources) owns no device memory. NativeLegacyDisplay
 * ::startHardware() maps provider index 0 with kIOMapInhibitCache and then
 * requires that the map describe the live GOP framebuffer exactly (physical
 * base, and length == rowBytes * height), so that one range is published on the
 * provider first. It comes from the console PE state, the same source the
 * backend validates - nothing is guessed. */
static int publish_gop_memory(IOService *provider)
{
    PE_Video video = {};
    if (!provider || PE_current_console(&video)) {
        printf("TB710FU GopDisplay: no console state (provider=%p)\n", (void *)provider);
        return 0;
    }
    if (!video.v_baseAddr || video.v_depth != 32 || !video.v_width || !video.v_height ||
        video.v_rotate || video.v_width > 16384 || video.v_height > 16384 ||
        video.v_rowBytes < video.v_width * 4u ||
        video.v_rowBytes > UINT64_MAX / video.v_height) {
        printf("TB710FU GopDisplay: unusable GOP console phys=%llx %ux%u depth=%u stride=%u rotate=%u\n",
               (unsigned long long)video.v_baseAddr, (unsigned)video.v_width,
               (unsigned)video.v_height, (unsigned)video.v_depth,
               (unsigned)video.v_rowBytes, (unsigned)video.v_rotate);
        return 0;
    }
    const uint64_t bytes = (uint64_t)video.v_rowBytes * video.v_height;
    IODeviceMemory *range = IODeviceMemory::withRange((IOPhysicalAddress)video.v_baseAddr, (IOByteCount)bytes);
    OSArray *ranges = range ? OSArray::withCapacity(1) : nullptr;
    if (!ranges || !ranges->setObject(range)) {
        printf("TB710FU GopDisplay: no memory for the GOP range (%llu bytes)\n",
               (unsigned long long)bytes);
        OSSafeReleaseNULL(range);
        OSSafeReleaseNULL(ranges);
        return 0;
    }
    provider->setDeviceMemory(ranges);
    OSSafeReleaseNULL(range);
    OSSafeReleaseNULL(ranges);
    printf("TB710FU GopDisplay: GOP framebuffer phys=%llx %ux%u stride=%u bytes=%llu published\n",
           (unsigned long long)video.v_baseAddr, (unsigned)video.v_width,
           (unsigned)video.v_height, (unsigned)video.v_rowBytes, (unsigned long long)bytes);
    return 1;
}

extern "C" void native_tb710fu_gop_display_start(void)
{
    if (__atomic_exchange_n(&gGopStarted, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }
    uint32_t enabled = 0;
    PE_parse_boot_argn("tb710fu-gop", &enabled, sizeof(enabled));
    if (enabled != 1) {
        printf("TB710FU GopDisplay: disabled (boot with tb710fu-gop=1 to register the display)\n");
        return;
    }
    printf("TB710FU GopDisplay: start entry\n");

    printf("TB710FU GopDisplay: loading com.apple.iokit.IOMobileGraphicsFamily\n");
    OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    printf("TB710FU GopDisplay: kext load=%x\n", loaded);
    const OSSymbol *name = OSSymbol::withCString("IOMobileFramebufferLegacy");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) {
        name->release();
    }
    uint32_t size = meta ? meta->getClassSize() : 0u;
    printf("TB710FU GopDisplay: IOMobileFramebufferLegacy meta=%p size=0x%x\n",
           (void *)meta, size);
    if (!meta || size != TB710FU_GOP_INSTANCE_SIZE) {
        printf("TB710FU GopDisplay: original class extent differs, display not registered\n");
        return;
    }
    bool isIOService = false;
    const OSMetaClass *parent = meta->getSuperClass();
    for (const OSMetaClass *cur = parent; cur; cur = cur->getSuperClass()) {
        if (strcmp(cur->getClassName(), "IOService") == 0) {
            isIOService = true;
            break;
        }
    }
    printf("TB710FU GopDisplay: super=%s isIOService=%d\n",
           parent ? parent->getClassName() : "(null)", isIOService ? 1 : 0);
    if (!isIOService) {
        printf("TB710FU GopDisplay: original class is not an IOService subclass, display not registered\n");
        return;
    }

    // Derive the live IOMobileFramebufferLegacy vtable (slot 0) from the live
    // metaclass address using the fixture's relative offset (verified on pl39/pl40b:
    // meta=0xfffffff00cc2cdf0 -> vtable=0xfffffff0086232c8 == __ZTV + 16).
    uintptr_t metaclass = (uintptr_t)meta;
    uintptr_t slide = metaclass - (uintptr_t)TB710FU_GOP_METACLASS_VA;
    uintptr_t vtable = (uintptr_t)TB710FU_GOP_VTABLE_VA + slide;
    if (!legacyVtableLooksReviewed(vtable)) {
        printf("TB710FU GopDisplay: live vtable at %p (slide %p) failed validation, display not registered\n",
               (void *)vtable, (void *)slide);
        return;
    }
    gLegacyParent = meta;
    gLegacyVtable = vtable;
    printf("TB710FU GopDisplay: live vtable=%p (slide=%p, %u slots validated)\n",
           (void *)vtable, (void *)slide, TB710FU_GOP_SLOT_COUNT);

    printf("TB710FU GopDisplay: preModLoad\n");
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        printf("TB710FU GopDisplay: preModLoad failed\n");
        return;
    }
    tb710fu_gop_meta_init();
    printf("TB710FU GopDisplay: NativeLegacyDisplay metaclass installed\n");
    OSReturn status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) {
        printf("TB710FU GopDisplay: postModLoad failed %x\n", status);
        return;
    }

    printf("TB710FU GopDisplay: waiting for IOResources\n");
    OSDictionary *matching = IOService::serviceMatching("IOResources");
    IOService *resources = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (matching) {
        matching->release();
    }
    printf("TB710FU GopDisplay: IOResources=%p\n", (void *)resources);
    if (!resources) {
        printf("TB710FU GopDisplay: IOResources unavailable\n");
        return;
    }
    (void)publish_gop_memory(resources);

    printf("TB710FU GopDisplay: allocating NativeLegacyDisplay\n");
    IOService *display = native_legacy_display_allocate();
    printf("TB710FU GopDisplay: display=%p\n", (void *)display);
    if (!display) {
        printf("TB710FU GopDisplay: allocation failed\n");
        resources->release();
        return;
    }
    /* PAC probe: two base-slot virtual calls through the vptr adoption just
     * installed. If the vptr or table signing were wrong this traps here, after
     * a breadcrumb, instead of inside init(). */
    printf("TB710FU GopDisplay: pac probe\n");
    display->retain();
    display->release();
    printf("TB710FU GopDisplay: pac probe ok\n");

    printf("TB710FU GopDisplay: init\n");
    bool inited = display->init();
    printf("TB710FU GopDisplay: init -> %d\n", inited ? 1 : 0);
    if (!inited) {
        display->release();
        resources->release();
        return;
    }
    /* Properties are only legal after init(): IORegistryEntry::setProperty()
     * takes reserved->fLock, and `reserved` is allocated by init(). pl42 set
     * IOClass before init, which dereferenced NULL inside that lock and died
     * with no output at all - the actual stop after adoption. */
    display->setProperty("IOClass", "NativeLegacyDisplay");
    display->setProperty("IOMatchCategory", "IOMobileFramebuffer");
    display->setProperty("IOProviderClass", "IOResources");

    printf("TB710FU GopDisplay: attach\n");
    bool attached = display->attach(resources);
    printf("TB710FU GopDisplay: attach -> %d\n", attached ? 1 : 0);
    if (!attached) {
        display->release();
        resources->release();
        return;
    }

    printf("TB710FU GopDisplay: start\n");
    bool started = display->start(resources);
    printf("TB710FU GopDisplay: start -> %d\n", started ? 1 : 0);
    if (started) {
        printf("TB710FU GopDisplay: registerService\n");
        display->registerService();
        printf("TB710FU GopDisplay: registered\n");
    }
    display->release();
    resources->release();
}

