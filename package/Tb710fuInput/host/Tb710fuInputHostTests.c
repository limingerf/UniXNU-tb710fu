/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host-side unit tests for the TB710FU input subsystem.
 *
 * Compiled and run by scripts/check-tb710fu-input-host.py with the distribution
 * C compiler. Nothing here touches IOKit: the GENI engine, the NT36532 wire
 * protocol and the SPMI arbiter are driven through the same callback interfaces
 * the probe installs, against software models of the hardware.
 *
 * The models are deliberately strict about what they answer: a GENI register
 * read returns the modelled word (so an unmodelled write shows up as a failed
 * expectation instead of being silently swallowed), and the NVT model decodes
 * the page-select/write/read opcodes so the transport is exercised end to end.
 */

#include "Tb710fuGeni.h"
#include "Tb710fuHid.h"
#include "Tb710fuInputHardware.h"
#include "Tb710fuKeys.h"
#include "Tb710fuNvt.h"
#include "Tb710fuNvtFirmware.h"
#include "Tb710fuSpmi.h"
#include "Tb710fuTracker.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(condition)                                                               \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(condition)) {                                                            \
            g_failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                \
        }                                                                              \
    } while (0)

#define CHECK_EQ(value, expected)                                                      \
    do {                                                                               \
        const long long got_ = (long long)(value);                                     \
        const long long want_ = (long long)(expected);                                 \
        g_checks++;                                                                    \
        if (got_ != want_) {                                                           \
            g_failures++;                                                              \
            printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__,       \
                   #value, got_, want_);                                               \
        }                                                                              \
    } while (0)

/* ================================ GENI model ================================ */

struct geni_model {
    uint32_t regs[0x1000 / 4];   /* GENI registers reach 0xE30 */
    uint32_t tx[256];
    int tx_count;
    uint32_t rx[256];
    int rx_count;
    unsigned rx_last_valid;
    uint32_t irq;
    int cmd_pending;
    int cmd_opcode;
    size_t cmd_len;
    unsigned fuse_reads;
    unsigned reads;
    int no_completion;
    int cancel_ack;
    unsigned delay_us;
};

static struct geni_model *model_of(void *ctx)
{
    return (struct geni_model *)ctx;
}

static void model_complete(struct geni_model *model)
{
    int i;

    if (!model->cmd_pending) return;
    if (model->cmd_opcode == (int)TB710FU_GENI_M_CMD_TX_ONLY) {
        if ((size_t)(model->tx_count * 4) < model->cmd_len) return;   /* still filling */
        model->irq |= TB710FU_GENI_M_CMD_DONE;
        model->cmd_pending = 0;
        model->tx_count = 0;
        return;
    }
    if (model->cmd_opcode == (int)TB710FU_GENI_M_CMD_RX_ONLY) {
        const int words = (int)((model->cmd_len + 3u) / 4u);

        model->rx_count = 0;
        while (model->rx_count < words && model->rx_count < 256) model->rx[model->rx_count++] = 0u;
        model->rx_last_valid = (unsigned)(model->cmd_len & 3u);
        if (model->rx_last_valid == 0u) model->rx_last_valid = 4u;
        model->irq |= TB710FU_GENI_M_CMD_DONE | TB710FU_GENI_M_RX_FIFO_LAST;
        model->cmd_pending = 0;
        return;
    }
    if ((size_t)(model->tx_count * 4) < model->cmd_len) return;   /* still filling */
    /* SPI loopback: the slave shifts back what was shifted in. */
    model->rx_count = 0;
    for (i = 0; i < model->tx_count; ++i) model->rx[model->rx_count++] = model->tx[i];
    model->rx_last_valid = (unsigned)(model->cmd_len & 3u);
    if (model->rx_last_valid == 0u) model->rx_last_valid = 4u;
    model->irq |= TB710FU_GENI_M_CMD_DONE | TB710FU_GENI_M_RX_FIFO_LAST;
    model->cmd_pending = 0;
    model->tx_count = 0;
}

static uint32_t model_read(void *ctx, uint32_t offset)
{
    struct geni_model *model = model_of(ctx);

    model->reads++;
    switch (offset) {
    case TB710FU_GENI_IF_DISABLE_RO:
        model->fuse_reads++;
        return model->regs[offset / 4];
    case TB710FU_GENI_M_IRQ_STATUS:
        if (!model->no_completion) model_complete(model);
        return model->irq;
    case TB710FU_GENI_TX_FIFO_STATUS:
        return (uint32_t)model->tx_count;
    case TB710FU_GENI_RX_FIFO_STATUS: {
        uint32_t status = (uint32_t)model->rx_count;

        if (model->rx_count > 0)
            status |= TB710FU_GENI_RX_LAST |
                      ((uint32_t)(model->rx_last_valid & 7u) << TB710FU_GENI_RX_LAST_VALID_SHIFT);
        return status;
    }
    case TB710FU_GENI_RX_FIFO: {
        uint32_t word = 0u;

        if (model->rx_count > 0) {
            word = model->rx[0];
            memmove(&model->rx[0], &model->rx[1], sizeof(model->rx[0]) * (size_t)(model->rx_count - 1));
            model->rx_count--;
            if (model->rx_count == 0) {
                model->irq &= ~(TB710FU_GENI_M_RX_FIFO_LAST | TB710FU_GENI_M_RX_FIFO_WATERMARK);
            }
        }
        return word;
    }
    default:
        return model->regs[offset / 4];
    }
}

