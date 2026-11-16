/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef TB710FU_UFS_READ_PROBE_H
#define TB710FU_UFS_READ_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define TB710FU_UFS_PROBE_BOOTARG 1u
#define TB710FU_UFS_SAMPLE_LUN 0u
#define TB710FU_UFS_SAMPLE_LBA 1u
#define TB710FU_UFS_BLOCK_BYTES 4096u
#define TB710FU_UFS_SAMPLE_CRC32 0x44b9c49au

enum tb710fu_ufs_probe_result {
    TB710FU_UFS_PROBE_OK,
    TB710FU_UFS_PROBE_DISABLED,
    TB710FU_UFS_PROBE_BAD_ARGUMENT,
    TB710FU_UFS_PROBE_STATE_REFUSED,
    TB710FU_UFS_PROBE_DMA_REFUSED,
    TB710FU_UFS_PROBE_TAKEOVER_REFUSED,
    TB710FU_UFS_PROBE_READ_FAILED,
    TB710FU_UFS_PROBE_QUARANTINED,
    TB710FU_UFS_PROBE_RESTORE_FAILED,
    TB710FU_UFS_PROBE_SAMPLE_INVALID,
    TB710FU_UFS_PROBE_CRC_MISMATCH
};

enum tb710fu_ufs_read_completion {
    TB710FU_UFS_READ_COMPLETE,
    TB710FU_UFS_READ_FAILED_IDLE,
    TB710FU_UFS_READ_TIMEOUT_ACTIVE
};

struct tb710fu_ufs_snapshot {
    uint8_t controller_powered;
    uint8_t link_active;
    uint8_t queue_enabled;
    uint8_t queue_idle;
    uint8_t sid60_route_verified;
    uint8_t dma_address_bits;
};

struct tb710fu_ufs_dma {
    uint8_t *bytes;
    uint64_t physical_address;
    size_t length;
    uint8_t owned;
};

struct tb710fu_ufs_probe_ops {
    void *context;
    /* Must only read live controller/SMMU state. */
    int (*read_snapshot)(void *context, struct tb710fu_ufs_snapshot *snapshot);
    int (*allocate_dma)(void *context, struct tb710fu_ufs_dma *dma);
    /* Must verify the entire buffer against the currently observed SID 0x60 mapping. */
    int (*verify_dma_mapping)(void *context, uint64_t physical_address,
                              size_t length, uint8_t address_bits);
    void (*release_dma)(void *context, struct tb710fu_ufs_dma *dma);
    /* Must recheck idle state atomically and leave the queue unchanged on refusal. */
    int (*takeover_idle_queue)(void *context);
    enum tb710fu_ufs_read_completion (*read10)(void *context, uint8_t lun,
                                               const uint8_t cdb[10],
                                               struct tb710fu_ufs_dma *dma);
    /* Nonzero means the controller can no longer DMA to the probe buffer. */
    int (*wait_idle)(void *context);
    /* Must restore the original queue only when the controller is idle. */
    int (*restore_queue)(void *context);
};

struct tb710fu_ufs_probe_report {
    enum tb710fu_ufs_probe_result result;
    uint32_t expected_crc32;
    uint32_t actual_crc32;
    uint32_t gpt_header_crc32;
};

uint32_t tb710fu_ufs_crc32(const uint8_t *bytes, size_t length);
uint32_t tb710fu_ufs_gpt_header_crc32(const uint8_t *bytes, size_t length);
int tb710fu_ufs_build_read10(uint64_t lba, uint16_t block_count, uint8_t cdb[10]);
int tb710fu_ufs_verify_gpt_sample(const uint8_t *bytes, size_t length,
                                  uint32_t expected_crc32,
                                  uint32_t *actual_crc32,
                                  uint32_t *header_crc32);
enum tb710fu_ufs_probe_result tb710fu_ufs_read_sample(
    unsigned boot_argument, uint32_t expected_crc32,
    const struct tb710fu_ufs_probe_ops *ops,
    struct tb710fu_ufs_probe_report *report);

#endif
