/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuNvt.h for provenance and the changes this port makes. */

#include "Tb710fuNvt.h"

#include <string.h>

/* ---- SPI framing ---- */

static int nvt_xfer(struct tb710fu_nvt_chip *chip, const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!chip || !chip->bus || !chip->bus->xfer || len == 0u)
        return TB710FU_NVT_ERR_PARAM;
    if (chip->bus->xfer(chip->bus->ctx, tx, rx, len) != 0)
        return TB710FU_NVT_ERR_BUS;
    return TB710FU_NVT_OK;
}

static int nvt_set_page(struct tb710fu_nvt_chip *chip, uint32_t address)
{
    uint8_t *scratch = chip->scratch;

    scratch[0] = 0xFFu;
    scratch[1] = (uint8_t)((address >> 15) & 0xFFu);
    scratch[2] = (uint8_t)((address >> 7) & 0xFFu);
    return nvt_xfer(chip, scratch, NULL, 3u);
}

int tb710fu_nvt_write(struct tb710fu_nvt_chip *chip, uint32_t address,
                      const uint8_t *data, size_t len)
{
    size_t done = 0;
    uint8_t *scratch;
    size_t chunk_cap;

    if (!chip || !chip->scratch || !data || len == 0u)
        return TB710FU_NVT_ERR_PARAM;
    scratch = chip->scratch;
    chunk_cap = chip->scratch_bytes > 1u ? chip->scratch_bytes - 1u : 0u;
    if (chunk_cap > TB710FU_NVT_WRITE_CHUNK) chunk_cap = TB710FU_NVT_WRITE_CHUNK;
    if (chunk_cap == 0u) return TB710FU_NVT_ERR_PARAM;

    while (done < len) {
        size_t chunk = len - done;
        int result;

        if (chunk > chunk_cap) chunk = chunk_cap;
        result = nvt_set_page(chip, address + (uint32_t)done);
        if (result != TB710FU_NVT_OK) return result;
        scratch[0] = (uint8_t)(((address + done) & 0x7Fu) | 0x80u);
        memcpy(&scratch[1], data + done, chunk);
        result = nvt_xfer(chip, scratch, NULL, chunk + 1u);
        if (result != TB710FU_NVT_OK) return result;
        done += chunk;
    }
    return TB710FU_NVT_OK;
}

int tb710fu_nvt_write_byte(struct tb710fu_nvt_chip *chip, uint32_t address, uint8_t value)
{
    return tb710fu_nvt_write(chip, address, &value, 1u);
}

int tb710fu_nvt_read(struct tb710fu_nvt_chip *chip, uint32_t address, uint8_t *data, size_t len)
{
    size_t done = 0;
    size_t chunk_cap;

    if (!chip || !chip->rx || !chip->scratch || !data || len == 0u)
        return TB710FU_NVT_ERR_PARAM;
    if (chip->rx_bytes < 3u || chip->scratch_bytes < 3u)
        return TB710FU_NVT_ERR_PARAM;
    chunk_cap = chip->rx_bytes - 2u;
    if (chunk_cap > chip->scratch_bytes - 2u) chunk_cap = chip->scratch_bytes - 2u;
    if (chunk_cap > TB710FU_NVT_READ_MAX) chunk_cap = TB710FU_NVT_READ_MAX;

    while (done < len) {
        size_t chunk = len - done;
        int result;

        if (chunk > chunk_cap) chunk = chunk_cap;
        result = nvt_set_page(chip, address + (uint32_t)done);
        if (result != TB710FU_NVT_OK) return result;
        chip->scratch[0] = (uint8_t)((address + done) & 0x7Fu);
        chip->scratch[1] = 0u;
        result = nvt_xfer(chip, chip->scratch, chip->rx, chunk + 2u);
        if (result != TB710FU_NVT_OK) return result;
        memcpy(data + done, &chip->rx[2], chunk);
        done += chunk;
    }
    return TB710FU_NVT_OK;
}

int tb710fu_nvt_chip_init(struct tb710fu_nvt_chip *chip, const struct tb710fu_nvt_bus *bus,
                          uint8_t *scratch, size_t scratch_bytes,
                          uint8_t *rx, size_t rx_bytes)
{
    if (!chip || !bus || !bus->xfer || !scratch || !rx ||
        scratch_bytes < 3u || rx_bytes < 3u)
        return TB710FU_NVT_ERR_PARAM;
    memset(chip, 0, sizeof(*chip));
    chip->bus = bus;
    chip->scratch = scratch;
    chip->scratch_bytes = scratch_bytes;
    chip->rx = rx;
    chip->rx_bytes = rx_bytes;
    chip->event_buf = TB710FU_NVT_EVENT_BUF;
    return TB710FU_NVT_OK;
}

