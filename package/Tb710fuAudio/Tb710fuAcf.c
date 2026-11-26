/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuAcf.h"

#include <stdbool.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

/* A fixed-size string field, empty when it has no terminator inside its width. */
static void text(char *to, size_t capacity, const uint8_t *from, uint32_t width)
{
    size_t n = 0;
    while (n < width && n + 1 < capacity && from[n]) {to[n] = (char)from[n]; ++n;}
    to[n] = 0;
}

static bool any_nonzero(const uint8_t *p, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; ++i) if (p[i]) return true;
    return false;
}

int tb710fu_acf_open(Tb710fuAcf *out, const uint8_t *bytes, uint32_t length, char *why, size_t why_bytes)
{
    if (why && why_bytes) why[0] = 0;
    if (!out) return Tb710fuAcfShort;
    memset(out, 0, sizeof *out);
    if (!bytes || length < TB710FU_ACF_HEADER_BYTES) {
        if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"shorter than the 0x50 header", 28);
        return Tb710fuAcfShort;
    }
    if (le32(bytes + Tb710fuAcfAtMagic) != TB710FU_ACF_MAGIC) {
        if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"magic is not 0x0A15F908", 23);
        return Tb710fuAcfMagic;
    }
    char vendor[TB710FU_ACF_VENDOR_BYTES + 1];
    text(vendor, sizeof vendor, bytes + Tb710fuAcfAtVendor, TB710FU_ACF_VENDOR_BYTES);
    if (strcmp(vendor, "awinic") != 0) {
        if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"vendor is not awinic", 20);
        return Tb710fuAcfVendor;
    }

    out->bytes = bytes;
    out->length = length;
    out->format = le32(bytes + Tb710fuAcfAtFormat);
    out->image_id = le32(bytes + Tb710fuAcfAtImageId);
    out->table_bytes = le32(bytes + Tb710fuAcfAtTableBytes);
    out->table_offset = le32(bytes + Tb710fuAcfAtTableOffset);
    out->record_count = le32(bytes + Tb710fuAcfAtRecordCount);
    out->profile_count = le32(bytes + Tb710fuAcfAtProfileCount);
    text(out->project, sizeof out->project, bytes + Tb710fuAcfAtProject, 4);
    text(out->vendor, sizeof out->vendor, bytes + Tb710fuAcfAtVendor, TB710FU_ACF_VENDOR_BYTES);

    /* The table is a whole number of 64-byte records, inside the file, and the count agrees. */
    if (!out->record_count || out->record_count > TB710FU_ACF_MAX_RECORDS ||
        out->table_bytes != out->record_count * TB710FU_ACF_RECORD_BYTES ||
        out->table_offset < TB710FU_ACF_HEADER_BYTES ||
        out->table_offset > length || out->table_bytes > length - out->table_offset) {
        if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"record table geometry", 21);
        return Tb710fuAcfTable;
    }

    const uint32_t table_end = out->table_offset + out->table_bytes;
    uint32_t expected_offset = table_end;   /* profiles follow the table, back to back */
    char model[TB710FU_ACF_MODEL_BYTES + 1] = "";
    for (uint32_t i = 0; i < out->record_count; ++i) {
        const uint8_t *at = bytes + out->table_offset + i * TB710FU_ACF_RECORD_BYTES;
        Tb710fuAcfRecord *r = &out->record[i];
        r->type = le32(at + Tb710fuAcfRecAtType);
        text(r->model, sizeof r->model, at + Tb710fuAcfRecAtModel, TB710FU_ACF_MODEL_BYTES);
        r->channel = le16(at + Tb710fuAcfRecAtChannel);
        r->rate_tag = le16(at + Tb710fuAcfRecAtRateTag);
        r->stride = le32(at + Tb710fuAcfRecAtStride);
        r->offset = le32(at + Tb710fuAcfRecAtOffset);
        if (!r->model[0]) {
            if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"record without a model", 22);
            return Tb710fuAcfErrRecord;
        }
        /* every record of one container names the same part */
        if (!model[0]) text(model, sizeof model, at + Tb710fuAcfRecAtModel, TB710FU_ACF_MODEL_BYTES);
        else if (strcmp(model, r->model) != 0) {
            if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"records name different models", 29);
            return Tb710fuAcfErrRecord;
        }
        if (r->stride < TB710FU_ACF_PROFILE_HEADER_BYTES + 4 || r->offset < table_end ||
            r->offset > length || r->stride > length - r->offset) {
            if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"profile block outside the file", 30);
            return Tb710fuAcfErrRecord;
        }
        /* The records of the stock file are a back-to-back chain: each block starts where the
         * previous one ended. A file that breaks the chain is refused rather than guessed at. */
        if (r->offset != expected_offset) {
            if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"profile blocks are not contiguous", 33);
            return Tb710fuAcfErrRecord;
        }
        expected_offset = r->offset + r->stride;
        r->block = bytes + r->offset;
        r->identity = le32(r->block + Tb710fuAcfProfAtIdentity);
        text(r->profile_model, sizeof r->profile_model, r->block + Tb710fuAcfProfAtModel, 8);
        if (r->type == TB710FU_ACF_TYPE_PROFILE) {
            const uint32_t pairs = le32(r->block + Tb710fuAcfProfAtPairs);
            if ((uint64_t)TB710FU_ACF_PROFILE_HEADER_BYTES + (uint64_t)pairs * 4 > r->stride) {
                if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"profile pair run exceeds its block", 34);
                return Tb710fuAcfProfile;
            }
            if (pairs && !any_nonzero(r->block + TB710FU_ACF_PROFILE_HEADER_BYTES, 4)) {
                if (why && why_bytes) text(why, why_bytes, (const uint8_t *)"profile pair run starts empty", 29);
                return Tb710fuAcfProfile;
            }
            r->pairs = pairs;
        }
    }
    return Tb710fuAcfOk;
}

const Tb710fuAcfRecord *tb710fu_acf_find(const Tb710fuAcf *acf, uint16_t channel, uint16_t rate_tag)
{
    if (!acf || !acf->record_count) return NULL;
    for (uint32_t i = 0; i < acf->record_count; ++i) {
        const Tb710fuAcfRecord *r = &acf->record[i];
        if (r->type == TB710FU_ACF_TYPE_PROFILE && r->channel == channel && r->rate_tag == rate_tag) return r;
    }
    return NULL;
}

int tb710fu_acf_pair(const Tb710fuAcf *acf, const Tb710fuAcfRecord *record, uint32_t index,
                     uint16_t *address, uint16_t *value)
{
    if (!acf || !record || !record->block || index >= record->pairs) return -1;
    const uint8_t *at = record->block + TB710FU_ACF_PROFILE_HEADER_BYTES + index * 4;
    if (at + 4 > acf->bytes + acf->length) return -1;
    if (address) *address = le16(at);
    if (value) *value = le16(at + 2);
    return 0;
}
