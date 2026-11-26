/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The audio DSP's packet router (GPR) as it runs on the "adsp_apps" GLINK channel
 * (sm8650.dtsi: glink-edge/gpr, compatible "qcom,gpr", qcom,glink-channels = "adsp_apps",
 * qcom,domain = <GPR_DOMAIN_ID_ADSP>, with the q6apm service at module 1 and q6prm at 2).
 *
 * Every packet starts with the same 24-byte header: version 3:0 and header size in words 7:4
 * of word 0, total packet size in bytes 31:8 of word 0, destination domain in bits 7:0 and
 * source domain in bits 15:8 of word 1, then source port, destination port, token and opcode as
 * 32-bit words; then the payload. Header size is six words, so the payload starts at byte 24.
 *
 * Ports are module instance ids: the APM answers on 1 (GPR_APM_MODULE_IID in the device tree's
 * gpr node), the DSP's power/resource manager on 2 (GPR_PRM_MODULE_IID). A client talks to the
 * APM on port 1 and receives its answers on the port it sent from.
 *
 * Domain numbers: the stock firmware and the sheng stack behind this package use MODEM 1,
 * ADSP 2, APPS 3 (UniXNU-sheng ShengGpr.h, hardware-verified on SM8550 against the same QDSP6
 * audio firmware). The device tree header that normally names them is not part of the DTS
 * snapshot this port reads, so the driver takes these two from boot arguments as well
 * (audio-gpr-dst / audio-gpr-src) and reports the pair it used at stage 3.
 *
 * Provenance: the encoding, the version/header checks, the basic-result shape and the
 * one-query-in-flight rule are UniXNU-sheng's ShengGpr.c (BSD-2-Clause-Patent). This module
 * adds the routing helpers the TB710FU engine needs (header patching, address matching) and
 * keeps the same no-I/O, no-wait contract: the owner sends the bytes and feeds what arrives. */
#ifndef TB710FU_GPR_H
#define TB710FU_GPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_GPR_HEADER_BYTES 24u
#define TB710FU_GPR_VERSION 0u
#define TB710FU_GPR_DOMAIN_MODEM 1u
#define TB710FU_GPR_DOMAIN_ADSP 2u
#define TB710FU_GPR_DOMAIN_APPS 3u
#define TB710FU_GPR_PORT_APM 1u
#define TB710FU_GPR_PORT_PRM 2u
#define TB710FU_GPR_PORT_CLIENT 1u
#define TB710FU_GPR_MAX_PACKET 512u
#define TB710FU_GPR_OP_BASIC_RESULT 0x02001005u
#define TB710FU_GPR_OP_APM_GET_SPF_STATE 0x01001021u
#define TB710FU_GPR_OP_APM_SPF_STATE 0x02001007u   /* payload {status}: 1 ready */

typedef struct {
    uint32_t version, header_bytes, size;
    uint32_t dst_domain, src_domain, src_port, dst_port, token, opcode;
    const uint8_t *payload;
    uint32_t payload_bytes;
} Tb710fuGprPacket;

enum {
    Tb710fuGprOk = 0,
    Tb710fuGprShort = -1,
    Tb710fuGprBadVersion = -2,
    Tb710fuGprBadHeader = -3,
    Tb710fuGprBadSize = -4,
};

typedef struct {
    uint32_t src_domain, dst_domain, port;
    uint32_t next_token;
    uint32_t sent, received, foreign, malformed;
} Tb710fuGpr;

void tb710fu_gpr_init(Tb710fuGpr *gpr, uint32_t src_domain, uint32_t dst_domain, uint32_t port);

/* Writes the 24-byte header of a packet whose total size is total_bytes. */
void tb710fu_gpr_header(uint8_t *out, uint32_t total_bytes, uint32_t dst_domain, uint32_t src_domain,
                        uint32_t src_port, uint32_t dst_port, uint32_t token, uint32_t opcode);
/* Rewrites word 0's size field once a builder knows how long its packet turned out. */
void tb710fu_gpr_patch_size(uint8_t *out, uint32_t total_bytes);

/* Encodes a whole packet (header plus payload) into out; returns its size, or 0 when it does not
 * fit TB710FU_GPR_MAX_PACKET or capacity. *token receives the token used. */
uint32_t tb710fu_gpr_encode(Tb710fuGpr *gpr, uint8_t *out, uint32_t capacity, uint32_t dst_port,
                            uint32_t opcode, const void *payload, uint32_t payload_bytes,
                            uint32_t *token);

int tb710fu_gpr_decode(const uint8_t *in, uint32_t length, Tb710fuGprPacket *packet);

/* The {opcode, status} pair of a basic result; 0 when the packet is one. */
int tb710fu_gpr_basic_result(const Tb710fuGprPacket *packet, uint32_t *opcode, uint32_t *status);

/* True when the packet is addressed to a client on port in our_domain and came from elsewhere. */
int tb710fu_gpr_is_answer(const Tb710fuGprPacket *packet, uint32_t port, uint32_t our_domain);

#ifdef __cplusplus
}
#endif
#endif
