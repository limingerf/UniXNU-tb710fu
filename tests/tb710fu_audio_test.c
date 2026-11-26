/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests for the TB710FU audio package's hardware-independent half: the ACF container reader,
 * the GENI I2C state machine, the AW88461 sequencing, the GPR codec, the AudioReach packet
 * builders, the graph state machine, and the ABI layout. Everything runs against a fake serial
 * engine, a fake amplifier bus and a fake APM, so a compile and a run here says the framing and
 * the protocol are right before anything touches the board.
 *
 * Run by scripts/check-tb710fu-audio-host.py. When the stock ACF file is present under
 * build/audio-refs (it is not committed: firmware never is), the same reader is also pointed at
 * the real 3,504-byte file and its container numbers are asserted. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "Tb710fuAcf.h"
#include "Tb710fuAw88461.h"
#include "Tb710fuAudioAbi.h"
#include "Tb710fuAudioEngine.h"
#include "Tb710fuAudioReach.h"
#include "Tb710fuGeniI2c.h"
#include "Tb710fuGpr.h"

static unsigned gChecks, gFailures;

#define CHECK(cond, ...)                                                                                  \
    do {                                                                                                  \
        gChecks++;                                                                                        \
        if (!(cond)) {                                                                                    \
            gFailures++;                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            printf(__VA_ARGS__);                                                                          \
            printf("\n");                                                                                 \
        }                                                                                                 \
    } while (0)

#define CHECK_EQ(a, b, ...)                                                                               \
    do {                                                                                                  \
        const long long tbGot = (long long)(a), tbWant = (long long)(b);                                  \
        gChecks++;                                                                                        \
        if (tbGot != tbWant) {                                                                            \
            gFailures++;                                                                                  \
            printf("FAIL %s:%d: got %lld want %lld -- ", __FILE__, __LINE__, tbGot, tbWant);              \
            printf(__VA_ARGS__);                                                                          \
            printf("\n");                                                                                 \
        }                                                                                                 \
    } while (0)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Is a 32-bit word anywhere in the packet? The parameter ids are what identifies a block. */
static int containsWord(const uint8_t *packet, uint32_t bytes, uint32_t word)
{
    for (uint32_t i = 0; i + 4 <= bytes; ++i)
        if (le32(packet + i) == word) return 1;
    return 0;
}

/* ------------------------------- fake serial engine ------------------------------- */

#define FAKE_REGS 0x400u

typedef struct {
    uint32_t reg[FAKE_REGS];
    uint32_t tx_words[8];
    uint32_t tx_count;
    uint32_t rx_words[8];
    uint32_t rx_count;
} FakeSe;

static uint32_t fakeRead(void *context, uint32_t offset)
{
    FakeSe *se = (FakeSe *)context;
    if ((offset >> 2) >= FAKE_REGS) return 0;
    return se->reg[offset >> 2];
}

static void fakeWrite(void *context, uint32_t offset, uint32_t value)
{
    FakeSe *se = (FakeSe *)context;
    if ((offset >> 2) >= FAKE_REGS) return;
    se->reg[offset >> 2] = value;
    if (offset == TB710FU_GENI_TX_FIFO && se->tx_count < 8) se->tx_words[se->tx_count++] = value;
}

static void fakeSeReset(FakeSe *se)
{
    memset(se, 0, sizeof *se);
    se->reg[TB710FU_GENI_FW_REVISION >> 2] = (TB710FU_GENI_PROTO_I2C << 8) | 0x07;   /* minor 7 */
    se->reg[TB710FU_GENI_CLK_CFG >> 2] = 1;                                          /* clock enabled */
    se->reg[TB710FU_GENI_SCL_COUNTERS >> 2] = 0x00030303;                            /* configured */
    se->reg[TB710FU_GENI_HW_PARAM_0 >> 2] = 0x00100000;                              /* 16-word TX FIFO */
    se->reg[TB710FU_GENI_HW_PARAM_1 >> 2] = 0x00100000;
}

static Tb710fuGeniIo fakeIo(FakeSe *se)
{
    Tb710fuGeniIo io = {fakeRead, fakeWrite, se};
    return io;
}

