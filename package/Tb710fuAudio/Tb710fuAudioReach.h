/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The audio DSP's graph manager (APM) commands for TB710FU's speaker and microphone paths, as GPR
 * packets. Two graphs per direction: a device graph that owns the hardware endpoint and a stream
 * graph that owns the shared-memory endpoint; the stream graph's modules connect into the device
 * graph's endpoint.
 *
 * TB710FU's endpoints, from sm8650-lenovo-tb710fu.dts:
 *  - speakers: the "Secondary TDM0 Playback" link, cpu sound-dai = q6apmbedai SECONDARY_TDM_RX_0,
 *    dai-tdm-slot-num 4, dai-tdm-slot-width 32, and four Awinic AW88461 codecs each taking one
 *    slot (dai-tdm-slot-rx-mask <1 1 1 1>). The APM side therefore opens the I2S/TDM sink module
 *    (0x0700100A) on the secondary interface with a four-slot, 32-bit frame, and the stream is
 *    four channels (the four amps are the four slots).
 *  - microphones: the "WCD Capture" link, cpu sound-dai = q6apmbedai TX_CODEC_DMA_TX_3, codec =
 *    wcd939x 1 / swr2 / lpass_txmacro (the WCD9395's DMIC inputs on SWR2, per the board DTS's
 *    TX port map: "DMIC0,1,2,3 & MBHC <=> SWR2 Port 3"). The APM side opens the codec-DMA source
 *    module (0x07001024) on TX3, exactly as the sheng stack does for its TX3 capture.
 *
 * All ids below are the sheng stack's, which run against the same QDSP6 AudioReach firmware:
 * subgraph/container/module/parameter ids as UniXNU-sheng ShengAudioReach.c/.h
 * (BSD-2-Clause-Patent) verified on SM8550. The only TB710FU-specific values are the TDM frame
 * shape (4 slots of 32 bits, from the DTS), the codec-DMA channel mask, and the optional TDM
 * parameter packet, which is new here and is marked unverified: it is only sent when
 * cfg->send_tdm_parameter is set, because the stock file's frame shape alone normally decides
 * the TDM format on the APM side.
 *
 * Pure encoding: no I/O, no waits. Every builder patches the GPR size field itself and returns 0
 * when the packet does not fit. */
#ifndef TB710FU_AUDIOREACH_H
#define TB710FU_AUDIOREACH_H

#include <stdint.h>

#include "Tb710fuGpr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_AR_APM_IID 1u
#define TB710FU_AR_OP_GRAPH_OPEN 0x01001000u
#define TB710FU_AR_OP_GRAPH_PREPARE 0x01001001u
#define TB710FU_AR_OP_GRAPH_START 0x01001002u
#define TB710FU_AR_OP_GRAPH_STOP 0x01001003u
#define TB710FU_AR_OP_GRAPH_CLOSE 0x01001004u
#define TB710FU_AR_OP_SET_CFG 0x01001006u
#define TB710FU_AR_OP_MEM_MAP 0x0100100Cu
#define TB710FU_AR_OP_MEM_UNMAP 0x0100100Du
#define TB710FU_AR_OP_MEM_MAP_DONE 0x02001001u   /* payload {handle} */
#define TB710FU_AR_OP_WRITE 0x0400100Au          /* DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 */
#define TB710FU_AR_OP_WRITE_DONE 0x05001004u
#define TB710FU_AR_OP_READ 0x0400100Bu           /* DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2 */
#define TB710FU_AR_OP_READ_DONE 0x05001005u

/* Modules and the instance ids the topology gives them. */
#define TB710FU_AR_MOD_WR_SHMEM 0x07001000u
#define TB710FU_AR_MOD_RD_SHMEM 0x07001001u
#define TB710FU_AR_MOD_PCM_CNV 0x07001003u
#define TB710FU_AR_MOD_PCM_ENC 0x07001004u
#define TB710FU_AR_MOD_PCM_DEC 0x07001005u
#define TB710FU_AR_MOD_I2S_SINK 0x0700100Au
#define TB710FU_AR_MOD_CODEC_DMA_SOURCE 0x07001024u

#define TB710FU_AR_SG_PLAY 0x4001u
#define TB710FU_AR_SG_SPEAKER 0x4005u
#define TB710FU_AR_SG_CAPTURE 0x4003u
#define TB710FU_AR_SG_MIC 0x4007u
#define TB710FU_AR_IID_PLAY_SHMEM 0x6001u
#define TB710FU_AR_IID_PLAY_DEC 0x6002u
#define TB710FU_AR_IID_PLAY_CNV 0x6003u
#define TB710FU_AR_IID_SPEAKER_I2S 0x6051u
#define TB710FU_AR_IID_MIC_DMA 0x6070u
#define TB710FU_AR_IID_CAP_CNV 0x6022u
#define TB710FU_AR_IID_CAP_ENC 0x6023u
#define TB710FU_AR_IID_CAP_SHMEM 0x6024u

typedef struct {
    uint16_t rate;      /* Hz */
    uint16_t bits;      /* 16 or 32 */
    uint16_t channels;
} Tb710fuFormat;

/* The speaker endpoint's format on the wire and the three interface selectors the APM's I2S
 * module takes. Defaults come from the DTS link (secondary TDM, 4 slots of 32 bits); the driver
 * lets boot arguments override them so a hardware run can correct the selectors without a
 * rebuild. */