static void model_write(void *ctx, uint32_t offset, uint32_t value)
{
    struct geni_model *model = model_of(ctx);

    switch (offset) {
    case TB710FU_GENI_TX_FIFO:
        if (model->tx_count < 256) model->tx[model->tx_count++] = value;
        return;
    case TB710FU_GENI_M_CMD0:
        model->cmd_pending = 1;
        model->cmd_opcode = (int)((value >> TB710FU_GENI_M_OPCODE_SHIFT) & 0x7u);
        model->cmd_len = (size_t)model->regs[TB710FU_GENI_SPI_TX_TRANS_LEN / 4];
        if (model->cmd_opcode == (int)TB710FU_GENI_M_CMD_RX_ONLY ||
            model->cmd_opcode == (int)TB710FU_GENI_M_CMD_TX_RX)
            model->cmd_len = (size_t)model->regs[TB710FU_GENI_SPI_RX_TRANS_LEN / 4];
        model->irq = 0u;
        return;
    case TB710FU_GENI_M_CMD_CTRL:
        if ((value & TB710FU_GENI_M_CMD_CANCEL) != 0u && model->cancel_ack)
            model->irq |= TB710FU_GENI_M_CMD_CANCEL_DONE;
        if ((value & TB710FU_GENI_M_CMD_ABORT) != 0u && model->cancel_ack)
            model->irq |= TB710FU_GENI_M_CMD_ABORT_DONE;
        return;
    case TB710FU_GENI_M_IRQ_CLEAR:
        model->irq &= ~value;
        return;
    default:
        model->regs[offset / 4] = value;
        return;
    }
}

static uint32_t model_read_wrapper(void *ctx, uint32_t offset)
{
    struct geni_model *model = model_of(ctx);

    if (offset == TB710FU_QUP_HW_VER) return model->regs[0x100 / 4];
    return 0u;
}

static void model_delay_us(void *ctx, unsigned microseconds)
{
    model_of(ctx)->delay_us += microseconds;
}

static void geni_model_reset(struct geni_model *model, struct tb710fu_geni_spi *spi)
{
    memset(model, 0, sizeof(*model));
    memset(spi, 0, sizeof(*spi));
    model->regs[TB710FU_GENI_FW_REVISION_RO / 4] = (TB710FU_GENI_PROTO_SPI << TB710FU_GENI_FW_PROTO_SHIFT) | 0x30u;
    /* QUPv3 3.10: depth field is 8 bits wide, FIFO 16 words of 32 bits. */
    model->regs[0x100 / 4] = 0x300A0000u;
    model->regs[TB710FU_GENI_HW_PARAM_0 / 4] = (32u << 24) | (16u << 16);
    model->regs[TB710FU_GENI_DMA_MODE_EN / 4] = 0u;
    spi->io.read = model_read;
    spi->io.write = model_write;
    spi->io.read_wrapper = model_read_wrapper;
    spi->io.delay_us = model_delay_us;
    spi->io.ctx = model;
    spi->poll_limit = 64u;
    spi->poll_only = 1;   /* the probe services completion by polling */
}