static void testGeni(void)
{
    FakeSe se;
    Tb710fuGeni geni;
    fakeSeReset(&se);
    const Tb710fuGeniIo io = fakeIo(&se);
    CHECK_EQ(tb710fu_geni_prepare(&geni, &io, 0x34, 1), Tb710fuGeniOk, "master hub channel prepares");
    CHECK_EQ(geni.proto, TB710FU_GENI_PROTO_I2C, "protocol is the I2C firmware");
    CHECK_EQ(geni.tx_fifo_words, 16, "master hub FIFO floor");
    CHECK_EQ(se.reg[TB710FU_GENI_M_IRQ_EN >> 2], TB710FU_GENI_M_IRQ_WANTED, "only our interrupts are enabled");
    CHECK_EQ(se.reg[TB710FU_GENI_DMA_MODE >> 2], 0, "no DMA: the hub has none");

    /* A register write: four bytes, write opcode, the address in bits 15:9, no stop stretch. */
    const uint8_t out[4] = {0x00, 0x03, 0xff, 0xff};
    CHECK_EQ(tb710fu_geni_write(&geni, out, 4), Tb710fuGeniOk, "write accepted");
    CHECK_EQ(se.reg[TB710FU_GENI_TX_LEN >> 2], 4, "four command bytes announced");
    const uint32_t cmd0 = se.reg[TB710FU_GENI_M_CMD0 >> 2];
    CHECK_EQ(cmd0 >> 27, TB710FU_GENI_OP_WRITE, "write opcode");
    CHECK_EQ((cmd0 >> 9) & 0x7f, 0x34, "the device address travels in M_CMD0");
    CHECK_EQ(cmd0 & TB710FU_GENI_STOP_STRETCH, 0, "a write ends with a stop");
    CHECK_EQ(tb710fu_geni_poll(&geni), Tb710fuGeniPending, "nothing yet");
    /* The engine asks for the FIFO, we push one word holding the four bytes. */
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_TX_WATERMARK), Tb710fuGeniPending, "watermark feeds");
    CHECK_EQ(se.tx_count, 1, "one FIFO word for four bytes");
    CHECK_EQ(se.tx_words[0], 0xffff0300u, "bytes packed least significant first");
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_CMD_DONE), Tb710fuGeniDone, "write completes");

    /* A register read: command with the stop held, then the read command. */
    const uint8_t in[2] = {0x00, 0x03};
    se.tx_count = 0;
    CHECK_EQ(tb710fu_geni_read(&geni, in, 2, 2), Tb710fuGeniOk, "read accepted");
    CHECK_EQ(se.reg[TB710FU_GENI_TX_LEN >> 2], 2, "two command bytes");
    CHECK(se.reg[TB710FU_GENI_M_CMD0 >> 2] & TB710FU_GENI_STOP_STRETCH, "read holds the stop");
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_TX_WATERMARK), Tb710fuGeniPending, "watermark feeds the command");
    CHECK_EQ(se.tx_words[0], 0x0300u, "command word");
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_CMD_DONE), Tb710fuGeniPending, "the read starts");
    CHECK_EQ(se.reg[TB710FU_GENI_M_CMD0 >> 2] >> 27, TB710FU_GENI_OP_READ, "read opcode");
    CHECK_EQ(se.reg[TB710FU_GENI_RX_LEN >> 2], 2, "two bytes asked for");
    se.reg[TB710FU_GENI_RX_FIFO >> 2] = 0x0000ffffu;   /* busy degrade: one word, two bytes */
    se.reg[TB710FU_GENI_RX_FIFO_STATUS >> 2] = 1;
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_CMD_DONE), Tb710fuGeniDone, "read completes");
    CHECK_EQ(geni.rx_filled, 2, "two bytes out of the FIFO");
    CHECK_EQ(geni.rx[0], 0xff, "first byte");
    CHECK_EQ(geni.rx[1], 0xff, "second byte");
    CHECK_EQ(geni.done, 2, "two completions counted");

    /* A failing command: the error bits decide, CMD_DONE or not. */
    se.tx_count = 0;
    CHECK_EQ(tb710fu_geni_write(&geni, out, 4), Tb710fuGeniOk, "write accepted");
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_TX_WATERMARK), Tb710fuGeniPending, "watermark");
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_CMD_FAILURE | TB710FU_GENI_M_CMD_DONE),
             Tb710fuGeniFailed, "NACK is a failure");
    CHECK_EQ(geni.error, Tb710fuGeniErrBus, "bus error recorded");
    CHECK_EQ(geni.failed, 1, "one failure counted");

    /* A stuck command: cancel, then abort, then the transfer is given up. */
    CHECK_EQ(tb710fu_geni_write(&geni, out, 4), Tb710fuGeniOk, "write accepted");
    CHECK_EQ(tb710fu_geni_timeout(&geni), Tb710fuGeniPending, "cancel ordered");
    CHECK_EQ(se.reg[TB710FU_GENI_M_CMD_CTRL >> 2] & TB710FU_GENI_CMD_CTRL_CANCEL, TB710FU_GENI_CMD_CTRL_CANCEL,
             "cancel register written");
    CHECK_EQ(geni.phase, Tb710fuGeniCancelling, "cancelling");
    CHECK_EQ(tb710fu_geni_timeout(&geni), Tb710fuGeniPending, "abort ordered");
    CHECK_EQ(se.reg[TB710FU_GENI_M_CMD_CTRL >> 2] & TB710FU_GENI_CMD_CTRL_ABORT, TB710FU_GENI_CMD_CTRL_ABORT,
             "abort register written");
    CHECK_EQ(tb710fu_geni_event(&geni, TB710FU_GENI_M_CMD_ABORT), Tb710fuGeniFailed, "abort ends it");
    CHECK_EQ(geni.error, Tb710fuGeniErrTimeout, "timeout recorded");
    CHECK_EQ(tb710fu_geni_busy(&geni), 0, "engine idle again");

    /* A serial engine that is not an I2C engine, or held disabled, is refused before any write. */
    fakeSeReset(&se);
    se.reg[TB710FU_GENI_FW_REVISION >> 2] = (5u << 8);   /* SPI */
    Tb710fuGeni other;
    CHECK_EQ(tb710fu_geni_prepare(&other, &io, 0x34, 1), Tb710fuGeniErrConfig, "non-I2C protocol refused");
    se.reg[TB710FU_GENI_FW_REVISION >> 2] = (TB710FU_GENI_PROTO_I2C << 8);
    se.reg[TB710FU_GENI_IF_DISABLE >> 2] = 1;
    CHECK_EQ(tb710fu_geni_prepare(&other, &io, 0x34, 1), Tb710fuGeniErrConfig, "disabled engine refused");
    se.reg[TB710FU_GENI_IF_DISABLE >> 2] = 0;
    se.reg[TB710FU_GENI_CLK_CFG >> 2] = 0;
    se.reg[TB710FU_GENI_SCL_COUNTERS >> 2] = 0;
    CHECK_EQ(tb710fu_geni_prepare(&other, &io, 0x34, 0), Tb710fuGeniErrConfig, "a classic SE needs its clock");
    CHECK_EQ(tb710fu_geni_prepare(&other, &io, 0x34, 1), Tb710fuGeniOk, "the hub wrapper owns the timing");
    printf("ok   geni: prepare, write framing, read framing, NACK, cancel/abort, refusals\n");
}

/* -------------------------------- fake ACF file --------------------------------- */

