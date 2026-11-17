// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc,
// package/SourceRootPolicies.cpp; see Tb710fuSourceStorage/README.md.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_kern_xnu.h>
#include <mach/mach_vm.h>
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"
#include "Tb710fuSourceStorage/RootPolicyBundle.h"

#if !defined(NATIVE_VERIFIED_POLICY_HANDOFF) || !NATIVE_VERIFIED_POLICY_HANDOFF
#error SourceRootPolicies requires the authenticated policy handoff interface
#endif

extern "C" {
extern const uint8_t verified_root_key[];
extern const struct vr_crypto vr_xnu_crypto;
NativeVerifiedBlockPolicy verified_root_policies[tb710fu::kRootPolicyLimit];
uint64_t verified_root_policy_count;
}

namespace {

// A failed/reentrant attempt cannot overwrite the published pointers. The
// startup caller runs synchronously before starting NativeUfs; consumers still
// require that ordering (NativeUfs reads the existing ABI globals directly).
uint32_t loading;

struct LoadingAttempt {
    ~LoadingAttempt() { __atomic_store_n(&loading, 0, __ATOMIC_RELEASE); }
};

struct PolicyCopy {
    vm_offset_t address = 0;
    vm_size_t size = 0;
    ~PolicyCopy()
    {
        if (address) kmem_free(kernel_map, address, size);
    }
};

bool authenticatePolicy(const uint8_t *bytes, size_t length, bool headerOnly, void *)
{
    struct vr_view view = {};
    return (headerOnly ? vr_open_header(bytes, length, verified_root_key, 1, &vr_xnu_crypto, &view) :
                         vr_open(bytes, length, verified_root_key, 1, &vr_xnu_crypto, &view)) == VR_OK;
}

} // namespace

extern "C" bool native_source_root_policies_load(void)
{
    if (__atomic_load_n(&verified_root_policy_count, __ATOMIC_ACQUIRE)) return true;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&loading, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return __atomic_load_n(&verified_root_policy_count, __ATOMIC_ACQUIRE) != 0;
    LoadingAttempt attempt;
    // A concurrent successful caller may have published before we acquired the
    // attempt. Its read-only allocation remains owned by the published table.
    if (__atomic_load_n(&verified_root_policy_count, __ATOMIC_ACQUIRE)) return true;
    IORegistryEntry *entry = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!entry) return false;
    OSData *property = OSDynamicCast(OSData, entry->getProperty("RootPolicies"));
    uint64_t physical = 0, length = 0;
    if (property && property->getLength() == 16) {
        const auto *data = static_cast<const uint8_t *>(property->getBytesNoCopy());
        physical = tb710fu::rootPolicyRead64(data);
        length = tb710fu::rootPolicyRead64(data + 8);
    }
    entry->release();
    if (!tb710fu::rootPolicyHandoffRange(physical, length)) return false;
    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(physical, length, kIODirectionIn);
    if (!descriptor) return false;
    IOMemoryMap *mapping = descriptor->map();
    if (!mapping) { descriptor->release(); return false; }
    PolicyCopy copy;
    copy.size = round_page(static_cast<vm_size_t>(length));
    const kern_return_t allocated = kmem_alloc(kernel_map, &copy.address, copy.size,
        static_cast<kma_flags_t>(KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY);
    const bool mapped = mapping->getVirtualAddress() && mapping->getLength() >= length;
    if (allocated == KERN_SUCCESS && mapped)
        memcpy(reinterpret_cast<void *>(copy.address), reinterpret_cast<const void *>(mapping->getVirtualAddress()), length);
    mapping->release();
    descriptor->release();
    if (allocated != KERN_SUCCESS || !mapped) return false;
    // Remove write permission (including maximum permission) before parsing or
    // authenticating the private copy. The firmware mapping is no longer used.
    if (mach_vm_protect(kernel_map, copy.address, copy.size, TRUE, VM_PROT_READ) != KERN_SUCCESS) return false;
    tb710fu::RootPolicyBundle bundle = {};
    if (!tb710fu::openRootPolicyBundle(reinterpret_cast<const uint8_t *>(copy.address),
                                     static_cast<size_t>(length), authenticatePolicy, nullptr, &bundle)) return false;
    for (size_t i = 0; i < bundle.count; ++i)
        verified_root_policies[i] = {bundle.policies[i].manifest, bundle.policies[i].manifestBytes};
    // Successful policies own this allocation for the rest of the boot. No
    // failure path has touched either global, and no success path frees it.
    copy.address = 0;
    __atomic_store_n(&verified_root_policy_count, static_cast<uint64_t>(bundle.count), __ATOMIC_RELEASE);
    return true;
}
