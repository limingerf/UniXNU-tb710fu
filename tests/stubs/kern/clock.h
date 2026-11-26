/* Stub: see IOKit/OSObject.h. */
#ifndef STUB_KERN_CLOCK_H
#define STUB_KERN_CLOCK_H
#include <stdint.h>

uint64_t mach_absolute_time();
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);

#endif