#define ACF_RECORDS 8u
#define ACF_PROFILE 0x16Cu
#define ACF_PAIRS 75u
static uint8_t gAcf[TB710FU_ACF_HEADER_BYTES + ACF_RECORDS * TB710FU_ACF_RECORD_BYTES + ACF_RECORDS * ACF_PROFILE];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void buildAcf(void)
{
    memset(gAcf, 0, sizeof gAcf);
    put32(gAcf + Tb710fuAcfAtMagic, TB710FU_ACF_MAGIC);
    memcpy(gAcf + Tb710fuAcfAtProject, "P805", 4);
    memcpy(gAcf + Tb710fuAcfAtVendor, "awinic", 6);
    put32(gAcf + Tb710fuAcfAtFormat, 0x01000000u);
    put32(gAcf + Tb710fuAcfAtImageId, 0x2B2u);
    put32(gAcf + Tb710fuAcfAtTableBytes, ACF_RECORDS * TB710FU_ACF_RECORD_BYTES);
    put32(gAcf + Tb710fuAcfAtRecordCount, ACF_RECORDS);
    put32(gAcf + Tb710fuAcfAtTableOffset, TB710FU_ACF_HEADER_BYTES);
    put32(gAcf + Tb710fuAcfAtProfileCount, 1);
    const uint32_t table = TB710FU_ACF_HEADER_BYTES;
    const uint32_t first = table + ACF_RECORDS * TB710FU_ACF_RECORD_BYTES;
    for (uint32_t i = 0; i < ACF_RECORDS; ++i) {
        uint8_t *r = gAcf + table + i * TB710FU_ACF_RECORD_BYTES;
        put32(r + Tb710fuAcfRecAtType, TB710FU_ACF_TYPE_PROFILE);
        memcpy(r + Tb710fuAcfRecAtModel, "aw88461", 8);
        put16(r + Tb710fuAcfRecAtChannel, (uint16_t)(i / 2));
        put16(r + 0x16, 0xFFFF);
        put16(r + 0x18, 0xFFFF);
        put16(r + Tb710fuAcfRecAtRateTag, (uint16_t)((i % 2) ? 10 : 0));
        put16(r + 0x1E, 4);
        put32(r + Tb710fuAcfRecAtStride, ACF_PROFILE);
        put32(r + Tb710fuAcfRecAtOffset, first + i * ACF_PROFILE);
    }
    for (uint32_t i = 0; i < ACF_RECORDS; ++i) {
        uint8_t *p = gAcf + first + i * ACF_PROFILE;
        put32(p + Tb710fuAcfProfAtIdentity, 0x47B5u + i);
        memcpy(p + Tb710fuAcfProfAtModel, "AW88461", 8);
        put32(p + Tb710fuAcfProfAtPairs, ACF_PAIRS);
        for (uint32_t k = 0; k < ACF_PAIRS; ++k) {
            put16(p + Tb710fuAcfProfAtBody + k * 4, (uint16_t)(0x3u + k));
            put16(p + Tb710fuAcfProfAtBody + k * 4 + 2, (uint16_t)(0xFF00u + i));
        }
    }
}

static void testAcf(void)
{
    buildAcf();
    Tb710fuAcf acf;
    char why[48];
    CHECK_EQ(tb710fu_acf_open(&acf, gAcf, (uint32_t)sizeof gAcf, why, sizeof why), Tb710fuAcfOk,
             "the synthetic container opens (%s)", why);
    CHECK_EQ(strcmp(acf.vendor, "awinic"), 0, "vendor");
    CHECK_EQ(strcmp(acf.project, "P805"), 0, "project tag");
    CHECK_EQ(acf.record_count, ACF_RECORDS, "eight records");
    CHECK_EQ(acf.image_id, 0x2B2u, "image id");
    CHECK_EQ(acf.record[0].pairs, ACF_PAIRS, "seventy-five pairs");
    CHECK_EQ(acf.record[0].stride, ACF_PROFILE, "profile stride");
    CHECK_EQ(acf.record[0].channel, 0, "channel of the first record");
    CHECK_EQ(acf.record[1].rate_tag, 10, "second record is the 96 kHz tuning");
    CHECK_EQ(strcmp(acf.record[0].model, "aw88461"), 0, "record model");
    CHECK_EQ(strcmp(acf.record[0].profile_model, "AW88461"), 0, "profile model");
    const Tb710fuAcfRecord *ch1 = tb710fu_acf_find(&acf, 1, 0);
    CHECK(ch1 != NULL, "channel 1 has the 48 kHz profile");
    CHECK_EQ(ch1->channel, 1, "found the right record");
    CHECK_EQ(ch1->identity, 0x47B5u + 2, "identity travels with the record");
    uint16_t address = 0, value = 0;
    CHECK_EQ(tb710fu_acf_pair(&acf, ch1, 0, &address, &value), 0, "the first pair reads");
    CHECK_EQ(address, 0x03, "first register");
    CHECK_EQ(value, 0xFF02u, "first value");
    CHECK_EQ(tb710fu_acf_pair(&acf, ch1, ACF_PAIRS, &address, &value), -1, "past the run refused");

    /* Refusals: each one a container that does not add up. */
    uint8_t broken[sizeof gAcf];
    memcpy(broken, gAcf, sizeof gAcf);
    put32(broken + Tb710fuAcfAtMagic, 0xDEADBEEFu);
    CHECK_EQ(tb710fu_acf_open(&acf, broken, sizeof broken, why, sizeof why), Tb710fuAcfMagic, "magic refused (%s)", why);
    memcpy(broken, gAcf, sizeof gAcf);
    memcpy(broken + Tb710fuAcfAtVendor, "other", 6);
    CHECK_EQ(tb710fu_acf_open(&acf, broken, sizeof broken, why, sizeof why), Tb710fuAcfVendor, "vendor refused");
    memcpy(broken, gAcf, sizeof gAcf);
    put32(broken + Tb710fuAcfAtRecordCount, ACF_RECORDS + 1);
    CHECK_EQ(tb710fu_acf_open(&acf, broken, sizeof broken, why, sizeof why), Tb710fuAcfTable, "count/table mismatch refused");
    memcpy(broken, gAcf, sizeof gAcf);
    put32(broken + TB710FU_ACF_HEADER_BYTES + Tb710fuAcfRecAtOffset, TB710FU_ACF_HEADER_BYTES);
    CHECK_EQ(tb710fu_acf_open(&acf, broken, sizeof broken, why, sizeof why), Tb710fuAcfErrRecord,
             "a profile pointing into the table refused");
    memcpy(broken, gAcf, sizeof gAcf);
    put32(broken + TB710FU_ACF_HEADER_BYTES + ACF_RECORDS * TB710FU_ACF_RECORD_BYTES + Tb710fuAcfProfAtPairs,
          ACF_PAIRS + 1);
    CHECK_EQ(tb710fu_acf_open(&acf, broken, sizeof broken, why, sizeof why), Tb710fuAcfProfile,
             "a pair run longer than its block refused");
    CHECK_EQ(tb710fu_acf_open(&acf, gAcf, TB710FU_ACF_HEADER_BYTES - 1, why, sizeof why), Tb710fuAcfShort,
             "a short file refused");
    printf("ok   acf: container, records, lookup, pairs, five refusals\n");

    /* The shipped file, when the caller staged it (never committed: firmware is not source). */
    const char *path = getenv("TB710FU_ACF_FILE");
    if (!path || !*path) path = "build/audio-refs/aw88461_tb710fu.bin";
    FILE *file = fopen(path, "rb");
    if (!file) {
        printf("skip acf: no stock file at %s (set TB710FU_ACF_FILE to check it)\n", path);
        return;
    }
    uint8_t *real = (uint8_t *)malloc(64 * 1024);
    if (!real) { fclose(file); return; }
    const size_t got = fread(real, 1, 64 * 1024, file);
    fclose(file);
    CHECK_EQ(got, 3504, "the stock file is 3,504 bytes");
    Tb710fuAcf stock;
    const int opened = tb710fu_acf_open(&stock, real, (uint32_t)got, why, sizeof why);
    CHECK_EQ(opened, Tb710fuAcfOk, "the stock container opens (%s)", why);
    if (opened == Tb710fuAcfOk) {
        CHECK_EQ(stock.record_count, 8, "eight profiles in the stock file");
        CHECK_EQ(stock.table_bytes, 512, "table is eight 64-byte records");
        CHECK_EQ(stock.image_id, 0x2B2u, "stock image id");
        CHECK_EQ(strcmp(stock.project, "P805"), 0, "stock project tag");
        CHECK_EQ(stock.record[0].pairs, 75, "stock pairs per profile");
        CHECK_EQ(stock.record[0].stride, 364, "stock stride");
        CHECK_EQ(stock.record[0].identity, 0x4A38u, "stock profile 0 identity");
        CHECK_EQ(stock.record[1].identity, 0x47B5u, "stock profile 1 identity");
        uint16_t a = 0, v = 0;
        CHECK_EQ(tb710fu_acf_pair(&stock, &stock.record[0], 0, &a, &v), 0, "stock first pair");
        CHECK_EQ(a, 0x0003, "stock first register");
        CHECK_EQ(v, 0xFFFF, "stock control word");
        const Tb710fuAcfRecord *ch2 = tb710fu_acf_find(&stock, 2, 0);
        CHECK(ch2 != NULL, "stock channel 2 profile present");
        if (ch2) CHECK_EQ(ch2->identity, 0x4A68u, "stock channel 2 identity");
    }
    free(real);
    printf("ok   acf: stock file at %s\n", path);
}

