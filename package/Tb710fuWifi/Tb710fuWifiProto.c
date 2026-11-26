/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuWifiProto.h"

static void put16(uint8_t *out, uint16_t v) { out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)(v >> 16); out[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *in) { return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8)); }
static uint32_t get32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

/* ---- QRTR ---- */

int tb710fu_wifi_qrtr_decode(const uint8_t *packet, uint32_t bytes, struct tb710fu_wifi_qrtr *header)
{
    if (!packet || !header || bytes < TB710FU_WIFI_QRTR_HDR_BYTES) return 0;
    header->version = get32(packet + 0);
    header->type = get32(packet + 4);
    header->src_node = get32(packet + 8);
    header->src_port = get32(packet + 12);
    header->dst_node = get32(packet + 16);
    header->dst_port = get32(packet + 20);
    header->confirm_rx = get32(packet + 24);
    header->size = get32(packet + 28);
    if (header->version != TB710FU_WIFI_QRTR_VERSION) return 0;
    if (header->type < TB710FU_WIFI_QRTR_HELLO || header->type > TB710FU_WIFI_QRTR_DEL_CLIENT) return 0;
    if (header->size != bytes - TB710FU_WIFI_QRTR_HDR_BYTES) return 0;
    return 1;
}

uint32_t tb710fu_wifi_qrtr_encode(const struct tb710fu_wifi_qrtr *header, const uint8_t *payload,
                                  uint8_t *out, uint32_t capacity)
{
    if (!header || !out) return 0;
    const uint32_t total = TB710FU_WIFI_QRTR_HDR_BYTES + header->size;
    if (capacity < total) return 0;
    put32(out + 0, TB710FU_WIFI_QRTR_VERSION);
    put32(out + 4, header->type);
    put32(out + 8, header->src_node);
    put32(out + 12, header->src_port);
    put32(out + 16, header->dst_node);
    put32(out + 20, header->dst_port);
    put32(out + 24, header->confirm_rx);
    put32(out + 28, header->size);
    for (uint32_t i = 0; i < header->size; ++i) out[TB710FU_WIFI_QRTR_HDR_BYTES + i] = payload ? payload[i] : 0;
    return total;
}

/* ---- QMI ---- */

int tb710fu_wifi_qmi_decode(const uint8_t *packet, uint32_t bytes, struct tb710fu_wifi_qmi *header)
{
    if (!packet || !header || bytes < TB710FU_WIFI_QMI_HDR_BYTES) return 0;
    header->type = packet[0];
    header->transaction = packet[1];
    header->message_id = get16(packet + 2);
    header->message_len = get16(packet + 4);
    if (header->type != TB710FU_WIFI_QMI_REQUEST && header->type != TB710FU_WIFI_QMI_RESPONSE &&
        header->type != TB710FU_WIFI_QMI_INDICATION) return 0;
    if (header->message_len != bytes - TB710FU_WIFI_QMI_HDR_BYTES) return 0;
    return 1;
}

uint32_t tb710fu_wifi_qmi_encode(const struct tb710fu_wifi_qmi *header, const uint8_t *body,
                                 uint8_t *out, uint32_t capacity)
{
    if (!header || !out) return 0;
    const uint32_t total = TB710FU_WIFI_QMI_HDR_BYTES + header->message_len;
    if (capacity < total) return 0;
    out[0] = header->type;
    out[1] = header->transaction;
    put16(out + 2, header->message_id);
    put16(out + 4, header->message_len);
    for (uint32_t i = 0; i < header->message_len; ++i) out[TB710FU_WIFI_QMI_HDR_BYTES + i] = body ? body[i] : 0;
    return total;
}

uint32_t tb710fu_wifi_qmi_tlv_encode(uint8_t type, uint16_t length, const uint8_t *value,
                                     uint8_t *out, uint32_t capacity)
{
    if (!out || (length && !value)) return 0;
    const uint32_t total = TB710FU_WIFI_QMI_TLV_BYTES + length;
    if (capacity < total) return 0;
    out[0] = type;
    put16(out + 1, length);
    for (uint32_t i = 0; i < length; ++i) out[3 + i] = value[i];
    return total;
}

int tb710fu_wifi_qmi_tlv_next(const uint8_t *body, uint32_t bytes, uint32_t *offset,
                              struct tb710fu_wifi_qmi_tlv *tlv)
{
    if (!body || !offset || !tlv) return 0;
    if (*offset >= bytes) return 0;
    if (bytes - *offset < TB710FU_WIFI_QMI_TLV_BYTES) return 0;
    tlv->type = body[*offset];
    tlv->length = get16(body + *offset + 1);
    if (tlv->length > bytes - *offset - TB710FU_WIFI_QMI_TLV_BYTES) return 0;
    tlv->value = body + *offset + TB710FU_WIFI_QMI_TLV_BYTES;
    *offset += TB710FU_WIFI_QMI_TLV_BYTES + tlv->length;
    return 1;
}

/* ---- HTC ---- */

int tb710fu_wifi_htc_decode(const uint8_t *frame, uint32_t bytes, struct tb710fu_wifi_htc_frame *header)
{
    if (!frame || !header || bytes < TB710FU_WIFI_HTC_HDR_BYTES) return 0;
    const uint32_t word0 = get32(frame);
    header->endpoint = word0 & 0xffu;
    header->flags = (word0 >> 8) & 0xffu;
    header->payload_len = (word0 >> 16) & 0xffffu;
    if (header->payload_len > TB710FU_WIFI_HTC_MAX_PAYLOAD) return 0;
    if (bytes - TB710FU_WIFI_HTC_HDR_BYTES < header->payload_len) return 0;
    return 1;
}

