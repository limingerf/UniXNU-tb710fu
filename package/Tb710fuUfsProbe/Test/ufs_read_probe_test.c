/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../UfsReadProbe.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

struct mock {
    struct tb710fu_ufs_snapshot snapshot;
    uint8_t bytes[TB710FU_UFS_BLOCK_BYTES];
    int map_ok;
    unsigned map_fail_on_call;
    int takeover_ok;
    int idle_ok;
    int restore_ok;
    enum tb710fu_ufs_read_completion completion;
    unsigned invalid_snapshot_on_call;
    unsigned snapshots;
    unsigned allocations;
    unsigned releases;
    unsigned map_checks;
    unsigned takeovers;
    unsigned reads;
    unsigned idle_waits;
    unsigned restores;
    uint8_t last_cdb[10];
    uint8_t last_lun;
};

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static void make_gpt_sample(uint8_t *bytes)
{
    memset(bytes, 0, TB710FU_UFS_BLOCK_BYTES);
    memcpy(bytes, "EFI PART", 8);
    put_le32(bytes + 8, 0x00010000);
    put_le32(bytes + 12, 92);
    put_le32(bytes + 16, 0);
    put_le64(bytes + 24, 1);
    put_le64(bytes + 32, 127);
    put_le64(bytes + 40, 34);
    put_le64(bytes + 48, 126);
    put_le64(bytes + 72, 2);
    put_le32(bytes + 80, 4);
    put_le32(bytes + 84, 128);
    put_le32(bytes + 16, tb710fu_ufs_gpt_header_crc32(bytes, TB710FU_UFS_BLOCK_BYTES));
}

static int read_snapshot(void *context, struct tb710fu_ufs_snapshot *snapshot)
{
    struct mock *m = context;
    ++m->snapshots;
    *snapshot = m->snapshot;
    if (m->snapshots == m->invalid_snapshot_on_call) {
        snapshot->sid60_route_verified = 0;
    }
    return 1;
}

static int allocate_dma(void *context, struct tb710fu_ufs_dma *dma)
{
    struct mock *m = context;
    ++m->allocations;
    dma->bytes = m->bytes;
    dma->physical_address = 0x90000000;
    dma->length = sizeof(m->bytes);
    dma->owned = 1;
    return 1;
}

static int verify_dma_mapping(void *context, uint64_t physical_address,
                              size_t length, uint8_t address_bits)
{
    struct mock *m = context;
    ++m->map_checks;
    return m->map_ok && m->map_checks != m->map_fail_on_call &&
           physical_address == 0x90000000 &&
           length == TB710FU_UFS_BLOCK_BYTES && address_bits == 36;
}

static void release_dma(void *context, struct tb710fu_ufs_dma *dma)
{
    struct mock *m = context;
    ++m->releases;
    dma->owned = 0;
}

static int takeover_idle_queue(void *context)
{
    struct mock *m = context;
    ++m->takeovers;
    return m->takeover_ok;
}

static enum tb710fu_ufs_read_completion read10(void *context, uint8_t lun,
                                               const uint8_t cdb[10],
                                               struct tb710fu_ufs_dma *dma)
{
    struct mock *m = context;
    ++m->reads;
    m->last_lun = lun;
    memcpy(m->last_cdb, cdb, 10);
    if (m->completion == TB710FU_UFS_READ_COMPLETE) {
        memcpy(dma->bytes, m->bytes, sizeof(m->bytes));
    }
    return m->completion;
}

static int wait_idle(void *context)
{
    struct mock *m = context;
    ++m->idle_waits;
    return m->idle_ok;
}

static int restore_queue(void *context)
{
    struct mock *m = context;
    ++m->restores;
    return m->restore_ok;
}

static struct tb710fu_ufs_probe_ops make_ops(struct mock *m)
{
    struct tb710fu_ufs_probe_ops ops = {
        .context = m,
        .read_snapshot = read_snapshot,
        .allocate_dma = allocate_dma,
        .verify_dma_mapping = verify_dma_mapping,
        .release_dma = release_dma,
        .takeover_idle_queue = takeover_idle_queue,
        .read10 = read10,
        .wait_idle = wait_idle,
        .restore_queue = restore_queue,
    };
    return ops;
}

static struct mock make_mock(void)
{
    struct mock m = {0};
    m.snapshot.controller_powered = 1;
    m.snapshot.link_active = 1;
    m.snapshot.queue_enabled = 1;
    m.snapshot.queue_idle = 1;
    m.snapshot.sid60_route_verified = 1;
    m.snapshot.dma_address_bits = 36;
    m.map_ok = 1;
    m.takeover_ok = 1;
    m.idle_ok = 1;
    m.restore_ok = 1;
    m.completion = TB710FU_UFS_READ_COMPLETE;
    make_gpt_sample(m.bytes);
    return m;
}

static void test_disabled_is_inert(void)
{
    struct mock m = make_mock();
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    struct tb710fu_ufs_probe_report report;
    assert(tb710fu_ufs_read_sample(0, 0, &ops, &report) == TB710FU_UFS_PROBE_DISABLED);
    assert(m.snapshots == 0 && m.allocations == 0 && m.takeovers == 0 && m.reads == 0);
}

static void test_unknown_hardware_state_refuses_before_dma(void)
{
    struct mock m = make_mock();
    m.snapshot.sid60_route_verified = 0;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, tb710fu_ufs_crc32(m.bytes, sizeof(m.bytes)),
                                   &ops, NULL) == TB710FU_UFS_PROBE_STATE_REFUSED);
    assert(m.allocations == 0 && m.takeovers == 0 && m.reads == 0);
}