typedef struct {
    uint32_t lpaif;         /* 0: LPAIF 0 (sheng's hardware-verified value) */
    uint32_t interface;     /* 1: secondary, for SECONDARY_TDM_RX_0 */
    uint32_t sd_line;       /* 0: TDM0's data line */
    uint32_t ws_source;     /* 1: internal */
    uint32_t slots;         /* 4: dai-tdm-slot-num */
    uint32_t slot_bits;     /* 32: dai-tdm-slot-width */
    uint32_t channels;      /* 4: one per AW88461 */
    int send_tdm_parameter; /* 1: also send the (unverified) TDM parameter packet */
} Tb710fuTdmConfig;

/* The capture endpoint: codec DMA TX3 (the board's WCD Capture link). codec_dma_index is the
 * q6 codec-DMA interface number, 4 for TX_CODEC_DMA_TX_3 in the sheng numbering. */
typedef struct {
    uint32_t lpaif;             /* 1: LPAIF RXTX */
    uint32_t codec_dma_index;   /* 4: TX_CODEC_DMA_TX_3 */
    uint32_t channel_mask;      /* 0x3: the first two DMIC channels */
} Tb710fuCaptureConfig;

/* Speaker device graph: one I2S/TDM sink module. */
uint32_t tb710fu_ar_open_speaker(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token);
uint32_t tb710fu_ar_config_speaker(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                                   const Tb710fuFormat *format, const Tb710fuTdmConfig *tdm);

/* Playback stream graph: shared-memory writer, PCM decoder, PCM converter; the converter
 * connects into the speaker device graph's sink. */
uint32_t tb710fu_ar_open_playback(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token);
uint32_t tb710fu_ar_config_playback(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                                    const Tb710fuFormat *format);
/* The shared-memory endpoint's input media format; the destination port is the module instance. */
uint32_t tb710fu_ar_config_playback_shmem(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr,
                                          uint32_t token, const Tb710fuFormat *format);

/* Microphone device graph: one codec-DMA source module on TX3. */
uint32_t tb710fu_ar_open_microphone(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token);
uint32_t tb710fu_ar_config_microphone(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                                      const Tb710fuFormat *format, const Tb710fuCaptureConfig *capture);

/* Capture stream graph: converter, encoder, shared-memory reader. */
uint32_t tb710fu_ar_open_capture(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token);
uint32_t tb710fu_ar_config_capture(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                                   const Tb710fuFormat *format);

/* PREPARE / START / STOP / CLOSE of one subgraph. */
uint32_t tb710fu_ar_graph_command(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                                  uint32_t opcode, uint32_t subgraph);

/* Maps a physically contiguous region (rounded up to 4 KiB); the answer is MEM_MAP_DONE with a
 * handle. */
uint32_t tb710fu_ar_map(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                        uint64_t address, uint32_t bytes);
uint32_t tb710fu_ar_unmap(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                          uint32_t handle);
/* One buffer for the playback shared-memory endpoint; the answer is WRITE_DONE with the token. */
uint32_t tb710fu_ar_write(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                          uint64_t address, uint32_t handle, uint32_t bytes);
/* One empty buffer for the capture shared-memory endpoint; the answer is READ_DONE. */
uint32_t tb710fu_ar_read(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                         uint64_t address, uint32_t handle, uint32_t bytes);

/* The DSP's power/resource manager (PRM, port 2): an LPASS clock and a hardware block vote. */
#define TB710FU_AR_OP_PRM_REQUEST 0x0100100Fu
#define TB710FU_AR_OP_PRM_RELEASE 0x01001010u
#define TB710FU_AR_OP_PRM_REQUEST_DONE 0x02001002u
#define TB710FU_AR_OP_PRM_RELEASE_DONE 0x02001003u
#define TB710FU_AR_CLK_TX_CORE_MCLK 0x30Cu
#define TB710FU_AR_HW_MACRO 1u
#define TB710FU_AR_HW_DCODEC 2u
uint32_t tb710fu_ar_prm_clock(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                              uint32_t clock_id, uint32_t hz);
uint32_t tb710fu_ar_prm_vote(uint8_t *out, uint32_t capacity, const Tb710fuGpr *gpr, uint32_t token,
                             uint32_t block, int enable);

/* The answer shapes the engine validates. */
typedef struct {
    uint32_t opcode;
    uint32_t token;
    uint32_t src_port;
    uint32_t result_opcode;
    uint32_t status;      /* basic result status, framework state, memory handle, data status */
    uint32_t handle;
    uint64_t address;
    uint32_t data_bytes;
    uint32_t offset;
} Tb710fuArAnswer;

enum {
    Tb710fuArUnknown = 0,
    Tb710fuArBasicResult = 1,
    Tb710fuArMapped = 2,
    Tb710fuArWriteDone = 3,
    Tb710fuArReadDone = 4,
    Tb710fuArFrameworkState = 5,
    Tb710fuArPrmDone = 6,
    Tb710fuArBad = -1,
};

int tb710fu_ar_parse(const uint8_t *in, uint32_t length, Tb710fuArAnswer *answer);

#ifdef __cplusplus
}
#endif
#endif
