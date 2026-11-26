/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The Awinic "ACF" profile file the TB710FU stock firmware ships as awinic/aw88461_tb710fu.bin
 * (MD5SUMS: 7ba6173fedee5eaea1a1e41a2164bb88, identical to aw882xx_acf.bin). It is the smart
 * amplifier's board tuning: a container of per-channel/per-rate profiles, each a run of 16-bit
 * register writes. This parser is a bounds-checked reader only: it never writes hardware and it
 * refuses a file whose container does not add up.
 *
 * Layout, measured on the stock 3,504-byte file (all little endian):
 *   0x00  u32  magic 0x0A15F908
 *   0x04  char project[4]           "P805" (the vendor's board tag)
 *   0x1C  char vendor[16]           "awinic"
 *   0x2C  u32  format version       0x01000000
 *   0x30  u32  image id             0x000002B2 in the stock file
 *   0x34  u32  table bytes          0x00000200 (512 = 8 records of 64 bytes)
 *   0x38  u32  record count         8
 *   0x3C  u32  table offset         0x00000050
 *   0x40  u32  profile count        1 (one container of profiles)
 *   0x50  records, 64 bytes each:
 *         +0x00 u32  type           2 = register profile (the only type the stock file uses)
 *         +0x04 char model[16]      "aw88461"
 *         +0x14 u16  channel        the amp's channel (0..3), the device tree's awinic,audio-channel
 *         +0x16 u16  reserved       0xFFFF
 *         +0x18 u16  reserved       0xFFFF
 *         +0x1A u16  rate tag       0 = the 8..48 kHz tuning, 10 = the 96 kHz tuning (stock file)
 *         +0x1C u16  constant       4
 *         +0x20 u32  stride         0x0000016C (364): the profile block's size
 *         +0x24 u32  profile offset absolute file offset of the profile block
 *         +0x28 u32  constant       0x34
 *         (rest zero)
 *   profile block, 364 bytes: a 64-byte header then u32 count at +0x3C and count
 *   (u16 register, u16 value) pairs from +0x40; 0x40 + 4 * count == stride holds for every
 *   profile of the stock file (8 profiles, 75 pairs each).
 *   +0x00 u32  identity word      per-profile vendor id (0x47B5 / 0x4A38 family)
 *   +0x18 char model[8]           "AW88461"
 *
 * The pair stream is the amplifier's real init sequence: the stock file starts with
 * 0x0003 <- 0xFFFF and continues through 0x0004..0x0056. Nothing here interprets register
 * semantics; Tb710fuAw88461.c replays the pairs in order.
 *
 * Provenance: the container was reverse-documented from the shipped file itself (byte offsets
 * above are asserted by tests/tb710fu_audio_test.c); the reader follows this package's usual
 * "every field checked before use" rule and is pure C so the host test can run it. */
#ifndef TB710FU_ACF_H
#define TB710FU_ACF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_ACF_MAGIC 0x0A15F908u
#define TB710FU_ACF_HEADER_BYTES 0x50u
#define TB710FU_ACF_RECORD_BYTES 0x40u
#define TB710FU_ACF_PROFILE_HEADER_BYTES 0x40u
#define TB710FU_ACF_TYPE_PROFILE 2u
#define TB710FU_ACF_MAX_RECORDS 32u
#define TB710FU_ACF_MODEL_BYTES 16u
#define TB710FU_ACF_VENDOR_BYTES 16u

/* Header field offsets (see the layout above). */
enum {
    Tb710fuAcfAtMagic = 0x00,
    Tb710fuAcfAtProject = 0x04,
    Tb710fuAcfAtVendor = 0x1C,
    Tb710fuAcfAtFormat = 0x2C,
    Tb710fuAcfAtImageId = 0x30,
    Tb710fuAcfAtTableBytes = 0x34,
    Tb710fuAcfAtRecordCount = 0x38,
    Tb710fuAcfAtTableOffset = 0x3C,
    Tb710fuAcfAtProfileCount = 0x40,
};

/* Record field offsets relative to the record. */
enum {
    Tb710fuAcfRecAtType = 0x00,
    Tb710fuAcfRecAtModel = 0x04,
    Tb710fuAcfRecAtChannel = 0x14,
    Tb710fuAcfRecAtRateTag = 0x1A,
    Tb710fuAcfRecAtConstant = 0x1C,
    Tb710fuAcfRecAtStride = 0x20,
    Tb710fuAcfRecAtOffset = 0x24,
};

/* Profile block field offsets relative to the profile. */
enum {
    Tb710fuAcfProfAtIdentity = 0x00,
    Tb710fuAcfProfAtModel = 0x18,
    Tb710fuAcfProfAtPairs = 0x3C,
    Tb710fuAcfProfAtBody = 0x40,
};

enum {
    Tb710fuAcfOk = 0,
    Tb710fuAcfShort = 1,      /* fewer bytes than the header */
    Tb710fuAcfMagic = 2,      /* not an ACF container */
    Tb710fuAcfVendor = 3,     /* not an awinic container */
    Tb710fuAcfTable = 4,      /* the record table does not add up */
    Tb710fuAcfErrRecord = 5,  /* a record's profile block is outside the file or overlaps */
    Tb710fuAcfProfile = 6,    /* a profile's pair run does not fit its block */
};

typedef struct {
    uint32_t type;
    uint16_t channel;
    uint16_t rate_tag;
    uint32_t stride;          /* bytes of the profile block */
    uint32_t offset;          /* absolute offset of the profile block */
    uint32_t pairs;           /* register pairs in the block */
    uint32_t identity;        /* the block's vendor identity word */
    char model[TB710FU_ACF_MODEL_BYTES + 1];
    char profile_model[9];
    const uint8_t *block;     /* the profile block (stride bytes) */
} Tb710fuAcfRecord;

typedef struct {
    const uint8_t *bytes;
    uint32_t length;
    uint32_t format;
    uint32_t image_id;
    uint32_t table_bytes;
    uint32_t table_offset;
    uint32_t record_count;
    uint32_t profile_count;
    Tb710fuAcfRecord record[TB710FU_ACF_MAX_RECORDS];
    char project[5];
    char vendor[TB710FU_ACF_VENDOR_BYTES + 1];
} Tb710fuAcf;

/* Validates the container and fills *out (its record[] points into bytes, which must outlive it).
 * Returns a Tb710fuAcf* code; why (when not null, 48 bytes) receives a one-line reason on failure. */
int tb710fu_acf_open(Tb710fuAcf *out, const uint8_t *bytes, uint32_t length, char *why, size_t why_bytes);

/* The first record whose profile type is register-profile and whose channel and rate tag match,
 * or null. rate_tag: the 0 tuning (8..48 kHz) unless the caller asks for 10 (96 kHz). */
const Tb710fuAcfRecord *tb710fu_acf_find(const Tb710fuAcf *acf, uint16_t channel, uint16_t rate_tag);

/* Pair index of a record's profile: 0 when the index is inside the run. */
int tb710fu_acf_pair(const Tb710fuAcf *acf, const Tb710fuAcfRecord *record, uint32_t index,
                     uint16_t *address, uint16_t *value);

#ifdef __cplusplus
}
#endif
#endif
