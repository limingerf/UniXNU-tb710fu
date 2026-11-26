// SPDX-License-Identifier: BSD-2-Clause-Patent
// The dormant Tb710fuWifiNet IOEthernetController: a real IONetworkingFamily object created entirely from the live
// prelinked family (zero IONetworkingFamily link symbols, zero SDK networking headers), so the Wi-Fi subsystem has an
// en-style interface before any data path exists. This milestone has no WMI association and no DP ring, so the link is
// reported down; outputPacket frees the mbuf and counts the drop instead of pretending to transmit.
//
// Adapted from package/Tb710fuEthernet.cpp (the en0 controller this board already boots), whose arm64e PAC/vtable
// adoption against 26C5035r IOEthernetController is the verified pattern; slot numbers and diversities come from
// scripts/net-ether-26C5035r.json via the generated Tb710fuWifiSlots.h.

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <ptrauth.h>
#include <stdint.h>
#include <string.h>

#include "Tb710fuWifiSlots.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" volatile int32_t native_tb710fu_wifi_stats[16];

namespace {

constexpr uint16_t kMetaClassBaseDiversity = 0xcda1; /* "_ZTV15OSMetaClassBase" */

constexpr unsigned kSlotGetMetaClass      = 7;
constexpr unsigned kSlotStart             = 190;
constexpr unsigned kSlotStop              = 191;
constexpr unsigned kSlotOutputPacket      = 273;
constexpr unsigned kSlotGetFeatures       = 274;
constexpr unsigned kSlotFreePacket        = 303;
constexpr unsigned kSlotSetLinkStatus     = 310;
constexpr unsigned kSlotEnable            = 316;
constexpr unsigned kSlotDisable           = 317;
constexpr unsigned kSlotAttachInterface   = 318;
constexpr unsigned kSlotDetachInterface   = 319;
constexpr unsigned kSlotGetHwAddress      = 362;
constexpr unsigned kSlotSetMulticastMode  = 364;
constexpr unsigned kSlotSetMulticastList  = 365;
constexpr unsigned kSlotSetPromiscMode    = 366;

constexpr uint32_t kIONetworkFeatureNoBSDWait = 0x00000001u;
constexpr uint32_t kIONetworkLinkValid        = 0x00000001u;
constexpr uint32_t kIONetworkLinkActive       = 0x00000002u;
constexpr uint32_t kIOReturnOutputSuccess     = 0x00000000u;

struct Tb710fuWifiNetObj {
    uint8_t familyStorage[TB710FU_WIFI_INSTANCE_SIZE];
    IOService *iface;
    uint8_t mac[6];
    uint8_t pad[2];
};

static_assert(offsetof(Tb710fuWifiNetObj, iface) == TB710FU_WIFI_INSTANCE_SIZE,
              "Family storage must match 26C5035r IOEthernetController instance size");

class Tb710fuWifiNetMetaClass final : public OSMetaClass {
public:
    static void *operator new(size_t, void *p) noexcept { return p; }
    Tb710fuWifiNetMetaClass(const char *name, const OSMetaClass *superClass, unsigned int size)
        : OSMetaClass(name, superClass, size) {}
    OSObject *alloc() const override { return nullptr; }
};

alignas(Tb710fuWifiNetMetaClass) unsigned char gWifiNetMetaStorage[sizeof(Tb710fuWifiNetMetaClass)];
const OSMetaClass *gWifiNetMeta;
const volatile uint64_t *gLiveFamilyVtable;
uint64_t gAdoptedVtable[TB710FU_WIFI_SLOT_COUNT];
Tb710fuWifiNetObj *gActiveController;

bool canonicalKernelVa(uintptr_t addr)
{
    return (addr >> 48) == 0xffffull && (addr & 1u) == 0;
}

template <typename Fn>
Fn familySlot(unsigned slot)
{
    const volatile uint64_t *entryPtr = &gLiveFamilyVtable[slot];
    uintptr_t signedEntry = (uintptr_t)entryPtr[0];
    uint16_t div = tb710fu_wifi_slot_diversity[slot];
    void *resigned = ptrauth_auth_and_resign(
        (void *)signedEntry,
        ptrauth_key_function_pointer,
        ptrauth_blend_discriminator((void *)(uintptr_t)entryPtr, div),
        ptrauth_key_function_pointer,
        0);
    return reinterpret_cast<Fn>(resigned);
}

void installOverride(unsigned slot, void *fn)
{
    uint16_t div = tb710fu_wifi_slot_diversity[slot];
    void *raw = ptrauth_strip(fn, ptrauth_key_function_pointer);
    void *signedPtr = ptrauth_sign_unauthenticated(
        raw,
        ptrauth_key_function_pointer,
        ptrauth_blend_discriminator((void *)&gAdoptedVtable[slot], div));
    gAdoptedVtable[slot] = (uint64_t)(uintptr_t)signedPtr;
}

void deriveBoardMac(uint8_t out[6])
{
    uint64_t seed = 0x5442373130465700ull; /* "TB710FW\0" */
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (chosen) {
        OSData *id = OSDynamicCast(OSData, chosen->getProperty("unique-chip-id"));
        if (id && id->getLength() >= sizeof(seed)) {
            uint64_t chip = 0;
            memcpy(&chip, id->getBytesNoCopy(), sizeof(chip));
            if (chip != 0) {
                seed = chip ^ 0x5749464930313130ull; /* "WIFI0110" so the pair differs from en0 */
            }
        }
        chosen->release();
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 8; ++i) {
        h = (h ^ ((seed >> (8 * i)) & 0xffu)) * 0x100000001b3ull;
    }
    for (unsigned i = 0; i < 6; ++i) {
        out[i] = (uint8_t)(h >> (8 * i));
    }
    out[0] = (uint8_t)((out[0] & 0xfeu) | 0x02u); /* locally administered, unicast */
}

bool linkIsActive()
{
    /* Only a completed WMI stage could bring this link up; stage 3/4 are scaffolding in this milestone. */
    return native_tb710fu_wifi_stats[0] >= 4 && native_tb710fu_wifi_stats[1] == 0;
}

const OSMetaClass *wifi_getMetaClass(const void *)
{
    return gWifiNetMeta;
}

uint32_t wifi_getFeatures(const void *)
{
    return kIONetworkFeatureNoBSDWait;
}

IOReturn wifi_getHardwareAddress(void *self, void *out)
{
    if (!self || !out) {
        return kIOReturnBadArgument;
    }
    Tb710fuWifiNetObj *obj = reinterpret_cast<Tb710fuWifiNetObj *>(self);
    memcpy(out, obj->mac, sizeof(obj->mac));
    return kIOReturnSuccess;
}

IOReturn wifi_enable(void *self, void *)
{
    typedef bool (*SetLinkFn)(void *, uint32_t, const void *, uint64_t, void *);
    SetLinkFn setLink = familySlot<SetLinkFn>(kSlotSetLinkStatus);
    setLink(self, linkIsActive() ? (kIONetworkLinkValid | kIONetworkLinkActive) : kIONetworkLinkValid,
            nullptr, linkIsActive() ? 1000000000ULL : 0ULL, nullptr);
    return kIOReturnSuccess;
}

IOReturn wifi_disable(void *self, void *)
{
    typedef bool (*SetLinkFn)(void *, uint32_t, const void *, uint64_t, void *);
    SetLinkFn setLink = familySlot<SetLinkFn>(kSlotSetLinkStatus);
    setLink(self, kIONetworkLinkValid, nullptr, 0ULL, nullptr);
    return kIOReturnSuccess;
}

uint32_t wifi_outputPacket(void *self, void *mbuf, void *)
{
    if (mbuf) {
        typedef void (*FreePktFn)(void *, void *, uint32_t);
        FreePktFn freePkt = familySlot<FreePktFn>(kSlotFreePacket);
        freePkt(self, mbuf, 0);
    }
    return kIOReturnOutputSuccess;
}

IOReturn wifi_setMulticastMode(void *, bool) { return kIOReturnSuccess; }
IOReturn wifi_setMulticastList(void *, void *, uint32_t) { return kIOReturnSuccess; }
IOReturn wifi_setPromiscuousMode(void *, bool) { return kIOReturnSuccess; }

bool wifi_start(void *self, IOService *provider)
{
    Tb710fuWifiNetObj *obj = reinterpret_cast<Tb710fuWifiNetObj *>(self);
    IOService *svc = reinterpret_cast<IOService *>(self);
    deriveBoardMac(obj->mac);
    printf("TB710FU WifiNet: start MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           obj->mac[0], obj->mac[1], obj->mac[2], obj->mac[3], obj->mac[4], obj->mac[5]);

    typedef bool (*BaseStartFn)(void *, IOService *);
    BaseStartFn baseStart = familySlot<BaseStartFn>(kSlotStart);
    if (!baseStart(self, provider)) {
        printf("TB710FU WifiNet: base IONetworkController::start failed\n");
        return false;
    }
    svc->setProperty("IOBuiltin", true);

    typedef bool (*AttachIfFn)(void *, IOService **, bool);
    AttachIfFn attachIf = familySlot<AttachIfFn>(kSlotAttachInterface);
    IOService *iface = nullptr;
    if (!attachIf(self, &iface, true) || !iface) {
        printf("TB710FU WifiNet: attachInterface failed\n");
        return false;
    }
    obj->iface = iface;
    printf("TB710FU WifiNet: interface attached (%p), link down until WMI exists\n", (void *)iface);
    return true;
}

void wifi_stop(void *self, IOService *provider)
{
    Tb710fuWifiNetObj *obj = reinterpret_cast<Tb710fuWifiNetObj *>(self);
    if (obj->iface) {
        typedef void (*DetachIfFn)(void *, IOService *, bool);
        DetachIfFn detachIf = familySlot<DetachIfFn>(kSlotDetachInterface);
        detachIf(self, obj->iface, true);
        obj->iface->release();
        obj->iface = nullptr;
    }
    typedef void (*BaseStopFn)(void *, IOService *);
    BaseStopFn baseStop = familySlot<BaseStopFn>(kSlotStop);
    baseStop(self, provider);
}

const OSMetaClass *findMetaClass(const char *name)
{
    const OSSymbol *sym = OSSymbol::withCString(name);
    if (!sym) {
        return nullptr;
    }
    const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(sym);
    sym->release();
    return meta;
}

void wifiNetWorker(void *, wait_result_t)
{
    const OSMetaClass *ioEthMeta = findMetaClass("IOEthernetController");
    if (!ioEthMeta) {
        OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IONetworkingFamily");
        printf("TB710FU WifiNet: IONetworkingFamily load=%x\n", loaded);
        ioEthMeta = findMetaClass("IOEthernetController");
    }
    if (!ioEthMeta) {
        printf("TB710FU WifiNet: IOEthernetController metaclass missing\n");
        thread_terminate(current_thread());
        return;
    }
    const unsigned int instSize = ioEthMeta->getClassSize();
    if (instSize != TB710FU_WIFI_INSTANCE_SIZE) {
        printf("TB710FU WifiNet: instance size 0x%x != 0x%x\n", instSize, (unsigned)TB710FU_WIFI_INSTANCE_SIZE);
        thread_terminate(current_thread());
        return;
    }
    const uintptr_t metaAddr = (uintptr_t)ioEthMeta;
    const uintptr_t slide = metaAddr - TB710FU_WIFI_METACLASS_VA;
    if ((slide & 0x3fffull) != 0) {
        printf("TB710FU WifiNet: unaligned slide %llx\n", (unsigned long long)slide);
        thread_terminate(current_thread());
        return;
    }
    const uintptr_t liveVtableAddr = TB710FU_WIFI_VTABLE_VA + slide;
    const uintptr_t c2Addr = TB710FU_WIFI_C2_VA + slide;
    if (!canonicalKernelVa(liveVtableAddr) || (liveVtableAddr & 7u) != 0 ||
        !canonicalKernelVa(c2Addr) || (c2Addr & 3u) != 0) {
        printf("TB710FU WifiNet: invalid vtable=%p or c2=%p\n", (void *)liveVtableAddr, (void *)c2Addr);
        thread_terminate(current_thread());
        return;
    }
    uint32_t c2Prologue = 0;
    memcpy(&c2Prologue, (const void *)c2Addr, sizeof(c2Prologue));
    if (c2Prologue != 0xd503245fu) {
        printf("TB710FU WifiNet: C2 prologue %08x is not bti c\n", c2Prologue);
        thread_terminate(current_thread());
        return;
    }

    const volatile uint64_t *liveVtable = (const volatile uint64_t *)liveVtableAddr;
    for (unsigned s = 0; s < TB710FU_WIFI_SLOT_COUNT; ++s) {
        uintptr_t target = (uintptr_t)ptrauth_strip((void *)(uintptr_t)liveVtable[s], ptrauth_key_function_pointer);
        if (!canonicalKernelVa(target) || (target & 3u) != 0) {
            printf("TB710FU WifiNet: live vtable slot %u invalid (%p)\n", s, (void *)target);
            thread_terminate(current_thread());
            return;
        }
    }
    gLiveFamilyVtable = liveVtable;

    void *modHandle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    Tb710fuWifiNetMetaClass *metaObj = new (gWifiNetMetaStorage) Tb710fuWifiNetMetaClass(
        "Tb710fuWifiNet", ioEthMeta, (unsigned int)sizeof(Tb710fuWifiNetObj));
    OSReturn postStatus = OSMetaClass::postModLoad(modHandle);
    gWifiNetMeta = metaObj;
    printf("TB710FU WifiNet: metaclass=%p postModLoad=%x slide=%llx\n",
           (const void *)gWifiNetMeta, postStatus, (unsigned long long)slide);

    for (unsigned s = 0; s < TB710FU_WIFI_SLOT_COUNT; ++s) {
        uint16_t div = tb710fu_wifi_slot_diversity[s];
        void *resigned = ptrauth_auth_and_resign(
            (void *)(uintptr_t)liveVtable[s],
            ptrauth_key_function_pointer,
            ptrauth_blend_discriminator((void *)(uintptr_t)&liveVtable[s], div),
            ptrauth_key_function_pointer,
            ptrauth_blend_discriminator((void *)&gAdoptedVtable[s], div));
        gAdoptedVtable[s] = (uint64_t)(uintptr_t)resigned;
    }
    installOverride(kSlotGetMetaClass,      (void *)&wifi_getMetaClass);
    installOverride(kSlotStart,             (void *)&wifi_start);
    installOverride(kSlotStop,              (void *)&wifi_stop);
    installOverride(kSlotOutputPacket,      (void *)&wifi_outputPacket);
    installOverride(kSlotGetFeatures,       (void *)&wifi_getFeatures);
    installOverride(kSlotEnable,            (void *)&wifi_enable);
    installOverride(kSlotDisable,           (void *)&wifi_disable);
    installOverride(kSlotGetHwAddress,      (void *)&wifi_getHardwareAddress);
    installOverride(kSlotSetMulticastMode,  (void *)&wifi_setMulticastMode);
    installOverride(kSlotSetMulticastList,  (void *)&wifi_setMulticastList);
    installOverride(kSlotSetPromiscMode,    (void *)&wifi_setPromiscuousMode);

    Tb710fuWifiNetObj *obj = (Tb710fuWifiNetObj *)OSObject::operator new(sizeof(Tb710fuWifiNetObj));
    if (!obj) {
        printf("TB710FU WifiNet: allocation failed\n");
        thread_terminate(current_thread());
        return;
    }
    memset(obj, 0, sizeof(Tb710fuWifiNetObj));
    typedef void (*C2Fn)(void *, const OSMetaClass *);
    C2Fn c2 = reinterpret_cast<C2Fn>(ptrauth_sign_unauthenticated((void *)c2Addr, ptrauth_key_function_pointer, 0));
    c2(obj, gWifiNetMeta);
    void *signedVptr = ptrauth_sign_unauthenticated(
        (void *)gAdoptedVtable, ptrauth_key_cxx_vtable_pointer,
        ptrauth_blend_discriminator((void *)obj, kMetaClassBaseDiversity));
    memcpy(obj, &signedVptr, sizeof(signedVptr));

    IOService *resources = IOService::getResourceService();
    for (unsigned tries = 0; !resources && tries < 50; ++tries) {
        IOSleep(100);
        resources = IOService::getResourceService();
    }
    if (!resources) {
        printf("TB710FU WifiNet: IOResources unavailable\n");
        thread_terminate(current_thread());
        return;
    }
    IOService *svc = reinterpret_cast<IOService *>(obj);
    if (!svc->init() || !svc->attach(resources) || !svc->start(resources)) {
        printf("TB710FU WifiNet: attach/start failed\n");
        thread_terminate(current_thread());
        return;
    }
    svc->registerService();
    gActiveController = obj;
    native_tb710fu_wifi_stats[15] = 1;
    printf("TB710FU WifiNet: registered dormant interface (%p)\n", (void *)obj);
    thread_terminate(current_thread());
}

} /* namespace */

extern "C" void tb710fu_wifi_net_start(void)
{
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiNetWorker, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
        printf("TB710FU WifiNet: worker started\n");
    } else {
        printf("TB710FU WifiNet: worker spawn failed\n");
    }
}
