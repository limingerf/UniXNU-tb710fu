// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB710FU board hooks for UniXNU/AppleKeyStoreState (NativeKeyStoreEvents.cpp).
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

extern "C" {
volatile uint32_t tb710fu_aks_opens = 0;
volatile uint32_t tb710fu_aks_refused = 0;
volatile uint32_t tb710fu_aks_calls = 0;
volatile uint32_t tb710fu_aks_last_sel = 0;
volatile int32_t tb710fu_aks_last_rc = 0;

void native_keyevents_open_refused(void)
{
    __atomic_fetch_add(&tb710fu_aks_refused, 1u, __ATOMIC_RELAXED);
}

bool native_aks_state_enabled(void)
{
    unsigned v = 0;
    return PE_parse_boot_argn("aks", &v, sizeof(v)) && v == 1;
}

uint32_t native_aks_lock_state(void)
{
    uint32_t v = 3; /* 3 = no passcode (default for macOS bring-up) */
    (void)PE_parse_boot_argn("aks-sls", &v, sizeof(v));
    return v;
}

void native_aks_call(uint32_t selector, int32_t result)
{
    __atomic_fetch_add(&tb710fu_aks_calls, 1u, __ATOMIC_RELAXED);
    tb710fu_aks_last_sel = selector;
    tb710fu_aks_last_rc = result;
}

void native_aks_opened(void)
{
    __atomic_fetch_add(&tb710fu_aks_opens, 1u, __ATOMIC_RELAXED);
}

size_t native_aks_device_id(uint8_t *out, size_t capacity)
{
    if (!out || !capacity) {
        return 0;
    }
    uint64_t chip = 0;
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        OSData *id = OSDynamicCast(OSData, chosen->getProperty("unique-chip-id"));
        if (id && id->getLength() >= sizeof(chip)) {
            memcpy(&chip, id->getBytesNoCopy(), sizeof(chip));
        }
        chosen->release();
    }
    if (!chip) {
        return 0;
    }
    size_t n = capacity < sizeof(chip) ? capacity : sizeof(chip);
    memcpy(out, &chip, n);
    return n;
}
}
