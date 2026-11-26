/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuGeniI2c.h"

#include <stdbool.h>

static uint32_t rd(Tb710fuGeni *geni, uint32_t offset) {return geni->io.read(geni->io.context, offset);}

static void wr(Tb710fuGeni *geni, uint32_t offset, uint32_t value)
{
    geni->io.write(geni->io.context, offset, value);
}

/* A FIFO depth field (bits 21:16); a field that reads zero takes the smallest depth these engines
 * have, and a master hub channel is documented at 16 words. */
static uint32_t fifo_words(uint32_t param, uint32_t floor)
{
    const uint32_t words = (param >> 16) & 0x3fu;
    return words > floor ? words : floor;
}

int tb710fu_geni_prepare(Tb710fuGeni *geni, const Tb710fuGeniIo *io, uint8_t address, int master_hub)
{
    if (!geni || !io || !io->read || !io->write) return Tb710fuGeniErrConfig;
    for (unsigned i = 0; i < sizeof *geni; ++i) ((uint8_t *)geni)[i] = 0;
    geni->io = *io;
    geni->address = address;
    geni->phase = Tb710fuGeniIdle;

    geni->proto = (rd(geni, TB710FU_GENI_FW_REVISION) >> 8) & 0xffu;
    geni->clock_config = rd(geni, TB710FU_GENI_CLK_CFG);
    geni->timing_counters = rd(geni, TB710FU_GENI_SCL_COUNTERS);
    if (geni->proto != TB710FU_GENI_PROTO_I2C) return Tb710fuGeniErrConfig;
    if (rd(geni, TB710FU_GENI_IF_DISABLE) & 1u) return Tb710fuGeniErrConfig;
    /* A classic SE needs its clock divider and SCL counters set by whoever came before us. The
     * master hub wrapper owns them, so there the values are only reported. */
    if (!master_hub && (!(geni->clock_config & 1u) || !geni->timing_counters)) return Tb710fuGeniErrConfig;

    wr(geni, TB710FU_GENI_DMA_MODE, 0);
    /* 8-bit words, 4 per FIFO word, least significant byte first on the wire. */
    wr(geni, TB710FU_GENI_TX_PACK0, 0x7f8feu);
    wr(geni, TB710FU_GENI_TX_PACK1, 0xffefeu);
    wr(geni, TB710FU_GENI_RX_PACK0, 0x7f8feu);
    wr(geni, TB710FU_GENI_RX_PACK1, 0xffefeu);
    wr(geni, TB710FU_GENI_BYTE_GRAN, 0);
    wr(geni, TB710FU_GENI_TX_WATERMARK, 0);
    const uint32_t floor = master_hub ? TB710FU_GENI_MASTER_HUB_FIFO_WORDS : 1u;
    geni->tx_fifo_words = fifo_words(rd(geni, TB710FU_GENI_HW_PARAM_0), floor);
    geni->rx_fifo_words = fifo_words(rd(geni, TB710FU_GENI_HW_PARAM_1), floor);
    wr(geni, TB710FU_GENI_M_IRQ_CLEAR, 0xffffffffu);
    /* Exactly the bits this core handles: one the boot chain left enabled (RX watermark, RX last)
     * would hold the level line up. */
    wr(geni, TB710FU_GENI_M_IRQ_EN, TB710FU_GENI_M_IRQ_WANTED);
    wr(geni, TB710FU_GENI_SE_IRQ_EN, rd(geni, TB710FU_GENI_SE_IRQ_EN) | TB710FU_GENI_SE_M_IRQ_EN);
    return Tb710fuGeniOk;
}

