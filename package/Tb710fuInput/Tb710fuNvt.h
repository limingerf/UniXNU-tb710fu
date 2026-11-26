/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Novatek NT36532 wire protocol and frame decoding.
 *
 * Provenance, both parts explicitly not board facts:
 *   - the SPI wire protocol (0xFF page select, 0x80 write, 0x40 read, the
 *     0x3F command acknowledgement window) and the controller RAM addresses
 *     are the NT36532 vendor protocol as ported by UniXNU/ShengNovatekTouch
 *     (drivers/ShengNovatekTouch/NvtProtocol.c, revision 3b44b1b7). The
 *     controller is the same part the TB710FU DTS names
 *     ("novatek,NVT-ts-spi", firmware-name "novatek/novatek_nt36532_fw.bin").
 *   - the frame layout (257-byte header, 0x40-byte peek block, 60x40 mutual
 *     nodes at 0x40 alignment) is the raw-matrix frame the shipped firmware
 *     emits at the event buffer, same source.
 *
 * What this port changes relative to the early work:
 *   - identify is non-destructive by default. The old path always reset the
 *     engine and the bootloader before reading the chip id, which kills a
 *     controller the firmware has already trained. Reset is now an explicit
 *     argument and the firmware download path is the only caller that passes
 *     it.
 *   - node geometry is computed from the declared panel extent instead of a
 *     copied 0.1 mm table: node (row, column) maps to the pixel centre of its
 *     cell in a 60x40 grid over TB710FU_PANEL_WIDTH x TB710FU_PANEL_HEIGHT.
 *     The absolute sensor pitch is not in any pinned source, so it stays a
 *     calibration item (docs/INPUT_TOUCH_KEYS.md) rather than a hardcoded
 *     number from another board.
 *   - every read is length-checked and every framing step is bounded.
 */
#ifndef TB710FU_NVT_H
#define TB710FU_NVT_H

#include <stdint.h>
#include <stddef.h>

/* ---- controller RAM (vendor protocol addresses) ---- */
#define TB710FU_NVT_EVENT_BUF        0x125800u  /* raw frame buffer, single die  */
#define TB710FU_NVT_EVENT_BUF_E      0x11C400u  /* NT36532E cascade              */
#define TB710FU_NVT_BOOT_RDY         0x1FB50Du
#define TB710FU_NVT_TX_AUTO_COPY_EN  0x1FC925u
#define TB710FU_NVT_ILM_DES          0x1FB528u
#define TB710FU_NVT_ILM_LENGTH       0x1FB518u
#define TB710FU_NVT_DLM_DES          0x1FB52Cu
#define TB710FU_NVT_DLM_LENGTH       0x1FB530u
#define TB710FU_NVT_G_ILM_CHECKSUM   0x1FB500u
#define TB710FU_NVT_G_DLM_CHECKSUM   0x1FB504u
#define TB710FU_NVT_R_ILM_CHECKSUM   0x1FB520u
#define TB710FU_NVT_R_DLM_CHECKSUM   0x1FB524u
#define TB710FU_NVT_BLD_ILM_DLM_CRC  0x1FB533u
#define TB710FU_NVT_ENB_CASC         0x1FB12Cu
#define TB710FU_NVT_CHIP_VER_TRIM    0x1FB104u
#define TB710FU_NVT_SWRST_SIF        0x1FB43Eu
#define TB710FU_NVT_ENG_RST_ADDR     0x7FFF80u

/* ---- frame geometry ---- */
#define TB710FU_NVT_HEADER_BYTES     257u
#define TB710FU_NVT_PAYLOAD_BYTES    5160u
#define TB710FU_NVT_FRAME_BYTES      (TB710FU_NVT_HEADER_BYTES + TB710FU_NVT_PAYLOAD_BYTES)
#define TB710FU_NVT_READ_BYTES       (TB710FU_NVT_FRAME_BYTES + 1u) /* + status byte */
#define TB710FU_NVT_PEEK_OFFSET      TB710FU_NVT_HEADER_BYTES
#define TB710FU_NVT_PEEK_BYTES       32u
#define TB710FU_NVT_PEEK_COUNTER     2u    /* uint16 LE */
#define TB710FU_NVT_PEEK_MARKER      8u    /* uint16 LE */
#define TB710FU_NVT_PEEK_CRC16       4u
#define TB710FU_NVT_PEEK_CRC16_INV   12u
#define TB710FU_NVT_PEEK_MARKER_INV  16u
#define TB710FU_NVT_PEEK_TYPE        24u   /* low byte */
#define TB710FU_NVT_MATRIX_OFFSET    (TB710FU_NVT_HEADER_BYTES + 0x40u)
#define TB710FU_NVT_FRAME_TYPE_BASELINE 2u
#define TB710FU_NVT_FRAME_TYPE_TOUCH    4u

/* ---- sensor matrix (60 columns x 40 rows, vendor frame layout) ---- */
#define TB710FU_NVT_COLUMNS          60u
#define TB710FU_NVT_ROWS             40u
#define TB710FU_NVT_NODES            (TB710FU_NVT_COLUMNS * TB710FU_NVT_ROWS)
#define TB710FU_NVT_MAX_CONTACTS     10u
#define TB710FU_NVT_MAX_PEAKS        16u
#define TB710FU_NVT_COORD_UNITS      16   /* sub-pixel fixed point for centroids */

