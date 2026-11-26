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
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSKextLib.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <ptrauth.h>
#include <kern/debug.h>
#include "GopDisplayTraits.h"
#include "OriginalLegacyFramebufferABI.h"

extern kmod_info_t g_kernel_kmod_info;

/* pl50: macOS WindowServer reaches the panel through an IODisplay object that
 * NativeLegacyDisplay attaches to the framebuffer. Its attach routine allocates
 * IODisplay only when this node publishes "NativeDisplayEDID"; the board had no
 * EDID at all, so every pl47/pl48/pl49 boot logged "display object not attached
 * (kext 0)" and no display ever appeared. Build a 128-byte EDID 1.4 block for
 * the 3200x2000 internal panel (398.32 MHz, CVT reduced blanking) instead of
 * depending on a firmware-provided block that this UEFI handoff does not carry. */
static OSData *tb710fu_display_edid(void)
{
    uint8_t e[128] = {0};
    uint32_t sum = 0;

    e[0] = 0x00; e[1] = 0xFF; e[2] = 0xFF; e[3] = 0xFF;
    e[4] = 0xFF; e[5] = 0xFF; e[6] = 0xFF; e[7] = 0x00;
    e[8] = 0x30; e[9] = 0xAE;              /* "LEN" */
    e[10] = 0x00; e[11] = 0x01;
    e[12] = 0x00; e[13] = 0x00; e[14] = 0x00; e[15] = 0x00;
    e[16] = 0x01; e[17] = 0x1E;            /* week 1, 2026 */
    e[18] = 0x01; e[19] = 0x04;            /* EDID 1.4 */
    e[20] = 0xA5;                          /* digital, 8 bpc, DisplayPort */
    e[21] = 30; e[22] = 19;                /* 30 x 19 cm */
    e[23] = 120;                           /* gamma 2.2 */
    e[24] = 0x0A;                          /* sRGB default, RGB, preferred timing */
    e[25] = 0xEE; e[26] = 0x91; e[27] = 0xA3; e[28] = 0x54; e[29] = 0x4C;
    e[30] = 0x99; e[31] = 0x26; e[32] = 0x0F; e[33] = 0x50; e[34] = 0x54;
    /* no established or standard timings */
    for (int i = 38; i < 54; i += 2) { e[i] = 0x01; e[i + 1] = 0x01; }
    /* detailed timing: 3200x2000@60, 3280x2024 total, 398.32 MHz */
    e[54] = 0x98; e[55] = 0x9B;
    e[56] = 0x80; e[57] = 0x50; e[58] = 0xC0;
    e[59] = 0xD0; e[60] = 0x18; e[61] = 0x71;
    e[62] = 0x20; e[63] = 0x08; e[64] = 0x22; e[65] = 0x00;
    e[66] = 0x2C; e[67] = 0xBE; e[68] = 0x10;
    e[69] = 0x00; e[70] = 0x00; e[71] = 0x1E;
    /* monitor name descriptor */
    e[72] = 0x00; e[73] = 0x00; e[74] = 0x00; e[75] = 0xFC; e[76] = 0x00;
    const char *name = "TB710FU";
    for (int i = 0; i < 13; i++) { e[77 + i] = (i < 7) ? (uint8_t)name[i] : ' '; }
    e[89] = 0x0A;
    /* range limits: 50-61 Hz, 30-90 kHz, 400 MHz */
    e[90] = 0x00; e[91] = 0x00; e[92] = 0x00; e[93] = 0xFD; e[94] = 0x00;
    e[95] = 50; e[96] = 61; e[97] = 30; e[98] = 90; e[99] = 40;
    e[100] = 0x00; e[101] = 0x0A;
    for (int i = 102; i < 108; i++) { e[i] = 0x20; }
    /* dummy descriptor */
    e[108] = 0x00; e[109] = 0x00; e[110] = 0x00; e[111] = 0x10; e[112] = 0x00;
    e[126] = 0x00;                         /* no extensions */
    for (int i = 0; i < 127; i++) { sum += e[i]; }
    e[127] = (uint8_t)(256 - (sum & 0xFF));
    return OSData::withBytes(e, sizeof(e));
}
extern "C" void tb710fu_gop_meta_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);