/* ------------------------------- fake amplifier bus ------------------------------ */

#define AMP_REGS 256u
typedef struct {
    uint16_t reg[AMP_REGS];
    uint32_t writes, reads, writes_before_first_read;
    int nack_all;
    int nack_address;
} FakeAmp;

static int fakeAmpWrite(void *context, uint16_t address, uint16_t value)
{
    FakeAmp *bus = (FakeAmp *)context;
    if (bus->nack_all) return -1;
    if (bus->nack_address >= 0 && address == (uint16_t)bus->nack_address) return -1;
    if (address >= AMP_REGS) return -1;
    bus->reg[address] = value;
    bus->writes++;
    return 0;
}

static int fakeAmpRead(void *context, uint16_t address, uint16_t *value)
{
    FakeAmp *bus = (FakeAmp *)context;
    if (bus->nack_all) return -1;
    if (address >= AMP_REGS) return -1;
    bus->reads++;
    *value = bus->reg[address];
    return 0;
}

static void testAmp(void)
{
    buildAcf();
    Tb710fuAcf acf;
    char why[48];
    CHECK_EQ(tb710fu_acf_open(&acf, gAcf, (uint32_t)sizeof gAcf, why, sizeof why), Tb710fuAcfOk, "container");
    const Tb710fuAcfRecord *record = tb710fu_acf_find(&acf, 0, 0);
    CHECK(record != NULL, "channel 0 profile");

    FakeAmp bus;
    memset(&bus, 0, sizeof bus);
    bus.nack_address = -1;
    bus.reg[0] = 0x1201;   /* a plausible identity read */
    bus.reg[1] = 0x0000;
    bus.reg[2] = 0x0001;
    bus.reg[3] = 0xFFFF;
    Tb710fuAmpBus ampBus = {fakeAmpWrite, fakeAmpRead, &bus};
    Tb710fuAmp amp;
    CHECK_EQ(tb710fu_amp_probe(&amp, &ampBus, 0x34, 0, record->identity), 0, "a part that answers is present");
    CHECK_EQ(amp.probe[0], 0x1201, "probe recorded the first read");
    CHECK_EQ(amp.identity, record->identity, "identity comes from the ACF");

    bus.nack_all = 1;
    Tb710fuAmp silent;
    CHECK_EQ(tb710fu_amp_probe(&silent, &ampBus, 0x35, 1, 0), -1, "a silent part is not present");
    CHECK_EQ(silent.present, 0, "not present");
    bus.nack_all = 0;

    /* A part that answers but reads all-ones is a floating bus, not a part. */
    memset(bus.reg, 0xFF, sizeof bus.reg);
    Tb710fuAmp floating;
    CHECK_EQ(tb710fu_amp_probe(&floating, &ampBus, 0x36, 2, 0), -1, "all-ones reads are refused");
    memset(&bus, 0, sizeof bus);
    bus.nack_address = -1;

    /* Load: the whole profile in order, with the read-back spot checks. */
    CHECK_EQ(tb710fu_amp_load(&amp, &acf, record, 8), 0, "profile replayed");
    CHECK_EQ(amp.pairs, ACF_PAIRS, "every pair written");
    CHECK_EQ(amp.writes, ACF_PAIRS, "one write per pair");
    CHECK_EQ(amp.readback, 10, "read-back every eighth pair");
    CHECK_EQ(amp.readback_bad, 0, "read-back matches the writes");
    CHECK(bus.reg[0x03] == 0xFF00, "the first pair reached the part");
    CHECK(bus.reg[0x03 + ACF_PAIRS - 1] == 0xFF00, "the last pair reached the part");

    /* A write the part refuses: counted, and after eight in a row the load gives up. */
    bus.nack_address = 0x10;
    Tb710fuAmp stubborn;
    CHECK_EQ(tb710fu_amp_probe(&stubborn, &ampBus, 0x37, 3, 0), 0, "present");
    const int load = tb710fu_amp_load(&stubborn, &acf, record, 0);
    CHECK_EQ(load, -1, "a refused pair fails the load");
    CHECK(stubborn.pair_errors >= 1, "the refusal is counted");

    /* Mute and unmute write the control word the ACF itself writes first. */
    CHECK_EQ(tb710fu_amp_mute(&amp, 1), 0, "mute accepted");
    CHECK_EQ(bus.reg[TB710FU_AMP_REG_CONTROL], TB710FU_AMP_CONTROL_STANDBY, "standby word");
    CHECK_EQ(amp.muted, 1, "muted");
    CHECK_EQ(tb710fu_amp_mute(&amp, 0), 0, "unmute accepted");
    CHECK_EQ(bus.reg[TB710FU_AMP_REG_CONTROL], TB710FU_AMP_CONTROL_ACTIVE, "active word");
    CHECK_EQ(amp.muted, 0, "unmuted");

    /* Volume: 0 dB is the top of the six-bit field, -96 dB the bottom, 1.5 dB per step. */
    CHECK_EQ(tb710fu_amp_volume_word(0), 0x3F, "0 dB");
    CHECK_EQ(tb710fu_amp_volume_word(-96), 0x00, "-96 dB");
    CHECK_EQ(tb710fu_amp_volume_word(-1), 0x3E, "-1 dB is one step below unity");
    CHECK_EQ(tb710fu_amp_volume_word(-48), 0x1F, "-48 dB is half the field");
    CHECK_EQ(tb710fu_amp_volume_word(-200), 0x00, "below the floor clamps");
    bus.reg[TB710FU_AMP_REG_VOLUME] = 0xF250;   /* channel tuning that must survive the patch */
    CHECK_EQ(tb710fu_amp_volume(&amp, -12), 0, "volume accepted");
    CHECK_EQ((bus.reg[TB710FU_AMP_REG_VOLUME] >> 8) & 0x3F, 0x37, "-12 dB is eight steps down");
    CHECK_EQ(bus.reg[TB710FU_AMP_REG_VOLUME] & 0x00C0, 0x0040, "the rest of the word is untouched");
    printf("ok   amp: probe, presence rules, load, refused writes, mute, volume field\n");
}

