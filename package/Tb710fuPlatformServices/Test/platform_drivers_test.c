// SPDX-License-Identifier: BSD-2-Clause-Patent
// platform_drivers_test.c - the staged ladder, the SY7758 brightness arithmetic
// and the TSENS decoding, with no hardware and no Apple headers.
//
// The values pinned here are the ones the drivers will compute on the board:
// the 12-bit split of a level, the level a nits request maps to, the
// millidegrees a status word means, and which sensor belongs to which group
// (sm8650.dtsi's thermal-zones table).
#include "../Tb710fuBacklightCore.h"
#include "../Tb710fuPlatformStage.h"
#include "../Tb710fuThermalCore.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_stage_parsing(void)
{
    unsigned level = 123u;

    /* Absent: the key is simply not there, and the level stays 0. */
    assert(tb710fu_bootarg_stage("", TB710FU_BOOTARG_BACKLIGHT,
                                 TB710FU_STAGE_BACKLIGHT_MAX, &level) == 0);
    assert(level == 0u);
    assert(tb710fu_bootarg_stage("platform-svc=2", TB710FU_BOOTARG_BACKLIGHT,
                                 TB710FU_STAGE_BACKLIGHT_MAX, &level) == 0);
    assert(level == 0u);

    /* The three arguments of the bring-up, together and apart. */
    assert(tb710fu_bootarg_stage("platform-svc=2 backlight=1 thermal=1",
                                 TB710FU_BOOTARG_PLATFORM,
                                 TB710FU_STAGE_PLATFORM_MAX, &level) == 1);
    assert(level == 2u);
    assert(tb710fu_bootarg_stage("platform-svc=2 backlight=1 thermal=1",
                                 TB710FU_BOOTARG_BACKLIGHT,
                                 TB710FU_STAGE_BACKLIGHT_MAX, &level) == 1);
    assert(level == 1u);
    assert(tb710fu_bootarg_stage("platform-svc=2 backlight=1 thermal=1",
                                 TB710FU_BOOTARG_THERMAL,
                                 TB710FU_STAGE_THERMAL_MAX, &level) == 1);
    assert(level == 1u);

    /* Tab and newline separate tokens like a space does. */
    assert(tb710fu_bootarg_stage("kext-a=1\tbacklight=2\n",
                                 TB710FU_BOOTARG_BACKLIGHT,
                                 TB710FU_STAGE_BACKLIGHT_MAX, &level) == 1);
    assert(level == 2u);

    /* Above the maximum: clamped, and the clamp is observable. */
    int clamped = 0;
    assert(tb710fu_stage_clamp(3u, 2u, &clamped) == 2u);
    assert(clamped == 1);
    assert(tb710fu_stage_clamp(2u, 2u, &clamped) == 2u);
    assert(clamped == 0);
    assert(tb710fu_bootarg_stage("thermal=7", TB710FU_BOOTARG_THERMAL,
                                 TB710FU_STAGE_THERMAL_MAX, &level) == 1);
    assert(level == 1u);

    /* A value that does not parse leaves the ladder off and says so. */
    assert(tb710fu_bootarg_stage("backlight=abc", TB710FU_BOOTARG_BACKLIGHT,
                                 TB710FU_STAGE_BACKLIGHT_MAX, &level) == -1);
    assert(level == 0u);
    assert(tb710fu_bootarg_stage("backlight=", TB710FU_BOOTARG_BACKLIGHT,
                                 TB710FU_STAGE_BACKLIGHT_MAX, &level) == -1);
    assert(tb710fu_bootarg_value("backlight=1x", "backlight", &level) == -1);
    assert(tb710fu_bootarg_value("backlight=-1", "backlight", &level) == -1);
    /* 32-bit overflow is refused rather than truncated to a valid level. */
    assert(tb710fu_bootarg_value("backlight=4294967296", "backlight",
                                 &level) == -1);

    /* Hex is accepted the way PE_parse_boot_argn accepts 0x. */
    assert(tb710fu_bootarg_value("backlight=0x2", "backlight", &level) == 1);
    assert(level == 2u);

    /* A prefix that is not the key does not match. */
    assert(tb710fu_bootarg_value("xbacklight=1", "backlight", &level) == 0);
    assert(tb710fu_bootarg_value("backlightx=1", "backlight", &level) == 0);
    /* A repeated key: the first parseable one wins, like the boot-args blob. */
    assert(tb710fu_bootarg_value("backlight=1 backlight=2", "backlight",
                                 &level) == 1);
    assert(level == 1u);
    /* The first occurrence of the key decides, as with the platform's own
     * parser: a malformed first value is an error, not a reason to look for a
     * later duplicate. */
    assert(tb710fu_bootarg_value("backlight= backlight=2", "backlight",
                                 &level) == -1);
}