static void test_geni(void)
{
    struct geni_model model;
    struct tb710fu_geni_spi spi;
    uint8_t tx[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t rx[8];
    int result;

    geni_model_reset(&model, &spi);
    CHECK_EQ(tb710fu_geni_read_info(&spi, 0), TB710FU_GENI_OK);
    CHECK_EQ(model.fuse_reads, 0u);           /* 0x64 is never read during bring-up */
    CHECK_EQ(spi.info.protocol, TB710FU_GENI_PROTO_SPI);
    CHECK_EQ(spi.info.tx_fifo_depth, 16u);
    CHECK_EQ(spi.info.fifo_width_bits, 32u);

    /* An unclocked window reads all ones: no device, and nothing is written. */
    geni_model_reset(&model, &spi);
    model.regs[TB710FU_GENI_FW_REVISION_RO / 4] = 0xFFFFFFFFu;
    CHECK_EQ(tb710fu_geni_read_info(&spi, 0), TB710FU_GENI_ERR_NODEV);
    CHECK_EQ(model.reads > 0u, 1);

    /* Explicit fuse probe is the only path that touches 0x64. */
    geni_model_reset(&model, &spi);
    model.regs[TB710FU_GENI_IF_DISABLE_RO / 4] = TB710FU_GENI_FIFO_IF_DISABLE;
    CHECK_EQ(tb710fu_geni_read_info(&spi, 1), TB710FU_GENI_OK);
    CHECK_EQ(model.fuse_reads, 1u);
    CHECK_EQ(spi.info.fifo_disabled, 1);
    CHECK_EQ(tb710fu_geni_init(&spi), TB710FU_GENI_ERR_NOTSUP);
    CHECK_EQ(spi.state, TB710FU_GENI_ERROR);

    /* A non-SPI protocol is refused instead of being driven as SPI. */
    geni_model_reset(&model, &spi);
    model.regs[TB710FU_GENI_FW_REVISION_RO / 4] = (2u << TB710FU_GENI_FW_PROTO_SHIFT);
    CHECK_EQ(tb710fu_geni_init(&spi), TB710FU_GENI_ERR_PROTO);

    /* Bring-up, divider and mode. */
    geni_model_reset(&model, &spi);
    CHECK_EQ(tb710fu_geni_init(&spi), TB710FU_GENI_OK);
    CHECK_EQ(spi.state, TB710FU_GENI_IDLE);
    CHECK_EQ(model.regs[TB710FU_GENI_IRQ_EN / 4], 0u);   /* polled: no interrupt */
    CHECK_EQ(model.regs[TB710FU_GENI_RX_WATERMARK / 4], 13u);
    CHECK_EQ(tb710fu_geni_set_clock(&spi, 9600000u, 19200000u, 0u), TB710FU_GENI_OK);
    CHECK_EQ(model.regs[TB710FU_GENI_SER_M_CLK_CFG / 4],
             (2u << TB710FU_GENI_CLK_DIV_SHIFT) | TB710FU_GENI_SER_CLK_EN);
    CHECK_EQ(model.regs[TB710FU_GENI_CLK_SEL / 4], 0u);
    CHECK_EQ(tb710fu_geni_setup(&spi, TB710FU_SPI_MODE_CPOL | TB710FU_SPI_MODE_CPHA, 0u),
             TB710FU_GENI_OK);
    CHECK_EQ(model.regs[TB710FU_GENI_SPI_CPHA / 4], TB710FU_GENI_CPHA_BIT);
    CHECK_EQ(model.regs[TB710FU_GENI_SPI_CPOL / 4], TB710FU_GENI_CPOL_BIT);
    CHECK_EQ(model.regs[TB710FU_GENI_SPI_DEMUX_SEL / 4], 0u);

    /* Full duplex loopback: what went into the FIFO comes back byte for byte. */
    memset(rx, 0, sizeof(rx));
    CHECK_EQ(tb710fu_geni_transfer(&spi, tx, rx, sizeof(tx), 0), TB710FU_GENI_OK);
    CHECK_EQ(memcmp(tx, rx, sizeof(tx)), 0);
    CHECK_EQ(spi.state, TB710FU_GENI_IDLE);
    CHECK_EQ(model.regs[TB710FU_GENI_SPI_WORD_LEN / 4],
             (8u - TB710FU_GENI_WORD_LEN_MIN) & 0x3FFu);

    /* TX only. */
    CHECK_EQ(tb710fu_geni_transfer(&spi, tx, NULL, 4u, 0), TB710FU_GENI_OK);

    /* RX only returns zeroes in the model. */
    memset(rx, 0xFF, sizeof(rx));
    CHECK_EQ(tb710fu_geni_transfer(&spi, NULL, rx, 4u, 0), TB710FU_GENI_OK);
    CHECK_EQ(rx[0], 0u);

    /* Wrong length is a program error, not a bus error. */
    CHECK_EQ(tb710fu_geni_transfer(&spi, NULL, NULL, 4u, 0), TB710FU_GENI_ERR_PARAM);

    /* A dead engine times out, snapshots its state and recovers through cancel. */
    geni_model_reset(&model, &spi);
    model.no_completion = 1;
    model.cancel_ack = 1;
    CHECK_EQ(tb710fu_geni_init(&spi), TB710FU_GENI_OK);
    CHECK_EQ(tb710fu_geni_set_clock(&spi, 9600000u, 19200000u, 0u), TB710FU_GENI_OK);
    result = tb710fu_geni_transfer(&spi, tx, rx, 4u, 0);
    CHECK_EQ(result, TB710FU_GENI_ERR_TIMEOUT);
    CHECK_EQ(spi.snap_valid, 1);
    CHECK_EQ(spi.state, TB710FU_GENI_IDLE);

    /* Without an acknowledgement the engine ends in the error state. */
    geni_model_reset(&model, &spi);
    model.no_completion = 1;
    CHECK_EQ(tb710fu_geni_init(&spi), TB710FU_GENI_OK);
    CHECK_EQ(tb710fu_geni_set_clock(&spi, 9600000u, 19200000u, 0u), TB710FU_GENI_OK);
    CHECK_EQ(tb710fu_geni_transfer(&spi, tx, rx, 4u, 0), TB710FU_GENI_ERR_TIMEOUT);
    CHECK_EQ(spi.state, TB710FU_GENI_ERROR);
}

/* ================================= NVT model ================================ */

struct nvt_model {
    uint8_t memory[0x200000];
    uint32_t page;
    unsigned transfers;
    int fail;
};

static int nvt_model_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct nvt_model *model = (struct nvt_model *)ctx;
    uint32_t address;

    if (!tx || len == 0u) return -1;
    model->transfers++;
    if (model->fail) return -1;
    if (len == 3u && tx[0] == 0xFFu) {
        model->page = ((uint32_t)tx[1] << 15) | ((uint32_t)tx[2] << 7);
        return 0;
    }
    if ((tx[0] & 0x80u) != 0u) {
        address = model->page + (uint32_t)(tx[0] & 0x7Fu);
        if (address + len - 1u > sizeof(model->memory)) return -1;
        memcpy(&model->memory[address], &tx[1], len - 1u);
        return 0;
    }
    address = model->page + (uint32_t)(tx[0] & 0x7Fu);
    if (address + len - 2u > sizeof(model->memory)) return -1;
    if (rx) {
        size_t i;

        rx[0] = 0u;
        if (len > 1u) rx[1] = 0u;
        for (i = 2u; i < len; ++i) rx[i] = model->memory[address + i - 2u];
    }
    return 0;
}

/* The controller's burst carries one leading byte that the read path strips
 * (frame[0] in tb710fu_nvt_read_frame), so a frame written in the decoder's
 * coordinates lives one byte before the event buffer. */
static uint32_t nvt_frame_base(uint32_t event_buf)
{
    return event_buf - 1u;
}

static void nvt_put_word(struct nvt_model *model, uint32_t address, int16_t value)
{
    model->memory[address] = (uint8_t)(value & 0xFF);
    model->memory[address + 1u] = (uint8_t)((value >> 8) & 0xFF);
}

static void nvt_fill_matrix(struct nvt_model *model, uint32_t base, int16_t value)
{
    size_t i;

    for (i = 0; i < TB710FU_NVT_NODES; ++i)
        nvt_put_word(model, base + TB710FU_NVT_MATRIX_OFFSET + (uint32_t)i * 2u, value);
}

static void nvt_make_frame(struct nvt_model *model, uint32_t event_buf, unsigned type,
                           unsigned counter)
{
    uint8_t *frame = &model->memory[nvt_frame_base(event_buf)];
    const uint32_t peek = TB710FU_NVT_PEEK_OFFSET;

    memset(frame, 0, TB710FU_NVT_FRAME_BYTES);
    frame[peek + TB710FU_NVT_PEEK_COUNTER] = (uint8_t)(counter & 0xFF);
    frame[peek + TB710FU_NVT_PEEK_COUNTER + 1u] = (uint8_t)((counter >> 8) & 0xFF);
    frame[peek + TB710FU_NVT_PEEK_MARKER] = 0xAAu;
    frame[peek + TB710FU_NVT_PEEK_MARKER + 1u] = 0x55u;
    frame[peek + TB710FU_NVT_PEEK_MARKER_INV] = 0x55u;
    frame[peek + TB710FU_NVT_PEEK_MARKER_INV + 1u] = 0xAAu;
    frame[peek + TB710FU_NVT_PEEK_TYPE] = (uint8_t)type;
}

