/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Entry points exported by Tb710fuInputProbe.cpp for the boot-time service
 * list in package/Macos27Services.cpp. Everything else in this directory is
 * plain C and is meant to be called only from those three functions.
 */
#ifndef TB710FU_INPUT_H
#define TB710FU_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reads tb710fu-touch / tb710fu-keys (and the tuning args) from the boot
 * arguments and starts the subsystems that are switched on. Returns 0 in every
 * case: this subsystem reports by log line, never by failing the boot. */
int tb710fu_input_start(void);

/* Terminal stop: both worker threads end with thread_terminate(), so a stopped
 * subsystem cannot be restarted in the same boot. */
void tb710fu_input_stop(void);

/* Optional: validate the stock novatek_nt36532_fw.bin packed by the merge as an
 * addon blob. Never uploads anything to the controller. Returns 0 when the
 * image parses and the DLM checksum matches. */
int tb710fu_input_firmware_check(const void *image, unsigned bytes);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_INPUT_H */
