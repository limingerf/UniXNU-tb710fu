/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The one dependency the battery has outside this package: a PMIC GLINK channel to the audio DSP. The ADSP
 * transport (firmware load, SMP2P, doorbells, GLINK FIFOs, QRTR) is a separate subsystem; when it is built it
 * registers this provider by defining these three functions. The battery code itself never talks to hardware.
 *
 * Contract, copied in shape from UniXNU-sheng's ShengAdsp channel (hardware-verified on SM8550): the transport
 * calls "state" with up=1 once the channel named here is open on the other side, up=0 when it goes away, and
 * "receive" for every message that arrives on it; "send" queues one message of at most the mtu asked for. Both
 * callbacks may run on the transport's thread, so the battery only copies into a ring and signals its work loop. */
#ifndef TB710FU_POWER_LINK_H
#define TB710FU_POWER_LINK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Tb710fuPowerLinkState)(void *context, int up);
typedef void (*Tb710fuPowerLinkReceive)(void *context, const uint8_t *data, uint32_t length);

typedef struct {
    Tb710fuPowerLinkState state;
    Tb710fuPowerLinkReceive receive;
} Tb710fuPowerLinkClient;

typedef struct Tb710fuPowerLink Tb710fuPowerLink;

/* Opens (or joins) the named channel. Returns null while no transport is built, or when the DSP has not opened it.
 * mtu is the largest message either side sends; priority is the GLINK intent priority. */
Tb710fuPowerLink *tb710fu_power_link_acquire(const char *channel, uint32_t mtu, uint32_t priority,
                                            const Tb710fuPowerLinkClient *client, void *context);
/* Queues one message; 0 on success, non-zero when the channel is down or the FIFO is full (the caller's timer skips
 * the field and retries on the next cycle). */
int tb710fu_power_link_send(Tb710fuPowerLink *link, const void *data, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
