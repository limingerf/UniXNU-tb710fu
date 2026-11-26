/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The one macOS class this port derives from, declared with the extent the
 * prelinked IOGraphicsFamily registers (drivers/AdrenoMetal/OriginalAcceleratorABI.h
 * in the framework states the same 136 bytes and no operational overrides).
 *
 * Why a local declaration: the class is a prelinked kext class of 26C5035r, and
 * XnuPrelink re-lays Apple kext text in this fileset, so no original method
 * address survives at a fixture address. This header therefore only states
 * ancestry and instance extent; every method the object answers is either
 * IOService's own (linked from the kernel headers) or this port's override.
 *
 * IOAccelerator adds no virtuals of its own: IOGPU's Metal discovery reads
 * properties off the registry entry and takes the accelerator port, so a
 * subclass that adds only newUserClient and the property reads keeps the base
 * table's shape. That is the property the upstream ABI review records, and
 * scripts/check-tb710fu-gpu-host.py re-checks both statements here. */
#ifndef TB710FU_GPU_ABI_H
#define TB710FU_GPU_ABI_H

#include <IOKit/IOService.h>

class IOAccelerator : public IOService {
    OSDeclareAbstractStructors(IOAccelerator)
public:
    /* The family adds no method slot of its own; nothing is declared here on
     * purpose, so this port can only override IOService's own slots. */
};

/* 26C5035r: IOService is 0x88 bytes (OriginalAESABI.h asserts the same value), and
 * IOAccelerator adds no fields. An instance of this port's subclass is therefore
 * exactly the extent the family's allocator expects, which the port also checks
 * against the live metaclass at boot before registering anything. */
static_assert(sizeof(IOService) == 136, "IOService extent changed");
static_assert(sizeof(IOAccelerator) == 136, "IOAccelerator extent changed");

#endif /* TB710FU_GPU_ABI_H */