static void test_dma_mapping_must_be_verified(void)
{
    struct mock m = make_mock();
    m.map_ok = 0;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, 0, &ops, NULL) == TB710FU_UFS_PROBE_DMA_REFUSED);
    assert(m.allocations == 1 && m.releases == 1 && m.takeovers == 0 && m.reads == 0);
}

static void test_read_and_crc(void)
{
    struct mock m = make_mock();
    const uint32_t expected = tb710fu_ufs_crc32(m.bytes, sizeof(m.bytes));
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    struct tb710fu_ufs_probe_report report;
    assert(tb710fu_ufs_read_sample(1, expected, &ops, &report) == TB710FU_UFS_PROBE_OK);
    assert(m.last_lun == 0 && m.last_cdb[0] == 0x28);
    assert(m.last_cdb[2] == 0 && m.last_cdb[3] == 0 && m.last_cdb[4] == 0 && m.last_cdb[5] == 1);
    assert(m.last_cdb[7] == 0 && m.last_cdb[8] == 1);
    assert(report.actual_crc32 == expected);
    assert(report.gpt_header_crc32 == tb710fu_ufs_gpt_header_crc32(m.bytes, sizeof(m.bytes)));
    assert(m.takeovers == 1 && m.reads == 1 && m.restores == 1 && m.releases == 1);

    m = make_mock();
    ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, expected ^ 1u, &ops, &report) == TB710FU_UFS_PROBE_CRC_MISMATCH);
    assert(m.restores == 1 && m.releases == 1);
}

static void test_timeout_quarantines_dma(void)
{
    struct mock m = make_mock();
    m.idle_ok = 0;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, 0, &ops, NULL) == TB710FU_UFS_PROBE_QUARANTINED);
    assert(m.reads == 1 && m.restores == 0 && m.releases == 0);
}

static void test_post_read_sid_change_quarantines_dma(void)
{
    struct mock m = make_mock();
    m.invalid_snapshot_on_call = 3;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, 0, &ops, NULL) == TB710FU_UFS_PROBE_QUARANTINED);
    assert(m.reads == 1 && m.restores == 0 && m.releases == 0);
}

static void test_takeover_refusal_preserves_firmware_queue(void)
{
    struct mock m = make_mock();
    m.takeover_ok = 0;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, 0, &ops, NULL) == TB710FU_UFS_PROBE_TAKEOVER_REFUSED);
    assert(m.takeovers == 1 && m.reads == 0 && m.restores == 0 && m.releases == 1);
}

static void test_failed_read_restores_and_releases(void)
{
    struct mock m = make_mock();
    m.completion = TB710FU_UFS_READ_FAILED_IDLE;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, 0, &ops, NULL) == TB710FU_UFS_PROBE_READ_FAILED);
    assert(m.restores == 1 && m.releases == 1);
}

static void test_restore_failure_quarantines_dma(void)
{
    struct mock m = make_mock();
    m.restore_ok = 0;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, 0, &ops, NULL) == TB710FU_UFS_PROBE_RESTORE_FAILED);
    assert(m.restores == 1 && m.releases == 0);
}

static void test_dma_mapping_is_rechecked_after_takeover(void)
{
    struct mock m = make_mock();
    m.map_fail_on_call = 3;
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, 0, &ops, NULL) == TB710FU_UFS_PROBE_DMA_REFUSED);
    assert(m.takeovers == 1 && m.reads == 0 && m.restores == 1 && m.releases == 1);
}

static void test_header_crc_is_checked(void)
{
    struct mock m = make_mock();
    m.bytes[16] ^= 1;
    const uint32_t block_crc = tb710fu_ufs_crc32(m.bytes, sizeof(m.bytes));
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, block_crc, &ops, NULL) == TB710FU_UFS_PROBE_SAMPLE_INVALID);
    assert(m.restores == 1 && m.releases == 1);
}

static void test_bad_gpt_header_is_rejected(void)
{
    struct mock m = make_mock();
    m.bytes[0] = 'X';
    uint32_t block_crc = tb710fu_ufs_crc32(m.bytes, sizeof(m.bytes));
    struct tb710fu_ufs_probe_ops ops = make_ops(&m);
    assert(tb710fu_ufs_read_sample(1, block_crc, &ops, NULL) == TB710FU_UFS_PROBE_SAMPLE_INVALID);
    assert(m.restores == 1 && m.releases == 1);
}

int main(void)
{
    uint8_t cdb[10];
    assert(TB710FU_UFS_SAMPLE_CRC32 == 0x44b9c49au);
    assert(tb710fu_ufs_build_read10(1, 1, cdb));
    assert(!tb710fu_ufs_build_read10(UINT64_C(0x100000000), 1, cdb));
    assert(!tb710fu_ufs_build_read10(1, 2, cdb));
    assert(tb710fu_ufs_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    test_disabled_is_inert();
    test_unknown_hardware_state_refuses_before_dma();
    test_dma_mapping_must_be_verified();
    test_read_and_crc();
    test_timeout_quarantines_dma();
    test_post_read_sid_change_quarantines_dma();
    test_takeover_refusal_preserves_firmware_queue();
    test_failed_read_restores_and_releases();
    test_restore_failure_quarantines_dma();
    test_dma_mapping_is_rechecked_after_takeover();
    test_bad_gpt_header_is_rejected();
    test_header_crc_is_checked();
    return 0;
}