/* ------------------------------------- GPR -------------------------------------- */

static void testGpr(void)
{
    Tb710fuGpr gpr;
    tb710fu_gpr_init(&gpr, TB710FU_GPR_DOMAIN_APPS, TB710FU_GPR_DOMAIN_ADSP, 1);
    uint8_t packet[64];
    uint32_t token = 0;
    const uint32_t payload[2] = {0x11223344u, 0xAABBCCDDu};
    const uint32_t bytes = tb710fu_gpr_encode(&gpr, packet, sizeof packet, TB710FU_GPR_PORT_APM, 0x01001006u,
                                              payload, sizeof payload, &token);
    CHECK_EQ(bytes, TB710FU_GPR_HEADER_BYTES + 8, "packet size is header plus payload");
    CHECK_EQ(token, 1, "first token");
    Tb710fuGprPacket decoded;
    CHECK_EQ(tb710fu_gpr_decode(packet, bytes, &decoded), Tb710fuGprOk, "packet decodes");
    CHECK_EQ(decoded.version, 0, "version 0");
    CHECK_EQ(decoded.header_bytes, 24, "six-word header");
    CHECK_EQ(decoded.size, bytes, "size field");
    CHECK_EQ(decoded.dst_domain, TB710FU_GPR_DOMAIN_ADSP, "destination domain");
    CHECK_EQ(decoded.src_domain, TB710FU_GPR_DOMAIN_APPS, "source domain");
    CHECK_EQ(decoded.dst_port, TB710FU_GPR_PORT_APM, "destination port");
    CHECK_EQ(decoded.src_port, 1, "source port");
    CHECK_EQ(decoded.opcode, 0x01001006u, "opcode");
    CHECK_EQ(decoded.payload_bytes, 8, "payload bytes");

    uint32_t token2 = 0;
    (void)tb710fu_gpr_encode(&gpr, packet, sizeof packet, 1, 0x01001000u, NULL, 0, &token2);
    CHECK_EQ(token2, 2, "tokens increment");

    /* Malformed packets are refused with the reason, not guessed at. */
    uint8_t bad[64];
    memcpy(bad, packet, TB710FU_GPR_HEADER_BYTES);
    put32(bad, 1u | (6u << 4) | ((TB710FU_GPR_HEADER_BYTES + 8) << 8));   /* version 1 */
    CHECK_EQ(tb710fu_gpr_decode(bad, TB710FU_GPR_HEADER_BYTES, &decoded), Tb710fuGprBadVersion, "version refused");
    put32(bad, (6u << 4) | ((TB710FU_GPR_HEADER_BYTES + 400) << 8));
    CHECK_EQ(tb710fu_gpr_decode(bad, TB710FU_GPR_HEADER_BYTES, &decoded), Tb710fuGprBadSize, "size beyond the bytes refused");
    put32(bad, (4u << 4) | ((TB710FU_GPR_HEADER_BYTES) << 8));
    CHECK_EQ(tb710fu_gpr_decode(bad, TB710FU_GPR_HEADER_BYTES, &decoded), Tb710fuGprBadHeader, "short header refused");
    CHECK_EQ(tb710fu_gpr_decode(bad, 8, &decoded), Tb710fuGprShort, "truncated packet refused");

    /* Basic result: {opcode, status}. A reply as the DSP builds it: from ADSP, addressed to us. */
    uint8_t result[32];
    uint32_t answer_opcode = 0, status = 0;
    Tb710fuGpr dsp;
    tb710fu_gpr_init(&dsp, TB710FU_GPR_DOMAIN_ADSP, TB710FU_GPR_DOMAIN_APPS, 2);
    const uint32_t result_payload[2] = {0x01001006u, 0u};
    const uint32_t rb = tb710fu_gpr_encode(&dsp, result, sizeof result, 1, TB710FU_GPR_OP_BASIC_RESULT,
                                           result_payload, sizeof result_payload, &token);
    CHECK_EQ(rb, TB710FU_GPR_HEADER_BYTES + 8, "result size");
    CHECK_EQ(tb710fu_gpr_decode(result, rb, &decoded), Tb710fuGprOk, "result decodes");
    CHECK_EQ(tb710fu_gpr_basic_result(&decoded, &answer_opcode, &status), 0, "basic result parsed");
    CHECK_EQ(answer_opcode, 0x01001006u, "answered opcode");
    CHECK_EQ(status, 0, "status");
    CHECK_EQ(tb710fu_gpr_is_answer(&decoded, 1, TB710FU_GPR_DOMAIN_APPS), 1, "answer matches our port and domain");
    CHECK_EQ(tb710fu_gpr_is_answer(&decoded, 2, TB710FU_GPR_DOMAIN_APPS), 0, "another client's answer does not");
    CHECK_EQ(tb710fu_gpr_is_answer(&decoded, 1, TB710FU_GPR_DOMAIN_ADSP), 0, "a packet we sent is not an answer");
    printf("ok   gpr: header, routing, tokens, four refusals, basic result\n");
}

/* ---------------------------------- AudioReach ----------------------------------- */

