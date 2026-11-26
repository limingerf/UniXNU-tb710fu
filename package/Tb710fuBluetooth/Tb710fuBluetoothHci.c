/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuBluetoothHci.h"

uint32_t tb_bt_h4_header_len(uint8_t type)
{
    switch (type) {
    case TB_BT_H4_CMD: return 3u; /* opcode:16, plen:8 */
    case TB_BT_H4_ACL: return 4u; /* handle+flags:16, dlen:16 */
    case TB_BT_H4_SCO: return 3u; /* handle+flags:16, dlen:8 */
    case TB_BT_H4_EVT: return 2u; /* event code, then plen */
    case TB_BT_H4_ISO: return 4u; /* handle+flags:16, dlen:16 */
    default: return 0u;
    }
}

uint32_t tb_bt_h4_packet_len(uint8_t type, const uint8_t *header)
{
    if (!header) return 0u;
    switch (type) {
    case TB_BT_H4_CMD: return 3u + header[2];
    case TB_BT_H4_ACL: return 4u + header[2] + ((uint32_t)header[3] << 8);
    case TB_BT_H4_SCO: return 3u + header[2];
    case TB_BT_H4_EVT: return 2u + header[1];
    case TB_BT_H4_ISO: return 4u + header[2] + ((uint32_t)header[3] << 8);
    default: return 0u;
    }
}

uint32_t tb_bt_hci_command_packet(uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *out,
                                  uint32_t cap)
{
    if (!out || cap < 4u + (uint32_t)plen || (!params && plen)) return 0u;
    out[0] = TB_BT_H4_CMD;
    out[1] = (uint8_t)(opcode & 0xffu);
    out[2] = (uint8_t)((opcode >> 8) & 0xffu);
    out[3] = plen;
    for (uint32_t i = 0; i < plen; ++i) out[4 + i] = params[i];
    return 4u + plen;
}

bool tb_bt_hci_parse_cmd_complete(const uint8_t *body, uint32_t len, uint16_t *opcode, uint8_t *status,
                                  const uint8_t **params, uint32_t *params_len)
{
    if (!body || len < 6u || body[0] != TB_BT_EVT_CMD_COMPLETE) return false;
    const uint32_t nparams = body[1];
    /* body = evt, plen, then nparams bytes: ncmd, opcode (2), status, return parameters after status. */
    if (nparams < 4u || nparams + 2u > len) return false;
    const uint16_t op = (uint16_t)body[3] | ((uint16_t)body[4] << 8);
    if (opcode) *opcode = op;
    if (status) *status = body[5];
    if (params) *params = body + 6;
    if (params_len) *params_len = nparams - 4u;
    return true;
}

bool tb_bt_hci_parse_cmd_status(const uint8_t *body, uint32_t len, uint16_t *opcode, uint8_t *status)
{
    if (!body || len < 6u || body[0] != TB_BT_EVT_CMD_STATUS) return false;
    const uint32_t nparams = body[1];
    if (nparams < 4u || nparams + 2u > len) return false;
    if (opcode) *opcode = (uint16_t)body[4] | ((uint16_t)body[5] << 8);
    if (status) *status = body[2];
    return true;
}

void tb_bt_h4_parser_init(TbBtH4Parser *p, TbBtH4Sink sink, void *ctx)
{
    if (!p) return;
    p->sink = sink;
    p->sink_ctx = ctx;
    tb_bt_h4_parser_reset(p);
    p->packets = 0u;
    p->drops = 0u;
}

void tb_bt_h4_parser_reset(TbBtH4Parser *p)
{
    if (!p) return;
    p->type = 0u;
    p->header_len = 0u;
    p->body_need = 0u;
    p->body_have = 0u;
    p->in_packet = false;
}

int tb_bt_h4_parser_feed(TbBtH4Parser *p, const uint8_t *bytes, uint32_t len)
{
    if (!p || (!bytes && len)) return 0;
    int complete = 0;
    for (uint32_t i = 0; i < len; ++i) {
        const uint8_t b = bytes[i];
        if (!p->in_packet) {
            const uint32_t header = tb_bt_h4_header_len(b);
            if (!header) {
                p->drops++; /* not an H4 type byte: skip it, the next byte may be one */
                continue;
            }
            p->type = b;
            p->header_len = header;
            p->body_need = 0u;
            p->body_have = 0u;
            p->in_packet = true;
            continue;
        }
        if (p->body_have >= TB_BT_H4_MAX_PACKET) { /* cannot happen: body_need is checked below */
            p->drops++;
            tb_bt_h4_parser_reset(p);
            continue;
        }
        p->body[p->body_have++] = b;
        if (!p->body_need) {
            if (p->body_have < p->header_len) continue;
            const uint32_t total = tb_bt_h4_packet_len(p->type, p->body);
            if (total < p->header_len || total > TB_BT_H4_MAX_PACKET) {
                p->drops++;
                tb_bt_h4_parser_reset(p);
                continue;
            }
            p->body_need = total;
        }
        if (p->body_have == p->body_need) {
            p->packets++;
            complete++;
            if (p->sink) p->sink(p->sink_ctx, p->type, p->body, p->body_need);
            tb_bt_h4_parser_reset(p);
        }
    }
    return complete;
}
