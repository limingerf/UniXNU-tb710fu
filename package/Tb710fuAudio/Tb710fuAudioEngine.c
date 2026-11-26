/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuAudioEngine.h"

#include <stdbool.h>
#include <string.h>

static uint32_t step_of(const Tb710fuAudioGraph *g) {return g->step;}

/* The step the sequence goes to after this one succeeds. */
static uint32_t next_step(const Tb710fuAudioGraph *g, uint32_t step)
{
    switch (step) {
    case Tb710fuGraphSpfProbe: return Tb710fuGraphSpeakerOpen;
    case Tb710fuGraphSpeakerOpen: return Tb710fuGraphPlaybackOpen;
    case Tb710fuGraphPlaybackOpen: return Tb710fuGraphSpeakerCfg;
    case Tb710fuGraphSpeakerCfg: return Tb710fuGraphPlaybackCfg;
    case Tb710fuGraphPlaybackCfg: return Tb710fuGraphShmemCfg;
    case Tb710fuGraphShmemCfg: return Tb710fuGraphSpeakerPrepare;
    case Tb710fuGraphSpeakerPrepare: return Tb710fuGraphPlaybackPrepare;
    case Tb710fuGraphPlaybackPrepare: return Tb710fuGraphSpeakerStart;
    case Tb710fuGraphSpeakerStart: return Tb710fuGraphPlaybackStart;
    case Tb710fuGraphPlaybackStart:
        if (g->config.open_capture) return Tb710fuGraphMicOpen;
        return g->config.map_buffer ? Tb710fuGraphMap : Tb710fuGraphRunning;
    case Tb710fuGraphMicOpen: return Tb710fuGraphCaptureOpen;
    case Tb710fuGraphCaptureOpen: return Tb710fuGraphMicCfg;
    case Tb710fuGraphMicCfg: return Tb710fuGraphCaptureCfg;
    case Tb710fuGraphCaptureCfg: return Tb710fuGraphMicPrepare;
    case Tb710fuGraphMicPrepare: return Tb710fuGraphCapturePrepare;
    case Tb710fuGraphCapturePrepare: return Tb710fuGraphMicStart;
    case Tb710fuGraphMicStart: return Tb710fuGraphCaptureStart;
    case Tb710fuGraphCaptureStart:
        return g->config.map_buffer ? Tb710fuGraphMap : Tb710fuGraphRunning;
    case Tb710fuGraphMap: return Tb710fuGraphRunning;
    default: return Tb710fuGraphFailed;
    }
}

/* What the step asks the APM for; also what a valid answer must be. */
static uint32_t step_opcode(uint32_t step, uint32_t *out_dst)
{
    if (out_dst) *out_dst = TB710FU_GPR_PORT_APM;
    switch (step) {
    case Tb710fuGraphSpfProbe: return TB710FU_GPR_OP_APM_GET_SPF_STATE;
    case Tb710fuGraphSpeakerOpen: return TB710FU_AR_OP_GRAPH_OPEN;
    case Tb710fuGraphPlaybackOpen: return TB710FU_AR_OP_GRAPH_OPEN;
    case Tb710fuGraphSpeakerCfg: return TB710FU_AR_OP_SET_CFG;
    case Tb710fuGraphPlaybackCfg: return TB710FU_AR_OP_SET_CFG;
    case Tb710fuGraphShmemCfg: return TB710FU_AR_OP_SET_CFG;
    case Tb710fuGraphSpeakerPrepare: return TB710FU_AR_OP_GRAPH_PREPARE;
    case Tb710fuGraphPlaybackPrepare: return TB710FU_AR_OP_GRAPH_PREPARE;
    case Tb710fuGraphSpeakerStart: return TB710FU_AR_OP_GRAPH_START;
    case Tb710fuGraphPlaybackStart: return TB710FU_AR_OP_GRAPH_START;
    case Tb710fuGraphMicOpen: return TB710FU_AR_OP_GRAPH_OPEN;
    case Tb710fuGraphCaptureOpen: return TB710FU_AR_OP_GRAPH_OPEN;
    case Tb710fuGraphMicCfg: return TB710FU_AR_OP_SET_CFG;
    case Tb710fuGraphCaptureCfg: return TB710FU_AR_OP_SET_CFG;
    case Tb710fuGraphMicPrepare: return TB710FU_AR_OP_GRAPH_PREPARE;
    case Tb710fuGraphCapturePrepare: return TB710FU_AR_OP_GRAPH_PREPARE;
    case Tb710fuGraphMicStart: return TB710FU_AR_OP_GRAPH_START;
    case Tb710fuGraphCaptureStart: return TB710FU_AR_OP_GRAPH_START;
    case Tb710fuGraphMap: return TB710FU_AR_OP_MEM_MAP;
    default: return 0;
    }
}

