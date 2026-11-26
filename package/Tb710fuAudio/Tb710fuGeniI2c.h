/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* One register transfer at a time on the SM8650 GENI serial engine that runs the I2C firmware,
 * as an event-driven state machine: the owner starts a transfer, then feeds the engine's M_IRQ
 * status (from its poll loop or a timer) until the transfer is done or failed, and calls the
 * timeout step when its deadline passes. Nothing here waits or spins, and nothing here touches
 * memory: the only I/O is through the caller's 32-bit aligned register accessors.
 *
 * TB710FU's amplifier bus is i2c_hub_4 (sm8650.dtsi: i2c_hub_4: i2c@990000, 0x4000 bytes,
 * GIC SPI 468, "qcom,geni-i2c-master-hub"). A master hub channel is the same GENI serial engine
 * register block as a classic geni-i2c SE, with two differences the Linux driver documents
 * (i2c-qcom-geni.c, driver data for "qcom,geni-i2c-master-hub"): it has a core clock, no DMA,
 * and a 16-word TX FIFO. Hence master_hub below: no DMA setup, a 16-word FIFO floor, and the
 * timing registers (CLK_CFG, SCL counters) reported rather than required, because the hub
 * wrapper programs them.
 *
 * A register write on the amplifier is {register, value} bytes; a register read is the command
 * bytes with the stop condition held, then the data bytes. Both fit the engine's in-FIFO form,
 * so this module carries no buffer/FIFO streaming path: the longest transfer the amplifier needs
 * is four bytes out and four bytes in.
 *
 * Provenance: the register offsets, the M_IRQ bits, the STP_STRETCH bit, the TX watermark fill
 * and the cancel-then-abort timeout sequence follow UniXNU-sheng's GeniI2c.c/.h (BSD-2-Clause-Patent),
 * whose engine state machine was written against the same Qualcomm GENI I2C firmware. The
 * master-hub differences come from the Linux driver's device data cited above. */
#ifndef TB710FU_GENI_I2C_H
#define TB710FU_GENI_I2C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Serial engine registers (offsets inside the channel's 16 KiB window). */
#define TB710FU_GENI_FW_REVISION 0x68u
#define TB710FU_GENI_IF_DISABLE 0x64u
#define TB710FU_GENI_CLK_CFG 0x48u
#define TB710FU_GENI_BYTE_GRAN 0x254u
#define TB710FU_GENI_DMA_MODE 0x258u
#define TB710FU_GENI_TX_PACK0 0x260u
#define TB710FU_GENI_TX_PACK1 0x264u
#define TB710FU_GENI_TX_LEN 0x26cu
#define TB710FU_GENI_RX_LEN 0x270u
#define TB710FU_GENI_SCL_COUNTERS 0x278u
#define TB710FU_GENI_RX_PACK0 0x284u
#define TB710FU_GENI_RX_PACK1 0x288u
#define TB710FU_GENI_M_CMD0 0x600u
#define TB710FU_GENI_M_CMD_CTRL 0x604u
#define TB710FU_GENI_M_IRQ_STATUS 0x610u
#define TB710FU_GENI_M_IRQ_EN 0x614u
#define TB710FU_GENI_M_IRQ_CLEAR 0x618u
#define TB710FU_GENI_TX_FIFO 0x700u
#define TB710FU_GENI_RX_FIFO 0x780u
#define TB710FU_GENI_TX_FIFO_STATUS 0x800u
#define TB710FU_GENI_RX_FIFO_STATUS 0x804u
#define TB710FU_GENI_TX_WATERMARK 0x80cu
#define TB710FU_GENI_RX_WATERMARK 0x810u
#define TB710FU_GENI_RX_RFR_WATERMARK 0x814u
#define TB710FU_GENI_SE_IRQ_EN 0xe1cu
#define TB710FU_GENI_HW_PARAM_0 0xe24u   /* bits 21:16: TX FIFO depth in words */
#define TB710FU_GENI_HW_PARAM_1 0xe28u   /* bits 21:16: RX FIFO depth in words */

#define TB710FU_GENI_TX_FIFO_WORDS(status) ((status) & 0x0fffffffu)
#define TB710FU_GENI_RX_FIFO_WORDS(status) ((status) & 0x01ffffffu)

/* M_IRQ bits. */
#define TB710FU_GENI_M_CMD_DONE (1u << 0)
#define TB710FU_GENI_M_CMD_OVERRUN (1u << 1)
#define TB710FU_GENI_M_ILLEGAL_CMD (1u << 2)
#define TB710FU_GENI_M_CMD_FAILURE (1u << 3)
#define TB710FU_GENI_M_CMD_CANCEL (1u << 4)
#define TB710FU_GENI_M_CMD_ABORT (1u << 5)
#define TB710FU_GENI_M_GP_IRQ (0x3fu << 9)    /* I2C: NACK, bus error, arbitration lost */
#define TB710FU_GENI_M_RX_WATERMARK (1u << 26)
#define TB710FU_GENI_M_RX_LAST (1u << 27)
#define TB710FU_GENI_M_TX_WATERMARK (1u << 30)
#define TB710FU_GENI_M_ERRORS                                                                   \
    (TB710FU_GENI_M_CMD_OVERRUN | TB710FU_GENI_M_ILLEGAL_CMD | TB710FU_GENI_M_CMD_FAILURE |       \
     TB710FU_GENI_M_GP_IRQ)
