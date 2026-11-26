/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Control-path codecs for the WCN7850 stack, all little-endian and bounds checked:
 *   QRTR v1    net/qrtr/qrtr.h: 32-byte header, type 1..9
 *   QMI       6-byte header (type, transaction, message id, length) and 3-byte TLVs
 *   HTC       ath12k htc.h: 8-byte frame header (endpoint, flags, payload length) + message ids 1..8
 *   WMI       ath12k wmi.h: 4-byte wmi_cmd_hdr (cmd id in bits 23:0) and 4-byte TLVs (len 15:0, tag 31:16)
 * plus the DP ring geometry the data path will use. The descriptors themselves are ath12k hal descriptors and are
 * deliberately left to the DP milestone: only counts/offsets that can be proven are encoded here. */
#ifndef TB710FU_WIFI_PROTO_H
#define TB710FU_WIFI_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- QRTR ---- */
#define TB710FU_WIFI_QRTR_HDR_BYTES 32u
#define TB710FU_WIFI_QRTR_VERSION   1u

enum {
    TB710FU_WIFI_QRTR_HELLO = 1,
    TB710FU_WIFI_QRTR_BYE = 2,
    TB710FU_WIFI_QRTR_NEW_SERVER = 3,
    TB710FU_WIFI_QRTR_DEL_SERVER = 4,
    TB710FU_WIFI_QRTR_DATA = 5,
    TB710FU_WIFI_QRTR_NEW_LOOKUP = 6,
    TB710FU_WIFI_QRTR_DEL_LOOKUP = 7,
    TB710FU_WIFI_QRTR_RESUME_TX = 8,
    TB710FU_WIFI_QRTR_DEL_CLIENT = 9
};

struct tb710fu_wifi_qrtr {
    uint32_t version, type;
    uint32_t src_node, src_port, dst_node, dst_port;
    uint32_t confirm_rx, size;
};

int tb710fu_wifi_qrtr_decode(const uint8_t *packet, uint32_t bytes, struct tb710fu_wifi_qrtr *header);
uint32_t tb710fu_wifi_qrtr_encode(const struct tb710fu_wifi_qrtr *header, const uint8_t *payload,
                                  uint8_t *out, uint32_t capacity);

/* ---- QMI ---- */
#define TB710FU_WIFI_QMI_HDR_BYTES 6u
#define TB710FU_WIFI_QMI_TLV_BYTES 3u
#define TB710FU_WIFI_QMI_MAX_TLV 4096u

enum {
    TB710FU_WIFI_QMI_REQUEST = 0,
    TB710FU_WIFI_QMI_RESPONSE = 2,
    TB710FU_WIFI_QMI_INDICATION = 4
};

struct tb710fu_wifi_qmi {
    uint8_t type, transaction;
    uint16_t message_id, message_len;
};

struct tb710fu_wifi_qmi_tlv {
    uint8_t type;
    uint16_t length;
    const uint8_t *value;
};

int tb710fu_wifi_qmi_decode(const uint8_t *packet, uint32_t bytes, struct tb710fu_wifi_qmi *header);
uint32_t tb710fu_wifi_qmi_encode(const struct tb710fu_wifi_qmi *header, const uint8_t *body,
                                 uint8_t *out, uint32_t capacity);
uint32_t tb710fu_wifi_qmi_tlv_encode(uint8_t type, uint16_t length, const uint8_t *value,
                                     uint8_t *out, uint32_t capacity);
int tb710fu_wifi_qmi_tlv_next(const uint8_t *body, uint32_t bytes, uint32_t *offset,
                              struct tb710fu_wifi_qmi_tlv *tlv);

/* ---- HTC ---- */
#define TB710FU_WIFI_HTC_HDR_BYTES 8u
#define TB710FU_WIFI_HTC_MAX_PAYLOAD 4096u

enum {
    TB710FU_WIFI_HTC_MSG_READY = 1,
    TB710FU_WIFI_HTC_MSG_CONNECT_SERVICE = 2,
    TB710FU_WIFI_HTC_MSG_CONNECT_SERVICE_RESP = 3,
    TB710FU_WIFI_HTC_MSG_SETUP_COMPLETE = 4,
    TB710FU_WIFI_HTC_MSG_SETUP_COMPLETE_EX = 5
};

enum {
    TB710FU_WIFI_HTC_STATUS_SUCCESS = 0,
    TB710FU_WIFI_HTC_STATUS_FAILED = 2
};

#define TB710FU_WIFI_HTC_SVC_ID_WMI_CONTROL 0x0100u /* SVC(WMI group 1, index 0) */

struct tb710fu_wifi_htc_frame {
    uint32_t endpoint, flags, payload_len;
};

struct tb710fu_wifi_htc_message {
    uint16_t message_id;
    uint32_t service_id, flags, meta_len, max_message_len;
    uint32_t credits, credit_size, max_endpoints, status;
};

int tb710fu_wifi_htc_decode(const uint8_t *frame, uint32_t bytes, struct tb710fu_wifi_htc_frame *header);
uint32_t tb710fu_wifi_htc_encode(const struct tb710fu_wifi_htc_frame *header, const uint8_t *payload,
                                 uint8_t *out, uint32_t capacity);
uint64_t tb710fu_wifi_htc_connect_words(uint32_t service_id, uint32_t flags, uint32_t meta_len);
int tb710fu_wifi_htc_ready_words(uint64_t words, struct tb710fu_wifi_htc_message *message);
int tb710fu_wifi_htc_connect_response(uint64_t words, int32_t eid,
                                      struct tb710fu_wifi_htc_message *message);

/* ---- WMI ---- */
#define TB710FU_WIFI_WMI_HDR_BYTES 4u
#define TB710FU_WIFI_WMI_CMD_ID_MASK 0x00ffffffu
#define TB710FU_WIFI_WMI_TLV_LEN_MASK 0x0000ffffu
#define TB710FU_WIFI_WMI_TLV_TAG_SHIFT 16u

#define TB710FU_WIFI_WMI_INIT_CMDID 0x1u
#define TB710FU_WIFI_WMI_SERVICE_READY_EVENTID 0x1u
#define TB710FU_WIFI_WMI_READY_EVENTID 0x2u

uint32_t tb710fu_wifi_wmi_encode_cmd(uint32_t cmd_id, const uint8_t *body, uint32_t length,
                                     uint8_t *out, uint32_t capacity);
int tb710fu_wifi_wmi_decode_cmd(const uint8_t *packet, uint32_t bytes, uint32_t *cmd_id, uint32_t *body_offset);
uint32_t tb710fu_wifi_wmi_tlv(uint32_t tag, uint32_t length);

/* ---- DP rings (geometry only) ---- */
struct tb710fu_wifi_dp_plan {
    uint32_t tx_elems, rx_elems, elem_bytes;
    uint32_t tx_bytes, rx_bytes;
};

int tb710fu_wifi_dp_plan_make(uint32_t tx_elems, uint32_t rx_elems, uint32_t elem_bytes,
                              struct tb710fu_wifi_dp_plan *plan);
uint32_t tb710fu_wifi_dp_next(uint32_t index, uint32_t elems);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_WIFI_PROTO_H */
