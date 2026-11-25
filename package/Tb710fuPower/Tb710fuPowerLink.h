/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The one dependency the battery has outside this package: a PMIC GLINK channel to the audio DSP. The ADSP
 * transport (firmware load, SMP2P, doorbells, GLINK FIFOs) is a separate subsystem (package/Tb710fuAdsp);
 * when it is built and started it provides these two functions. The battery code itself never talks to hardware.
 *
 * Contract, copied in shape from UniXNU-sheng's ShengAdsp channel (hardware-verified on SM8550): "acquire"
 * registers the channel with the transport and returns at once, whether or not the DSP has opened it yet;
 * the transport then calls "state" with up=1 once the channel is open on the other side and up=0 when it goes
 * away, and "receive" for every message that arrives on it; "send" queues one message of at most intent_size
 * bytes. Both callbacks run on the transport's thread, so the battery only copies into a ring and signals its
 * work loop. A null return means no transport is running (the adsp boot argument was not given) or its
 * channel table is full; the DSP opening the channel later is reported through "state", not by a null pointer. */
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

/* Registers the named channel now. intent_size is the largest message either side sends and intent_count the
 * number of receive buffers the transport advertises for it. */
Tb710fuPowerLink *tb710fu_power_link_acquire(const char *channel, uint32_t intent_size, uint32_t intent_count,
                                            const Tb710fuPowerLinkClient *client, void *context);
/* Queues one message; 0 on success, non-zero when the channel is down or the queue is full (the caller's timer
 * skips the field and retries on the next cycle). */
int tb710fu_power_link_send(Tb710fuPowerLink *link, const void *data, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