static void test_nvt(void)
{
    static struct nvt_model model;
    struct tb710fu_nvt_bus bus;
    struct tb710fu_nvt_chip chip;
    struct tb710fu_nvt_core core;
    struct tb710fu_nvt_peek peek;
    struct tb710fu_nvt_contact contacts[TB710FU_NVT_MAX_CONTACTS];
    uint8_t scratch[256];
    uint8_t rx[TB710FU_NVT_READ_MAX];
    uint8_t frame[TB710FU_NVT_READ_BYTES];
    uint8_t data[62];
    unsigned i;

    memset(&model, 0, sizeof(model));
    bus.xfer = nvt_model_xfer;
    bus.delay_ms = NULL;
    bus.ctx = &model;

    CHECK_EQ(tb710fu_nvt_chip_init(&chip, &bus, scratch, sizeof(scratch), rx, sizeof(rx)),
             TB710FU_NVT_OK);
    CHECK_EQ(tb710fu_nvt_chip_init(&chip, &bus, scratch, 2u, rx, sizeof(rx)),
             TB710FU_NVT_ERR_PARAM);

    /* Page-select + write + read round trip across a chunk boundary. */
    for (i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(0x40u + i);
    CHECK_EQ(tb710fu_nvt_write(&chip, 0x1FB104u, data, sizeof(data)), TB710FU_NVT_OK);
    memset(data, 0, sizeof(data));
    CHECK_EQ(tb710fu_nvt_read(&chip, 0x1FB104u, data, sizeof(data)), TB710FU_NVT_OK);
    for (i = 0; i < sizeof(data); ++i) CHECK_EQ(data[i], (uint8_t)(0x40u + i));

    /* Chip identity: NT36532 id bytes, single die. */
    memset(&model, 0, sizeof(model));
    model.memory[TB710FU_NVT_CHIP_VER_TRIM] = 0x0Eu;
    model.memory[TB710FU_NVT_CHIP_VER_TRIM + 3u] = 0x32u;
    model.memory[TB710FU_NVT_CHIP_VER_TRIM + 4u] = 0x65u;
    model.memory[TB710FU_NVT_CHIP_VER_TRIM + 5u] = 0x03u;
    model.memory[TB710FU_NVT_ENB_CASC] = 0x01u;
    CHECK_EQ(tb710fu_nvt_identify(&chip, 0), TB710FU_NVT_OK);
    CHECK_EQ(chip.identified, 1);
    CHECK_EQ(chip.is_e, 1);
    CHECK_EQ(chip.cascade, 0);
    CHECK_EQ(chip.event_buf, TB710FU_NVT_EVENT_BUF);

    /* A wrong id is refused without ever resetting the controller. */
    memset(&model, 0, sizeof(model));
    model.memory[TB710FU_NVT_CHIP_VER_TRIM + 3u] = 0x00u;
    CHECK_EQ(tb710fu_nvt_identify(&chip, 0), TB710FU_NVT_ERR_CHIP);
    CHECK_EQ(chip.identified, 0);

    /* Frame read is a single burst from the event buffer, one status byte first. */
    memset(&model, 0, sizeof(model));
    nvt_make_frame(&model, TB710FU_NVT_EVENT_BUF, TB710FU_NVT_FRAME_TYPE_TOUCH, 7u);
    CHECK_EQ(tb710fu_nvt_read_frame(&chip, frame), TB710FU_NVT_OK);
    CHECK_EQ(frame[0], 0u);
    CHECK_EQ(tb710fu_nvt_frame_parse(frame, TB710FU_NVT_FRAME_BYTES, &peek), 1);
    CHECK_EQ(peek.counter, 7u);
    CHECK_EQ(peek.type, TB710FU_NVT_FRAME_TYPE_TOUCH);
    CHECK_EQ(peek.header_ok, 1);

    /* Marker without its complement is a truncated or mid-update burst. */
    model.memory[nvt_frame_base(TB710FU_NVT_EVENT_BUF) + TB710FU_NVT_PEEK_OFFSET + TB710FU_NVT_PEEK_MARKER_INV] = 0x00u;
    CHECK_EQ(tb710fu_nvt_read_frame(&chip, frame), TB710FU_NVT_OK);
    CHECK_EQ(tb710fu_nvt_frame_parse(frame, TB710FU_NVT_FRAME_BYTES, &peek), 0);

    /* Matrix decode and node geometry. */
    nvt_make_frame(&model, TB710FU_NVT_EVENT_BUF, TB710FU_NVT_FRAME_TYPE_TOUCH, 9u);
    nvt_put_word(&model, nvt_frame_base(TB710FU_NVT_EVENT_BUF) + TB710FU_NVT_MATRIX_OFFSET, -1234);
    CHECK_EQ(tb710fu_nvt_read_frame(&chip, frame), TB710FU_NVT_OK);
    {
        int16_t matrix[TB710FU_NVT_NODES];

        tb710fu_nvt_matrix_decode(frame, matrix);
        CHECK_EQ(matrix[0], -1234);
    }
    CHECK_EQ(tb710fu_nvt_node_x(0, (int32_t)TB710FU_PANEL_WIDTH),
             (int32_t)(3200 * TB710FU_NVT_COORD_UNITS / (2 * 60)));
    CHECK(tb710fu_nvt_node_x(30, (int32_t)TB710FU_PANEL_WIDTH) / TB710FU_NVT_COORD_UNITS > 1500);
    CHECK(tb710fu_nvt_node_x(30, (int32_t)TB710FU_PANEL_WIDTH) / TB710FU_NVT_COORD_UNITS < 1700);
    CHECK(tb710fu_nvt_node_y(20, (int32_t)TB710FU_PANEL_HEIGHT) / TB710FU_NVT_COORD_UNITS > 950);
    CHECK(tb710fu_nvt_node_y(20, (int32_t)TB710FU_PANEL_HEIGHT) / TB710FU_NVT_COORD_UNITS < 1100);
    CHECK_EQ(tb710fu_nvt_node_x(-5, (int32_t)TB710FU_PANEL_WIDTH),
             tb710fu_nvt_node_x(0, (int32_t)TB710FU_PANEL_WIDTH));
    CHECK_EQ(tb710fu_nvt_node_y(99, (int32_t)TB710FU_PANEL_HEIGHT),
             tb710fu_nvt_node_y(39, (int32_t)TB710FU_PANEL_HEIGHT));

    /* Baseline seeds the reference; the next touch frame yields one contact. */
    tb710fu_nvt_core_reset(&core);
    nvt_make_frame(&model, TB710FU_NVT_EVENT_BUF, TB710FU_NVT_FRAME_TYPE_BASELINE, 10u);
    nvt_fill_matrix(&model, nvt_frame_base(TB710FU_NVT_EVENT_BUF), 5000);
    CHECK_EQ(tb710fu_nvt_read_frame(&chip, frame), TB710FU_NVT_OK);
    CHECK_EQ(tb710fu_nvt_core_process(&core, frame, (int32_t)TB710FU_PANEL_WIDTH,
                                      (int32_t)TB710FU_PANEL_HEIGHT, contacts,
                                      (int)TB710FU_NVT_MAX_CONTACTS), 0);
    CHECK_EQ(core.have_reference, 1);

    nvt_make_frame(&model, TB710FU_NVT_EVENT_BUF, TB710FU_NVT_FRAME_TYPE_TOUCH, 11u);
    nvt_fill_matrix(&model, nvt_frame_base(TB710FU_NVT_EVENT_BUF), 5000);
    for (i = 0; i < 3u; ++i) {
        unsigned r;

        for (r = 0; r < 3u; ++r) {
            const unsigned node = (unsigned)(19 + (int)r) * TB710FU_NVT_COLUMNS + (29u + i);

            nvt_put_word(&model,
                         nvt_frame_base(TB710FU_NVT_EVENT_BUF) + TB710FU_NVT_MATRIX_OFFSET +
                             node * 2u,
                         5900);
        }
    }
    CHECK_EQ(tb710fu_nvt_read_frame(&chip, frame), TB710FU_NVT_OK);
    CHECK_EQ(tb710fu_nvt_core_process(&core, frame, (int32_t)TB710FU_PANEL_WIDTH,
                                      (int32_t)TB710FU_PANEL_HEIGHT, contacts,
                                      (int)TB710FU_NVT_MAX_CONTACTS), 1);
    CHECK_EQ(contacts[0].area, 9);
    CHECK_EQ(contacts[0].peak, 900);
    CHECK(contacts[0].x > 1500 && contacts[0].x < 1750);
    CHECK(contacts[0].y > 900 && contacts[0].y < 1150);

    /* A frame whose header broke is refused, not decoded. */
    model.memory[nvt_frame_base(TB710FU_NVT_EVENT_BUF) + TB710FU_NVT_PEEK_OFFSET + TB710FU_NVT_PEEK_TYPE] = 0x77u;
    CHECK_EQ(tb710fu_nvt_read_frame(&chip, frame), TB710FU_NVT_OK);
    CHECK_EQ(tb710fu_nvt_core_process(&core, frame, (int32_t)TB710FU_PANEL_WIDTH,
                                      (int32_t)TB710FU_PANEL_HEIGHT, contacts,
                                      (int)TB710FU_NVT_MAX_CONTACTS), TB710FU_NVT_ERR_FRAME);

    /* A bus failure is reported as a bus error. */
    model.fail = 1;
    CHECK_EQ(tb710fu_nvt_read(&chip, 0x1FB104u, data, 4u), TB710FU_NVT_ERR_BUS);
    model.fail = 0;

    /* Argument checking. */
    CHECK_EQ(tb710fu_nvt_read(&chip, 0x1FB104u, data, 0u), TB710FU_NVT_ERR_PARAM);
    CHECK_EQ(tb710fu_nvt_core_process(NULL, frame, 3200, 2000, contacts, 10), TB710FU_NVT_ERR_PARAM);
}

/* ================================= tracker ================================= */

static void test_tracker(void)
{
    struct tb710fu_tracker tracker;
    struct tb710fu_track_input in[2];
    struct tb710fu_track_point out[TB710FU_TRACK_SLOTS];
    uint8_t lifted[TB710FU_TRACK_SLOTS];
    int count;
    int i;

    tb710fu_tracker_init(&tracker, (int32_t)TB710FU_HID_TOUCH_LOGICAL_X,
                         (int32_t)TB710FU_HID_TOUCH_LOGICAL_Y);
    CHECK_EQ(tracker.match_radius, 199);
    CHECK_EQ(tracker.jitter, 2);

    memset(in, 0, sizeof(in));
    in[0].x = 1600;
    in[0].y = 1000;
    in[0].pressure = 500;

    /* One finger held for five frames keeps its identifier and position. */
    for (i = 0; i < 5; ++i) {
        count = tb710fu_tracker_update(&tracker, in, 1, out, TB710FU_TRACK_SLOTS);
        CHECK_EQ(count, 1);
        CHECK_EQ(out[0].id, 1u);
        CHECK_EQ(out[0].x, 1600);
        CHECK_EQ(out[0].y, 1000);
    }

    /* A small move keeps the identifier; a jump past the gate opens a new one. */
    in[0].x = 1650;
    count = tb710fu_tracker_update(&tracker, in, 1, out, TB710FU_TRACK_SLOTS);
    CHECK_EQ(count, 1);
    CHECK_EQ(out[0].id, 1u);
    CHECK_EQ(out[0].x, 1650);

    in[0].x = 2400;
    count = tb710fu_tracker_update(&tracker, in, 1, out, TB710FU_TRACK_SLOTS);
    CHECK_EQ(count, 1);
    CHECK_EQ(out[0].id, 2u);
    CHECK_EQ(out[0].x, 2400);

    /* Two fingers keep distinct identifiers across a frame. */
    in[0].x = 1000;
    in[0].y = 500;
    in[1].x = 2000;
    in[1].y = 1500;
    count = tb710fu_tracker_update(&tracker, in, 2, out, TB710FU_TRACK_SLOTS);
    CHECK_EQ(count, 2);

    /* Clamping: a calibration overshoot can never exceed the HID maximum. */
    in[0].x = 5000;
    in[0].y = -20;
    count = tb710fu_tracker_update(&tracker, in, 1, out, TB710FU_TRACK_SLOTS);
    CHECK_EQ(count, 1);
    CHECK_EQ(out[0].x, (int32_t)TB710FU_HID_TOUCH_LOGICAL_X);
    CHECK_EQ(out[0].y, 0);

    /* Aging: absent for max_missing frames, still pending; then dropped. */
    tb710fu_tracker_init(&tracker, 3199, 1999);
    in[0].x = 100;
    in[0].y = 100;
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 1, out, TB710FU_TRACK_SLOTS), 1);
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 0, out, TB710FU_TRACK_SLOTS), 0);
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 0, out, TB710FU_TRACK_SLOTS), 0);
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 0, out, TB710FU_TRACK_SLOTS), 0);
    CHECK_EQ(tracker.slot[0].active, 1);            /* 3 missing = still alive */
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 0, out, TB710FU_TRACK_SLOTS), 0);
    CHECK_EQ(tracker.slot[0].active, 0);

    /* Flush lifts everything. */
    tb710fu_tracker_init(&tracker, 3199, 1999);
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 1, out, TB710FU_TRACK_SLOTS), 1);
    CHECK_EQ(tb710fu_tracker_flush(&tracker, lifted, TB710FU_TRACK_SLOTS), 1);
    CHECK_EQ(lifted[0], tracker.next_id - 1u);
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 1, out, TB710FU_TRACK_SLOTS), 1);
    CHECK_EQ(tb710fu_tracker_update(&tracker, in, 1, NULL, TB710FU_TRACK_SLOTS), -1);
}

