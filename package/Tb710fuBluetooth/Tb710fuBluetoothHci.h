/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* HCI H4 framing over UART (Bluetooth Core spec Vol 4 Part A, "HCI UART Transport Layer"), plus the
 * Command Complete / Command Status decoding this driver relies on. Pure C, no IOKit.
 *
 * Byte layouts used below, from the Core spec:
 *   H4 command  = 01 | opcode:16 | plen:8 | params
 *   H4 event    = 04 | evt:8 | plen:8 | params
 *   Command Complete params = ncmd:1 | opcode:16 | return parameters (status first for most commands)
 *   Command Status params   = status:1 | ncmd:1 | opcode:16
 * The sink receives the packet body, i.e. everything after the H4 type byte (for an event that is the
 * event code, its length, and the parameters), so the parsers work on the same bytes the wire carried. */
#ifndef TB710FU_BLUETOOTH_HCI_H
#define TB710FU_BLUETOOTH_HCI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_BT_H4_CMD 0x01u
#define TB_BT_H4_ACL 0x02u
#define TB_BT_H4_SCO 0x03u
#define TB_BT_H4_EVT 0x04u
#define TB_BT_H4_ISO 0x05u

#define TB_BT_EVT_CMD_COMPLETE 0x0eu
#define TB_BT_EVT_CMD_STATUS 0x0fu
#define TB_BT_EVT_DISCONN_COMPLETE 0x05u
#define TB_BT_EVT_HW_ERROR 0x10u
#define TB_BT_EVT_VENDOR 0xffu
#define TB_BT_OP_RESET 0x0c03u
#define TB_BT_OP_READ_BD_ADDR 0x1009u

#define TB_BT_H4_MAX_PACKET 1024u

/* Bytes that follow the type byte before the payload starts. */
uint32_t tb_bt_h4_header_len(uint8_t type);
/* Total bytes after the type byte for a packet whose header has been read (header inclusive). */
uint32_t tb_bt_h4_packet_len(uint8_t type, const uint8_t *header);
/* Builds a complete H4 command packet into out; returns the packet length or 0 on argument error. */
uint32_t tb_bt_hci_command_packet(uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *out,
                                  uint32_t cap);
/* From a Command Complete body: opcode, status (first return parameter), and the parameters after it.
 * Returns false when the body is not a well-formed Command Complete. */
bool tb_bt_hci_parse_cmd_complete(const uint8_t *body, uint32_t len, uint16_t *opcode, uint8_t *status,
                                  const uint8_t **params, uint32_t *params_len);
/* Same for Command Status (no return parameters beyond the status). */
bool tb_bt_hci_parse_cmd_status(const uint8_t *body, uint32_t len, uint16_t *opcode, uint8_t *status);

typedef void (*TbBtH4Sink)(void *ctx, uint8_t type, const uint8_t *body, uint32_t len);

typedef struct {
    TbBtH4Sink sink;
    void *sink_ctx;
    uint8_t type;
    uint8_t body[TB_BT_H4_MAX_PACKET];
    uint32_t header_len;
    uint32_t body_need; /* total body length once the header is complete, 0 before that */
    uint32_t body_have;
    bool in_packet;
    uint32_t packets;
    uint32_t drops; /* unknown type bytes and packets larger than TB_BT_H4_MAX_PACKET */
} TbBtH4Parser;

void tb_bt_h4_parser_init(TbBtH4Parser *p, TbBtH4Sink sink, void *ctx);
void tb_bt_h4_parser_reset(TbBtH4Parser *p);
/* Feeds bytes; returns the number of complete packets handed to the sink. Rejected input is counted in
 * p->drops and the parser resynchronises on the next type byte. */
int tb_bt_h4_parser_feed(TbBtH4Parser *p, const uint8_t *bytes, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* TB710FU_BLUETOOTH_HCI_H */