int tb710fu_nvt_bootloader_reset(struct tb710fu_nvt_chip *chip)
{
    const uint8_t magic = 0x69u;
    int result;

    result = tb710fu_nvt_write(chip, TB710FU_NVT_SWRST_SIF, &magic, 1u);
    if (result == TB710FU_NVT_OK && chip->bus->delay_ms)
        chip->bus->delay_ms(chip->bus->ctx, 5u);
    return result;
}

static int nvt_engine_reset(struct tb710fu_nvt_chip *chip)
{
    const uint8_t magic = 0x5Au;
    int result;

    result = tb710fu_nvt_write(chip, TB710FU_NVT_ENG_RST_ADDR, &magic, 1u);
    if (result == TB710FU_NVT_OK && chip->bus->delay_ms) {
        chip->bus->delay_ms(chip->bus->ctx, 1u);
        chip->bus->delay_ms(chip->bus->ctx, 10u);
    }
    return result;
}

/* The bootloader exposes CHIP_VER_TRIM only after the vendor's 7-byte address
 * write; in application mode the same page read returns the id directly. */
static int nvt_read_chip_id(struct tb710fu_nvt_chip *chip, int bootloader_style)
{
    const uint32_t address = TB710FU_NVT_CHIP_VER_TRIM;
    int result;

    if (bootloader_style) {
        result = nvt_set_page(chip, address);
        if (result != TB710FU_NVT_OK) return result;
        memset(chip->scratch, 0, 7u);
        chip->scratch[0] = (uint8_t)((address & 0x7Fu) | 0x80u);
        result = nvt_xfer(chip, chip->scratch, NULL, 7u);
        if (result != TB710FU_NVT_OK) return result;
    }
    return tb710fu_nvt_read(chip, address, chip->id, sizeof(chip->id));
}

static int nvt_id_looks_like_nt36532(const uint8_t *id)
{
    /* id[3..5] = 0x32 0x65 0x03 identifies the NT36532 family in the vendor
     * protocol; the early work accepted the same three bytes. */
    return id[3] == 0x32u && id[4] == 0x65u && id[5] == 0x03u;
}

int tb710fu_nvt_identify(struct tb710fu_nvt_chip *chip, int reset_first)
{
    int attempt;
    unsigned attempts = reset_first ? 5u : 1u;

    if (!chip || !chip->bus) return TB710FU_NVT_ERR_PARAM;

    chip->identified = 0;
    if (reset_first && nvt_engine_reset(chip) != TB710FU_NVT_OK)
        return TB710FU_NVT_ERR_BUS;

    for (attempt = 0; attempt < (int)attempts; ++attempt) {
        int result;

        if (reset_first) {
            result = tb710fu_nvt_bootloader_reset(chip);
            if (result != TB710FU_NVT_OK) return result;
        }
        memset(chip->id, 0, sizeof(chip->id));
        result = nvt_read_chip_id(chip, reset_first);
        if (result != TB710FU_NVT_OK) return result;
        if (nvt_id_looks_like_nt36532(chip->id)) break;
        if (!reset_first) return TB710FU_NVT_ERR_CHIP;
        if (chip->bus->delay_ms) chip->bus->delay_ms(chip->bus->ctx, 10u);
    }
    if (attempt >= (int)attempts) return TB710FU_NVT_ERR_CHIP;

    chip->is_e = (chip->id[0] == 0x0Eu);
    chip->cascade = 0;
    if (!reset_first) {
        /* ENB_CASC bit 0 set means a single die; only a successful read is
         * allowed to change the event buffer. */
        uint8_t casc = 0u;
        int result = tb710fu_nvt_read(chip, TB710FU_NVT_ENB_CASC, &casc, 1u);

        if (result == TB710FU_NVT_OK) chip->cascade = (casc & 0x01u) == 0u;
    }
    chip->event_buf = (chip->is_e && chip->cascade) ? TB710FU_NVT_EVENT_BUF_E
                                                    : TB710FU_NVT_EVENT_BUF;
    chip->identified = 1;
    return TB710FU_NVT_OK;
}