/* =================================== HID =================================== */

static void test_hid(void)
{
    struct tb710fu_hid_contact contacts[TB710FU_HID_TOUCH_MAX_CONTACTS];
    uint8_t report[TB710FU_HID_TOUCH_BYTES];
    uint8_t keys[TB710FU_HID_KEY_BYTES];
    unsigned length;
    int i;

    CHECK_EQ(tb710fu_hid_report_bits(tb710fu_hid_touch_descriptor,
                                    tb710fu_hid_touch_descriptor_bytes, 0u),
             TB710FU_HID_TOUCH_BYTES * 8u);
    CHECK_EQ(tb710fu_hid_report_bits(tb710fu_hid_keys_descriptor,
                                     tb710fu_hid_keys_descriptor_bytes, 0u),
             TB710FU_HID_KEY_BYTES * 8u);
    CHECK_EQ(tb710fu_hid_report_bits(tb710fu_hid_keys_descriptor, 4u, 0u), 0u);

    /* The logical maxima really are the panel extent, in two-byte items. */
    {
        const uint8_t x_max[3] = {0x26u, 0x7Fu, 0x0Cu};
        const uint8_t y_max[3] = {0x26u, 0xCFu, 0x07u};
        unsigned found_x = 0u;
        unsigned found_y = 0u;

        for (i = 0; i + 3 <= (int)tb710fu_hid_touch_descriptor_bytes; ++i) {
            if (memcmp(&tb710fu_hid_touch_descriptor[i], x_max, 3u) == 0) found_x++;
            if (memcmp(&tb710fu_hid_touch_descriptor[i], y_max, 3u) == 0) found_y++;
        }
        CHECK_EQ(found_x, TB710FU_HID_TOUCH_MAX_CONTACTS);
        CHECK_EQ(found_y, TB710FU_HID_TOUCH_MAX_CONTACTS);
    }

    for (i = 0; i < (int)TB710FU_HID_TOUCH_MAX_CONTACTS; ++i) {
        contacts[i].id = 0u;
        contacts[i].active = 0;
        contacts[i].x = 0u;
        contacts[i].y = 0u;
    }
    contacts[0].id = 3u;
    contacts[0].active = 1;
    contacts[0].x = 1234u;
    contacts[0].y = 567u;
    contacts[1].id = 4u;
    contacts[1].active = 1;
    contacts[1].x = 3199u;
    contacts[1].y = 1999u;
    length = tb710fu_hid_touch_report(contacts, 2u, report, sizeof(report));
    CHECK_EQ(length, TB710FU_HID_TOUCH_BYTES);
    CHECK_EQ(report[0], 2u);
    CHECK_EQ(report[1], 3u);
    CHECK_EQ(report[2], 0x03u);
    CHECK_EQ(report[3], 0xD2u);
    CHECK_EQ(report[4], 0x04u);
    CHECK_EQ(report[5], 0x37u);
    CHECK_EQ(report[6], 0x02u);
    CHECK_EQ(report[7], 4u);
    CHECK_EQ(report[9], 0x7Fu);   /* X of the second finger, low byte 3199 */
    CHECK_EQ(report[10], 0x0Cu);
    CHECK_EQ(report[11], 0xCFu);  /* Y of the second finger, low byte 1999 */
    CHECK_EQ(report[12], 0x07u);
    for (i = 13; i < (int)TB710FU_HID_TOUCH_BYTES; ++i) CHECK_EQ(report[i], 0u);

    /* An inactive contact is omitted, not emitted with a stale position. */
    contacts[0].active = 0;
    length = tb710fu_hid_touch_report(contacts, 1u, report, sizeof(report));
    CHECK_EQ(length, TB710FU_HID_TOUCH_BYTES);
    CHECK_EQ(report[1], 0u);
    CHECK_EQ(report[2], 0u);

    CHECK_EQ(tb710fu_hid_touch_report(contacts, 2u, report, TB710FU_HID_TOUCH_BYTES - 1u), 0u);
    CHECK_EQ(tb710fu_hid_touch_report(NULL, 1u, report, sizeof(report)), 0u);
    /* An empty frame is a valid all-lifted report, not an error. */
    CHECK_EQ(tb710fu_hid_touch_report(NULL, 0u, report, sizeof(report)), TB710FU_HID_TOUCH_BYTES);
    CHECK_EQ(report[0], 0u);

    CHECK_EQ(tb710fu_hid_key_report(0u, keys, sizeof(keys)), 1u);
    CHECK_EQ(keys[0], 0u);
    CHECK_EQ(tb710fu_hid_key_report(TB710FU_HID_KEY_POWER | TB710FU_HID_KEY_VOLUME_DOWN, keys,
                                    sizeof(keys)), 1u);
    CHECK_EQ(keys[0], 0x05u);
    CHECK_EQ(tb710fu_hid_key_report(TB710FU_HID_KEY_POWER, keys, 0u), 0u);
}