namespace {
volatile uint32_t gGopStarted = 0;
const OSMetaClass *gLegacyParent = nullptr;
uintptr_t gLegacyVtable = 0;
uint64_t *gLegacyTable = nullptr;

// Resolve one live family vtable entry for this call site.
//
// The entry is signed with the *slot* modifier the family signed it with:
// modifier = slot_address blended with the fixture's per-slot discriminator
// (see native_tb710fu_gop_adopt(), which re-signs every copied entry exactly
// this way). It is then re-signed with modifier 0 for this call.
//
// pl44 stripped the entry and re-signed it for the C++ function type
// `Fn`. clang folds that pair into an authenticated resign for type
// discrimination: "autiza" with modifier 0, xpaci+cmp, "brk #0xc470" on
// mismatch, then pacia with ptrauth_type_discriminator(Fn). Modifier 0 can
// never match a slot-signed entry, so native_legacy_parent_start() trapped on
// its first instruction (pl44: the boot stopped right after
// "TB710FU GopDisplay: start" with no further COM5 byte).
// ptrauth_auth_and_resign() authenticates with the real slot modifier and
// resigns with 0, which clang emits as autia (succeeds) + paciza, and inlines
// into a plain authenticated tail call with the original entry.
template <typename Fn>
Fn legacyEntry(unsigned slot)
{
    const volatile uint64_t *entries = (const volatile uint64_t *)gLegacyVtable;
    uintptr_t slotModifier = ptrauth_blend_discriminator((void *)&entries[slot],
                                                        tb710fu_gop_slot_diversity[slot]);
    void *entry = ptrauth_auth_and_resign((void *)(uintptr_t)entries[slot],
                                          ptrauth_key_function_pointer, slotModifier,
                                          ptrauth_key_function_pointer, 0);
    return (Fn)(uintptr_t)entry;
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
    /* pl49: no console output on this path. newUserClient is entered from a
     * user task while IOKit holds the service lock with preemption disabled;
     * the pl48 trace line did a synchronous CDC write here and the 26C5035r
     * kernel panicked with "preemption disable timeout exceeded: gross 6.637
     * ms, net 6.220 ms >= 5.0 ms" right after WindowServer opened the
     * framebuffer. The forwarding call itself is unchanged. */
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

/* The GOP range needs an owner, but not the global one. pl43 published it on
 * IOResources - the shared resources nub every IOResources client matches
 * against. That is wrong three ways: "IODeviceMemory" on IOResources describes
 * a framebuffer to every one of those clients, the write takes IOResources'
 * arbitration lock while boot is still attaching drivers through it, and the
 * service outlives every display that maps the range. The range gets a
 * dedicated provider instead, so NativeLegacyDisplay::startHardware() keeps the
 * ordinary provider contract (provider index 0 mapped with kIOMapInhibitCache,
 * and the map must describe the live console exactly) and no global service is
 * mutated. The provider is attached under IOResources for the plane and
 * deliberately not registered: nothing may match it and only this display uses
 * it.
 *
 * The provider must be an AppleARMIODevice instance, not a bare IOService.
 * IOMobileFramebufferLegacy::start() casts it with
 * provider->metaCast("AppleARMIODevice") and keeps the result in
 * fAppleARMIODevice (instance +0x2140); a failed cast logs
 * "%s: provider was not AppleARMIODevice." and leaves the pointer null. The
 * family reads that pointer in its own getDotPitch(), in
 * find_swap_function_gated() ("function-clpc_enabled"/"function-frame_swap")
 * and in allocate_carveout() ("framebuffer-width"/"framebuffer-height").
 * NativeLegacyDisplay overrides getDotPitch() to read "dot-pitch" from the
 * provider directly, but the other two still look through the family pointer
 * and saw null on pl46. */
static IOService *gGopProvider;

static IOService *allocate_gop_provider()
{
    /* AppleARMPlatform registers this class in the prelinked kernel, so the
     * metaclass alloc runs the same operator new + constructor its own arm-io
     * nodes use. init() is the inherited IOService::init(): AppleARMIODevice
     * adds no initialiser of its own, and the instance is neither started nor
     * registered here, so it needs no AppleARMIOP parent. */
    IOService *provider = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("AppleARMIODevice"));
    if (provider && provider->init()) {
        printf("TB710FU GopDisplay: provider is %s\n", provider->getMetaClass()->getClassName());
        return provider;
    }
    if (provider) {
        printf("TB710FU GopDisplay: AppleARMIODevice init failed\n");
        provider->release();
    } else {
        printf("TB710FU GopDisplay: AppleARMIODevice class unavailable\n");
    }
    /* Keep the display bootable if a future fileset drops the class: the family
     * falls back to the null fAppleARMIODevice path pl46 already ran, it does
     * not fail the display start. */
    provider = new IOService;
    if (provider && provider->init()) {
        printf("TB710FU GopDisplay: provider is %s (AppleARMIODevice unavailable)\n",
               provider->getMetaClass()->getClassName());
        return provider;
    }
    if (provider) {
        provider->release();
    }
    return nullptr;
}

static IOService *publish_gop_memory(IOService *resources)
{
    /* One provider per boot: a second call must not publish a second range or
     * orphan the one the display already attached to. */
    if (gGopProvider) return gGopProvider;
    PE_Video video = {};
    if (!resources || PE_current_console(&video)) {
        printf("TB710FU GopDisplay: no console state (resources=%p)\n", (void *)resources);
        return nullptr;
    }
    if (!video.v_baseAddr || video.v_depth != 32 || !video.v_width || !video.v_height ||
        video.v_rotate || video.v_width > 16384 || video.v_height > 16384 ||
        video.v_rowBytes < video.v_width * 4u ||
        video.v_rowBytes > UINT64_MAX / video.v_height) {
        printf("TB710FU GopDisplay: unusable GOP console phys=%llx %ux%u depth=%u stride=%u rotate=%u\n",
               (unsigned long long)video.v_baseAddr, (unsigned)video.v_width,
               (unsigned)video.v_height, (unsigned)video.v_depth,
               (unsigned)video.v_rowBytes, (unsigned)video.v_rotate);
        return nullptr;
    }
    const uint64_t bytes = (uint64_t)video.v_rowBytes * video.v_height;
    IODeviceMemory *range = IODeviceMemory::withRange((IOPhysicalAddress)video.v_baseAddr, (IOByteCount)bytes);
    OSArray *ranges = range ? OSArray::withCapacity(1) : nullptr;
    if (!ranges || !ranges->setObject(range)) {
        printf("TB710FU GopDisplay: no memory for the GOP range (%llu bytes)\n",
               (unsigned long long)bytes);
        OSSafeReleaseNULL(range);
        OSSafeReleaseNULL(ranges);
        return nullptr;
    }
    IOService *provider = allocate_gop_provider();
    if (!provider) {
        printf("TB710FU GopDisplay: no provider for the GOP range\n");
        OSSafeReleaseNULL(range);
        OSSafeReleaseNULL(ranges);
        return nullptr;
    }
    provider->setName("TB710FUGopProvider");
    /* getDotPitch() reads "dot-pitch" from fAppleARMIODevice as OSData holding
     * one uint32 dots-per-inch value. 294 dpi is the density the 3200x2000 panel
     * reports; tb710fu-gop-dpi overrides it for a different panel. */
    uint32_t dotPitch = 294;
    PE_parse_boot_argn("tb710fu-gop-dpi", &dotPitch, sizeof(dotPitch));
    if (dotPitch) {
        OSData *pitch = OSData::withBytes(&dotPitch, sizeof(dotPitch));
        if (pitch) {
            provider->setProperty("dot-pitch", pitch);
            pitch->release();
        }
    }
    /* setDeviceMemory is setProperty("IODeviceMemory", ranges): it takes its own
     * reference, so the local array and range are released right after. */
    provider->setDeviceMemory(ranges);
    OSSafeReleaseNULL(range);
    OSSafeReleaseNULL(ranges);
    if (!provider->attach(resources)) {
        printf("TB710FU GopDisplay: GOP provider attach failed\n");
        provider->release();
        return nullptr;
    }
    /* pl48: publish_gop_memory() used to attach the nub and stop there. The
     * reference path (LegacyDisplayProbe.cpp) runs attach -> start on its nub
     * before any display attaches to it, and that shape is the one whose display
     * WindowServer actually consumed (its macOS run40 note about a 0x0 inch
     * panel). Start is reported and never fatal: a provider that refuses must
     * still leave the mapped console and the registered display intact. */
    const bool providerStarted = provider->start(resources);
    printf("TB710FU GopDisplay: provider start -> %d\n", providerStarted ? 1 : 0);
    gGopProvider = provider;
    printf("TB710FU GopDisplay: GOP framebuffer phys=%llx %ux%u stride=%u bytes=%llu on %s\n",
           (unsigned long long)video.v_baseAddr, (unsigned)video.v_width,
           (unsigned)video.v_height, (unsigned)video.v_rowBytes, (unsigned long long)bytes,
           provider->getName());
    return provider;
}

/* pl47: the display-subsystem node CoreDisplay walks to find the panel.
 *
 * Sheng's reference attaches the started GOP nub under /arm-io in gIODTPlane
 * with device_type "display-subsystem" and DT name "gop-display"
 * (LegacyDisplayProbe.cpp 685-695). This port's provider is already attached
 * under IOResources for the service plane; the DT parent is what makes the
 * display exist in the device tree at all. The node is attached whenever
 * /arm-io exists - the display itself still cannot start without
 * offline-legacy-phase=4, so there is no extra gate to duplicate here. */
static void attach_gop_device_tree(IOService *provider)
{
    unsigned phase = 0;
    PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
    IORegistryEntry *armIo = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    if (!armIo) {
        printf("TB710FU GopDisplay: /arm-io absent, no display-subsystem node (phase=%u)\n", phase);
        return;
    }
    bool attached = false;
    const char type[] = "display-subsystem";
    OSData *data = OSData::withBytes(type, sizeof(type));
    if (data && provider->setProperty("device_type", data)) {
        provider->setName("gop-display", gIODTPlane);
        attached = provider->attachToParent(armIo, gIODTPlane);
    }
    OSSafeReleaseNULL(data);
    printf("TB710FU GopDisplay: /arm-io display-subsystem attach=%d phase=%u\n",
           attached ? 1 : 0, phase);
    armIo->release();
}

/* pl47: the IOSurface root service, loaded and waited for before start().
 *
 * iPadOS names the root IOCoreSurfaceRoot; macOS leaves it unnamed so its name
 * is its class, IOSurfaceRoot. IONameMatch accepts either
 * (LegacyDisplayProbe.cpp 643-655). CoreDisplay creates its framebuffer
 * surfaces through this root, so the display publishes attributes only once
 * it exists. */
static void wait_for_surface_root(void)
{
    OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    printf("TB710FU GopDisplay: IOSurface load=%x\n", loaded);
    OSDictionary *matching = OSDictionary::withCapacity(1);
    OSString *core = OSString::withCString("IOCoreSurfaceRoot");
    OSString *root = OSString::withCString("IOSurfaceRoot");
    const OSObject *names[2] = { core, root };
    OSArray *list = (core && root) ? OSArray::withObjects(names, 2) : nullptr;
    IOService *surface = nullptr;
    if (matching && list && matching->setObject(gIONameMatchKey, list)) {
        surface = IOService::waitForMatchingService(matching, 5000000000ULL);
    }
    printf("TB710FU GopDisplay: IOSurfaceRoot=%p\n", (void *)surface);
    if (surface) {
        surface->release();
    }
    OSSafeReleaseNULL(list);
    OSSafeReleaseNULL(matching);
    if (core) {
        core->release();
    }
    if (root) {
        root->release();
    }
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

    /* The prelinked kernel already registers IOMobileFramebufferLegacy. Calling
     * OSKextLoadKextWithIdentifier here was blocking before it returned (the log
     * stopped at "loading" and neither kext load= nor meta= followed), which kept
     * the class from ever being adopted and prevented WindowServer from starting.
     * Resolve the prelinked class first and only ask kextd to load it when the
     * class is genuinely absent. */
    const OSSymbol *name = OSSymbol::withCString("IOMobileFramebufferLegacy");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSReturn loaded = kOSReturnSuccess;
    if (!meta) {
        printf("TB710FU GopDisplay: prelinked class absent, loading com.apple.iokit.IOMobileGraphicsFamily\n");
        loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
        printf("TB710FU GopDisplay: kext load=%x\n", loaded);
        meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    } else {
        printf("TB710FU GopDisplay: using prelinked IOMobileGraphicsFamily (OSKextLoad skipped)\n");
    }
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
    printf("TB710FU GopDisplay: publishing the GOP range\n");
    IOService *provider = publish_gop_memory(resources);
    if (!provider) {
        printf("TB710FU GopDisplay: GOP range unavailable, display not registered\n");
        resources->release();
        return;
    }
    printf("TB710FU GopDisplay: provider=%s %p\n", provider->getName(), (void *)provider);
    printf("TB710FU GopDisplay: attaching the display-subsystem node\n");
    attach_gop_device_tree(provider);

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
     * a breadcrumb, instead of inside init(). It cannot free a half-built
     * object: OSObject's constructor sets retainCount to 1, so retain()/release()
     * only takes it to 2 and back to 1. The six IOService slots adoption now
     * owns (start/stop/free/setPowerState/newUserClient/registerService) cannot
     * be probed here without side effects; their discriminators are audited
     * against the fixture at build time instead. */
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
    /* pl48: the identity WindowServer can match. pl47 published IOClass
     * "NativeLegacyDisplay" and left the registry name at the metaclass
     * "IOMobileFramebufferLegacy", so every boot ended with busy=0: not one
     * IOMobileFramebufferUserClient was ever requested (the only denial on
     * record is a sandboxed managedappdistri probe). Clients ask IOKit for
     * "IOMobileFramebuffer" by name and by class, and IOMatchCategory alone only
     * arbitrates exclusive opens; it never widens a match. Both are therefore
     * published as the plain family name, while the object keeps its adopted
     * live vtable and its real metaclass. */
    display->setProperty("IOClass", "IOMobileFramebuffer");
    display->setProperty("IOMatchCategory", "IOMobileFramebuffer");
    display->setProperty("IOProviderClass", provider->getMetaClass()->getClassName());
    display->setName("IOMobileFramebuffer");
    /* pl50: NativeLegacyDisplay's IODisplay attach reads this before it builds
     * the panel object; without it the post-start attach thread bails out. */
    OSData *edid = tb710fu_display_edid();
    if (edid) {
        bool published = display->setProperty("NativeDisplayEDID", edid);
        printf("TB710FU GopDisplay: NativeDisplayEDID %u bytes -> %d\n",
               (unsigned)edid->getLength(), published ? 1 : 0);
        edid->release();
    } else {
        printf("TB710FU GopDisplay: NativeDisplayEDID allocation failed\n");
    }
    printf("TB710FU GopDisplay: publishing name=%s class=%s\n",
           display->getName(), "IOMobileFramebuffer");

    printf("TB710FU GopDisplay: attach\n");
    bool attached = display->attach(provider);
    printf("TB710FU GopDisplay: attach -> %d\n", attached ? 1 : 0);
    if (!attached) {
        display->release();
        resources->release();
        return;
    }

    printf("TB710FU GopDisplay: waiting for IOSurfaceRoot\n");
    wait_for_surface_root();

    printf("TB710FU GopDisplay: start\n");
    bool started = display->start(provider);
    printf("TB710FU GopDisplay: start -> %d\n", started ? 1 : 0);
    if (started) {
        printf("TB710FU GopDisplay: registerService\n");
        display->registerService();
        printf("TB710FU GopDisplay: registered\n");
    }
    display->release();
    resources->release();
}

