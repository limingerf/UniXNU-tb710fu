/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>

#include "Tb710fuBluetoothQca.h"

uint16_t tb_bt_qca_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t tb_bt_qca_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void note(const TbBtQcaIo *io, const char *what, uint32_t a, uint32_t b)
{
    if (io && io->note) io->note(io->ctx, what, a, b);
}

int tb_bt_qca_tlv_parse(const uint8_t *fw, uint32_t size, TbBtQcaTlv *out)
{
    if (!fw || size < 4u || !out) return -1;
    uint32_t type_len = tb_bt_qca_get_u32(fw);
    out->type_len = type_len;
    out->type = (uint8_t)(type_len & 0xffu);
    out->length = type_len >> 8;
    if (out->length + 4u > size) return -1;
    if (out->length + 4u != size) {
        /* Not fatal on hardware (qca_tlv_check_data only requires the declared length to fit), but a
         * mismatch means the file was truncated or padded, so report it and let the caller decide. */
        return 1;
    }
    return 0;
}

int tb_bt_qca_patch_parse(const uint8_t *fw, uint32_t size, TbBtQcaPatch *out)
{
    if (!fw || !out || size < 4u + 24u) return -1;
    const uint8_t *p = fw + 4u; /* struct tlv_type_patch starts after the 4-byte TLV header */
    out->total_size = tb_bt_qca_get_u32(p + 0);
    out->data_length = tb_bt_qca_get_u32(p + 4);
    out->format_version = p[8];
    out->signature = p[9];
    out->download_mode = p[10];
    out->product_id = tb_bt_qca_get_u16(p + 12);
    out->rom_build = tb_bt_qca_get_u16(p + 14);
    out->patch_version = tb_bt_qca_get_u16(p + 16);
    out->entry = tb_bt_qca_get_u32(p + 20);
    /* data_length counts the patch payload that follows this 24-byte header. */
    if (out->data_length + 4u + 24u > size) return -1;
    return 0;
}

uint32_t tb_bt_qca_segment_count(uint32_t size)
{
    return (size + TB_BT_QCA_MAX_SEGMENT - 1u) / TB_BT_QCA_MAX_SEGMENT;
}

int tb_bt_qca_nvm_prepare(uint8_t *fw, uint32_t size, uint8_t baud_code, TbBtQcaNvm *out)
{
    if (!fw || !out || size < 4u) return -1;
    TbBtQcaTlv tlv;
    if (tb_bt_qca_tlv_parse(fw, size, &tlv) < 0) return -1;

    uint32_t header = 4u;
    /* A file can wrap its tag set in a type-4 container (qca_tlv_check_data); WCN7850's hmtnv20.bin
     * starts straight with type 2, but accepting the wrapper costs four bytes of code. */
    if (tlv.type == TB_BT_QCA_TLV_WRAPPER) {
        if (size < 8u) return -1;
        const uint32_t inner = tb_bt_qca_get_u32(fw + 4u);
        if ((inner & 0xffu) != TB_BT_QCA_TLV_NVM) return -1;
        if ((inner >> 8) + 8u > size) return -1;
        header = 8u;
        tlv.length = inner >> 8;
    } else if (tlv.type != TB_BT_QCA_TLV_NVM) {
        return -1;
    }

    uint32_t off = header;
    const uint32_t end = header + tlv.length;
    if (end > size) return -1;
    out->tags = 0u;
    out->tag_hci = 0u;
    out->tag_deep_sleep = 0u;
    out->tag_bd_addr = 0u;
    out->bd_addr_present = false;
    out->baud_code_written = 0u;

    while (off + 12u <= end && out->tags < TB_BT_QCA_MAX_TAGS) {
        const uint16_t id = tb_bt_qca_get_u16(fw + off);
        const uint16_t len = tb_bt_qca_get_u16(fw + off + 2u);
        uint8_t *data = fw + off + 12u;
        if (off + 12u + len > end) return -1;
        out->tags++;
        switch (id) {
        case TB_BT_QCA_TAG_HCI:
            /* btqca: data[0] |= 0x80 (software inband sleep), data[1] = baud code for WCN3991 and later. */
            if (len < 3u) return -1;
            out->tag_hci++;
            data[0] = (uint8_t)(data[0] | 0x80u);
            data[1] = baud_code;
            out->baud_code_written = data[1];
            break;
        case TB_BT_QCA_TAG_DEEP_SLEEP:
            if (len < 1u) return -1;
            out->tag_deep_sleep++;
            data[0] = (uint8_t)(data[0] | 0x01u);
            break;
        case TB_BT_QCA_TAG_BD_ADDR:
            if (len != 6u) return -1;
            out->tag_bd_addr++;
            out->bd_addr_present = true;
            for (unsigned i = 0; i < 6u; ++i) out->bd_addr[i] = data[i];
            break;
        default:
            break;
        }
        off += 12u + len;
    }
    if (off != end) return -1; /* trailing bytes inside the declared TLV length */
    return 0;
}