static void test_stage_actions(void)
{
    /* Level 0 never touches hardware; level 1 always diagnoses (the log is
     * its product, so a failed probe has to be printed); level 2 serves only
     * what it could read. */
    assert(tb710fu_stage_action(0u, 1) == TB710FU_STAGE_NONE);
    assert(tb710fu_stage_action(0u, 0) == TB710FU_STAGE_NONE);
    assert(tb710fu_stage_action(1u, 1) == TB710FU_STAGE_DIAGNOSE);
    assert(tb710fu_stage_action(1u, 0) == TB710FU_STAGE_DIAGNOSE);
    assert(tb710fu_stage_action(2u, 1) == TB710FU_STAGE_SERVE);
    assert(tb710fu_stage_action(2u, 0) == TB710FU_STAGE_NONE);
    /* Above the maximum behaves as the maximum. */
    assert(tb710fu_stage_action(9u, 1) == TB710FU_STAGE_SERVE);

    /* The gate each driver repeats at its own entry point. */
    unsigned stage = 99u;
    assert(tb710fu_stage_allow("", TB710FU_BOOTARG_THERMAL,
                               TB710FU_STAGE_THERMAL_MAX, 1u, &stage) == 0);
    assert(stage == 0u);
    assert(tb710fu_stage_allow("thermal=1", TB710FU_BOOTARG_THERMAL,
                               TB710FU_STAGE_THERMAL_MAX, 1u, &stage) == 1);
    assert(stage == 1u);
    assert(tb710fu_stage_allow("backlight=1", TB710FU_BOOTARG_BACKLIGHT,
                               TB710FU_STAGE_BACKLIGHT_MAX, 2u, &stage) == 0);
    assert(stage == 1u);
    assert(tb710fu_stage_allow("backlight=2", TB710FU_BOOTARG_BACKLIGHT,
                               TB710FU_STAGE_BACKLIGHT_MAX, 2u, &stage) == 1);
    /* A level above the maximum is clamped before the comparison. */
    assert(tb710fu_stage_allow("backlight=9", TB710FU_BOOTARG_BACKLIGHT,
                               TB710FU_STAGE_BACKLIGHT_MAX, 2u, &stage) == 1);
    assert(stage == 2u);
    /* Unparsable and absent levels both stay off, and a null out pointer is
     * refused rather than written. */
    assert(tb710fu_stage_allow("thermal=x", TB710FU_BOOTARG_THERMAL,
                               TB710FU_STAGE_THERMAL_MAX, 1u, &stage) == 0);
    assert(stage == 0u);
    assert(tb710fu_stage_allow("thermal=1", TB710FU_BOOTARG_THERMAL,
                               TB710FU_STAGE_THERMAL_MAX, 1u, NULL) == 0);
}

