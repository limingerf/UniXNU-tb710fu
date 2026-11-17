/* SPDX-License-Identifier: MIT */
#include "LiveProtocol.h"

static void clear_bytes(void *output, size_t length)
{
    uint8_t *out = (uint8_t *)output;
    for (size_t index = 0; index < length; ++index) out[index] = 0;
}

static void copy_bytes(uint8_t *out, const uint8_t *source, size_t length)
{
    for (size_t index = 0; index < length; ++index) out[index] = source[index];
}

static uint32_t crc32(const uint8_t *bytes, size_t length)
{
    uint32_t value = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        value ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((0u - (value & 1u)) & 0xedb88320u);
    }
    return ~value;
}

static void le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void le32(uint8_t *out, uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
        out[index] = (uint8_t)(value >> (index * 8u));
}

static size_t decimal(char *out, uint64_t value)
{
    char digits[20];
    size_t length = 0;
    do {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    for (size_t index = 0; index < length; ++index)
        out[index] = digits[length - index - 1u];
    return length;
}

static size_t filename(char *out, uint64_t generation, uint64_t start, uint64_t end)
{
    static const char fixed_prefix[] = "TB710FU-ramoops-fixed-";
    static const char live_prefix[] = "TB710FU-ramoops-live-";
    const char *prefix = generation == 0 ? fixed_prefix : live_prefix;
    static const char suffix[] = ".log";
    size_t length = 0;
    while (prefix[length] != '\0') {
        out[length] = prefix[length];
        ++length;
    }
    length += decimal(out + length, generation);
    out[length++] = '-';
    length += decimal(out + length, start);
    out[length++] = '-';
    length += decimal(out + length, end);
    for (size_t index = 0; index < sizeof(suffix) - 1u; ++index)
        out[length++] = suffix[index];
    out[length] = '\0';
    return length;
}

void tb710fu_live_log_sender_init(struct tb710fu_live_log_sender *sender)
{
    if (sender != NULL) clear_bytes(sender, sizeof(*sender));
}

int tb710fu_live_log_can_snapshot(const struct tb710fu_live_log_sender *sender)
{
    return sender != NULL && (!sender->valid || sender->info.rounds >= 2u);
}

int tb710fu_live_log_get_info(const struct tb710fu_live_log_sender *sender,
    struct tb710fu_live_log_info *out)
{
    if (sender == NULL || out == NULL || !sender->valid) return 0;
    *out = sender->info;
    return 1;
}

enum tb710fu_live_log_result tb710fu_live_log_snapshot(
    struct tb710fu_live_log_sender *sender, const uint8_t *bytes, size_t length,
    uint64_t generation, uint64_t start, uint64_t end)
{
    static const uint8_t mime[] = "text/plain";
    if (sender == NULL || length > TB710FU_LIVE_LOG_MAX_BYTES ||
        end < start || end - start != (uint64_t)length ||
        (length != 0 && bytes == NULL)) return TB710FU_LIVE_LOG_INVALID;
    if (length == 0) return TB710FU_LIVE_LOG_EMPTY;
    if (!tb710fu_live_log_can_snapshot(sender)) return TB710FU_LIVE_LOG_BUSY;

    clear_bytes(&sender->info, sizeof(sender->info));
    clear_bytes(sender->envelope, sizeof(sender->envelope));
    sender->info.generation = generation;
    sender->info.start = start;
    sender->info.end = end;
    sender->info.bytes = (uint32_t)length;
    size_t name_length = filename(sender->info.filename, generation, start, end);
    size_t body_offset = 26u + name_length + sizeof(mime) - 1u;
    uint32_t body_crc = crc32(bytes, length);
    uint8_t *envelope = sender->envelope;
    envelope[0] = 'L'; envelope[1] = 'M'; envelope[2] = 'S'; envelope[3] = '1';
    envelope[4] = 2; /* Envelope v2, uncompressed. */
    le16(envelope + 6, (uint16_t)name_length);
    le16(envelope + 8, (uint16_t)(sizeof(mime) - 1u));
    le32(envelope + 10, (uint32_t)length);
    le32(envelope + 14, body_crc);
    le32(envelope + 18, (uint32_t)length);
    le32(envelope + 22, body_crc);
    copy_bytes(envelope + 26, (const uint8_t *)sender->info.filename, name_length);
    copy_bytes(envelope + 26 + name_length, mime, sizeof(mime) - 1u);
    copy_bytes(envelope + body_offset, bytes, length);
    sender->info.envelope_bytes = (uint32_t)(body_offset + length);
    sender->info.payload_checksum = crc32(envelope, sender->info.envelope_bytes);
    sender->info.session_id = sender->info.payload_checksum ^ 0xb710f001u;
    sender->info.block_count = (sender->info.envelope_bytes +
        TB710FU_LIVE_LOG_BLOCK_BYTES - 1u) / TB710FU_LIVE_LOG_BLOCK_BYTES;
    sender->source_in_round = 0;
    sender->valid = 1;
    return TB710FU_LIVE_LOG_READY;
}

int tb710fu_live_log_next_frame(struct tb710fu_live_log_sender *sender,
    uint8_t out[TB710FU_LIVE_LOG_FRAME_BYTES])
{
    if (sender == NULL || out == NULL || !sender->valid) return 0;
    uint32_t sequence = sender->info.next_sequence;
    /* Profile 0 is systematic except every fifth repair sequence. Skipping
     * repair numbers preserves the unchanged receiver's source-block mapping. */
    if (sequence % 5u == 4u) ++sequence;
    uint32_t block = (((uint64_t)(sequence / 5u) * 4u + sequence % 5u) %
        sender->info.block_count);
    size_t offset = (size_t)block * TB710FU_LIVE_LOG_BLOCK_BYTES;
    size_t length = sender->info.envelope_bytes - offset;
    if (length > TB710FU_LIVE_LOG_BLOCK_BYTES) length = TB710FU_LIVE_LOG_BLOCK_BYTES;
    clear_bytes(out, TB710FU_LIVE_LOG_FRAME_BYTES);
    out[0] = 'O'; out[1] = 'S'; out[2] = 2; /* Profile byte remains 0. */
    le32(out + 4, sender->info.session_id);
    le32(out + 8, sequence);
    le32(out + 12, sender->info.block_count);
    le16(out + 16, TB710FU_LIVE_LOG_BLOCK_BYTES);
    le32(out + 20, sender->info.envelope_bytes);
    le32(out + 24, sender->info.payload_checksum);
    copy_bytes(out + 28, sender->envelope + offset, length);
    le32(out + 748, crc32(out, 748));
    sender->info.next_sequence = sequence + 1u;
    if (++sender->source_in_round == sender->info.block_count) {
        sender->source_in_round = 0;
        if (sender->info.rounds != UINT32_MAX) ++sender->info.rounds;
    }
    return 1;
}