/* A subgraph command carries the subgraph id; the SET_CFG of the shared-memory endpoint goes to
 * the endpoint's own port, which is why dst is an output here. */
static uint32_t step_subgraph(uint32_t step)
{
    switch (step) {
    case Tb710fuGraphSpeakerPrepare:
    case Tb710fuGraphSpeakerStart: return TB710FU_AR_SG_SPEAKER;
    case Tb710fuGraphPlaybackPrepare:
    case Tb710fuGraphPlaybackStart: return TB710FU_AR_SG_PLAY;
    case Tb710fuGraphMicPrepare:
    case Tb710fuGraphMicStart: return TB710FU_AR_SG_MIC;
    case Tb710fuGraphCapturePrepare:
    case Tb710fuGraphCaptureStart: return TB710FU_AR_SG_CAPTURE;
    default: return 0;
    }
}

void tb710fu_graph_init(Tb710fuAudioGraph *graph, const Tb710fuAudioPort *port, const Tb710fuGraphConfig *config)
{
    if (!graph) return;
    memset(graph, 0, sizeof *graph);
    if (port) graph->port = *port;
    if (config) graph->config = *config;
    graph->step = Tb710fuGraphSpfProbe;
    graph->state = Tb710fuAudioGraphOpen;
    graph->buffer_address = graph->config.buffer_address;
    graph->buffer_bytes = graph->config.buffer_bytes;
}

