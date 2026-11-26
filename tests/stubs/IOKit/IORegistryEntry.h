/* Stub: see IOKit/OSObject.h. */
#ifndef STUB_IOKIT_IOREGISTRYENTRY_H
#define STUB_IOKIT_IOREGISTRYENTRY_H
#include "IOKit/OSObject.h"
#include <stdbool.h>

class IORegistryEntry : public OSObject {
public:
    virtual bool init();
    virtual void free();
    virtual bool setProperty(const char *key, const char *value);
    virtual bool setProperty(const char *key, unsigned long long value, unsigned int width = 32);
    virtual bool setProperty(const char *key, bool value);
};

#endif
