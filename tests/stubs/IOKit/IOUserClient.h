/* Stub: see IOKit/OSObject.h. */
#ifndef STUB_IOKIT_IOUSERCLIENT_H
#define STUB_IOKIT_IOUSERCLIENT_H
#include "IOKit/IOService.h"

typedef IOReturn (*IOExternalMethodAction)(OSObject *target, void *reference,
                                           struct IOExternalMethodArguments *arguments);

struct IOExternalMethodArguments {
    uint32_t selector;
    const void *structureInput;
    uint64_t structureInputSize;
    void *structureOutput;
    uint64_t structureOutputSize;
    const uint64_t *scalarInput;
    uint32_t scalarInputCount;
    uint64_t *scalarOutput;
    uint32_t scalarOutputCount;
};

struct IOExternalMethodDispatch {
    IOExternalMethodAction action;
    uint32_t checkScalarInputCount;
    uint32_t checkStructureInputSize;
    uint32_t checkScalarOutputCount;
    uint32_t checkStructureOutputSize;
};

enum {
    kIOReturnSuccess = 0,
    kIOReturnError = -1,
    kIOReturnNoMemory = -2,
    kIOReturnUnsupported = -3,
    kIOReturnBadArgument = -4,
    kIOReturnNotReady = -5,
    kIOReturnNoSpace = -6,
    kIOReturnIOError = -7,
    kIOReturnTimeout = -8,
};

class IOUserClient : public IOService {
public:
    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type);
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    virtual IOReturn clientClose();
    virtual IOReturn clientDied();
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                    IOExternalMethodDispatch *dispatch = 0, OSObject *target = 0,
                                    void *reference = 0);
};

#endif
