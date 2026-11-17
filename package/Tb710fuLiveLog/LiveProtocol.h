/* SPDX-License-Identifier: MIT */
#ifndef TB710FU_LIVE_LOG_PROTOCOL_H
#define TB710FU_LIVE_LOG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define TB710FU_LIVE_LOG_MAX_BYTES 4096u
#define TB710FU_LIVE_LOG_NAME_BYTES 96u
#define TB710FU_LIVE_LOG_BLOCK_BYTES 720u
#define TB710FU_LIVE_LOG_FRAME_BYTES 752u
#define TB710FU_LIVE_LOG_ENVELOPE_BYTES \
    (26u + TB710FU_LIVE_LOG_NAME_BYTES + 10u + TB710FU_LIVE_LOG_MAX_BYTES)

enum tb710fu_live_log_result {
    TB710FU_LIVE_LOG_INVALID = 0,
    TB710FU_LIVE_LOG_READY = 1,
    TB710FU_LIVE_LOG_BUSY = 2,
    TB710FU_LIVE_LOG_EMPTY = 3
};

struct tb710fu_live_log_info {
    uint64_t generation;
    uint64_t start;
    uint64_t end;
    uint32_t session_id;
    uint32_t payload_checksum;
    uint32_t block_count;
    uint32_t envelope_bytes;
    uint32_t rounds;
    uint32_t next_sequence;
    uint32_t bytes;
    char filename[TB710FU_LIVE_LOG_NAME_BYTES];
};

/* One owner serializes these APIs. No allocation, waits, I/O or clock access.
 * snapshot copies the exact log bytes into an immutable LMS1 file envelope.
 * start/end are absolute byte offsets; end - start must equal length.
 * An empty log is reported as EMPTY and does not replace a valid transfer.
 * Once accepted, at least two complete source-block rounds must be emitted
 * before another snapshot may replace it. More rounds may be sent indefinitely.
 * Each next_frame call emits one profile-0 systematic OneSend frame, skipping
 * repair sequence numbers. The native adapter owns timing and snapshot policy.
 * A receiver must finish/reset (or retain a receiver per session) when the
 * native adapter publishes a new immutable snapshot/session. Generation zero
 * names a fixed recovery snapshot; positive generations name live snapshots.
 */
struct tb710fu_live_log_sender {
    struct tb710fu_live_log_info info;
    uint32_t source_in_round;
    uint32_t valid;
    uint8_t envelope[TB710FU_LIVE_LOG_ENVELOPE_BYTES];
};

void tb710fu_live_log_sender_init(struct tb710fu_live_log_sender *sender);
enum tb710fu_live_log_result tb710fu_live_log_snapshot(
    struct tb710fu_live_log_sender *sender, const uint8_t *bytes, size_t length,
    uint64_t generation, uint64_t start, uint64_t end);
int tb710fu_live_log_next_frame(struct tb710fu_live_log_sender *sender,
    uint8_t out[TB710FU_LIVE_LOG_FRAME_BYTES]);
int tb710fu_live_log_can_snapshot(const struct tb710fu_live_log_sender *sender);
int tb710fu_live_log_get_info(const struct tb710fu_live_log_sender *sender,
    struct tb710fu_live_log_info *out);

#endif