/* Vendor commands answer with a payload whose first byte is cresp and whose second is rtype
 * (struct edl_event_hdr). cresp 0 (EDL_CMD_REQ_RES_EVT) means accepted. */
static int qca_vendor(const TbBtQcaIo *io, uint16_t subop, const uint8_t *extra, uint8_t extra_len,
                      TbBtQcaReply *reply, uint32_t timeout_ms)
{
    if (!io || !io->send || !io->wait_reply) return -1;
    uint8_t params[1 + 16];
    if (extra_len > 16u) return -1;
    params[0] = (uint8_t)subop;
    for (uint8_t i = 0; i < extra_len; ++i) params[1 + i] = extra[i];
    if (io->send(io->ctx, TB_BT_QCA_PATCH_OPCODE, params, (uint8_t)(1u + extra_len)) < 0) return -1;
    TbBtQcaReply r = {0};
    if (io->wait_reply(io->ctx, TB_BT_QCA_PATCH_OPCODE, &r, timeout_ms) != 0) return -1;
    if (reply) *reply = r;
    if (r.kind != TB_BT_QCA_REPLY_CC) return -1;
    if (r.status != TB_BT_QCA_EDL_CMD_REQ_RES) return -1;
    if (r.len < 1u || r.data[0] != subop) {
        note(io, "qca-rtype", r.data[0], subop);
        return -1;
    }
    return 0;
}

int tb_bt_qca_read_version(const TbBtQcaIo *io, TbBtQcaVersion *ver)
{
    if (!ver) return -1;
    TbBtQcaReply r = {0};
    if (qca_vendor(io, TB_BT_QCA_VER_SUBOP, NULL, 0u, &r, 1000u) != 0) return -1;
    ver->raw_len = r.len < 16u ? r.len : 16u;
    for (uint32_t i = 0; i < ver->raw_len; ++i) ver->raw[i] = r.data[i];
    /* btqca: for WCN3991 and later the version follows one extra byte after rtype, i.e. reply offset 2
     * (reply[0] = rtype). Fall back to offset 1 for a 13-byte reply so a layout change is reported, not
     * mistaken for a dead controller. */
    uint32_t at = 2u;
    if (r.len == 13u) at = 1u;
    if (r.len < at + 12u) {
        note(io, "ver-short", r.len, at);
        return -1;
    }
    ver->parse_offset = at;
    /* struct qca_btsoc_version { __le32 product_id; __le16 patch_ver; __le16 rom_ver; __le32 soc_id; } */
    ver->product_id = tb_bt_qca_get_u32(r.data + at);
    ver->patch_version = tb_bt_qca_get_u16(r.data + at + 4u);
    ver->rom_version = tb_bt_qca_get_u16(r.data + at + 6u);
    ver->soc_id = tb_bt_qca_get_u32(r.data + at + 8u);
    note(io, "soc-version", ver->product_id, ver->rom_version);
    note(io, "soc-id", ver->soc_id, ver->patch_version);
    return 0;
}

int tb_bt_qca_set_baudrate(const TbBtQcaIo *io, uint8_t baud_code)
{
    if (!io || !io->send || !io->wait_reply) return -1;
    const uint8_t param = baud_code;
    if (io->send(io->ctx, TB_BT_QCA_SET_BAUD_OPCODE, &param, 1u) < 0) return -1;
    TbBtQcaReply r = {0};
    if (io->wait_reply(io->ctx, TB_BT_QCA_SET_BAUD_OPCODE, &r, 1000u) != 0) return -1;
    /* qca_set_baudrate in hci_qca expects a Command Complete carrying the EDL_SET_BAUDRATE_RSP_EVT
     * (0x92) in the return parameters; the status byte itself is the HCI status. */
    if (r.kind != TB_BT_QCA_REPLY_CC) return -1;
    if (r.status != 0u) return -1;
    if (r.len >= 1u && r.data[0] != 0x92u) note(io, "baud-rsp", r.data[0], 0x92u);
    note(io, "baud-set", baud_code, r.len);
    return 0;
}

