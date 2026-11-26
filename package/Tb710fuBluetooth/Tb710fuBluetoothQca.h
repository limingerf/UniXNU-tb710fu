/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Qualcomm QCA firmware loading for a WCN7850 Hamilton over UART.
 *
 * Formats and command codes are taken from drivers/bluetooth/btqca.h and btqca.c (Linux master):
 *   tlv_type_hdr      { __le32 type_len; }  type = low byte, length = value >> 8
 *   tlv_type_patch    { total_size, data_length, format_version, signature, download_mode, reserved1,
 *                       product_id:16, rom_build:16, patch_version:16, reserved2:16, entry } = 24 bytes
 *   tlv_type_nvm      { tag_id:16, tag_len:16, reserve1:32, reserve2:32, data[] } = 12 bytes + data
 *   segment size      MAX_SIZE_PER_TLV_SEGMENT = 243
 *   patch segment cmd EDL_PATCH_CMD_OPCODE 0xFC00, param[0] = EDL_PATCH_TLV_REQ_CMD 0x1E, param[1] = size
 *   version req       0xFC00 / EDL_PATCH_VER_REQ_CMD 0x19   (CC on WCN3991 and later)
 *   board id          0xFC00 / EDL_GET_BID_REQ_CMD 0x23
 *   build info        0xFC00 / EDL_GET_BUILD_INFO_CMD 0x20
 *   logging off       0xFC17 with { QCA_DISABLE_LOGGING_SUB_OP 0x14, 0x00 }
 *   baud switch       0xFC48 with one byte from enum qca_baudrate (3.2 Mbaud = 17)
 *   result header     struct edl_event_hdr { u8 cresp; u8 rtype; u8 data[] }, cresp 0 is success
 * Segmentation and acknowledgement rules are qca_download_firmware / qca_tlv_send_segment:
 *   - the file is sent whole, 243 bytes per command, including its 4-byte TLV header;
 *   - a patch file with download_mode 1 or 3 is not acked per segment (the controller answers only the
 *     last one, and Linux has to inject a Command Complete to keep its own bookkeeping happy);
 *   - an NVM file keeps the default mode and is acked on every segment (WCN3991 and later: Command
 *     Complete, cresp 0, rtype 0x1E);
 *   - NVM tag 17 (HCI) gets data[0] |= 0x80 and data[1] = the baud code for WCN3991 and later, tag 27
 *     (deep sleep) gets data[0] |= 0x01, tag 2 carries the BD_ADDR. Those writes are why the driver needs
 *     a mutable copy of the NVM image.
 * Everything here is byte-oriented get_u16le/get_u32le: no packed struct loads, so nothing depends on
 * unaligned access behaviour.
 */
#ifndef TB710FU_BLUETOOTH_QCA_H
#define TB710FU_BLUETOOTH_QCA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_BT_QCA_PATCH_OPCODE 0xfc00u
#define TB_BT_QCA_SEG_SUBOP 0x1eu
#define TB_BT_QCA_VER_SUBOP 0x19u
#define TB_BT_QCA_BID_SUBOP 0x23u
#define TB_BT_QCA_BUILD_INFO_SUBOP 0x20u
#define TB_BT_QCA_SET_BAUD_OPCODE 0xfc48u
#define TB_BT_QCA_DISABLE_LOGGING_OPCODE 0xfc17u
#define TB_BT_QCA_WRITE_BD_ADDR_OPCODE 0xfc14u
#define TB_BT_QCA_READ_BD_ADDR_OPCODE 0x1009u
#define TB_BT_QCA_LOGGING_SUBOP 0x14u
#define TB_BT_QCA_EDL_CMD_REQ_RES 0x00u
#define TB_BT_QCA_TLV_PATCH 1u
#define TB_BT_QCA_TLV_NVM 2u
#define TB_BT_QCA_TLV_WRAPPER 4u
#define TB_BT_QCA_MAX_SEGMENT 243u
#define TB_BT_QCA_BAUD_3200000 17u
#define TB_BT_QCA_TAG_BD_ADDR 2u
#define TB_BT_QCA_TAG_HCI 17u
#define TB_BT_QCA_TAG_DEEP_SLEEP 27u
#define TB_BT_QCA_MAX_TAGS 512u

/* The download mode values a patch file can carry (enum qca_tlv_dnld_mode). */
#define TB_BT_QCA_DNLD_MODE_VSE 1u
#define TB_BT_QCA_DNLD_MODE_CC 2u
#define TB_BT_QCA_DNLD_MODE_VSE_CC 3u

typedef struct {
    uint32_t type_len;
    uint8_t type;
    uint32_t length;
} TbBtQcaTlv;