int tb710fu_nvt_read_frame(struct tb710fu_nvt_chip *chip, uint8_t *frame)
{
    if (!chip || !frame) return TB710FU_NVT_ERR_PARAM;
    frame[0] = 0u;
    return tb710fu_nvt_read(chip, chip->event_buf, &frame[1], TB710FU_NVT_FRAME_BYTES);
}

int tb710fu_nvt_frame_parse(const uint8_t *frame, size_t bytes, struct tb710fu_nvt_peek *peek)
{
    const size_t base = TB710FU_NVT_PEEK_OFFSET;
    uint16_t marker;
    uint16_t marker_inv;
    unsigned type;

    if (!frame || !peek || bytes < TB710FU_NVT_FRAME_BYTES)
        return 0;
    marker = (uint16_t)((uint16_t)frame[base + TB710FU_NVT_PEEK_MARKER] |
                        ((uint16_t)frame[base + TB710FU_NVT_PEEK_MARKER + 1u] << 8));
    marker_inv = (uint16_t)((uint16_t)frame[base + TB710FU_NVT_PEEK_MARKER_INV] |
                            ((uint16_t)frame[base + TB710FU_NVT_PEEK_MARKER_INV + 1u] << 8));
    peek->counter = (unsigned)((uint16_t)frame[base + TB710FU_NVT_PEEK_COUNTER] |
                               ((uint16_t)frame[base + TB710FU_NVT_PEEK_COUNTER + 1u] << 8));
    type = frame[base + TB710FU_NVT_PEEK_TYPE];
    peek->type = type;
    peek->marker = marker;
    peek->marker_inv = marker_inv;
    /* The marker and its complement are the framing invariant the vendor
     * firmware maintains; a mismatch means the burst was truncated or the
     * controller was mid-update. Absolute pixel validity is not claimed. */
    peek->header_ok = marker != 0u && ((marker ^ marker_inv) == 0xFFFFu) &&
                      (type == TB710FU_NVT_FRAME_TYPE_BASELINE ||
                       type == TB710FU_NVT_FRAME_TYPE_TOUCH);
    return peek->header_ok;
}

void tb710fu_nvt_matrix_decode(const uint8_t *frame, int16_t *matrix)
{
    size_t i;

    if (!frame || !matrix) return;
    for (i = 0u; i < TB710FU_NVT_NODES; ++i) {
        const size_t offset = TB710FU_NVT_MATRIX_OFFSET + i * 2u;

        matrix[i] = (int16_t)((uint16_t)frame[offset] | ((uint16_t)frame[offset + 1u] << 8));
    }
}

int32_t tb710fu_nvt_node_x(int column, int32_t panel_width)
{
    if (column < 0) column = 0;
    if (column >= (int)TB710FU_NVT_COLUMNS) column = (int)TB710FU_NVT_COLUMNS - 1;
    if (panel_width <= 0) return 0;
    /* Pixel centre of the cell this node owns: ((2c + 1) * width) / (2 * C). */
    return (int32_t)(((long long)(2 * column + 1) * panel_width * TB710FU_NVT_COORD_UNITS) /
                     (2 * (long long)TB710FU_NVT_COLUMNS));
}

int32_t tb710fu_nvt_node_y(int row, int32_t panel_height)
{
    if (row < 0) row = 0;
    if (row >= (int)TB710FU_NVT_ROWS) row = (int)TB710FU_NVT_ROWS - 1;
    if (panel_height <= 0) return 0;
    return (int32_t)(((long long)(2 * row + 1) * panel_height * TB710FU_NVT_COORD_UNITS) /
                     (2 * (long long)TB710FU_NVT_ROWS));
}

void tb710fu_nvt_core_reset(struct tb710fu_nvt_core *core)
{
    if (!core) return;
    memset(core, 0, sizeof(*core));
}

/* ---- thresholds (vendor calibration constants, same family as the reference) ---- */
#define NVT_NOISE       100
#define NVT_UP          220
#define NVT_DOWN        300
#define NVT_COORD_REDUCE 25
#define NVT_FLOOR_PERCENT 30
#define NVT_MERGE_SPAN  2
#define NVT_MIN_AREA    2
#define NVT_PALM_NODES  60
#define NVT_REF_RATE    32
#define NVT_REF_SETTLE  8

/* Walk uphill until no neighbour is strictly larger. Ties never move, so the
 * walk cannot cycle; the step cap only guards against a corrupted delta array. */