#define TB710FU_GENI_M_IRQ_WANTED                                                               \
    (TB710FU_GENI_M_CMD_DONE | TB710FU_GENI_M_ERRORS | TB710FU_GENI_M_CMD_CANCEL |                \
     TB710FU_GENI_M_CMD_ABORT | TB710FU_GENI_M_TX_WATERMARK)
#define TB710FU_GENI_SE_M_IRQ_EN (1u << 2)
#define TB710FU_GENI_CMD_CTRL_ABORT (1u << 1)
#define TB710FU_GENI_CMD_CTRL_CANCEL (1u << 2)
#define TB710FU_GENI_PROTO_I2C 3u
#define TB710FU_GENI_OP_WRITE 1u
#define TB710FU_GENI_OP_READ 2u
#define TB710FU_GENI_STOP_STRETCH (1u << 2)
#define TB710FU_GENI_MAX_BYTES 8u
#define TB710FU_GENI_MASTER_HUB_FIFO_WORDS 16u

typedef struct {
    /* A 32-bit aligned read/write of one engine register; the caller supplies the mapping. */
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void *context;
} Tb710fuGeniIo;

enum {
    Tb710fuGeniIdle = 0,
    Tb710fuGeniTxWait = 1,
    Tb710fuGeniWriteDone = 2,
    Tb710fuGeniReadDone = 3,
    Tb710fuGeniCancelling = 4,
    Tb710fuGeniAborting = 5,
};

enum {Tb710fuGeniPending = 0, Tb710fuGeniDone = 1, Tb710fuGeniFailed = 2};

enum {
    Tb710fuGeniOk = 0,
    Tb710fuGeniErrBusy = 1,
    Tb710fuGeniErrBus = 2,
    Tb710fuGeniErrTimeout = 3,
    Tb710fuGeniErrNoData = 4,
    Tb710fuGeniErrState = 5,
    Tb710fuGeniErrConfig = 6,   /* the engine is not an I2C engine, or is held in reset */
};

typedef struct {
    Tb710fuGeniIo io;
    uint8_t address;
    uint8_t phase;
    uint8_t reading;
    uint8_t hold;
    uint8_t error;
    uint8_t tx[TB710FU_GENI_MAX_BYTES];
    uint8_t rx[TB710FU_GENI_MAX_BYTES];
    uint8_t tx_words;
    uint8_t data_bytes;
    uint32_t length;        /* command bytes */
    uint32_t rx_length;
    uint32_t rx_filled;
    uint32_t error_status;  /* the M_IRQ status of the last failure */
    uint32_t proto;
    uint32_t clock_config;
    uint32_t timing_counters;
    uint32_t tx_fifo_words;
    uint32_t rx_fifo_words;
    uint32_t done;
    uint32_t failed;
    uint32_t timeouts;
    uint32_t stray;
} Tb710fuGeni;

/* Checks the engine carries the I2C firmware and enables exactly the interrupts this core handles.
 * master_hub relaxes the timing-register requirements (the hub wrapper programs them) and floors
 * the TX FIFO at 16 words. Returns Tb710fuGeniOk, or Tb710fuGeniErrConfig with the observed
 * fields left in the structure for the log. */
int tb710fu_geni_prepare(Tb710fuGeni *geni, const Tb710fuGeniIo *io, uint8_t address, int master_hub);

/* Starts a write of length (1..8) bytes; the data goes into the FIFO when the engine asks for it. */
int tb710fu_geni_write(Tb710fuGeni *geni, const uint8_t *bytes, uint32_t length);
/* Starts a write of command_length (1..8) bytes with the stop condition held, then a read of
 * read_length (1..8) bytes into geni->rx. */
int tb710fu_geni_read(Tb710fuGeni *geni, const uint8_t *command, uint32_t command_length,
                      uint32_t read_length);

/* Feeds M_IRQ status bits (already read and cleared by the caller). */
int tb710fu_geni_event(Tb710fuGeni *geni, uint32_t status);
/* Reads and clears the status itself (for a poll loop). */
int tb710fu_geni_poll(Tb710fuGeni *geni);
/* The owner's deadline passed: cancel a pending command, abort one that does not acknowledge. */
int tb710fu_geni_timeout(Tb710fuGeni *geni);

static inline int tb710fu_geni_busy(const Tb710fuGeni *geni) {return geni->phase != Tb710fuGeniIdle;}

#ifdef __cplusplus
}
#endif
#endif