typedef struct {
    uint32_t total_size;
    uint32_t data_length;
    uint8_t format_version;
    uint8_t signature;
    uint8_t download_mode;
    uint16_t product_id;
    uint16_t rom_build;
    uint16_t patch_version;
    uint32_t entry;
} TbBtQcaPatch;

typedef struct {
    uint32_t product_id;
    uint16_t patch_version;
    uint16_t rom_version;
    uint32_t soc_id;
    uint8_t raw[16];
    uint32_t raw_len;
    uint32_t parse_offset; /* where the version structure was found in the reply */
} TbBtQcaVersion;

typedef struct {
    uint32_t tags;
    uint32_t tag_hci;
    uint32_t tag_deep_sleep;
    uint32_t tag_bd_addr;
    uint8_t bd_addr[6];
    bool bd_addr_present;
    uint8_t baud_code_written;
} TbBtQcaNvm;

enum { TB_BT_QCA_REPLY_NONE = 0, TB_BT_QCA_REPLY_CC = 1, TB_BT_QCA_REPLY_CS = 2, TB_BT_QCA_REPLY_VSE = 3, TB_BT_QCA_REPLY_TIMEOUT = 4 };

typedef struct {
    int kind;
    uint8_t status; /* first return parameter: HCI status, or the QCA cresp for vendor commands */
    uint8_t data[64];
    uint32_t len;
} TbBtQcaReply;

typedef struct {
    /* Queue one H4 command. Fire-and-forget: the caller may wait afterwards or not. */
    int (*send)(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t plen);
    /* Wait for the next Command Complete / Command Status / vendor event that answers opcode. */
    int (*wait_reply)(void *ctx, uint16_t opcode, TbBtQcaReply *out, uint32_t timeout_ms);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void (*note)(void *ctx, const char *what, uint32_t a, uint32_t b);
    void *ctx;
} TbBtQcaIo;

typedef struct {
    uint32_t patch_segments;
    uint32_t patch_acked;
    uint32_t nvm_segments;
    uint32_t nvm_acked;
    uint32_t errors;
    uint8_t last_status;
    uint16_t board_id;
    TbBtQcaPatch patch;
    TbBtQcaNvm nvm;
} TbBtQcaStats;

/* Byte helpers, exposed because the host test checks the parser against raw firmware bytes. */
uint16_t tb_bt_qca_get_u16(const uint8_t *p);
uint32_t tb_bt_qca_get_u32(const uint8_t *p);
int tb_bt_qca_tlv_parse(const uint8_t *fw, uint32_t size, TbBtQcaTlv *out);
int tb_bt_qca_patch_parse(const uint8_t *fw, uint32_t size, TbBtQcaPatch *out);
uint32_t tb_bt_qca_segment_count(uint32_t size);
/* Applies the three NVM tag rewrites on a mutable copy. Returns 0 or -1 on a malformed image. */
int tb_bt_qca_nvm_prepare(uint8_t *fw, uint32_t size, uint8_t baud_code, TbBtQcaNvm *out);

/* 0xFC00 / 0x19. Returns 0 when the reply matched, with the version filled in. */
int tb_bt_qca_read_version(const TbBtQcaIo *io, TbBtQcaVersion *ver);
/* 0xFC48 with the enum value from the table above. */
int tb_bt_qca_set_baudrate(const TbBtQcaIo *io, uint8_t baud_code);
/* Whole-file downloads, in the order btqca sends them. */
int tb_bt_qca_download_patch(const TbBtQcaIo *io, const uint8_t *fw, uint32_t size, TbBtQcaStats *st);
int tb_bt_qca_download_nvm(const TbBtQcaIo *io, const uint8_t *fw, uint32_t size, TbBtQcaStats *st);
int tb_bt_qca_read_board_id(const TbBtQcaIo *io, uint16_t *bid);
int tb_bt_qca_disable_logging(const TbBtQcaIo *io);
int tb_bt_qca_read_build_info(const TbBtQcaIo *io, uint8_t *label, uint32_t cap, uint32_t *len);
/* Read_BD_ADDR (standard 0x1009) and the vendor write (0xFC14). The write takes the address in the order
 * Read_BD_ADDR prints it and swaps it for the wire, as qca_set_bdaddr does; the caller re-reads to confirm. */
int tb_bt_qca_read_bdaddr(const TbBtQcaIo *io, uint8_t addr[6]);
int tb_bt_qca_write_bdaddr(const TbBtQcaIo *io, const uint8_t addr[6]);

#ifdef __cplusplus
}
#endif
#endif /* TB710FU_BLUETOOTH_QCA_H */