uint32_t tb710fu_wifi_htc_encode(const struct tb710fu_wifi_htc_frame *header, const uint8_t *payload,
                                 uint8_t *out, uint32_t capacity)
{
    if (!header || !out) return 0;
    if (header->endpoint > 0xffu || header->flags > 0xffu || header->payload_len > 0xffffu) return 0;
    const uint32_t total = TB710FU_WIFI_HTC_HDR_BYTES + header->payload_len;
    if (capacity < total) return 0;
    put32(out, (header->endpoint & 0xffu) | ((header->flags & 0xffu) << 8) | ((header->payload_len & 0xffffu) << 16));
    put32(out + 4, 0);
    for (uint32_t i = 0; i < header->payload_len; ++i) out[TB710FU_WIFI_HTC_HDR_BYTES + i] = payload ? payload[i] : 0;
    return total;
}

uint64_t tb710fu_wifi_htc_connect_words(uint32_t service_id, uint32_t flags, uint32_t meta_len)
{
    const uint32_t word0 = ((service_id & 0xffffu) << 16) | TB710FU_WIFI_HTC_MSG_CONNECT_SERVICE;
    const uint32_t word1 = ((meta_len & 0xffu) << 16) | (flags & 0xffffu);
    return (uint64_t)word0 | ((uint64_t)word1 << 32);
}

int tb710fu_wifi_htc_ready_words(uint64_t words, struct tb710fu_wifi_htc_message *message)
{
    if (!message) return 0;
    const uint32_t word0 = (uint32_t)words;
    const uint32_t word1 = (uint32_t)(words >> 32);
    if ((word0 & 0xffffu) != TB710FU_WIFI_HTC_MSG_READY) return 0;
    message->message_id = TB710FU_WIFI_HTC_MSG_READY;
    message->credits = word0 >> 16;
    message->credit_size = word1 & 0xffffu;
    message->max_endpoints = (word1 >> 16) & 0xffu;
    return 1;
}

int tb710fu_wifi_htc_connect_response(uint64_t words, int32_t eid,
                                      struct tb710fu_wifi_htc_message *message)
{
    if (!message || eid < 0) return 0;
    const uint32_t word0 = (uint32_t)words;
    const uint32_t word1 = (uint32_t)(words >> 32);
    if ((word0 & 0xffffu) != TB710FU_WIFI_HTC_MSG_CONNECT_SERVICE_RESP) return 0;
    message->message_id = TB710FU_WIFI_HTC_MSG_CONNECT_SERVICE_RESP;
    message->service_id = word0 >> 16;
    message->status = word1 & 0xffu;
    message->max_message_len = word1 >> 16;
    return 1;
}

/* ---- WMI ---- */

uint32_t tb710fu_wifi_wmi_encode_cmd(uint32_t cmd_id, const uint8_t *body, uint32_t length,
                                     uint8_t *out, uint32_t capacity)
{
    if (!out || (length && !body)) return 0;
    const uint32_t total = TB710FU_WIFI_WMI_HDR_BYTES + length;
    if (capacity < total) return 0;
    put32(out, cmd_id & TB710FU_WIFI_WMI_CMD_ID_MASK);
    for (uint32_t i = 0; i < length; ++i) out[TB710FU_WIFI_WMI_HDR_BYTES + i] = body[i];
    return total;
}

int tb710fu_wifi_wmi_decode_cmd(const uint8_t *packet, uint32_t bytes, uint32_t *cmd_id, uint32_t *body_offset)
{
    if (!packet || !cmd_id || !body_offset || bytes < TB710FU_WIFI_WMI_HDR_BYTES) return 0;
    *cmd_id = get32(packet) & TB710FU_WIFI_WMI_CMD_ID_MASK;
    *body_offset = TB710FU_WIFI_WMI_HDR_BYTES;
    return 1;
}

uint32_t tb710fu_wifi_wmi_tlv(uint32_t tag, uint32_t length)
{
    return (length & TB710FU_WIFI_WMI_TLV_LEN_MASK) | ((tag & 0xffffu) << TB710FU_WIFI_WMI_TLV_TAG_SHIFT);
}

/* ---- DP rings ---- */

int tb710fu_wifi_dp_plan_make(uint32_t tx_elems, uint32_t rx_elems, uint32_t elem_bytes,
                              struct tb710fu_wifi_dp_plan *plan)
{
    if (!plan || !tx_elems || !rx_elems || !elem_bytes || (elem_bytes & (elem_bytes - 1u)) != 0 ||
        elem_bytes < 16u || elem_bytes > 1024u) return 0;
    plan->tx_elems = tx_elems;
    plan->rx_elems = rx_elems;
    plan->elem_bytes = elem_bytes;
    plan->tx_bytes = tx_elems * elem_bytes;
    plan->rx_bytes = rx_elems * elem_bytes;
    return 1;
}

uint32_t tb710fu_wifi_dp_next(uint32_t index, uint32_t elems)
{
    if (!elems) return 0;
    ++index;
    return index >= elems ? 0u : index;
}