static void testReach(void)
{
    Tb710fuGpr gpr;
    tb710fu_gpr_init(&gpr, TB710FU_GPR_DOMAIN_APPS, TB710FU_GPR_DOMAIN_ADSP, 1);
    uint8_t packet[TB710FU_GPR_MAX_PACKET];
    Tb710fuFormat speaker = {48000, 32, 4};
    Tb710fuTdmConfig tdm = {0, 1, 0, 1, 4, 32, 4, 0};
    Tb710fuFormat capture = {48000, 16, 2};
    Tb710fuCaptureConfig mic = {1, 4, 0x3};
    Tb710fuGprPacket decoded;

    const uint32_t size = tb710fu_ar_open_speaker(packet, (uint32_t)sizeof packet, &gpr, 7);
    CHECK(size > TB710FU_GPR_HEADER_BYTES + 64, "speaker graph packet has content");
    CHECK_EQ(tb710fu_gpr_decode(packet, size, &decoded), Tb710fuGprOk, "speaker packet decodes");
    CHECK_EQ(decoded.size, size, "size patched to the real length");
    CHECK_EQ(decoded.opcode, TB710FU_AR_OP_GRAPH_OPEN, "GRAPH_OPEN");
    CHECK_EQ(decoded.token, 7, "token travels");
    CHECK_EQ((size - TB710FU_GPR_HEADER_BYTES) % 8, 0, "payload is a whole number of parameter blocks");

    const uint32_t cfg = tb710fu_ar_config_speaker(packet, (uint32_t)sizeof packet, &gpr, 9, &speaker, &tdm);
    CHECK_EQ(tb710fu_gpr_decode(packet, cfg, &decoded), Tb710fuGprOk, "speaker config decodes");
    CHECK_EQ(decoded.opcode, TB710FU_AR_OP_SET_CFG, "SET_CFG");
    CHECK_EQ(decoded.dst_port, TB710FU_AR_APM_IID, "config goes to the APM");
    CHECK(containsWord(packet + TB710FU_GPR_HEADER_BYTES, size - TB710FU_GPR_HEADER_BYTES, 0x08001019u),
          "the I2S interface parameter is present");
    Tb710fuTdmConfig withTdm = tdm;
    withTdm.send_tdm_parameter = 1;
    const uint32_t cfgTdm = tb710fu_ar_config_speaker(packet, (uint32_t)sizeof packet, &gpr, 10, &speaker, &withTdm);
    CHECK_EQ(cfgTdm, cfg + 24, "the optional TDM parameter adds a 16-byte block plus padding");
    const uint32_t cfgPlain = tb710fu_ar_config_speaker(packet, (uint32_t)sizeof packet, &gpr, 10, &speaker, &tdm);
    CHECK_EQ(cfgTdm, cfgPlain + 24, "and it is only there when asked for");

    const uint32_t playbackCfg = tb710fu_ar_config_playback(packet, (uint32_t)sizeof packet, &gpr, 11, &speaker);
    CHECK_EQ(tb710fu_gpr_decode(packet, playbackCfg, &decoded), Tb710fuGprOk, "playback config decodes");
    CHECK(containsWord(packet, playbackCfg, 0x08001008u), "PCM output parameter is present");

    const uint32_t shmem = tb710fu_ar_config_playback_shmem(packet, (uint32_t)sizeof packet, &gpr, 12, &speaker);
    CHECK_EQ(tb710fu_gpr_decode(packet, shmem, &decoded), Tb710fuGprOk, "shared-memory config decodes");
    CHECK_EQ(decoded.dst_port, TB710FU_AR_IID_PLAY_SHMEM, "the endpoint's own port receives its media format");

    const uint32_t micCfg = tb710fu_ar_config_microphone(packet, (uint32_t)sizeof packet, &gpr, 13, &capture, &mic);
    CHECK_EQ(tb710fu_gpr_decode(packet, micCfg, &decoded), Tb710fuGprOk, "microphone config decodes");
    CHECK(containsWord(packet, micCfg, 0x08001063u), "codec-DMA interface parameter is present");

    const uint32_t map = tb710fu_ar_map(packet, (uint32_t)sizeof packet, &gpr, 14, 0x0000000080000000ull, 0x4000);
    CHECK_EQ(tb710fu_gpr_decode(packet, map, &decoded), Tb710fuGprOk, "map decodes");
    CHECK_EQ(map, TB710FU_GPR_HEADER_BYTES + 20u + 16u,
             "map is a bare region description plus the trailing command space, as the reference builds it");

    const uint32_t write = tb710fu_ar_write(packet, (uint32_t)sizeof packet, &gpr, 15, 0x80000000ull, 0x1234, 0x1000);
    CHECK_EQ(tb710fu_gpr_decode(packet, write, &decoded), Tb710fuGprOk, "write decodes");
    CHECK_EQ(decoded.dst_port, TB710FU_AR_IID_PLAY_SHMEM, "write goes to the playback endpoint");
    CHECK_EQ(decoded.opcode, TB710FU_AR_OP_WRITE, "WRITE opcode");

    const uint32_t read = tb710fu_ar_read(packet, (uint32_t)sizeof packet, &gpr, 16, 0x80000000ull, 0x1234, 0x1000);
    CHECK_EQ(tb710fu_gpr_decode(packet, read, &decoded), Tb710fuGprOk, "read decodes");
    CHECK_EQ(decoded.opcode, TB710FU_AR_OP_READ, "READ opcode");

    const uint32_t prepare = tb710fu_ar_graph_command(packet, (uint32_t)sizeof packet, &gpr, 17,
                                                      TB710FU_AR_OP_GRAPH_PREPARE, TB710FU_AR_SG_SPEAKER);
    CHECK_EQ(tb710fu_gpr_decode(packet, prepare, &decoded), Tb710fuGprOk, "prepare decodes");
    CHECK_EQ(decoded.opcode, TB710FU_AR_OP_GRAPH_PREPARE, "PREPARE opcode");

    /* An answer of every shape the engine validates. */
    Tb710fuArAnswer answer;
    uint8_t reply[64];
    uint32_t token = 0;
    const uint32_t result[2] = {TB710FU_AR_OP_GRAPH_PREPARE, 0x2u};
    const uint32_t replyBytes = tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1, TB710FU_GPR_OP_BASIC_RESULT,
                                                   result, sizeof result, &token);
    CHECK_EQ(tb710fu_ar_parse(reply, replyBytes, &answer), Tb710fuArBasicResult, "basic result parsed");
    CHECK_EQ(answer.result_opcode, TB710FU_AR_OP_GRAPH_PREPARE, "answered opcode");
    CHECK_EQ(answer.status, 2, "failure status");
    const uint32_t mapDone[1] = {0x4321u};
    (void)tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1, TB710FU_AR_OP_MEM_MAP_DONE, mapDone, 4, &token);
    CHECK_EQ(tb710fu_ar_parse(reply, TB710FU_GPR_HEADER_BYTES + 4, &answer), Tb710fuArMapped, "map answer parsed");
    CHECK_EQ(answer.handle, 0x4321u, "handle");
    const uint32_t spf[1] = {1u};
    (void)tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1, TB710FU_GPR_OP_APM_SPF_STATE, spf, 4, &token);
    CHECK_EQ(tb710fu_ar_parse(reply, TB710FU_GPR_HEADER_BYTES + 4, &answer), Tb710fuArFrameworkState, "SPF state parsed");
    CHECK_EQ(answer.status, 1, "AudioReach ready");
    printf("ok   reach: graph open, configs, TDM switch, map, write, read, answers\n");
}

