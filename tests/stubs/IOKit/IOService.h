/* Stub: see IOKit/OSObject.h. */
#ifndef STUB_IOKIT_IOSERVICE_H
#define STUB_IOKIT_IOSERVICE_H
#include "IOKit/IORegistryEntry.h"

class IOUserClient;

class IOService : public IORegistryEntry {
public:
    virtual bool init();
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    virtual bool attach(IOService *provider);
    virtual void detach(IOService *provider);
    virtual void registerService();
    virtual bool terminate(unsigned int options = 0);
    virtual bool isInactive();
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler);
    static IOService *getResourceService();
    static IOService *getPlatform();
};

#endif