static void test_backlight_levels(void)
{
    /* Clamping is the part's range: 0..0xFF0. */
    assert(tb710fu_backlight_clamp(0u) == 0u);
    assert(tb710fu_backlight_clamp(1u) == 1u);
    assert(tb710fu_backlight_clamp(TB710FU_SY7758_MAX_LEVEL) ==
           TB710FU_SY7758_MAX_LEVEL);
    assert(tb710fu_backlight_clamp(4095u) == TB710FU_SY7758_MAX_LEVEL);
    assert(tb710fu_backlight_clamp(0xffffffffu) == TB710FU_SY7758_MAX_LEVEL);

    /* 0 or less is off; a positive request is at least one level. */
    assert(tb710fu_backlight_level_for_nits(0, 500u) == 0u);
    assert(tb710fu_backlight_level_for_nits(-65536, 500u) == 0u);
    assert(tb710fu_backlight_level_for_nits(1, 500u) == 1u);
    /* Half a nit on a 500 nit scale: 4080 * 0.5 / 500 = 4 (half up). */
    assert(tb710fu_backlight_level_for_nits(0x8000, 500u) == 4u);
    /* The top of the scale is the part's maximum, and above it clamps. */
    assert(tb710fu_backlight_level_for_nits(500 << 16, 500u) ==
           TB710FU_SY7758_MAX_LEVEL);
    assert(tb710fu_backlight_level_for_nits(2000 << 16, 500u) ==
           TB710FU_SY7758_MAX_LEVEL);
    /* Monotonic over the slider. */
    unsigned previous = 0u;
    for (unsigned nits = 0u; nits <= 600u; ++nits) {
        const unsigned level =
            tb710fu_backlight_level_for_nits((int32_t)(nits << 16), 500u);
        assert(level >= previous);
        previous = level;
    }

    /* The inverse the service publishes for BrightnessMilliNits. */
    assert(tb710fu_backlight_milli_nits_for_level(0u, 500u) == 0u);
    assert(tb710fu_backlight_milli_nits_for_level(TB710FU_SY7758_MAX_LEVEL,
                                                 500u) == 500000u);
    assert(tb710fu_backlight_milli_nits_for_level(2040u, 500u) == 250000u);
    assert(tb710fu_backlight_milli_nits_for_level(9000u, 500u) == 500000u);

    /* 12-bit split: 0x10 low byte, 0x11 low nibble. */
    uint8_t low = 0xaa;
    uint8_t high = 0xaa;
    tb710fu_backlight_split12(0u, &low, &high);
    assert(low == 0x00u && high == 0x00u);
    tb710fu_backlight_split12(0x0ffu, &low, &high);
    assert(low == 0xffu && high == 0x00u);
    tb710fu_backlight_split12(0x100u, &low, &high);
    assert(low == 0x00u && high == 0x01u);
    tb710fu_backlight_split12(TB710FU_SY7758_MAX_LEVEL, &low, &high);
    assert(low == 0xf0u && high == 0x0fu);
    /* In range but across the byte boundary. */
    tb710fu_backlight_split12(0x1ffu, &low, &high);
    assert(low == 0xffu && high == 0x01u);
    /* A level above the range splits as the clamp, not as a truncated 12-bit
     * value: 0x1000 would otherwise read as 0x000 and blank the panel. */
    tb710fu_backlight_split12(0x1000u, &low, &high);
    assert(low == 0xf0u && high == 0x0fu);
    assert(tb710fu_backlight_join12(low, high) == TB710FU_SY7758_MAX_LEVEL);
    /* Two nulls are allowed: the writer only needs one half at a time. */
    tb710fu_backlight_split12(0x123u, NULL, NULL);
    /* The high byte's upper nibble is not part of the level. */
    assert(tb710fu_backlight_join12(0x34u, 0xf2u) == 0x234u);

    /* Round trip over the whole range, in steps that exercise both halves. */
    for (unsigned level = 0u; level <= TB710FU_SY7758_MAX_LEVEL;
         level += 0x11u) {
        tb710fu_backlight_split12(level, &low, &high);
        assert(tb710fu_backlight_join12(low, high) == level);
    }

    /* Brightness mode: DEV_CTL bits 2:1, 2 means the 12-bit registers. */
    assert(tb710fu_backlight_mode_12bit(0x04u) == 1);
    assert(tb710fu_backlight_mode_12bit(0x05u) == 1); /* plus BL_CTLB */
    assert(tb710fu_backlight_mode_12bit(0x02u) == 0); /* 8-bit */
    assert(tb710fu_backlight_mode_12bit(0x06u) == 0); /* reserved */
    assert(tb710fu_backlight_mode_12bit(0x00u) == 0);
}

static void test_backlight_fade(void)
{
    /* No period: the request is a jump. */
    assert(tb710fu_backlight_fade_level(10u, 400u, 0u, 0u) == 400u);
    /* Start of the fade, middle, end. */
    assert(tb710fu_backlight_fade_level(10u, 400u, 0u, 1000u) == 10u);
    assert(tb710fu_backlight_fade_level(10u, 400u, 500u, 1000u) == 205u);
    assert(tb710fu_backlight_fade_level(10u, 400u, 1000u, 1000u) == 400u);
    /* Past the end never overshoots. */
    assert(tb710fu_backlight_fade_level(10u, 400u, 5000u, 1000u) == 400u);
    /* Fading down. */
    assert(tb710fu_backlight_fade_level(400u, 10u, 500u, 1000u) == 205u);
    assert(tb710fu_backlight_fade_level(400u, 10u, 1000u, 1000u) == 10u);
    /* from == to is a no-op at every point. */
    assert(tb710fu_backlight_fade_level(77u, 77u, 500u, 1000u) == 77u);
    /* A period beyond the arithmetic's bound is treated as a jump instead of
     * overflowing the product. */
    assert(tb710fu_backlight_fade_level(10u, 400u, 1u, 1ull << 44) == 400u);
    /* The result never leaves the part's range, even for out-of-range
     * endpoints. */
    assert(tb710fu_backlight_fade_level(5000u, 9000u, 0u, 1000u) ==
           TB710FU_SY7758_MAX_LEVEL);
}