/* ----------------------------------- the engine ----------------------------------- */

typedef struct {
    Tb710fuAudioGraph graph;
    uint32_t sent;
    uint32_t lastSize;
    uint8_t last[TB710FU_GPR_MAX_PACKET];
    int failAtStep;        /* -1: answer everything */
    uint32_t failStatus;
    int mapHandle;
} FakeApm;

static int apmAnswer(FakeApm *apm, const uint8_t *packet, uint32_t bytes)
{
    Tb710fuGprPacket request;
    if (tb710fu_gpr_decode(packet, bytes, &request) != Tb710fuGprOk) return -1;
    Tb710fuGpr gpr;
    tb710fu_gpr_init(&gpr, TB710FU_GPR_DOMAIN_APPS, TB710FU_GPR_DOMAIN_ADSP, 1);
    gpr.next_token = request.token - 1;   /* answer with the same token */
    uint8_t reply[64];
    uint32_t replyBytes = 0;
    uint32_t token = 0;

    /* The step decides the answer shape, but the opcode is what the simulated DSP sees. */
    if (request.opcode == TB710FU_GPR_OP_APM_GET_SPF_STATE) {
        const uint32_t state[1] = {apm->failAtStep == Tb710fuGraphSpfProbe ? 0u : 1u};
        replyBytes = tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1, TB710FU_GPR_OP_APM_SPF_STATE, state, 4, &token);
    } else if (request.opcode == TB710FU_AR_OP_MEM_MAP) {
        const uint32_t handle[1] = {(uint32_t)apm->mapHandle};
        replyBytes = tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1, TB710FU_AR_OP_MEM_MAP_DONE, handle, 4, &token);
    } else if (request.opcode == TB710FU_AR_OP_WRITE || request.opcode == TB710FU_AR_OP_READ) {
        const uint32_t done[4] = {0u, 0u, (uint32_t)apm->mapHandle, 0u};
        replyBytes = tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1,
                                        request.opcode == TB710FU_AR_OP_WRITE ? TB710FU_AR_OP_WRITE_DONE
                                                                              : TB710FU_AR_OP_READ_DONE,
                                        done, sizeof done, &token);
    } else {
        /* Only the step the test names refuses; every other command succeeds. */
        const uint32_t status = (int)apm->graph.step == apm->failAtStep ? apm->failStatus : 0u;
        const uint32_t result[2] = {request.opcode, status};
        replyBytes = tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1, TB710FU_GPR_OP_BASIC_RESULT, result,
                                        sizeof result, &token);
    }
    if (!replyBytes) return -1;
    apm->sent++;
    /* The engine is what consumes replies; the fake APM hands them straight back. */
    return tb710fu_graph_packet(&apm->graph, reply, replyBytes);
}

static int apmSend(void *context, const uint8_t *packet, uint32_t bytes)
{
    FakeApm *apm = (FakeApm *)context;
    apm->lastSize = bytes;
    memcpy(apm->last, packet, bytes);
    return apmAnswer(apm, packet, bytes) < 0 ? -1 : 0;
}

/* A transport that takes the packet and never answers: the stall a timeout must survive. */
static int sentNowhere(void *context, const uint8_t *packet, uint32_t bytes)
{
    (void)packet;
    (void)bytes;
    ++*(int *)context;
    return 0;
}