/* Starts one command: the bytes go into the FIFO when the engine raises its TX watermark. */
static int command(Tb710fuGeni *geni, const uint8_t *bytes, uint32_t length, int hold)
{
    if (!bytes || !length || length > TB710FU_GENI_MAX_BYTES) return Tb710fuGeniErrBusy;
    for (uint32_t i = 0; i < length; ++i) geni->tx[i] = bytes[i];
    geni->tx_words = (uint8_t)((length + 3) / 4);
    geni->length = length;
    geni->hold = (uint8_t)(hold ? 1 : 0);
    wr(geni, TB710FU_GENI_TX_LEN, length);
    wr(geni, TB710FU_GENI_M_CMD0,
       (TB710FU_GENI_OP_WRITE << 27) | ((uint32_t)geni->address << 9) |
           (hold ? TB710FU_GENI_STOP_STRETCH : 0));
    wr(geni, TB710FU_GENI_TX_WATERMARK, 1);
    geni->phase = Tb710fuGeniTxWait;
    return Tb710fuGeniOk;
}

int tb710fu_geni_write(Tb710fuGeni *geni, const uint8_t *bytes, uint32_t length)
{
    if (!geni || geni->phase != Tb710fuGeniIdle) return Tb710fuGeniErrBusy;
    geni->reading = 0;
    geni->rx_length = 0;
    geni->rx_filled = 0;
    geni->error = Tb710fuGeniOk;
    return command(geni, bytes, length, 0);
}

int tb710fu_geni_read(Tb710fuGeni *geni, const uint8_t *command_bytes, uint32_t command_length,
                      uint32_t read_length)
{
    if (!geni || geni->phase != Tb710fuGeniIdle) return Tb710fuGeniErrBusy;
    if (!read_length || read_length > TB710FU_GENI_MAX_BYTES) return Tb710fuGeniErrBusy;
    geni->reading = 1;
    geni->rx_length = read_length;
    geni->rx_filled = 0;
    geni->error = Tb710fuGeniOk;
    return command(geni, command_bytes, command_length, 1);
}

/* Pushes the command words the FIFO has room for; the last word in ends the TX wait. */
static void feed(Tb710fuGeni *geni)
{
    const uint32_t used = TB710FU_GENI_TX_FIFO_WORDS(rd(geni, TB710FU_GENI_TX_FIFO_STATUS));
    const uint32_t room = used < geni->tx_fifo_words ? geni->tx_fifo_words - used : 0;
    for (uint32_t k = 0; k < room && k < geni->tx_words; ++k) {
        const uint32_t base = k * 4;
        uint32_t word = 0;
        for (unsigned byte = 0; byte < 4 && base + byte < geni->length; ++byte)
            word |= (uint32_t)geni->tx[base + byte] << (8 * byte);
        wr(geni, TB710FU_GENI_TX_FIFO, word);
    }
    wr(geni, TB710FU_GENI_TX_WATERMARK, 0);
    geni->phase = Tb710fuGeniWriteDone;
}

/* Empties the RX FIFO, keeping only the bytes the transfer asked for. */
static void drain(Tb710fuGeni *geni)
{
    for (uint32_t words = TB710FU_GENI_RX_FIFO_WORDS(rd(geni, TB710FU_GENI_RX_FIFO_STATUS)); words;
         --words) {
        const uint32_t word = rd(geni, TB710FU_GENI_RX_FIFO);
        for (unsigned byte = 0; byte < 4 && geni->rx_filled < geni->rx_length; ++byte)
            geni->rx[geni->rx_filled++] = (uint8_t)(word >> (8 * byte));
    }
}

static int finish(Tb710fuGeni *geni, int error, uint32_t status)
{
    geni->phase = Tb710fuGeniIdle;
    wr(geni, TB710FU_GENI_TX_WATERMARK, 0);
    geni->length = 0;
    geni->tx_words = 0;
    if (!error) {
        geni->done++;
        geni->data_bytes = (uint8_t)(geni->rx_filled > TB710FU_GENI_MAX_BYTES ? TB710FU_GENI_MAX_BYTES : geni->rx_filled);
        return Tb710fuGeniDone;
    }
    geni->error = (uint8_t)error;
    geni->error_status = status;
    geni->rx_length = 0;
    geni->rx_filled = 0;
    geni->failed++;
    return Tb710fuGeniFailed;
}