static void test_thermal_status(void)
{
    /* Tenths of a degree, sign-extended from bit 11. */
    assert(tb710fu_thermal_status_mc(0x000u) == 0);
    assert(tb710fu_thermal_status_mc(0x064u) == 10000);
    assert(tb710fu_thermal_status_mc(0x45fu) == 111900);  /* 111.9 C */
    assert(tb710fu_thermal_status_mc(0x7ffu) == 204700);  /* 204.7 C */
    assert(tb710fu_thermal_status_mc(0xfffu) == -100);    /* -0.1 C */
    assert(tb710fu_thermal_status_mc(0x800u) == -204800); /* -204.8 C */
    /* The valid and flag bits above 11 do not reach the temperature. */
    assert(tb710fu_thermal_status_mc(0x00201064u) == 10000);

    /* A status word the block marked valid wins over earlier invalid ones. */
    const uint32_t valid[] = {0x00000064u, 0x00200064u, 0x00000000u};
    int32_t mc = 0;
    assert(tb710fu_thermal_pick(valid, 3u, &mc) == 1);
    assert(mc == 10000);
    /* Nothing valid: two consecutive reads that agree are the block's last
     * conversion. */
    const uint32_t agreeing[] = {0x00000063u, 0x00000064u, 0x00000064u};
    assert(tb710fu_thermal_pick(agreeing, 3u, &mc) == 1);
    assert(mc == 10000);
    /* Nothing valid and no two agree: no reading at all. */
    const uint32_t differing[] = {0x00000063u, 0x00000064u, 0x00000065u};
    assert(tb710fu_thermal_pick(differing, 3u, &mc) == 0);
    /* One read is enough for the valid path and never for the agreeing one. */
    assert(tb710fu_thermal_pick(differing, 1u, &mc) == 0);
    assert(tb710fu_thermal_pick(&valid[1], 1u, &mc) == 1);

    /* Plausibility is the field's own range, not a board policy. */
    assert(tb710fu_thermal_plausible(-40000) == 1);
    assert(tb710fu_thermal_plausible(119900) == 1);
    assert(tb710fu_thermal_plausible(-40001) == 0);
    assert(tb710fu_thermal_plausible(120000) == 0);
}

