/* SPDX-License-Identifier: MIT */
#ifndef TB710FU_LIVE_LOG_PROTOCOL_H
#define TB710FU_LIVE_LOG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define TB710FU_LIVE_LOG_MAX_BYTES 4096u
#define TB710FU_LIVE_CAPTURE_BYTES 65536u
enum tb710fu_live_capture_state {
    TB710FU_LIVE_CAPTURE_INVALID = 0,
    TB710FU_LIVE_CAPTURE_BUSY = 1,
    TB710FU_LIVE_CAPTURE_EMPTY = 2,
    TB710FU_LIVE_CAPTURE_READY = 3
};
#define TB710FU_LIVE_LOG_NAME_BYTES 96u
#define TB710FU_LIVE_LOG_BLOCK_BYTES 720u
#define TB710FU_LIVE_LOG_FRAME_BYTES 752u
#define TB710FU_LIVE_LOG_MIN_ROUNDS 16u
#define TB710FU_LIVE_LOG_ENVELOPE_BYTES \
    (26u + TB710FU_LIVE_LOG_NAME_BYTES + 10u + TB710FU_LIVE_LOG_MAX_BYTES)

enum tb710fu_live_log_result {
    TB710FU_LIVE_LOG_INVALID = 0,
    TB710FU_LIVE_LOG_READY = 1,
    TB710FU_LIVE_LOG_BUSY = 2,
    TB710FU_LIVE_LOG_EMPTY = 3
};

enum tb710fu_live_log_source {
    TB710FU_LIVE_LOG_RAMOOPS = 0,
    TB710FU_LIVE_LOG_XNU = 1
};

/* One current boot, no initialization function or allocation required. The
 * zero-initialized ring accepts bytes before the native display is ready.
 * Each writer/copy tries guard once and never spins. Contended or saturated
 * writes increment dropped; overwritten historical bytes are not drops.
 * written is the absolute count of bytes successfully captured this boot.
 * It saturates at UINT64_MAX, refusing further bytes. The independent dropped
 * counter is uint64 modulo arithmetic; exhausting it is not a new boot/reset.
 * Do not reset, copy, mutate or free this object while writers can use it.
 */
struct tb710fu_live_capture {
    uint32_t guard;
    uint64_t written;
    uint64_t dropped;
    uint8_t bytes[TB710FU_LIVE_CAPTURE_BYTES];
};

struct tb710fu_live_capture_info {
    uint64_t start;
    uint64_t end;
    uint64_t dropped;
    uint32_t copied;
    uint32_t state; /* enum tb710fu_live_capture_state */
};

/* putc returns 1 if stored, 0 if invalid/dropped. Copy returns 0..4096 raw
 * bytes, the newest bounded tail, without changing the ring or byte offsets.
 * capacity greater than 4096 is clamped; NULL/zero output is invalid (state 0).
 * A zero-initialized non-NULL ring with valid output is empty (state 2).
 * Busy/invalid results never modify output. info is required; it must not
 * alias capture or output. Output must not overlap the capture object.
 */
int tb710fu_live_capture_putc(struct tb710fu_live_capture *capture, char byte);
int tb710fu_live_capture_copy(struct tb710fu_live_capture *capture,
    uint8_t *out, size_t capacity, struct tb710fu_live_capture_info *info);

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
    enum tb710fu_live_log_source source;
    char filename[TB710FU_LIVE_LOG_NAME_BYTES];
};

/* One owner serializes these APIs. No allocation, waits, I/O or clock access.
 * snapshot copies the exact log bytes into an immutable LMS1 file envelope.
 * start/end are absolute byte offsets; end - start must equal length.
 * An empty log is reported as EMPTY and does not replace a valid transfer.
 * Once accepted, at least TB710FU_LIVE_LOG_MIN_ROUNDS complete source-block
 * rounds must be emitted before another snapshot may replace it. More rounds
 * may be sent indefinitely.
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
    enum tb710fu_live_log_source source;
    uint8_t envelope[TB710FU_LIVE_LOG_ENVELOPE_BYTES];
};

void tb710fu_live_log_sender_init(struct tb710fu_live_log_sender *sender);
/* Init defaults to RAMOOPS for existing raw users. Same-source selection is a
 * no-op. Changing source requires the minimum-round gate and clears the old
 * transfer, so an old envelope cannot be presented as the newly selected log.
 * Invalid enum or a busy transfer returns 0 and leaves all state unchanged.
 */
int tb710fu_live_log_set_source(struct tb710fu_live_log_sender *sender,
    enum tb710fu_live_log_source source);
enum tb710fu_live_log_result tb710fu_live_log_snapshot(
    struct tb710fu_live_log_sender *sender, const uint8_t *bytes, size_t length,
    uint64_t generation, uint64_t start, uint64_t end);
/* File bodies may include metadata and an immutable early backup as well as
 * the current source tail. The caller supplies that complete file unchanged.
 * Require nonempty body <=4096 and 0 <= end-start <= body length; start/end
 * still name only the actual current source range. An empty current range is
 * permitted for a metadata/backup-only file. The same minimum-round gate applies.
 * Neither snapshot API allows bytes to alias the sender's internal storage.
 */
enum tb710fu_live_log_result tb710fu_live_log_snapshot_file(
    struct tb710fu_live_log_sender *sender, const uint8_t *bytes, size_t length,
    uint64_t generation, uint64_t start, uint64_t end);
int tb710fu_live_log_next_frame(struct tb710fu_live_log_sender *sender,
    uint8_t out[TB710FU_LIVE_LOG_FRAME_BYTES]);
int tb710fu_live_log_can_snapshot(const struct tb710fu_live_log_sender *sender);
int tb710fu_live_log_get_info(const struct tb710fu_live_log_sender *sender,
    struct tb710fu_live_log_info *out);

#endif
