/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UfsReadProbe.h"

#define GPT_HEADER_MIN_BYTES 92u

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    return (uint64_t)read_le32(bytes) | ((uint64_t)read_le32(bytes + 4) << 32);
}

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
        const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
        crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
    return crc;
}

uint32_t tb710fu_ufs_crc32(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL && length != 0) {
        return 0;
    }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc = crc32_update(crc, bytes[i]);
    }
    return ~crc;
}

uint32_t tb710fu_ufs_gpt_header_crc32(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL || length < GPT_HEADER_MIN_BYTES) {
        return 0;
    }
    const uint32_t header_size = read_le32(bytes + 12);
    if (header_size < GPT_HEADER_MIN_BYTES || header_size > length) {
        return 0;
    }
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < header_size; ++i) {
        const uint8_t byte = (i >= 16 && i < 20) ? 0 : bytes[i];
        crc = crc32_update(crc, byte);
    }
    return ~crc;
}

int tb710fu_ufs_build_read10(uint64_t lba, uint16_t block_count, uint8_t cdb[10])
{
    if (cdb == NULL || lba > UINT32_MAX || block_count != 1) {
        return 0;
    }
    cdb[0] = 0x28;
    cdb[1] = 0;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[6] = 0;
    cdb[7] = (uint8_t)(block_count >> 8);
    cdb[8] = (uint8_t)block_count;
    cdb[9] = 0;
    return 1;
}

int tb710fu_ufs_verify_gpt_sample(const uint8_t *bytes, size_t length,
                                  uint32_t expected_crc32,
                                  uint32_t *actual_crc32,
                                  uint32_t *header_crc32)
{
    if (actual_crc32 != NULL) {
        *actual_crc32 = 0;
    }
    if (header_crc32 != NULL) {
        *header_crc32 = 0;
    }
    if (bytes == NULL || length != TB710FU_UFS_BLOCK_BYTES ||
        bytes[0] != 'E' || bytes[1] != 'F' || bytes[2] != 'I' || bytes[3] != ' ' ||
        bytes[4] != 'P' || bytes[5] != 'A' || bytes[6] != 'R' || bytes[7] != 'T' ||
        read_le64(bytes + 24) != TB710FU_UFS_SAMPLE_LBA) {
        return 0;
    }
    const uint32_t stored_header_crc = read_le32(bytes + 16);
    const uint32_t computed_header_crc = tb710fu_ufs_gpt_header_crc32(bytes, length);
    const uint32_t computed_sample_crc = tb710fu_ufs_crc32(bytes, length);
    if (actual_crc32 != NULL) {
        *actual_crc32 = computed_sample_crc;
    }
    if (header_crc32 != NULL) {
        *header_crc32 = computed_header_crc;
    }
    return stored_header_crc == computed_header_crc && computed_sample_crc == expected_crc32;
}

static int valid_snapshot(const struct tb710fu_ufs_snapshot *snapshot)
{
    return snapshot->controller_powered && snapshot->link_active &&
           snapshot->queue_enabled && snapshot->queue_idle &&
           snapshot->sid60_route_verified &&
           snapshot->dma_address_bits >= 32 && snapshot->dma_address_bits <= 64;
}

static int valid_dma(const struct tb710fu_ufs_dma *dma, uint8_t address_bits)
{
    if (dma->bytes == NULL || !dma->owned || dma->length != TB710FU_UFS_BLOCK_BYTES ||
        (dma->physical_address & (TB710FU_UFS_BLOCK_BYTES - 1u)) != 0 ||
        dma->physical_address > UINT64_MAX - (TB710FU_UFS_BLOCK_BYTES - 1u)) {
        return 0;
    }
    const uint64_t last = dma->physical_address + TB710FU_UFS_BLOCK_BYTES - 1u;
    return address_bits == 64 || (last >> address_bits) == 0;
}

static enum tb710fu_ufs_probe_result set_result(struct tb710fu_ufs_probe_report *report,
                                                 enum tb710fu_ufs_probe_result result)
{
    if (report != NULL) {
        report->result = result;
    }
    return result;
}

enum tb710fu_ufs_probe_result tb710fu_ufs_read_sample(
    unsigned boot_argument, uint32_t expected_crc32,
    const struct tb710fu_ufs_probe_ops *ops,
    struct tb710fu_ufs_probe_report *report)
{
    if (report != NULL) {
        report->result = TB710FU_UFS_PROBE_BAD_ARGUMENT;
        report->expected_crc32 = expected_crc32;
        report->actual_crc32 = 0;
        report->gpt_header_crc32 = 0;
    }
    if (boot_argument != TB710FU_UFS_PROBE_BOOTARG) {
        return set_result(report, TB710FU_UFS_PROBE_DISABLED);
    }
    if (ops == NULL || ops->read_snapshot == NULL || ops->allocate_dma == NULL ||
        ops->verify_dma_mapping == NULL ||
        ops->release_dma == NULL || ops->takeover_idle_queue == NULL ||
        ops->read10 == NULL || ops->wait_idle == NULL || ops->restore_queue == NULL) {
        return set_result(report, TB710FU_UFS_PROBE_BAD_ARGUMENT);
    }