/* ================================ SPMI + keys =============================== */

struct spmi_plane {
    uint32_t words[0x4000];
    uint8_t data[32][256];
};

static uint32_t spmi_read32(void *ctx, void *window, uint32_t offset)
{
    struct spmi_plane *plane = (struct spmi_plane *)window;

    (void)ctx;
    if (offset / 4u >= sizeof(plane->words) / sizeof(plane->words[0])) return 0u;
    return plane->words[offset / 4u];
}

static void spmi_write32(void *ctx, void *window, uint32_t offset, uint32_t value)
{
    struct spmi_plane *plane = (struct spmi_plane *)window;

    (void)ctx;
    if (offset / 4u >= sizeof(plane->words) / sizeof(plane->words[0])) return;
    plane->words[offset / 4u] = value;
    /* A write to a channel's command register runs the observer read: the
     * model answers it immediately from the per-APID data table. */
    if ((offset % TB710FU_SPMI_OBS_APID_STRIDE) == TB710FU_SPMI_CMD) {
        const unsigned apid = offset / TB710FU_SPMI_OBS_APID_STRIDE;
        const unsigned reg = (unsigned)((value >> 4) & 0xFFu);

        if (apid < 32u) {
            plane->words[(offset + TB710FU_SPMI_STATUS) / 4u] = TB710FU_SPMI_STATUS_DONE;
            plane->words[(offset + TB710FU_SPMI_RDATA0) / 4u] = plane->data[apid][reg];
        }
    }
}

