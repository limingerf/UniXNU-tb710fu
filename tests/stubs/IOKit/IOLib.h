/* Stub: see IOKit/OSObject.h. */
#ifndef STUB_IOKIT_IOLIB_H
#define STUB_IOKIT_IOLIB_H
#include "IOKit/OSObject.h"

class IOLock;

IOLock *IOLockAlloc();
void IOLockFree(IOLock *lock);
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);

void IODelay(unsigned int microseconds);
void IOSleep(unsigned int milliseconds);

void *IOMallocData(size_t size);
void IOFreeData(void *address, size_t size);

#endif