static void test_thermal_groups(void)
{
    /* Control word: bit 0 plus one bit per enabled sensor, 18:3. */
    const uint32_t fifteen = tb710fu_thermal_control_for(15u);
    assert((fifteen & TB710FU_TSENS_CTRL_ENABLE) != 0u);
    for (unsigned sensor = 0u; sensor < 15u; ++sensor) {
        assert(tb710fu_thermal_enabled(fifteen, sensor) == 1);
    }
    assert(tb710fu_thermal_enabled(fifteen, 15u) == 0);
    assert(tb710fu_thermal_enabled(0u, 0u) == 0);
    assert(tb710fu_thermal_enabled(TB710FU_TSENS_CTRL_ENABLE, 0u) == 0);
    /* The field has room for nineteen sensors; a larger count is bounded. */
    const uint32_t nineteen = tb710fu_thermal_control_for(19u);
    assert((nineteen >> 3) == 0x7ffffu);
    assert(tb710fu_thermal_control_for(64u) == nineteen);
    /* Out-of-field sensors cannot be enabled. */
    assert(tb710fu_thermal_enabled(nineteen, 19u) == 0);

    /* The board's counts, from sm8650.dtsi #qcom,sensors. */
    assert(tb710fu_thermal_sensor_count[0] == 15u);
    assert(tb710fu_thermal_sensor_count[1] == 16u);
    assert(tb710fu_thermal_sensor_count[2] == 13u);

    /* Zone-to-group: cpu0-1 on tsens1 4/5, cpu2-4 on tsens0 5..10, cpu5-6 on
     * tsens0 11..14, cpu7 on tsens1 1..3, cpuss on tsens0 1..4, gpu on tsens2
     * 1..8, modem on tsens2 9..12. */
    assert(tb710fu_thermal_group_of[1][4] == TB710FU_THERMAL_GROUP_CPU01);
    assert(tb710fu_thermal_group_of[1][5] == TB710FU_THERMAL_GROUP_CPU01);
    assert(tb710fu_thermal_group_of[0][5] == TB710FU_THERMAL_GROUP_CPU24);
    assert(tb710fu_thermal_group_of[0][10] == TB710FU_THERMAL_GROUP_CPU24);
    assert(tb710fu_thermal_group_of[0][11] == TB710FU_THERMAL_GROUP_CPU56);
    assert(tb710fu_thermal_group_of[0][14] == TB710FU_THERMAL_GROUP_CPU56);
    assert(tb710fu_thermal_group_of[1][1] == TB710FU_THERMAL_GROUP_CPU7);
    assert(tb710fu_thermal_group_of[1][3] == TB710FU_THERMAL_GROUP_CPU7);
    assert(tb710fu_thermal_group_of[0][1] == TB710FU_THERMAL_GROUP_CPUSS);
    assert(tb710fu_thermal_group_of[2][1] == TB710FU_THERMAL_GROUP_GPU);
    assert(tb710fu_thermal_group_of[2][8] == TB710FU_THERMAL_GROUP_GPU);
    assert(tb710fu_thermal_group_of[2][9] == TB710FU_THERMAL_GROUP_MODEM);
    assert(tb710fu_thermal_group_of[2][12] == TB710FU_THERMAL_GROUP_MODEM);
    /* No sensor is left without a group, and every group has a name. */
    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        for (unsigned sensor = 0u; sensor < TB710FU_TSENS_SENSORS; ++sensor) {
            assert(tb710fu_thermal_group_of[controller][sensor] <
                   TB710FU_THERMAL_GROUPS);
        }
    }
    for (unsigned group = 0u; group < TB710FU_THERMAL_GROUPS; ++group) {
        assert(tb710fu_thermal_group_name[group] != NULL);
        assert(tb710fu_thermal_group_name[group][0] != '\0');
    }

    /* A table with one hot core, one hot gpu: the group maxima pick the
     * hottest sensor of each group, and NONE groups stay NONE. */
    int32_t readings[TB710FU_TSENS_CONTROLLERS][TB710FU_TSENS_SENSORS];
    int32_t groups[TB710FU_THERMAL_GROUPS];

    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        for (unsigned sensor = 0u; sensor < TB710FU_TSENS_SENSORS; ++sensor) {
            readings[controller][sensor] = TB710FU_TSENS_NONE;
        }
    }
    readings[1][4] = 45000;
    readings[1][5] = 52000;
    readings[0][6] = 61000;
    readings[0][13] = 47000;
    readings[2][3] = 58000;
    readings[2][11] = 40000;
    /* C11 spells the const-adjustment of a two-dimensional array conversion
     * as an explicit cast; C23 accepts it silently. */
    const int32_t(*const table)[TB710FU_TSENS_SENSORS] =
        (const int32_t(*)[TB710FU_TSENS_SENSORS])readings;
    tb710fu_thermal_groups(table, groups);
    assert(groups[TB710FU_THERMAL_GROUP_CPU01] == 52000);
    assert(groups[TB710FU_THERMAL_GROUP_CPU24] == 61000);
    assert(groups[TB710FU_THERMAL_GROUP_CPU56] == 47000);
    assert(groups[TB710FU_THERMAL_GROUP_CPU7] == TB710FU_TSENS_NONE);
    assert(groups[TB710FU_THERMAL_GROUP_CPUSS] == TB710FU_TSENS_NONE);
    assert(groups[TB710FU_THERMAL_GROUP_GPU] == 58000);
    assert(groups[TB710FU_THERMAL_GROUP_MODEM] == 40000);
    assert(groups[TB710FU_THERMAL_GROUP_OTHER] == TB710FU_TSENS_NONE);
    assert(tb710fu_thermal_hottest(table) == 61000);
    /* An empty table has no hottest sensor, not a zero degree one. */
    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        for (unsigned sensor = 0u; sensor < TB710FU_TSENS_SENSORS; ++sensor) {
            readings[controller][sensor] = TB710FU_TSENS_NONE;
        }
    }
    tb710fu_thermal_groups(table, groups);
    assert(tb710fu_thermal_hottest(table) == TB710FU_TSENS_NONE);
    for (unsigned group = 0u; group < TB710FU_THERMAL_GROUPS; ++group) {
        assert(groups[group] == TB710FU_TSENS_NONE);
    }
}

int main(void)
{
    printf("platform_drivers_test: stages, SY7758 brightness, TSENS\n");
    test_stage_parsing();
    printf("ok  boot-argument ladder, delimiters, clamping\n");
    test_stage_actions();
    printf("ok  stage actions degrade to none\n");
    test_backlight_levels();
    printf("ok  brightness clamp, nits mapping, 12-bit split, mode\n");
    test_backlight_fade();
    printf("ok  fade curve and its bounds\n");
    test_thermal_status();
    printf("ok  status word to millidegrees, pick, plausibility\n");
    test_thermal_groups();
    printf("ok  sensor counts, zone groups, group maxima\n");
    printf("platform_drivers_test: PASS\n");
    return 0;
}
