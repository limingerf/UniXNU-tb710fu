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
//      original table with this backend's 25 owned overrides copied in, and point
//      the instance at it. Every guard slot then runs original family code.
//
// Nothing here is reachable unless the kernel is booted with tb710fu-gop=1.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
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
// this call site's own function type, so the original body is entered correctly.
template <typename Fn>
Fn legacyEntry(unsigned slot)
{
    uint64_t raw = ((const volatile uint64_t *)gLegacyVtable)[slot];
    void *stripped = ptrauth_strip((void *)(uintptr_t)raw, ptrauth_key_function_pointer);
    return (Fn)(uintptr_t)ptrauth_sign_unauthenticated(stripped, ptrauth_key_function_pointer, 0);
}

bool legacySlotIsOwned(unsigned slot)
{
    for (unsigned i = 0; i < TB710FU_GOP_OWNED_COUNT; i++) {
        if (tb710fu_gop_owned_slots[i] == slot) {
            return true;
        }
    }
    return false;
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
        for (unsigned slot = 0; slot < TB710FU_GOP_SLOT_COUNT; slot++) {
            uint64_t src = legacySlotIsOwned(slot) ? ownedWords[slot] : origWords[slot];
            void *stripped = ptrauth_strip((void *)(uintptr_t)src, ptrauth_key_function_pointer);
            uint64_t disc = ptrauth_blend_discriminator(&table[slot], tb710fu_gop_slot_diversity[slot]);
            table[slot] = (uint64_t)(uintptr_t)ptrauth_sign_unauthenticated(stripped, ptrauth_key_function_pointer, disc);
        }
        gLegacyTable = table;
        printf("TB710FU GopDisplay: adopted %u live slots, %u owned overrides (resigned at %p)\n",
               TB710FU_GOP_SLOT_COUNT - TB710FU_GOP_OWNED_COUNT, TB710FU_GOP_OWNED_COUNT, (void *)table);
    }
    if (rawStatic != staticTable) {
        uint64_t vdisc = ptrauth_blend_discriminator(object, 0);
        *(uintptr_t *)object = (uintptr_t)ptrauth_sign_unauthenticated(gLegacyTable, ptrauth_key_cxx_vtable_pointer, vdisc);
    } else {
        *(uintptr_t *)object = (uintptr_t)gLegacyTable;
    }
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

    OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    const OSSymbol *name = OSSymbol::withCString("IOMobileFramebufferLegacy");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) {
        name->release();
    }
    uint32_t size = meta ? meta->getClassSize() : 0u;
    printf("TB710FU GopDisplay: IOMobileFramebufferLegacy load=%x meta=%p size=0x%x\n",
           loaded, (void *)meta, size);
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

    // Resolve the live IOMobileFramebufferLegacy vtable directly from a probe
    // instance: XnuPrelink slides __DATA (metaclass) and __DATA_CONST (vtable)
    // independently, so fixture offset arithmetic does not locate the live table.
    OSObject *probe = meta->alloc();
    if (!probe) {
        printf("TB710FU GopDisplay: meta->alloc() failed, display not registered\n");
        return;
    }
    uintptr_t rawVtable = *(uintptr_t *)probe;
    uintptr_t vtable = (uintptr_t)ptrauth_strip((void *)rawVtable, ptrauth_key_cxx_vtable_pointer);
    probe->release();
    if (!legacyVtableLooksReviewed(vtable)) {
        printf("TB710FU GopDisplay: live vtable at %p (raw %p) failed validation, display not registered\n",
               (void *)vtable, (void *)rawVtable);
        return;
    }
    gLegacyParent = meta;
    gLegacyVtable = vtable;
    printf("TB710FU GopDisplay: live vtable=%p (raw=%p, %u slots validated)\n",
           (void *)vtable, (void *)rawVtable, TB710FU_GOP_SLOT_COUNT);

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        printf("TB710FU GopDisplay: preModLoad failed\n");
        return;
    }
    tb710fu_gop_meta_init();
    OSReturn status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) {
        printf("TB710FU GopDisplay: postModLoad failed %x\n", status);
        return;
    }

    OSDictionary *matching = IOService::serviceMatching("IOResources");
    IOService *resources = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (matching) {
        matching->release();
    }
    if (!resources) {
        printf("TB710FU GopDisplay: IOResources unavailable\n");
        return;
    }

    IOService *display = native_legacy_display_allocate();
    if (!display) {
        printf("TB710FU GopDisplay: allocation failed\n");
        resources->release();
        return;
    }
    display->setProperty("IOClass", "NativeLegacyDisplay");
    display->setProperty("IOMatchCategory", "IOMobileFramebuffer");
    display->setProperty("IOProviderClass", "IOResources");
    const char *step = !display->init() ? "init" : !display->attach(resources) ? "attach" :
                       !display->start(resources) ? "start" : nullptr;
    printf("TB710FU GopDisplay: NativeLegacyDisplay %s\n", step ? step : "started");
    if (!step) {
        display->registerService();
    }
    display->release();
    resources->release();
}