    struct tb710fu_ufs_snapshot snapshot = {0};
    if (!ops->read_snapshot(ops->context, &snapshot) || !valid_snapshot(&snapshot)) {
        return set_result(report, TB710FU_UFS_PROBE_STATE_REFUSED);
    }

    struct tb710fu_ufs_dma dma = {0};
    if (!ops->allocate_dma(ops->context, &dma)) {
        return set_result(report, TB710FU_UFS_PROBE_DMA_REFUSED);
    }
    if (!valid_dma(&dma, snapshot.dma_address_bits)) {
        ops->release_dma(ops->context, &dma);
        return set_result(report, TB710FU_UFS_PROBE_DMA_REFUSED);
    }
    if (!ops->verify_dma_mapping(ops->context, dma.physical_address, dma.length,
                                 snapshot.dma_address_bits)) {
        ops->release_dma(ops->context, &dma);
        return set_result(report, TB710FU_UFS_PROBE_DMA_REFUSED);
    }

    struct tb710fu_ufs_snapshot before_takeover = {0};
    if (!ops->read_snapshot(ops->context, &before_takeover) ||
        !valid_snapshot(&before_takeover) ||
        before_takeover.dma_address_bits != snapshot.dma_address_bits ||
        !ops->verify_dma_mapping(ops->context, dma.physical_address, dma.length,
                                 before_takeover.dma_address_bits)) {
        ops->release_dma(ops->context, &dma);
        return set_result(report, TB710FU_UFS_PROBE_STATE_REFUSED);
    }
    const uint8_t address_bits = before_takeover.dma_address_bits;
    if (!ops->takeover_idle_queue(ops->context)) {
        ops->release_dma(ops->context, &dma);
        return set_result(report, TB710FU_UFS_PROBE_TAKEOVER_REFUSED);
    }
    if (!ops->verify_dma_mapping(ops->context, dma.physical_address, dma.length,
                                 address_bits)) {
        if (!ops->wait_idle(ops->context) || !ops->restore_queue(ops->context)) {
            return set_result(report, TB710FU_UFS_PROBE_QUARANTINED);
        }
        ops->release_dma(ops->context, &dma);
        return set_result(report, TB710FU_UFS_PROBE_DMA_REFUSED);
    }

    uint8_t cdb[10];
    if (!tb710fu_ufs_build_read10(TB710FU_UFS_SAMPLE_LBA, 1, cdb)) {
        if (!ops->wait_idle(ops->context) || !ops->restore_queue(ops->context)) {
            return set_result(report, TB710FU_UFS_PROBE_QUARANTINED);
        }
        ops->release_dma(ops->context, &dma);
        return set_result(report, TB710FU_UFS_PROBE_BAD_ARGUMENT);
    }
    const enum tb710fu_ufs_read_completion completion =
        ops->read10(ops->context, TB710FU_UFS_SAMPLE_LUN, cdb, &dma);

    if (!ops->wait_idle(ops->context)) {
        /* Keep DMA memory owned forever if hardware may still write to it. */
        return set_result(report, TB710FU_UFS_PROBE_QUARANTINED);
    }
    struct tb710fu_ufs_snapshot after_read = {0};
    if (!ops->read_snapshot(ops->context, &after_read) || !valid_snapshot(&after_read) ||
        after_read.dma_address_bits != address_bits) {
        return set_result(report, TB710FU_UFS_PROBE_QUARANTINED);
    }
    if (!ops->restore_queue(ops->context)) {
        return set_result(report, TB710FU_UFS_PROBE_RESTORE_FAILED);
    }
    if (completion != TB710FU_UFS_READ_COMPLETE) {
        ops->release_dma(ops->context, &dma);
        return set_result(report, TB710FU_UFS_PROBE_READ_FAILED);
    }
    uint32_t actual_crc32 = 0;
    uint32_t header_crc32 = 0;
    const int sample_valid = tb710fu_ufs_verify_gpt_sample(
        dma.bytes, TB710FU_UFS_BLOCK_BYTES, expected_crc32,
        &actual_crc32, &header_crc32);
    if (report != NULL) {
        report->actual_crc32 = actual_crc32;
        report->gpt_header_crc32 = header_crc32;
    }
    ops->release_dma(ops->context, &dma);
    if (!sample_valid) {
        if (report != NULL && report->actual_crc32 != expected_crc32) {
            return set_result(report, TB710FU_UFS_PROBE_CRC_MISMATCH);
        }
        return set_result(report, TB710FU_UFS_PROBE_SAMPLE_INVALID);
    }
    return set_result(report, TB710FU_UFS_PROBE_OK);
}