static void test_spmi_and_keys(void)
{
    static struct spmi_plane core_plane;
    static struct spmi_plane obs_plane;
    static struct spmi_plane cnfg_plane;
    struct tb710fu_spmi spmi;
    struct tb710fu_keys keys;
    struct tb710fu_key_filter filter;
    uint8_t pressed;
    uint8_t value;
    unsigned i;

    memset(&core_plane, 0, sizeof(core_plane));
    memset(&obs_plane, 0, sizeof(obs_plane));
    memset(&cnfg_plane, 0, sizeof(cnfg_plane));

    /* Arbiter v7: version, APID table, and one mapping per key peripheral. */
    core_plane.words[0] = TB710FU_SPMI_VERSION_V7_MIN;
    core_plane.words[TB710FU_SPMI_FEATURES / 4u] = 32u;
    for (i = 0; i < 32u; ++i) {
        core_plane.words[(TB710FU_SPMI_APID_MAP / 4u) + i] = 0u;
        cnfg_plane.words[(TB710FU_SPMI_CNFG_OWNER / 4u) + i] = 0u;
    }
    core_plane.words[(TB710FU_SPMI_APID_MAP / 4u) + 5u] = 0x013u << 8;
    core_plane.words[(TB710FU_SPMI_APID_MAP / 4u) + 9u] = 0x18Du << 8;
    cnfg_plane.words[(TB710FU_SPMI_CNFG_OWNER / 4u) + 5u] = 0u;
    cnfg_plane.words[(TB710FU_SPMI_CNFG_OWNER / 4u) + 9u] = 1u;

    obs_plane.data[5][TB710FU_PON_RT_STS] = 0xC0u;             /* power + volume down */
    obs_plane.data[9][TB710FU_PM_GPIO_RT_STS] = 0x00u;         /* active-low: volume up */

    memset(&spmi, 0, sizeof(spmi));
    spmi.core = &core_plane;
    spmi.obs = &obs_plane;
    spmi.cnfg = &cnfg_plane;
    spmi.ee = 0u;
    spmi.read32 = spmi_read32;
    spmi.write32 = spmi_write32;
    spmi.barrier = NULL;

    CHECK_EQ(tb710fu_keys_open(&spmi, &keys), 0);
    CHECK_EQ(keys.pon_apid, 5);
    CHECK_EQ(keys.gpio_apid, 9);
    CHECK_EQ(keys.have_gpio, 1);

    CHECK_EQ(tb710fu_spmi_read8(&spmi, 5, TB710FU_PON_RT_STS, &value), 0);
    CHECK_EQ(value, 0xC0u);
    CHECK_EQ(tb710fu_spmi_read8(&spmi, 9, TB710FU_PM_GPIO_RT_STS, &value), 0);
    CHECK_EQ(value, 0x00u);
    CHECK_EQ(tb710fu_spmi_read8(&spmi, 33, 0u, &value), 4);
    CHECK_EQ(tb710fu_spmi_read8(&spmi, -1, 0u, &value), 4);

    /* An unknown PPID is reported, not guessed. */
    {
        int apid = -7;

        CHECK_EQ(tb710fu_spmi_find_apid(&spmi, 0x777u, &apid), 3);
        CHECK_EQ(apid, -1);
    }

    CHECK_EQ(tb710fu_keys_sample(&spmi, &keys, &pressed), 0);
    CHECK_EQ(pressed, TB710FU_KEY_SAMPLE_POWER | TB710FU_KEY_SAMPLE_VOLUME_DOWN |
                      TB710FU_KEY_SAMPLE_VOLUME_UP);
    CHECK_EQ(tb710fu_keys_hid_bits(pressed),
             TB710FU_HID_KEY_POWER | TB710FU_HID_KEY_VOLUME_DOWN | TB710FU_HID_KEY_VOLUME_UP);

    /* Debounce: one sample is not a key, two identical samples are. */
    tb710fu_key_filter_reset(&filter);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_POWER, 0u, 0u), 0u);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_POWER, 0u, 0u),
             TB710FU_KEY_SAMPLE_POWER);
    CHECK_EQ(tb710fu_key_filter_step(&filter, 0u, 0u, 0u), 0u);
    CHECK_EQ(tb710fu_key_filter_step(&filter, 0u, 0u, 0u), 0u);
    CHECK_EQ(filter.stable, 0u);

    /* Volume repeats after the delay; the power key never repeats. */
    tb710fu_key_filter_reset(&filter);
    tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_VOLUME_UP, 3u, 2u);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_VOLUME_UP, 3u, 2u),
             TB710FU_KEY_SAMPLE_VOLUME_UP);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_VOLUME_UP, 3u, 2u), 0u);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_VOLUME_UP, 3u, 2u), 0u);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_VOLUME_UP, 3u, 2u),
             TB710FU_KEY_SAMPLE_VOLUME_UP);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_VOLUME_UP, 3u, 2u), 0u);
    CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_VOLUME_UP, 3u, 2u),
             TB710FU_KEY_SAMPLE_VOLUME_UP);

    tb710fu_key_filter_reset(&filter);
    tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_POWER, 3u, 2u);
    tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_POWER, 3u, 2u);
    for (i = 0; i < 8u; ++i)
        CHECK_EQ(tb710fu_key_filter_step(&filter, TB710FU_KEY_SAMPLE_POWER, 3u, 2u), 0u);

    /* A missing PON peripheral is a refused key device, not a phantom key. */
    core_plane.words[(TB710FU_SPMI_APID_MAP / 4u) + 5u] = 0u;
    CHECK_EQ(tb710fu_keys_open(&spmi, &keys), 3);
    CHECK_EQ(tb710fu_keys_sample(&spmi, &keys, &pressed), 4);

    /* An SPMI plane that is not v7 is refused whole. */
    core_plane.words[0] = 0x50000000u;
    {
        int apid = 0;

        CHECK_EQ(tb710fu_spmi_find_apid(&spmi, 0x013u, &apid), 1);
        CHECK_EQ(tb710fu_spmi_read8(&spmi, 0, 0x10u, &value), 4);
    }
}