static int nvt_terminal_node(const int16_t *delta, int node)
{
    int steps = 0;

    while (steps++ < (int)TB710FU_NVT_NODES) {
        const int row = node / (int)TB710FU_NVT_COLUMNS;
        const int column = node % (int)TB710FU_NVT_COLUMNS;
        int best = node;

        for (int r = row - 1; r <= row + 1; ++r) {
            for (int c = column - 1; c <= column + 1; ++c) {
                int neighbour;

                if (r < 0 || r >= (int)TB710FU_NVT_ROWS ||
                    c < 0 || c >= (int)TB710FU_NVT_COLUMNS ||
                    (r == row && c == column))
                    continue;
                neighbour = r * (int)TB710FU_NVT_COLUMNS + c;
                if (delta[neighbour] > delta[best]) best = neighbour;
            }
        }
        if (best == node) return node;
        node = best;
    }
    return -1;
}

static int peak_index_of(const int *peak_node, int peaks, int node)
{
    int i;

    for (i = 0; i < peaks; ++i)
        if (peak_node[i] == node) return i;
    return -1;
}

static int alias_find(int *alias, int index)
{
    int guard = 0;

    while (alias[index] != index && guard++ < (int)TB710FU_NVT_MAX_PEAKS)
        index = alias[index];
    return index;
}

int tb710fu_nvt_core_process(struct tb710fu_nvt_core *core, const uint8_t *frame,
                             int32_t panel_width, int32_t panel_height,
                             struct tb710fu_nvt_contact *out, int max)
{
    int16_t matrix[TB710FU_NVT_NODES];
    int peak_node[TB710FU_NVT_MAX_PEAKS];
    int peak_value[TB710FU_NVT_MAX_PEAKS];
    int peak_alias[TB710FU_NVT_MAX_PEAKS];
    int peaks = 0;
    int candidates = 0;
    int frame_type;
    struct tb710fu_nvt_peek peek;
    int node;

    if (!core || !frame || !out || max <= 0 || panel_width <= 0 || panel_height <= 0)
        return TB710FU_NVT_ERR_PARAM;
    if (!tb710fu_nvt_frame_parse(frame, TB710FU_NVT_FRAME_BYTES, &peek))
        return TB710FU_NVT_ERR_FRAME;
    frame_type = (int)peek.type;
    core->frames_by_type[frame_type == (int)TB710FU_NVT_FRAME_TYPE_BASELINE ? 0 : 1]++;

    tb710fu_nvt_matrix_decode(frame, matrix);

    /* Baseline frames seed the reference directly; they are only emitted while
     * the panel is quiet. Without one the first touch frame is a seed too. */
    if (frame_type == (int)TB710FU_NVT_FRAME_TYPE_BASELINE ||
        !core->have_reference) {
        memcpy(core->reference, matrix, sizeof(core->reference));
        core->have_reference = 1;
        core->quiet_frames = 0u;
        return 0;
    }

    for (node = 0; node < (int)TB710FU_NVT_NODES; ++node)
        core->delta[node] = (int16_t)((int)matrix[node] - (int)core->reference[node]);
    memset(core->label, 0, sizeof(core->label));

    /* Candidate peaks: terminals of the ascent field that clear the up
     * threshold, merged pairwise within the span so one finger yields one. */
    for (node = 0; node < (int)TB710FU_NVT_NODES; ++node) {
        int terminal;

        if (core->delta[node] < NVT_NOISE) continue;
        terminal = nvt_terminal_node(core->delta, node);
        if (terminal < 0) continue;
        if (core->delta[terminal] < NVT_UP) continue;
        if (peak_index_of(peak_node, peaks, terminal) >= 0) continue;
        if (peaks >= (int)TB710FU_NVT_MAX_PEAKS) break;
        peak_node[peaks] = terminal;
        peak_value[peaks] = core->delta[terminal];
        peak_alias[peaks] = peaks;
        peaks++;
    }

