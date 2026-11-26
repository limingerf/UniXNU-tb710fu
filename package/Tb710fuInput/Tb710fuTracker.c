/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuTracker.h for the contract. */

#include "Tb710fuTracker.h"

#include <string.h>

static int32_t clamp_axis(int32_t value, int32_t maximum)
{
    if (maximum < 0) maximum = 0;
    if (value < 0) return 0;
    if (value > maximum) return maximum;
    return value;
}

static int32_t distance(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    const int64_t dx = (int64_t)ax - bx;
    const int64_t dy = (int64_t)ay - by;
    const int64_t d = dx * dx + dy * dy;

    return d > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)d;
}

void tb710fu_tracker_init(struct tb710fu_tracker *tracker, int32_t max_x, int32_t max_y)
{
    if (!tracker) return;
    memset(tracker, 0, sizeof(*tracker));
    tracker->max_x = max_x > 0 ? max_x : 0;
    tracker->max_y = max_y > 0 ? max_y : 0;
    /* A finger that moved less than a tenth of the short axis between two
     * frames is the same finger; anything beyond that is a new contact. */
    tracker->match_radius = (max_y > 0 ? max_y : 1000) / 10;
    tracker->jitter = 2;
    tracker->max_missing = 3u;
    tracker->next_id = 1u;
}

int tb710fu_tracker_update(struct tb710fu_tracker *tracker,
                           const struct tb710fu_track_input *in, int count,
                           struct tb710fu_track_point *out, int max)
{
    int used[TB710FU_TRACK_SLOTS];
    int matched[TB710FU_TRACK_SLOTS];
    int written = 0;
    int i;
    int slot;

    if (!tracker || !out || max <= 0) return -1;
    if (count < 0) count = 0;
    if (count > TB710FU_TRACK_SLOTS) count = TB710FU_TRACK_SLOTS;
    for (i = 0; i < TB710FU_TRACK_SLOTS; ++i) {
        used[i] = 0;
        matched[i] = -1;
    }

    /* Greedy nearest-neighbour: for every input, pick the closest unused active
     * slot inside the gate. Deterministic for a given frame order. */
    for (i = 0; i < count; ++i) {
        int32_t best = 0x7FFFFFFF;
        int best_slot = -1;
        const int32_t gate = tracker->match_radius * tracker->match_radius;

        for (slot = 0; slot < TB710FU_TRACK_SLOTS; ++slot) {
            int32_t d;

            if (!tracker->slot[slot].active || used[slot]) continue;
            d = distance(in[i].x, in[i].y, tracker->slot[slot].x, tracker->slot[slot].y);
            if (d <= gate && d < best) {
                best = d;
                best_slot = slot;
            }
        }
        if (best_slot >= 0) {
            struct tb710fu_track_slot *s = &tracker->slot[best_slot];
            const int32_t x = clamp_axis(in[i].x, tracker->max_x);
            const int32_t y = clamp_axis(in[i].y, tracker->max_y);

            /* Sub-jitter movement keeps the previous coordinate so a stationary
             * finger does not stream 1 px changes every frame. */
            if (distance(x, y, s->x, s->y) > tracker->jitter * tracker->jitter) {
                s->x = x;
                s->y = y;
            }
            s->pressure = in[i].pressure;
            s->missing = 0u;
            used[best_slot] = 1;
            matched[i] = best_slot;
        }
    }

    /* Unmatched inputs open a fresh slot when one is free. */
    for (i = 0; i < count; ++i) {
        if (matched[i] >= 0) continue;
        for (slot = 0; slot < TB710FU_TRACK_SLOTS; ++slot) {
            if (tracker->slot[slot].active) continue;
            tracker->slot[slot].active = 1;
            tracker->slot[slot].id = tracker->next_id;
            tracker->slot[slot].x = clamp_axis(in[i].x, tracker->max_x);
            tracker->slot[slot].y = clamp_axis(in[i].y, tracker->max_y);
            tracker->slot[slot].pressure = in[i].pressure;
            tracker->slot[slot].missing = 0u;
            used[slot] = 1;
            matched[i] = slot;
            tracker->next_id++;
            if (tracker->next_id == 0u) tracker->next_id = 1u;
            break;
        }
    }

    /* Age slots that were not refreshed in this frame. */
    for (slot = 0; slot < TB710FU_TRACK_SLOTS; ++slot) {
        if (!tracker->slot[slot].active || used[slot]) continue;
        tracker->slot[slot].missing++;
        if (tracker->slot[slot].missing > tracker->max_missing) {
            tracker->slot[slot].active = 0;
            tracker->slot[slot].missing = 0u;
        }
    }

    for (slot = 0; slot < TB710FU_TRACK_SLOTS && written < max; ++slot) {
        if (!tracker->slot[slot].active) continue;
        if (tracker->slot[slot].missing != 0u) continue;
        out[written].id = tracker->slot[slot].id;
        out[written].x = tracker->slot[slot].x;
        out[written].y = tracker->slot[slot].y;
        out[written].pressure = tracker->slot[slot].pressure;
        written++;
    }
    return written;
}

int tb710fu_tracker_flush(struct tb710fu_tracker *tracker, uint8_t *lifted, int max)
{
    int count = 0;
    int slot;

    if (!tracker || !lifted || max <= 0) return 0;
    for (slot = 0; slot < TB710FU_TRACK_SLOTS; ++slot) {
        if (!tracker->slot[slot].active) continue;
        if (count < max) lifted[count++] = tracker->slot[slot].id;
        tracker->slot[slot].active = 0;
        tracker->slot[slot].missing = 0u;
    }
    return count;
}
