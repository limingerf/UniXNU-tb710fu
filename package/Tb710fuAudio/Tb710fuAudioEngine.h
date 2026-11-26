/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The staged bring-up machine for TB710FU audio: one GPR packet in flight at a time, each step
 * validated by the answer that must come back before the next is sent. It never talks to a
 * transport itself (the port's send is the caller's GLINK client), never waits (the caller's
 * thread drives pump/timeout with a deadline), and never touches hardware. That is what makes
 * stage 3 verifiable on a host against a simulated APM, and what lets the driver stop cleanly at
 * any answer it does not understand instead of continuing into a half-open graph.
 *
 * Sequence, playback first and capture if asked for:
 *   SPF state -> open speaker -> open playback stream -> configure speaker -> configure playback
 *   stream -> configure playback shared memory -> prepare speaker -> prepare playback stream ->
 *   start speaker -> start playback stream -> map the region
 *   (capture) open microphone -> open capture stream -> configure microphone -> configure capture
 *   stream -> prepare microphone -> prepare capture stream -> start capture -> start microphone
 * then Running. A failure stops at its step with the status in last_status, and the driver reports
 * the step number: the point of the stages is to know exactly where the board stopped. */
#ifndef TB710FU_AUDIO_ENGINE_H
#define TB710FU_AUDIO_ENGINE_H

#include <stdint.h>

#include "Tb710fuAudioAbi.h"
#include "Tb710fuAudioReach.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_GRAPH_BUFFER_PACKETS 512u   /* how many 4 KiB chunks the mapped region holds */
#define TB710FU_GRAPH_STEP_MS 250u          /* one packet's answer deadline */

typedef struct {
    int (*send)(void *context, const uint8_t *packet, uint32_t bytes);
    void *context;
} Tb710fuAudioPort;

typedef struct {
    Tb710fuFormat speaker;      /* 48000, 32 bits, 4 channels: four TDM slots */
    Tb710fuFormat capture;      /* 48000, 16 bits, 2 channels */
    Tb710fuTdmConfig tdm;
    Tb710fuCaptureConfig mic;
    int open_capture;
    int map_buffer;
    uint64_t buffer_address;
    uint32_t buffer_bytes;
} Tb710fuGraphConfig;

enum {
    Tb710fuGraphSpfProbe = 0,
    Tb710fuGraphSpeakerOpen = 1,
    Tb710fuGraphPlaybackOpen = 2,
    Tb710fuGraphSpeakerCfg = 3,
    Tb710fuGraphPlaybackCfg = 4,
    Tb710fuGraphShmemCfg = 5,
    Tb710fuGraphSpeakerPrepare = 6,
    Tb710fuGraphPlaybackPrepare = 7,
    Tb710fuGraphSpeakerStart = 8,
    Tb710fuGraphPlaybackStart = 9,
    Tb710fuGraphMicOpen = 10,
    Tb710fuGraphCaptureOpen = 11,
    Tb710fuGraphMicCfg = 12,
    Tb710fuGraphCaptureCfg = 13,
    Tb710fuGraphMicPrepare = 14,
    Tb710fuGraphCapturePrepare = 15,
    Tb710fuGraphMicStart = 16,
    Tb710fuGraphCaptureStart = 17,
    Tb710fuGraphMap = 18,
    Tb710fuGraphRunning = 19,
    Tb710fuGraphFailed = 20,
};

typedef struct {
    Tb710fuGpr gpr;
    Tb710fuAudioPort port;
    Tb710fuGraphConfig config;
    uint32_t step;
    uint32_t waiting;         /* a command is in flight */
    uint32_t token;           /* the token of that command */
    uint64_t deadline_us;
    uint64_t buffer_address;
    uint32_t buffer_bytes;
    uint32_t handle;
    uint32_t state;           /* the Tb710fuAudio stage actually reached */
    uint32_t last_error;
    uint32_t last_status;
    uint32_t answers, timeouts, retries, failures, foreign;
    uint32_t write_done, write_started, read_done, read_started;
} Tb710fuAudioGraph;

/* Starts (or restarts) the sequence at the SPF probe. */
void tb710fu_graph_init(Tb710fuAudioGraph *graph, const Tb710fuAudioPort *port, const Tb710fuGraphConfig *config);

/* Sends the next packet when nothing is in flight. Returns 1 when Running, 0 pending, -1 failed. */
int tb710fu_graph_pump(Tb710fuAudioGraph *graph, uint64_t now_us);

/* Feeds one packet from the DSP. Returns 1 when the step it answered succeeded, 0 for an answer
 * that does not belong to the command in flight, -1 when it failed (see last_status). */
int tb710fu_graph_packet(Tb710fuAudioGraph *graph, const uint8_t *data, uint32_t bytes);

/* Called when now_us passes the deadline; retries once, then fails. Returns 1 when it gave up. */
int tb710fu_graph_timeout(Tb710fuAudioGraph *graph, uint64_t now_us);

/* The streaming side: one 4 KiB chunk at buffer_offset. write returns 0 when the command went out,
 * 1 when the endpoint is still busy with the previous chunk, -1 when not running. */
int tb710fu_graph_write(Tb710fuAudioGraph *graph, uint64_t buffer_offset, uint32_t bytes, uint64_t now_us);
int tb710fu_graph_read(Tb710fuAudioGraph *graph, uint64_t buffer_offset, uint32_t bytes, uint64_t now_us);

#ifdef __cplusplus
}
#endif
#endif