/* ---- wire transfer limits ---- */
#define TB710FU_NVT_READ_MAX         8192u
#define TB710FU_NVT_WRITE_CHUNK      62u   /* payload bytes per page-select write */

/* ---- status ---- */
#define TB710FU_NVT_OK               0
#define TB710FU_NVT_ERR_PARAM        (-22)
#define TB710FU_NVT_ERR_BUS          (-5)
#define TB710FU_NVT_ERR_CHIP         (-19)
#define TB710FU_NVT_ERR_FRAME        (-8)
#define TB710FU_NVT_ERR_TIMEOUT      (-110)
#define TB710FU_NVT_ERR_STATE        (-75)
#define TB710FU_NVT_ERR_FW           (-42)   /* firmware header inconsistent */

/* One SPI transaction. The driver gives this to GENI; the host tests provide
 * a model. `len` covers the whole transaction including the command byte. */
struct tb710fu_nvt_bus {
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *ctx, unsigned milliseconds);
    void *ctx;
};

struct tb710fu_nvt_chip {
    const struct tb710fu_nvt_bus *bus;
    uint8_t *scratch;
    size_t scratch_bytes;
    uint8_t *rx;
    size_t rx_bytes;
    uint32_t event_buf;
    uint8_t id[6];
    int identified;
    int is_e;
    int cascade;
    unsigned frames;
    unsigned frame_errors;
};

struct tb710fu_nvt_peek {
    unsigned counter;
    unsigned type;
    uint16_t marker;
    uint16_t marker_inv;
    int header_ok;
};

struct tb710fu_nvt_contact {
    uint8_t slot;      /* stable tracking id, assigned by the tracker */
    int32_t x;         /* panel pixel, 0..logical_max_x */
    int32_t y;         /* panel pixel, 0..logical_max_y */
    int32_t peak;      /* delta at the peak node */
    int32_t area;      /* contributing nodes */
    int32_t weight;    /* sum of centroid weights */
};

int tb710fu_nvt_chip_init(struct tb710fu_nvt_chip *chip, const struct tb710fu_nvt_bus *bus,
                          uint8_t *scratch, size_t scratch_bytes,
                          uint8_t *rx, size_t rx_bytes);

int tb710fu_nvt_read(struct tb710fu_nvt_chip *chip, uint32_t address, uint8_t *data, size_t len);
int tb710fu_nvt_write(struct tb710fu_nvt_chip *chip, uint32_t address, const uint8_t *data,
                      size_t len);
int tb710fu_nvt_write_byte(struct tb710fu_nvt_chip *chip, uint32_t address, uint8_t value);

/* Read the chip id without touching controller state. `expect_new_chip` is the
 * bring-up knob: when non-zero the controller is taken through the vendor
 * reset sequence first (engine reset 0x5A at 0x7FFF80, bootloader reset 0x69
 * at SWRST_SIF) which is required only when the firmware has to be reloaded. */
int tb710fu_nvt_identify(struct tb710fu_nvt_chip *chip, int reset_first);

int tb710fu_nvt_bootloader_reset(struct tb710fu_nvt_chip *chip);

/* Read the whole raw frame. `frame` must hold TB710FU_NVT_READ_BYTES; frame[0]
 * is the zero offset byte the controller needs in front of the burst. */
int tb710fu_nvt_read_frame(struct tb710fu_nvt_chip *chip, uint8_t *frame);

/* Header-only integrity check: the peek marker and its complement, plus a
 * plausible frame counter/type. Returns 1 when the header looks coherent. */
int tb710fu_nvt_frame_parse(const uint8_t *frame, size_t bytes, struct tb710fu_nvt_peek *peek);

/* Decode the 60x40 node area into signed deltas. */
void tb710fu_nvt_matrix_decode(const uint8_t *frame, int16_t *matrix);

/* Node centre in panel pixels (fixed point TB710FU_NVT_COORD_UNITS). */
int32_t tb710fu_nvt_node_x(int column, int32_t panel_width);
int32_t tb710fu_nvt_node_y(int row, int32_t panel_height);

struct tb710fu_nvt_core {
    int16_t reference[TB710FU_NVT_NODES];
    int16_t delta[TB710FU_NVT_NODES];
    uint8_t label[TB710FU_NVT_NODES];
    int have_reference;
    unsigned quiet_frames;
    unsigned frames_by_type[2];
};

void tb710fu_nvt_core_reset(struct tb710fu_nvt_core *core);

/* Turn one frame into up to `max` contacts. Returns the contact count, or a
 * negative TB710FU_NVT_ERR_* value for a malformed frame. */
int tb710fu_nvt_core_process(struct tb710fu_nvt_core *core, const uint8_t *frame,
                             int32_t panel_width, int32_t panel_height,
                             struct tb710fu_nvt_contact *out, int max);

#endif /* TB710FU_NVT_H */
