/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Multi-touch contact tracking for the TB710FU digitizer.
 *
 * The frame decoder reports anonymous blobs per frame; a HID digitizer needs
 * a contact identifier that survives across frames, otherwise every report
 * looks like a new finger and the system cannot drag or pinch. This tracker
 * matches this frame's blobs to the previous frame's slots by nearest
 * neighbour inside a radius gate, keeps the identifier while the blob exists,
 * ages a slot out after `max_missing` frames, and clamps every coordinate to
 * the declared panel extent so a calibration overshoot can never reach the
 * HID descriptor's logical maximum.
 */
#ifndef TB710FU_TRACKER_H
#define TB710FU_TRACKER_H

#include <stdint.h>

#define TB710FU_TRACK_SLOTS 10

struct tb710fu_track_input {
    int32_t x;
    int32_t y;
    int32_t pressure;   /* peak delta, arbitrary units */
};

struct tb710fu_track_point {
    uint8_t id;         /* stable contact identifier */
    int32_t x;          /* clamped panel pixel */
    int32_t y;          /* clamped panel pixel */
    int32_t pressure;
};

struct tb710fu_track_slot {
    int active;
    uint8_t id;
    int32_t x;
    int32_t y;
    int32_t pressure;
    unsigned missing;
};

struct tb710fu_tracker {
    struct tb710fu_track_slot slot[TB710FU_TRACK_SLOTS];
    uint8_t next_id;
    int32_t max_x;
    int32_t max_y;
    int32_t match_radius;  /* squared-distance gate per axis */
    int32_t jitter;        /* movement below this keeps the last coordinate */
    unsigned max_missing;
};

void tb710fu_tracker_init(struct tb710fu_tracker *tracker, int32_t max_x, int32_t max_y);

/* Feed one frame. Returns the number of contacts written to `out` (the blobs
 * seen in this frame, with stable ids and clamped coordinates). */
int tb710fu_tracker_update(struct tb710fu_tracker *tracker,
                           const struct tb710fu_track_input *in, int count,
                           struct tb710fu_track_point *out, int max);

/* Drop every active contact (used when the panel goes away or the frame
 * stream stalls), returning the identifiers that were lifted. */
int tb710fu_tracker_flush(struct tb710fu_tracker *tracker, uint8_t *lifted, int max);

#endif /* TB710FU_TRACKER_H */