int tb710fu_geni_event(Tb710fuGeni *geni, uint32_t status)
{
    if (!geni) return Tb710fuGeniFailed;
    if (!status) return Tb710fuGeniPending;
    switch (geni->phase) {
    case Tb710fuGeniIdle:
        geni->stray++;
        return Tb710fuGeniPending;
    case Tb710fuGeniCancelling:
        if (status & TB710FU_GENI_M_CMD_CANCEL) return finish(geni, Tb710fuGeniErrTimeout, geni->error_status);
        return Tb710fuGeniPending;
    case Tb710fuGeniAborting:
        if (status & TB710FU_GENI_M_CMD_ABORT) return finish(geni, Tb710fuGeniErrTimeout, geni->error_status);
        return Tb710fuGeniPending;
    default:
        break;
    }
    /* A failed command also ends with CMD_DONE; the error bits decide. */
    if (status & TB710FU_GENI_M_ERRORS) return finish(geni, Tb710fuGeniErrBus, status);
    if ((status & TB710FU_GENI_M_TX_WATERMARK) && geni->phase == Tb710fuGeniTxWait) feed(geni);
    if (!(status & TB710FU_GENI_M_CMD_DONE)) {
        /* The data of a read lands in the FIFO before the command completes; keep it. */
        if (geni->phase == Tb710fuGeniReadDone) drain(geni);
        return Tb710fuGeniPending;
    }
    switch (geni->phase) {
    case Tb710fuGeniTxWait:
        /* Completed before the command bytes went out: the engine refused the transfer. */
        return finish(geni, Tb710fuGeniErrState, status);
    case Tb710fuGeniWriteDone:
        if (!geni->reading) return finish(geni, Tb710fuGeniOk, 0);
        wr(geni, TB710FU_GENI_RX_LEN, geni->rx_length);
        wr(geni, TB710FU_GENI_M_CMD0, (TB710FU_GENI_OP_READ << 27) | ((uint32_t)geni->address << 9));
        geni->phase = Tb710fuGeniReadDone;
        return Tb710fuGeniPending;
    case Tb710fuGeniReadDone:
        drain(geni);
        if (!geni->rx_filled) return finish(geni, Tb710fuGeniErrNoData, status);
        if (geni->rx_filled < geni->rx_length) return finish(geni, Tb710fuGeniErrNoData, status);
        return finish(geni, Tb710fuGeniOk, 0);
    default:
        return finish(geni, Tb710fuGeniErrState, status);
    }
}

int tb710fu_geni_poll(Tb710fuGeni *geni)
{
    if (!geni || geni->phase == Tb710fuGeniIdle) return Tb710fuGeniPending;
    const uint32_t status = rd(geni, TB710FU_GENI_M_IRQ_STATUS);
    if (status) wr(geni, TB710FU_GENI_M_IRQ_CLEAR, status);
    return tb710fu_geni_event(geni, status);
}

int tb710fu_geni_timeout(Tb710fuGeni *geni)
{
    if (!geni) return Tb710fuGeniFailed;
    switch (geni->phase) {
    case Tb710fuGeniIdle:
        return Tb710fuGeniPending;
    case Tb710fuGeniCancelling:
        wr(geni, TB710FU_GENI_M_CMD_CTRL, TB710FU_GENI_CMD_CTRL_ABORT);
        geni->phase = Tb710fuGeniAborting;
        return Tb710fuGeniPending;
    case Tb710fuGeniAborting:
        return finish(geni, Tb710fuGeniErrTimeout, geni->error_status);
    default:
        geni->timeouts++;
        geni->error_status = rd(geni, TB710FU_GENI_M_IRQ_STATUS);
        wr(geni, TB710FU_GENI_TX_WATERMARK, 0);
        wr(geni, TB710FU_GENI_M_CMD_CTRL, TB710FU_GENI_CMD_CTRL_CANCEL);
        geni->phase = Tb710fuGeniCancelling;
        return Tb710fuGeniPending;
    }
}
