// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuPlatformStage.c - the ladder arithmetic, no I/O, no Apple headers.
#include "Tb710fuPlatformStage.h"

#include <stddef.h>

unsigned tb710fu_stage_clamp(unsigned requested, unsigned maximum, int *clamped)
{
    if (clamped != NULL) {
        *clamped = requested > maximum;
    }
    return requested > maximum ? maximum : requested;
}

enum tb710fu_stage_action tb710fu_stage_action(unsigned level, int probe_ok)
{
    if (level >= 2u) {
        return probe_ok ? TB710FU_STAGE_SERVE : TB710FU_STAGE_NONE;
    }
    if (level == 1u) {
        return TB710FU_STAGE_DIAGNOSE;
    }
    return TB710FU_STAGE_NONE;
}

static int tb710fu_is_separator(char character)
{
    return character == ' ' || character == '\t' || character == '\n' ||
        character == '\r';
}

static int tb710fu_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

static int tb710fu_is_hex(char character)
{
    return tb710fu_is_digit(character) ||
        (character >= 'a' && character <= 'f') ||
        (character >= 'A' && character <= 'F');
}

static unsigned tb710fu_digit_value(char character)
{
    if (tb710fu_is_digit(character)) {
        return (unsigned)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return (unsigned)(character - 'a') + 10u;
    }
    return (unsigned)(character - 'A') + 10u;
}

/* One token: key, '=', digits. Values are bounded to 32 bits; a longer literal
 * is out of range and rejected rather than truncated, because a truncated
 * "backlight=4294967299" would read as 3 and then be clamped to a real level. */
static int tb710fu_parse_token(const char *token, size_t length,
                               const char *key, size_t key_length,
                               unsigned *value)
{
    size_t index = 0;

    if (length <= key_length || token[key_length] != '=') {
        return 0;
    }
    for (index = 0; index < key_length; ++index) {
        if (token[index] != key[index]) {
            return 0;
        }
    }
    index = key_length + 1u;
    if (index >= length) {
        return -1; /* "backlight=" with no value */
    }

    unsigned parsed = 0;
    unsigned base = 10u;
    int digits = 0;
    if (index + 2u <= length && token[index] == '0' &&
        (token[index + 1u] == 'x' || token[index + 1u] == 'X')) {
        base = 16u;
        index += 2u;
    }
    for (; index < length; ++index) {
        const char character = token[index];
        const int valid = base == 16u ? tb710fu_is_hex(character)
                                      : tb710fu_is_digit(character);
        if (!valid) {
            return -1;
        }
        const unsigned digit = tb710fu_digit_value(character);
        if (parsed > (0xffffffffu - digit) / base) {
            return -1; /* would not fit 32 bits */
        }
        parsed = parsed * base + digit;
        ++digits;
    }
    if (digits == 0) {
        return -1;
    }
    *value = parsed;
    return 1;
}

int tb710fu_bootarg_value(const char *arguments, const char *key,
                          unsigned *value)
{
    if (arguments == NULL || key == NULL || value == NULL || key[0] == '\0') {
        return 0;
    }

    size_t key_length = 0;
    while (key[key_length] != '\0') {
        ++key_length;
    }

    size_t start = 0;
    while (arguments[start] != '\0') {
        while (arguments[start] != '\0' &&
               tb710fu_is_separator(arguments[start])) {
            ++start;
        }
        size_t end = start;
        while (arguments[end] != '\0' &&
               !tb710fu_is_separator(arguments[end])) {
            ++end;
        }
        if (end > start && arguments[start] == key[0]) {
            const int result = tb710fu_parse_token(
                arguments + start, end - start, key, key_length, value);
            if (result != 0) {
                return result;
            }
        }
        /* A token whose first character differs cannot be this key; a token
         * that shares it but does not parse is not this key either, so the
         * search continues (boot arguments may repeat a prefix). */
        start = end;
    }
    return 0;
}

int tb710fu_bootarg_stage(const char *arguments, const char *key,
                          unsigned maximum, unsigned *level)
{
    unsigned requested = 0;
    int clamped = 0;

    if (level == NULL) {
        return 0;
    }
    *level = 0;
    const int result = tb710fu_bootarg_value(arguments, key, &requested);
    if (result != 1) {
        return result;
    }
    *level = tb710fu_stage_clamp(requested, maximum, &clamped);
    return 1;
}

int tb710fu_stage_allow(const char *arguments, const char *key,
                        unsigned maximum, unsigned required, unsigned *level)
{
    int clamped = 0;

    if (level == NULL) {
        return 0;
    }
    *level = 0;
    unsigned requested = 0;
    if (tb710fu_bootarg_value(arguments, key, &requested) != 1) {
        return 0;
    }
    *level = tb710fu_stage_clamp(requested, maximum, &clamped);
    return *level >= required;
}