    for (int i = 0; i < peaks; ++i) {
        for (int j = i + 1; j < peaks; ++j) {
            const int ri = peak_node[i] / (int)TB710FU_NVT_COLUMNS;
            const int ci = peak_node[i] % (int)TB710FU_NVT_COLUMNS;
            const int rj = peak_node[j] / (int)TB710FU_NVT_COLUMNS;
            const int cj = peak_node[j] % (int)TB710FU_NVT_COLUMNS;

            if (ri - rj <= NVT_MERGE_SPAN && rj - ri <= NVT_MERGE_SPAN &&
                ci - cj <= NVT_MERGE_SPAN && cj - ci <= NVT_MERGE_SPAN) {
                const int weaker = peak_value[i] >= peak_value[j] ? j : i;
                const int stronger = weaker == i ? j : i;

                peak_alias[weaker] = stronger;
                peak_value[weaker] = 0;
            }
        }
    }

    /* Accumulate every clearing node onto its peak's canonical root. */
    {
        long long sum_w[TB710FU_NVT_MAX_PEAKS];
        long long sum_x[TB710FU_NVT_MAX_PEAKS];
        long long sum_y[TB710FU_NVT_MAX_PEAKS];
        int area[TB710FU_NVT_MAX_PEAKS];
        int peak[TB710FU_NVT_MAX_PEAKS];

        memset(sum_w, 0, sizeof(sum_w));
        memset(sum_x, 0, sizeof(sum_x));
        memset(sum_y, 0, sizeof(sum_y));
        memset(area, 0, sizeof(area));
        memset(peak, 0, sizeof(peak));

        for (node = 0; node < (int)TB710FU_NVT_NODES; ++node) {
            int terminal;
            int index;
            int root;
            int floor_weight;
            int weight;

            if (core->delta[node] <= NVT_NOISE) continue;
            terminal = nvt_terminal_node(core->delta, node);
            if (terminal < 0) continue;
            index = peak_index_of(peak_node, peaks, terminal);
            if (index < 0) continue;
            root = alias_find(peak_alias, index);
            if (peak_value[root] < NVT_UP) continue;
            floor_weight = (peak_value[root] * NVT_FLOOR_PERCENT) / 100;
            if (floor_weight < NVT_COORD_REDUCE) floor_weight = NVT_COORD_REDUCE;
            weight = (int)core->delta[node] - floor_weight;
            if (weight <= 0) continue;
            sum_w[root] += weight;
            sum_x[root] += (long long)weight *
                           tb710fu_nvt_node_x(node % (int)TB710FU_NVT_COLUMNS, panel_width);
            sum_y[root] += (long long)weight *
                           tb710fu_nvt_node_y(node / (int)TB710FU_NVT_COLUMNS, panel_height);
            area[root]++;
            if (core->delta[node] > peak[root]) peak[root] = core->delta[node];
            core->label[node] = (uint8_t)(root + 1);
        }

        for (int i = 0; i < peaks; ++i) {
            struct tb710fu_nvt_contact contact;
            int insert;
            int j;

            if (peak_alias[i] != i || peak_value[i] < NVT_DOWN) continue;
            if (area[i] < NVT_MIN_AREA || sum_w[i] <= 0) continue;
            if (area[i] >= NVT_PALM_NODES) continue;

            contact.slot = 0u; /* replaced by the tracker */
            contact.x = (int32_t)(sum_x[i] / sum_w[i] / TB710FU_NVT_COORD_UNITS);
            contact.y = (int32_t)(sum_y[i] / sum_w[i] / TB710FU_NVT_COORD_UNITS);
            contact.peak = peak[i] > 0 ? peak[i] : peak_value[i];
            contact.area = area[i];
            contact.weight = (int32_t)(sum_w[i] > 0x7FFFFFFF ? 0x7FFFFFFF : sum_w[i]);

            if (candidates == max && contact.peak <= out[max - 1].peak) continue;
            insert = candidates < max ? candidates : max - 1;
            for (j = insert; j > 0; --j) {
                if (out[j - 1].peak >= contact.peak) break;
                out[j] = out[j - 1];
            }
            out[j] = contact;
            if (candidates < max) candidates++;
        }
    }

    if (candidates == 0) {
        if (core->quiet_frames < NVT_REF_SETTLE) {
            core->quiet_frames++;
        } else {
            for (node = 0; node < (int)TB710FU_NVT_NODES; ++node) {
                int step = (int)core->delta[node] / NVT_REF_RATE;
                int updated;

                if (step == 0) continue;
                updated = (int)core->reference[node] + step;
                if (updated > 32767) updated = 32767;
                if (updated < -32768) updated = -32768;
                core->reference[node] = (int16_t)updated;
            }
        }
    } else {
        core->quiet_frames = 0u;
    }
    return candidates > max ? max : candidates;
}
