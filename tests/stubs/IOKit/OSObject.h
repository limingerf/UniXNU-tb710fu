/* Stub IOKit header for the Linux host parse of Tb710fuBluetooth.cpp (scripts/check-tb710fu-bt-host.py).
 * It mirrors the declarations this package uses and nothing else. It is NOT an Apple header and it proves
 * only that the translation unit is internally consistent: signatures, members, overloads and blocks.
 * Never compile a kernel object against these; scripts/build-tb710fu-bt.py uses the real XNU records. */
#ifndef STUB_IOKIT_OSOBJECT_H
#define STUB_IOKIT_OSOBJECT_H
#include <stdint.h>
#include <stddef.h>

typedef unsigned int UInt32;
typedef int SInt32;
typedef signed int IOReturn;
typedef uint64_t IOPhysicalAddress;
typedef uint64_t IOByteCount;
typedef uint64_t vm_size_t;
typedef uint64_t mach_vm_size_t;
typedef int task_t;

class OSObject;
class OSNumber;
class OSString;
class OSSymbol;
class OSDictionary;
class OSArray;
class OSMetaClass;
class IOService;
class IOUserClient;

class OSMetaClass {
public:
    OSMetaClass(const char *name, const OSMetaClass *super, unsigned int size);
    static const OSMetaClass *getMetaClassWithName(const OSSymbol *name);
    static const OSMetaClass *getMetaClassWithName(const char *name);
};

class OSObject {
public:
    virtual ~OSObject();
    static OSObject *alloc();
    virtual void retain() const;
    virtual void release() const;
    virtual void free();
    virtual const OSMetaClass *getMetaClass() const;
};

#define OSDeclareDefaultStructors(name)                     \
public:                                                     \
    static OSMetaClass gMetaClass;                          \
    name();                                                 \
    virtual ~name();

#define OSDefineMetaClassAndStructors(name, super)          \
    OSMetaClass name::gMetaClass(#name, 0, 0);              \
    name::name() {}                                         \
    name::~name() {}

#define OSTypeAlloc(type) (new type)

#endif