static int send_segment(const TbBtQcaIo *io, const uint8_t *data, uint32_t len, bool acked)
{
    uint8_t params[2u + TB_BT_QCA_MAX_SEGMENT];
    if (!len || len > TB_BT_QCA_MAX_SEGMENT) return -1;
    params[0] = TB_BT_QCA_SEG_SUBOP;
    params[1] = (uint8_t)len;
    for (uint32_t i = 0; i < len; ++i) params[2u + i] = data[i];
    if (io->send(io->ctx, TB_BT_QCA_PATCH_OPCODE, params, (uint8_t)(2u + len)) < 0) return -1;
    if (!acked) return 0;
    TbBtQcaReply r = {0};
    if (io->wait_reply(io->ctx, TB_BT_QCA_PATCH_OPCODE, &r, 2000u) != 0) return -1;
    if (r.kind != TB_BT_QCA_REPLY_CC) return -1;
    if (r.status != TB_BT_QCA_EDL_CMD_REQ_RES) return -1;
    if (r.len < 1u || r.data[0] != TB_BT_QCA_SEG_SUBOP) return -1;
    return 0;
}

int tb_bt_qca_download_patch(const TbBtQcaIo *io, const uint8_t *fw, uint32_t size, TbBtQcaStats *st)
{
    if (!io || !fw || !size) return -1;
    TbBtQcaTlv tlv;
    if (tb_bt_qca_tlv_parse(fw, size, &tlv) < 0 || tlv.type != TB_BT_QCA_TLV_PATCH) return -1;
    TbBtQcaPatch patch;
    if (tb_bt_qca_patch_parse(fw, size, &patch) < 0) return -1;
    if (st) st->patch = patch;
    note(io, "patch-mode", patch.download_mode, patch.product_id);

    /* Modes 1 and 3 mean the controller answers only the last segment (qca_tlv_send_segment). */
    const bool per_segment_ack = !(patch.download_mode == TB_BT_QCA_DNLD_MODE_VSE ||
                                   patch.download_mode == TB_BT_QCA_DNLD_MODE_VSE_CC);

    uint32_t sent = 0u;
    uint32_t index = 0u;
    while (sent < size) {
        uint32_t seg = size - sent;
        if (seg > TB_BT_QCA_MAX_SEGMENT) seg = TB_BT_QCA_MAX_SEGMENT;
        const bool last = (sent + seg) == size;
        if (send_segment(io, fw + sent, seg, per_segment_ack || last) < 0) {
            if (st) {
                st->errors++;
                st->last_status = 0xffu;
            }
            note(io, "patch-seg-fail", index, seg);
            return -1;
        }
        if (per_segment_ack || last) {
            if (st) st->patch_acked++;
        }
        sent += seg;
        index++;
    }
    if (st) st->patch_segments = index;
    note(io, "patch-segments", index, size);
    return 0;
}

int tb_bt_qca_download_nvm(const TbBtQcaIo *io, const uint8_t *fw, uint32_t size, TbBtQcaStats *st)
{
    if (!io || !fw || !size) return -1;
    TbBtQcaTlv tlv;
    if (tb_bt_qca_tlv_parse(fw, size, &tlv) < 0) return -1;
    if (tlv.type != TB_BT_QCA_TLV_NVM && tlv.type != TB_BT_QCA_TLV_WRAPPER) return -1;

    uint32_t sent = 0u;
    uint32_t index = 0u;
    while (sent < size) {
        uint32_t seg = size - sent;
        if (seg > TB_BT_QCA_MAX_SEGMENT) seg = TB_BT_QCA_MAX_SEGMENT;
        /* Every NVM segment is acked: qca_tlv_check_data leaves dnld_mode at QCA_SKIP_EVT_NONE. */
        if (send_segment(io, fw + sent, seg, true) < 0) {
            if (st) {
                st->errors++;
                st->last_status = 0xffu;
            }
            note(io, "nvm-seg-fail", index, seg);
            return -1;
        }
        if (st) st->nvm_acked++;
        sent += seg;
        index++;
    }
    if (st) st->nvm_segments = index;
    note(io, "nvm-segments", index, size);
    return 0;
}

