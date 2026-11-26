/* Stub: see IOKit/OSObject.h. */
#ifndef STUB_IOKIT_IODEVICEMEMORY_H
#define STUB_IOKIT_IODEVICEMEMORY_H
#include "IOKit/OSObject.h"
#include "IOKit/IOMemoryDescriptor.h"

class IOMemoryMap : public OSObject {
public:
    void *getVirtualAddress();
    IOByteCount getLength();
};

class IODeviceMemory : public IOMemoryDescriptor {
public:
    static IODeviceMemory *withRange(IOPhysicalAddress start, IOByteCount length);
    IOMemoryMap *map(unsigned int options = 0);
};

enum {
    kIOMapAnywhere = 0x00000001,
    kIOMapInhibitCache = 0x00000004,
};

#endif