/* ================================ firmware ================================= */

static void fw_put32(uint8_t *image, size_t offset, uint32_t value)
{
    image[offset] = (uint8_t)(value & 0xFF);
    image[offset + 1u] = (uint8_t)((value >> 8) & 0xFF);
    image[offset + 2u] = (uint8_t)((value >> 16) & 0xFF);
    image[offset + 3u] = (uint8_t)((value >> 24) & 0xFF);
}

static void test_firmware(void)
{
    static uint8_t image[0x40000];
    struct tb710fu_nvt_firmware firmware;
    uint32_t crc;
    unsigned i;

    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 7u + 3u);
    fw_put32(image, 0x00u, 0x200u);      /* ILM file offset */
    fw_put32(image, 0x04u, 0x00000000u); /* ILM SRAM */
    fw_put32(image, 0x08u, 0x1000u);     /* ILM bytes */
    fw_put32(image, 0x0Cu, 0x1200u);     /* DLM file offset */
    fw_put32(image, 0x10u, 0x00100000u); /* DLM SRAM */
    fw_put32(image, 0x14u, 0x800u);      /* DLM bytes */
    fw_put32(image, 0x18u, 0xF97BA55Au); /* stored ILM word, only recorded */
    crc = tb710fu_nvt_firmware_crc32(image + 0x1200u, 0x800u);
    fw_put32(image, 0x1Cu, crc);

    CHECK_EQ(tb710fu_nvt_firmware_parse(image, sizeof(image), &firmware), TB710FU_NVT_OK);
    CHECK_EQ(firmware.ilm_address, 0x200u);
    CHECK_EQ(firmware.ilm_bytes, 0x1000u);
    CHECK_EQ(firmware.dlm_address, 0x1200u);
    CHECK_EQ(firmware.dlm_bytes, 0x800u);
    CHECK_EQ(firmware.dlm_sram, 0x00100000u);
    CHECK_EQ(firmware.ilm_checksum, 0xF97BA55Au);
    CHECK_EQ(firmware.dlm_crc_valid, 1);
    CHECK_EQ(firmware.image_bytes, (uint32_t)sizeof(image));

    /* One byte in the DLM payload breaks the stored checksum, and the parser
     * says so instead of claiming the image is good. */
    image[0x1200u] ^= 0xFFu;
    CHECK_EQ(tb710fu_nvt_firmware_parse(image, sizeof(image), &firmware), TB710FU_NVT_OK);
    CHECK_EQ(firmware.dlm_crc_valid, 0);
    image[0x1200u] ^= 0xFFu;

    /* A region that runs past the file is refused. */
    fw_put32(image, 0x08u, 0x80000u);
    CHECK_EQ(tb710fu_nvt_firmware_parse(image, sizeof(image), &firmware), TB710FU_NVT_ERR_FW);
    fw_put32(image, 0x08u, 0x1000u);
    fw_put32(image, 0x14u, 0x40000u);
    CHECK_EQ(tb710fu_nvt_firmware_parse(image, sizeof(image), &firmware), TB710FU_NVT_ERR_FW);
    fw_put32(image, 0x14u, 0x800u);

    /* Zero counts, a truncated file and a misaligned size are refused. */
    fw_put32(image, 0x14u, 0u);
    CHECK_EQ(tb710fu_nvt_firmware_parse(image, sizeof(image), &firmware), TB710FU_NVT_ERR_FW);
    fw_put32(image, 0x14u, 0x800u);
    CHECK_EQ(tb710fu_nvt_firmware_parse(image, 0x20u, &firmware), TB710FU_NVT_ERR_FW);
    CHECK_EQ(tb710fu_nvt_firmware_parse(image, sizeof(image) + 1u, &firmware), TB710FU_NVT_ERR_FW);
    CHECK_EQ(tb710fu_nvt_firmware_parse(NULL, sizeof(image), &firmware), TB710FU_NVT_ERR_PARAM);
}

/* =================================== main ================================== */

int main(void)
{
    test_geni();
    test_nvt();
    test_tracker();
    test_hid();
    test_spmi_and_keys();
    test_firmware();

    if (g_failures) {
        printf("FAILED %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("PASS %d checks\n", g_checks);
    return 0;
}