int tb_bt_qca_read_board_id(const TbBtQcaIo *io, uint16_t *bid)
{
    if (!bid) return -1;
    TbBtQcaReply r = {0};
    if (qca_vendor(io, TB_BT_QCA_BID_SUBOP, NULL, 0u, &r, 1000u) != 0) return -1;
    /* btqca: *bid = (edl->data[1] << 8) + edl->data[2]; reply[0] is rtype, so data[1] is reply[2]. */
    if (r.len < 3u) return -1;
    *bid = (uint16_t)(((uint16_t)r.data[1] << 8) | r.data[2]);
    note(io, "board-id", *bid, r.len);
    return 0;
}

int tb_bt_qca_disable_logging(const TbBtQcaIo *io)
{
    if (!io || !io->send || !io->wait_reply) return -1;
    const uint8_t params[2] = {TB_BT_QCA_LOGGING_SUBOP, 0x00u};
    if (io->send(io->ctx, TB_BT_QCA_DISABLE_LOGGING_OPCODE, params, 2u) < 0) return -1;
    TbBtQcaReply r = {0};
    if (io->wait_reply(io->ctx, TB_BT_QCA_DISABLE_LOGGING_OPCODE, &r, 1000u) != 0) return -1;
    if (r.kind != TB_BT_QCA_REPLY_CC || r.status != 0u) return -1;
    note(io, "logging-off", r.status, r.len);
    return 0;
}

int tb_bt_qca_read_build_info(const TbBtQcaIo *io, uint8_t *label, uint32_t cap, uint32_t *len)
{
    if (!label || !len || !cap) return -1;
    *len = 0u;
    TbBtQcaReply r = {0};
    if (qca_vendor(io, TB_BT_QCA_BUILD_INFO_SUBOP, NULL, 0u, &r, 1000u) != 0) return -1;
    /* reply = [rtype][build_lbl_len][label...] (btqca reads edl->data[0] for the length). */
    if (r.len < 2u) return -1;
    const uint32_t n = r.data[1];
    if (2u + n > r.len) return -1;
    const uint32_t copy = n < cap ? n : cap;
    for (uint32_t i = 0; i < copy; ++i) label[i] = r.data[2 + i];
    *len = copy;
    note(io, "build-info", n, r.len);
    return 0;
}

int tb_bt_qca_read_bdaddr(const TbBtQcaIo *io, uint8_t addr[6])
{
    if (!io || !io->send || !io->wait_reply || !addr) return -1;
    if (io->send(io->ctx, TB_BT_QCA_READ_BD_ADDR_OPCODE, NULL, 0u) < 0) return -1;
    TbBtQcaReply r = {0};
    if (io->wait_reply(io->ctx, TB_BT_QCA_READ_BD_ADDR_OPCODE, &r, 1000u) != 0) return -1;
    if (r.kind != TB_BT_QCA_REPLY_CC || r.status != 0u) return -1;
    if (r.len < 6u) return -1;
    for (unsigned i = 0; i < 6u; ++i) addr[i] = r.data[i];
    return 0;
}

int tb_bt_qca_write_bdaddr(const TbBtQcaIo *io, const uint8_t addr[6])
{
    if (!io || !io->send || !io->wait_reply || !addr) return -1;
    /* qca_set_bdaddr hands the controller bdaddr_swapped, i.e. the reverse of the printed order. */
    uint8_t swapped[6];
    for (unsigned i = 0; i < 6u; ++i) swapped[i] = addr[5u - i];
    if (io->send(io->ctx, TB_BT_QCA_WRITE_BD_ADDR_OPCODE, swapped, 6u) < 0) return -1;
    TbBtQcaReply r = {0};
    if (io->wait_reply(io->ctx, TB_BT_QCA_WRITE_BD_ADDR_OPCODE, &r, 1000u) != 0) return -1;
    if (r.kind != TB_BT_QCA_REPLY_CC || r.status != 0u) return -1;
    note(io, "bdaddr-write", r.status, r.len);
    return 0;
}