static int emit(Tb710fuAudioGraph *g, uint64_t now_us, uint8_t *packet)
{
    uint32_t dst = TB710FU_GPR_PORT_APM;
    const uint32_t opcode = step_opcode(g->step, &dst);
    uint32_t token = 0;
    uint32_t size;

    switch (g->step) {
    case Tb710fuGraphSpfProbe: {
        /* The probe has no payload: an empty APM command header. */
        uint8_t payload[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        size = tb710fu_gpr_encode(&g->gpr, packet, TB710FU_GPR_MAX_PACKET, dst, opcode, payload, sizeof payload,
                                  &token);
        break;
    }
    case Tb710fuGraphSpeakerOpen: size = tb710fu_ar_open_speaker(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0); break;
    case Tb710fuGraphPlaybackOpen: size = tb710fu_ar_open_playback(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0); break;
    case Tb710fuGraphSpeakerCfg:
        size = tb710fu_ar_config_speaker(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0, &g->config.speaker,
                                         &g->config.tdm);
        break;
    case Tb710fuGraphPlaybackCfg:
        size = tb710fu_ar_config_playback(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0, &g->config.speaker);
        break;
    case Tb710fuGraphShmemCfg:
        size = tb710fu_ar_config_playback_shmem(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0, &g->config.speaker);
        break;
    case Tb710fuGraphSpeakerPrepare:
    case Tb710fuGraphPlaybackPrepare:
    case Tb710fuGraphSpeakerStart:
    case Tb710fuGraphPlaybackStart:
    case Tb710fuGraphMicPrepare:
    case Tb710fuGraphCapturePrepare:
    case Tb710fuGraphMicStart:
    case Tb710fuGraphCaptureStart:
        size = tb710fu_ar_graph_command(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0, opcode, step_subgraph(g->step));
        break;
    case Tb710fuGraphMicOpen: size = tb710fu_ar_open_microphone(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0); break;
    case Tb710fuGraphCaptureOpen: size = tb710fu_ar_open_capture(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0); break;
    case Tb710fuGraphMicCfg:
        size = tb710fu_ar_config_microphone(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0, &g->config.capture,
                                            &g->config.mic);
        break;
    case Tb710fuGraphCaptureCfg:
        size = tb710fu_ar_config_capture(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0, &g->config.capture);
        break;
    case Tb710fuGraphMap:
        size = tb710fu_ar_map(packet, TB710FU_GPR_MAX_PACKET, &g->gpr, 0, g->buffer_address, g->buffer_bytes);
        break;
    default:
        return -1;
    }
    if (!size) return -1;

    /* Every builder above wrote token 0; the graph owns the real token so answers can be matched,
     * and rewrites the header's token field in place. */
    token = g->gpr.next_token + 1u;
    g->gpr.next_token = token;
    g->token = token;
    packet[16] = (uint8_t)token;
    packet[17] = (uint8_t)(token >> 8);
    packet[18] = (uint8_t)(token >> 16);
    packet[19] = (uint8_t)(token >> 24);
    g->gpr.sent++;
    /* Arm before the send: a completion that arrives while the transport is still inside send()
     * (or before it returns on another core) must find the sequence waiting for exactly this token. */
    g->waiting = 1;
    g->deadline_us = now_us + (uint64_t)TB710FU_GRAPH_STEP_MS * 1000u;
    if (!g->port.send || g->port.send(g->port.context, packet, size)) {
        g->waiting = 0;
        return -1;
    }
    return 0;
}

int tb710fu_graph_pump(Tb710fuAudioGraph *graph, uint64_t now_us)
{
    if (!graph) return -1;
    if (graph->step == Tb710fuGraphRunning) return 1;
    if (graph->step == Tb710fuGraphFailed) return -1;
    if (graph->waiting) return 0;
    uint8_t packet[TB710FU_GPR_MAX_PACKET];
    if (emit(graph, now_us, packet)) {
        graph->last_error = Tb710fuAudioErrorGraph;
        graph->step = Tb710fuGraphFailed;
        graph->failures++;
        return -1;
    }
    return 0;
}

int tb710fu_graph_packet(Tb710fuAudioGraph *graph, const uint8_t *data, uint32_t bytes)
{
    if (!graph || !data) return 0;
    Tb710fuArAnswer answer;
    const int kind = tb710fu_ar_parse(data, bytes, &answer);
    if (kind < 0) {
        graph->foreign++;
        return 0;
    }
    graph->gpr.received++;
    if (answer.token != graph->token || !graph->waiting) {
        /* A data completion from an older chunk, or another client's traffic. */
        if (kind == Tb710fuArWriteDone) { graph->write_done++; return 0; }
        if (kind == Tb710fuArReadDone) { graph->read_done++; return 0; }
        graph->foreign++;
        return 0;
    }

    uint32_t status = 0;
    int ok = 0;
    switch (kind) {
    case Tb710fuArBasicResult:
        status = answer.status;
        ok = status == 0;
        break;
    case Tb710fuArFrameworkState:
        /* The APM answers 1 when AudioReach is up and 0 while it is still loading. */
        status = answer.status;
        ok = status == 1;
        break;
    case Tb710fuArMapped:
        graph->handle = answer.handle;
        ok = answer.handle != 0;
        break;
    case Tb710fuArWriteDone:
        graph->write_done++;
        ok = answer.status == 0;
        break;
    case Tb710fuArReadDone:
        graph->read_done++;
        ok = answer.status == 0;
        break;
    default:
        graph->foreign++;
        return 0;
    }
    graph->answers++;
    graph->last_status = status;
    if (!ok) {
        graph->waiting = 0;
        graph->last_error = Tb710fuAudioErrorGraph;
        graph->failures++;
        graph->step = Tb710fuGraphFailed;
        return -1;
    }
    graph->waiting = 0;
    /* Once the sequence is done, only data completions come back: they free the endpoint, they
     * do not advance the sequence. */
    if (graph->step == Tb710fuGraphRunning) return 1;
    const uint32_t from = step_of(graph);
    graph->step = next_step(graph, from);
    if (graph->step == Tb710fuGraphRunning)
        graph->state = graph->config.map_buffer ? Tb710fuAudioStreaming : Tb710fuAudioGraphOpen;
    return 1;
}

int tb710fu_graph_timeout(Tb710fuAudioGraph *graph, uint64_t now_us)
{
    if (!graph || !graph->waiting || now_us < graph->deadline_us) return 0;
    if (graph->retries < 2) {
        graph->retries++;
        graph->timeouts++;
        graph->waiting = 0;
        graph->token = 0;   /* the late answer must not be taken for the retry's */
        graph->deadline_us = now_us + (uint64_t)TB710FU_GRAPH_STEP_MS * 1000u;
        return 0;
    }
    graph->timeouts++;
    graph->waiting = 0;
    graph->last_error = Tb710fuAudioErrorTimeout;
    graph->failures++;
    graph->step = Tb710fuGraphFailed;
    return 1;
}

int tb710fu_graph_write(Tb710fuAudioGraph *graph, uint64_t buffer_offset, uint32_t bytes, uint64_t now_us)
{
    if (!graph || graph->step != Tb710fuGraphRunning) return -1;
    if (!graph->handle) return -1;
    uint8_t packet[TB710FU_GPR_MAX_PACKET];
    uint32_t token = 0;
    const uint32_t size = tb710fu_ar_write(packet, TB710FU_GPR_MAX_PACKET, &graph->gpr, 0,
                                           graph->buffer_address + buffer_offset, graph->handle, bytes);
    if (!size) return -1;
    token = graph->gpr.next_token + 1u;
    graph->gpr.next_token = token;
    graph->token = token;
    packet[16] = (uint8_t)token;
    packet[17] = (uint8_t)(token >> 8);
    packet[18] = (uint8_t)(token >> 16);
    packet[19] = (uint8_t)(token >> 24);
    graph->write_started++;
    graph->waiting = 1;
    graph->deadline_us = now_us + (uint64_t)TB710FU_GRAPH_STEP_MS * 1000u;
    if (!graph->port.send || graph->port.send(graph->port.context, packet, size)) {
        graph->waiting = 0;
        return -1;
    }
    return 0;
}

int tb710fu_graph_read(Tb710fuAudioGraph *graph, uint64_t buffer_offset, uint32_t bytes, uint64_t now_us)
{
    if (!graph || graph->step != Tb710fuGraphRunning) return -1;
    if (!graph->handle) return -1;
    uint8_t packet[TB710FU_GPR_MAX_PACKET];
    const uint32_t size = tb710fu_ar_read(packet, TB710FU_GPR_MAX_PACKET, &graph->gpr, 0,
                                          graph->buffer_address + buffer_offset, graph->handle, bytes);
    if (!size) return -1;
    const uint32_t token = graph->gpr.next_token + 1u;
    graph->gpr.next_token = token;
    graph->token = token;
    packet[16] = (uint8_t)token;
    packet[17] = (uint8_t)(token >> 8);
    packet[18] = (uint8_t)(token >> 16);
    packet[19] = (uint8_t)(token >> 24);
    graph->read_started++;
    graph->waiting = 1;
    graph->deadline_us = now_us + (uint64_t)TB710FU_GRAPH_STEP_MS * 1000u;
    if (!graph->port.send || graph->port.send(graph->port.context, packet, size)) {
        graph->waiting = 0;
        return -1;
    }
    return 0;
}