static void testEngine(void)
{
    for (int map = 0; map <= 1; ++map) {
        FakeApm apm;
        memset(&apm, 0, sizeof apm);
        apm.failAtStep = -1;
        apm.mapHandle = 0x5150;
        Tb710fuAudioPort port = {apmSend, &apm};
        Tb710fuGraphConfig config;
        memset(&config, 0, sizeof config);
        config.speaker.rate = 48000;
        config.speaker.bits = 32;
        config.speaker.channels = 4;
        config.capture.rate = 48000;
        config.capture.bits = 16;
        config.capture.channels = 2;
        config.tdm.lpaif = 0;
        config.tdm.interface = 1;
        config.tdm.sd_line = 0;
        config.tdm.ws_source = 1;
        config.tdm.slots = 4;
        config.tdm.slot_bits = 32;
        config.tdm.channels = 4;
        config.mic.lpaif = 1;
        config.mic.codec_dma_index = 4;
        config.mic.channel_mask = 0x3;
        config.open_capture = 1;
        config.map_buffer = map;
        config.buffer_address = 0x80000000ull;
        config.buffer_bytes = 0x4000;
        tb710fu_graph_init(&apm.graph, &port, &config);
        uint32_t steps = 0;
        int result = 0;
        while (steps++ < 64) {
            result = tb710fu_graph_pump(&apm.graph, (uint64_t)steps * 1000u);
            if (result != 0) break;
        }
        CHECK_EQ(result, 1, "%s: the sequence reached running", map ? "with the buffer mapped" : "without a buffer");
        CHECK_EQ(apm.graph.step, Tb710fuGraphRunning, "step is running");
        CHECK_EQ(apm.graph.answers, map ? 19u : 18u, "one answer per step");
        CHECK_EQ(apm.graph.failures, 0, "no failures");
        if (map) {
            CHECK_EQ(apm.graph.handle, 0x5150u, "the map handle is kept");
            CHECK_EQ(apm.graph.state, Tb710fuAudioStreaming, "state is streaming");
        } else {
            CHECK_EQ(apm.graph.state, Tb710fuAudioGraphOpen, "state is graph open");
        }
        /* Data completions after running are counted but must not move the sequence. */
        Tb710fuGpr gpr;
        tb710fu_gpr_init(&gpr, TB710FU_GPR_DOMAIN_APPS, TB710FU_GPR_DOMAIN_ADSP, 1);
        gpr.next_token = apm.graph.gpr.next_token - 1;
        uint8_t reply[64];
        uint32_t token = 0;
        const uint32_t done[4] = {0x80000000u, 0, 0x5150u, 0};
        const uint32_t replyBytes = tb710fu_gpr_encode(&gpr, reply, sizeof reply, 1, TB710FU_AR_OP_WRITE_DONE,
                                                       done, sizeof done, &token);
        Tb710fuGprPacket decoded;
        CHECK_EQ(tb710fu_gpr_decode(reply, replyBytes, &decoded), Tb710fuGprOk, "write-done packet encodes");
        CHECK_EQ(tb710fu_graph_packet(&apm.graph, reply, replyBytes), 0,
                 "a completion outside a command in flight is absorbed");
        CHECK_EQ(apm.graph.step, Tb710fuGraphRunning, "and does not advance the sequence");
        CHECK_EQ(apm.graph.write_done, 1, "counted");

        if (map) {
            CHECK_EQ(tb710fu_graph_write(&apm.graph, 0x1000, 0x1000, 5000), 0, "a chunk goes out");
            CHECK_EQ(apm.graph.write_started, 1, "write started");
            CHECK_EQ(tb710fu_graph_read(&apm.graph, 0x2000, 0x1000, 5000), 0, "a capture chunk is asked for");
            CHECK_EQ(apm.graph.read_started, 1, "read started");
        }
        printf("ok   engine: %s sequence, data completions, chunk commands\n", map ? "mapped" : "unmapped");
    }

    /* A refusal at a step stops there. */
    FakeApm apm;
    memset(&apm, 0, sizeof apm);
    apm.failAtStep = Tb710fuGraphSpeakerCfg;
    apm.failStatus = 0x4;
    Tb710fuAudioPort port = {apmSend, &apm};
    Tb710fuGraphConfig config;
    memset(&config, 0, sizeof config);
    config.speaker = (Tb710fuFormat){48000, 32, 4};
    config.capture = (Tb710fuFormat){48000, 16, 2};
    tb710fu_graph_init(&apm.graph, &port, &config);
    uint32_t steps = 0;
    int result = 0;
    while (steps++ < 64) {
        result = tb710fu_graph_pump(&apm.graph, (uint64_t)steps * 1000u);
        if (result != 0) break;
    }
    CHECK_EQ(result, -1, "a refused step fails the sequence");
    CHECK_EQ(apm.graph.step, Tb710fuGraphFailed, "failed");
    CHECK_EQ(apm.graph.last_status, 0x4, "the DSP's status is kept");
    CHECK_EQ(apm.graph.answers, 4u, "the probe, two graph opens and the refused config");
    printf("ok   engine: a refused step stops at that step\n");

    /* An answer that never comes: the port accepts the packet and nothing answers it, which is
     * what a stalled GLINK channel looks like. Two deadlines retry, the third fails the sequence. */
    static int accepted;
    Tb710fuAudioPort mutePort = {sentNowhere, &accepted};
    tb710fu_graph_init(&apm.graph, &mutePort, &config);
    CHECK_EQ(tb710fu_graph_pump(&apm.graph, 0), 0, "the probe goes out");
    CHECK_EQ(tb710fu_graph_timeout(&apm.graph, 300000), 0, "first deadline retries");
    CHECK_EQ(apm.graph.retries, 1, "one retry");
    CHECK_EQ(tb710fu_graph_pump(&apm.graph, 300001), 0, "the retry goes out");
    CHECK_EQ(tb710fu_graph_timeout(&apm.graph, 600000), 0, "second deadline retries");
    CHECK_EQ(tb710fu_graph_pump(&apm.graph, 600001), 0, "the second retry goes out");
    CHECK_EQ(tb710fu_graph_timeout(&apm.graph, 900000), 1, "the third deadline gives up");
    CHECK_EQ(apm.graph.step, Tb710fuGraphFailed, "failed");
    CHECK_EQ(apm.graph.last_error, Tb710fuAudioErrorTimeout, "timeout recorded");
    printf("ok   engine: two retries then a timeout\n");
}

/* ------------------------------------ the ABI ------------------------------------ */

static void testAbi(void)
{
    CHECK_EQ(TB710FU_AUDIO_ABI_MAGIC, 0x54413130u, "ABI magic");
    CHECK_EQ(TB710FU_AUDIO_ABI_VERSION, 3u, "ABI version");
    CHECK_EQ(sizeof(Tb710fuAudioStatus) % 8, 0, "status is a whole number of words");
    CHECK_EQ(offsetof(Tb710fuAudioStatus, magic), 0u, "magic first");
    CHECK_EQ(offsetof(Tb710fuAudioStatus, amp_identity[0]) % 4, 0u, "identity array is word aligned");
    CHECK_EQ(TB710FU_AUDIO_SHARED_BYTES, 16384u, "four 4 KiB chunks");
    CHECK_EQ(TB710FU_AUDIO_FRAMES_PER_CHUNK, 512u, "512 frames per chunk at four channels and 16 bits");
    CHECK_EQ(sizeof(Tb710fuAudioSharedHeader), TB710FU_AUDIO_SHARED_HEADER, "the header is exactly 64 bytes");

    CHECK_EQ(tb710fu_audio_gain_ppm(0), 1000000, "0 dB is unity");
    CHECK_EQ(tb710fu_audio_gain_ppm(-6), 501187, "-6 dB");
    CHECK_EQ(tb710fu_audio_gain_ppm(-96), 16, "-96 dB");
    CHECK_EQ(tb710fu_audio_gain_ppm(-120), 16, "below the floor clamps");
    int32_t previous = tb710fu_audio_gain_ppm(0);
    for (int db = -1; db >= -96; --db) {
        const int32_t ppm = tb710fu_audio_gain_ppm(db);
        CHECK(ppm <= previous, "gain is monotonic at %d dB", db);
        previous = ppm;
    }
    int16_t samples[4] = {1000, -1000, 32767, -32768};
    tb710fu_audio_apply_gain(samples, 4, 0);
    CHECK_EQ(samples[0], 1000, "unity leaves samples alone");
    tb710fu_audio_apply_gain(samples, 4, -6);
    CHECK_EQ(samples[0], 501, "half power halves a sample");
    CHECK_EQ(samples[2], 16422, "a hot sample scales");
    tb710fu_audio_apply_gain(samples, 4, -96);
    CHECK_EQ(samples[3], 0, "-96 dB rounds the low sample to zero");
    printf("ok   abi: layout, gain table, gain application\n");
}

int main(void)
{
    printf("TB710FU audio host tests\n");
    testAcf();
    testGeni();
    testAmp();
    testGpr();
    testReach();
    testEngine();
    testAbi();
    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    if (gFailures) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
