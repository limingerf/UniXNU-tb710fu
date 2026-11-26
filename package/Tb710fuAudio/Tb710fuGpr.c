/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuGpr.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void tb710fu_gpr_header(uint8_t *out, uint32_t total_bytes, uint32_t dst_domain, uint32_t src_domain,
                        uint32_t src_port, uint32_t dst_port, uint32_t token, uint32_t opcode)
{
    if (!out) return;
    /* word 0: version 3:0, header size in words 7:4, packet size in bytes 31:8 */
    put32(out, (TB710FU_GPR_VERSION & 0xfu) | (TB710FU_GPR_HEADER_BYTES / 4) << 4 | total_bytes << 8);
    put32(out + 4, (dst_domain & 0xffu) | (src_domain & 0xffu) << 8);
    put32(out + 8, src_port);
    put32(out + 12, dst_port);
    put32(out + 16, token);
    put32(out + 20, opcode);
}

void tb710fu_gpr_patch_size(uint8_t *out, uint32_t total_bytes)
{
    if (!out) return;
    put32(out, (TB710FU_GPR_VERSION & 0xfu) | (TB710FU_GPR_HEADER_BYTES / 4) << 4 | total_bytes << 8);
}

void tb710fu_gpr_init(Tb710fuGpr *gpr, uint32_t src_domain, uint32_t dst_domain, uint32_t port)
{
    if (!gpr) return;
    memset(gpr, 0, sizeof *gpr);
    gpr->src_domain = src_domain;
    gpr->dst_domain = dst_domain;
    gpr->port = port;
}

uint32_t tb710fu_gpr_encode(Tb710fuGpr *gpr, uint8_t *out, uint32_t capacity, uint32_t dst_port,
                            uint32_t opcode, const void *payload, uint32_t payload_bytes,
                            uint32_t *token)
{
    if (!gpr || !out || (!payload && payload_bytes)) return 0;
    const uint32_t total = TB710FU_GPR_HEADER_BYTES + payload_bytes;
    if (total > TB710FU_GPR_MAX_PACKET || total > capacity) return 0;
    const uint32_t used = gpr->next_token + 1u;
    gpr->next_token = used;
    tb710fu_gpr_header(out, total, gpr->dst_domain, gpr->src_domain, gpr->port, dst_port, used, opcode);
    if (payload_bytes) memcpy(out + TB710FU_GPR_HEADER_BYTES, payload, payload_bytes);
    if (token) *token = used;
    gpr->sent++;
    return total;
}

int tb710fu_gpr_decode(const uint8_t *in, uint32_t length, Tb710fuGprPacket *packet)
{
    if (!in || !packet || length < TB710FU_GPR_HEADER_BYTES) return Tb710fuGprShort;
    const uint32_t w0 = le32(in), w1 = le32(in + 4);
    packet->version = w0 & 0xfu;
    packet->header_bytes = ((w0 >> 4) & 0xfu) * 4u;
    packet->size = w0 >> 8;
    if (packet->version != TB710FU_GPR_VERSION) return Tb710fuGprBadVersion;
    if (packet->header_bytes < TB710FU_GPR_HEADER_BYTES) return Tb710fuGprBadHeader;
    if (packet->size < packet->header_bytes || packet->size > length) return Tb710fuGprBadSize;
    packet->dst_domain = w1 & 0xffu;
    packet->src_domain = (w1 >> 8) & 0xffu;
    packet->src_port = le32(in + 8);
    packet->dst_port = le32(in + 12);
    packet->token = le32(in + 16);
    packet->opcode = le32(in + 20);
    packet->payload = in + packet->header_bytes;
    packet->payload_bytes = packet->size - packet->header_bytes;
    return Tb710fuGprOk;
}

int tb710fu_gpr_basic_result(const Tb710fuGprPacket *packet, uint32_t *opcode, uint32_t *status)
{
    if (!packet || packet->opcode != TB710FU_GPR_OP_BASIC_RESULT ||
        packet->payload_bytes < 8 || !packet->payload)
        return -1;
    if (opcode) *opcode = le32(packet->payload);
    if (status) *status = le32(packet->payload + 4);
    return 0;
}

int tb710fu_gpr_is_answer(const Tb710fuGprPacket *packet, uint32_t port, uint32_t our_domain)
{
    /* An answer addressed to us: our domain, our port, and a sender that is not us. */
    return packet && packet->dst_domain == our_domain && packet->dst_port == port &&
           packet->src_domain != our_domain;
}
