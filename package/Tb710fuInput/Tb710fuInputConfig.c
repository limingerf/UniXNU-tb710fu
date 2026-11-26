/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuInputConfig.h for the policy this implements. */

#include "Tb710fuInputConfig.h"

#include <stddef.h>

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int name_is(const char *name, unsigned len, const char *want)
{
    unsigned i;

    for (i = 0u; i < len; ++i) {
        if (want[i] == '\0' || name[i] != want[i]) return 0;
    }
    return want[len] == '\0';
}

/* Accept exactly one decimal digit: the levels are small on purpose so that a
 * typo cannot silently select a higher privilege. */
static int parse_level(const char *value, int *out)
{
    if (!value || value[0] < '0' || value[0] > '9' || value[1] != '\0') return -1;
    *out = value[0] - '0';
    return 0;
}

void tb710fu_input_options_default(struct tb710fu_input_options *options)
{
    if (!options) return;
    options->touch_stage = (int)TB710FU_TOUCH_OFF;
    options->keys_stage = (int)TB710FU_KEYS_OFF;
    options->fuse_probe = 0;
    options->use_irq = 0;
    options->verbose = 0;
    options->frame_limit = 0;
}

int tb710fu_input_option(const char *name, unsigned name_len,
                         const char *value, struct tb710fu_input_options *options,
                         const char **error)
{
    int level = 0;

    if (error) *error = NULL;
    if (!name || !options) {
        if (error) *error = "null-argument";
        return 0;
    }

    if (name_is(name, name_len, "tb710fu-touch")) {
        if (parse_level(value, &level) != 0 || level > (int)TB710FU_TOUCH_FULL) {
            if (error) *error = "tb710fu-touch wants 0..3";
            return 0;
        }
        options->touch_stage = level;
        return 0;
    }
    if (name_is(name, name_len, "tb710fu-keys")) {
        if (parse_level(value, &level) != 0 || level > (int)TB710FU_KEYS_ON) {
            if (error) *error = "tb710fu-keys wants 0..1";
            return 0;
        }
        options->keys_stage = level;
        return 0;
    }
    if (name_is(name, name_len, "tb710fu-touch-fuse")) {
        if (parse_level(value, &level) != 0 || level > 1) {
            if (error) *error = "tb710fu-touch-fuse wants 0..1";
            return 0;
        }
        options->fuse_probe = level;
        return 0;
    }
    if (name_is(name, name_len, "tb710fu-touch-irq")) {
        if (parse_level(value, &level) != 0 || level > 1) {
            if (error) *error = "tb710fu-touch-irq wants 0..1";
            return 0;
        }
        options->use_irq = level;
        return 0;
    }
    if (name_is(name, name_len, "tb710fu-input-verbose")) {
        if (parse_level(value, &level) != 0 || level > 1) {
            if (error) *error = "tb710fu-input-verbose wants 0..1";
            return 0;
        }
        options->verbose = level;
        return 0;
    }
    if (name_is(name, name_len, "tb710fu-input-frames")) {
        int digits = 0;
        int number = 0;

        if (!value || !value[0]) {
            if (error) *error = "tb710fu-input-frames wants a count";
            return 0;
        }
        while (value[digits] >= '0' && value[digits] <= '9') {
            number = number * 10 + (value[digits] - '0');
            if (number > 100000) number = 100000;
            ++digits;
        }
        if (value[digits] != '\0') {
            if (error) *error = "tb710fu-input-frames wants a count";
            return 0;
        }
        options->frame_limit = number;
        return 0;
    }
    return 1; /* not ours */
}

int tb710fu_input_bootargs(const char *arguments,
                           struct tb710fu_input_options *options,
                           const char **first_error)
{
    const char *cursor = arguments;
    int applied = 0;

    if (first_error) *first_error = NULL;
    if (!options) return 0;
    tb710fu_input_options_default(options);
    if (!arguments) return 0;

    while (*cursor) {
        const char *start;
        const char *equals = NULL;
        const char *end;
        const char *value = NULL;
        const char *error = NULL;
        unsigned name_len;

        while (*cursor && is_space(*cursor)) ++cursor;
        if (!*cursor) break;
        start = cursor;
        while (*cursor && !is_space(*cursor)) {
            if (*cursor == '=' && !equals) equals = cursor;
            ++cursor;
        }
        end = cursor;
        if (equals) {
            value = equals + 1;
            name_len = (unsigned)(equals - start);
        } else {
            name_len = (unsigned)(end - start);
        }
        if (name_len == 0u) continue;
        if (tb710fu_input_option(start, name_len, value, options, &error) == 0) {
            ++applied;
            if (error && first_error && !*first_error) *first_error = error;
        }
    }
    return applied;
}
