/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The GPU user-client factory and its deferred metaclass installer.
 *
 * The connection class itself stays private to Tb710fuGpuClient.cpp: only the
 * accelerator's newUserClient needs to create one, and only the metaclass window
 * needs the static-init alias. The class is named AGXDeviceUserClient because
 * the original sandbox profiles admit that name (see the .cpp header). */
#ifndef TB710FU_GPU_CLIENT_H
#define TB710FU_GPU_CLIENT_H

#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>

extern "C" {
/* The constructor clang emits for Tb710fuGpuClient.cpp. The build moves it out
 * of __mod_init_func; only tb710fu_gpu_meta_init runs it, inside preModLoad. */
void tb710fu_gpu_client_static_init(void) __asm__("__GLOBAL__sub_I_Tb710fuGpuClient.cpp");

/* A retained, initialised connection (the accelerator hands it to the caller as
 * *handler). Null on allocation or init failure; no printing happens here, the
 * caller runs under the service lock. */
IOUserClient *tb710fu_gpu_client_create(task_t owningTask, void *securityID, UInt32 type,
                                        OSDictionary *properties);
}

#endif /* TB710FU_GPU_CLIENT_H */
