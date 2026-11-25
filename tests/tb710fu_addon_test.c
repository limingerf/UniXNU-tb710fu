/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host test of the addon reader (package/Tb710fuAdsp/Tb710fuAddon.c): the exact path, the fields, and every way a damaged
 * container must be refused. Synthetic containers only; the inflate half is kernel-only and tested on the device. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Tb710fuAddon.h"

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

typedef struct { uint8_t data[8192]; uint64_t at; uint32_t count; } Builder;

static void pad8(Builder *b)
{
    while (b->at % 8) b->data[b->at++] = 0;
}

static void entry(Builder *b, const char *path, uint32_t mode, uint64_t size, uint64_t packed_size)
{
    const uint32_t name_bytes = (uint32_t)strlen(path) + 1;
    memcpy(b->data + b->at, &name_bytes, 4);
    memcpy(b->data + b->at + 4, &mode, 4);
    memcpy(b->data + b->at + 8, &size, 8);
    memcpy(b->data + b->at + 16, &packed_size, 8);
    b->at += 24;
    memcpy(b->data + b->at, path, name_bytes);
    b->at += name_bytes;
    pad8(b);
    for (uint64_t i = 0; i < packed_size; ++i) b->data[b->at++] = (uint8_t)(0x40 + i);
    pad8(b);
    b->count++;
}

static void finish(Builder *b)
{
    memset(b->data, 0, 24);
    memcpy(b->data, "NGPUADD2", 8);
    memcpy(b->data + 8, &b->count, 4);
    const uint64_t total = b->at;
    memcpy(b->data + 16, &total, 8);
}

static const uint8_t *find(const Builder *b)
{
    return b->data;
}

int main(void)
{
    Builder b;
    Tb710fuAddonEntry e;

    /* One entry, exact name: found with its fields. */
    memset(&b, 0, sizeof b);
    b.at = 24;
    entry(&b, "EFI/UniXNU/firmware/Adsp/adsp_dtb.mbn", 0644, 4096, 1024);
    entry(&b, "EFI/UniXNU/firmware/Adsp/adsp.mbn", 0644, 34816000, 16);
    finish(&b);
    CHECK(tb710fu_addon_find(find(&b), b.at, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_OK);
    CHECK(e.size == 34816000 && e.packed_bytes == 16 && e.mode == 0644);
    CHECK(strcmp(e.path, "EFI/UniXNU/firmware/Adsp/adsp.mbn") == 0);
    CHECK(e.packed && e.packed[0] == 0x40);

    /* A name that is not in the container, and an empty container. */
    CHECK(tb710fu_addon_find(find(&b), b.at, "EFI/UniXNU/firmware/Adsp/other.mbn", &e) == TB710FU_ADDON_NOT_FOUND);
    memset(&b, 0, sizeof b);
    b.at = 24;
    finish(&b);
    CHECK(tb710fu_addon_find(find(&b), b.at, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_NOT_FOUND);

    /* Every damaged shape: magic, zero field, total, count, missing entry body, trailing bytes, interior NUL, oversize path. */
    memset(&b, 0, sizeof b);
    b.at = 24;
    entry(&b, "EFI/UniXNU/firmware/Adsp/adsp.mbn", 0644, 16, 8);
    finish(&b);
    uint8_t copy[8192];
    memcpy(copy, b.data, b.at);

    copy[0] = 'X';
    CHECK(tb710fu_addon_find(copy, b.at, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_DAMAGED);
    memcpy(copy, b.data, b.at);
    copy[13] = 1;
    CHECK(tb710fu_addon_find(copy, b.at, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_DAMAGED);
    memcpy(copy, b.data, b.at);
    {
        const uint64_t wrong = b.at + 8;
        memcpy(copy + 16, &wrong, 8);
    }
    CHECK(tb710fu_addon_find(copy, b.at, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_DAMAGED);
    memcpy(copy, b.data, b.at);
    {
        const uint32_t nine = 9;
        memcpy(copy + 8, &nine, 4);
    }
    CHECK(tb710fu_addon_find(copy, b.at, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_DAMAGED);
    CHECK(tb710fu_addon_find(b.data, b.at - 1, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_DAMAGED);
    memcpy(copy, b.data, b.at);
    copy[b.at] = 0;
    CHECK(tb710fu_addon_find(copy, b.at + 1, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_DAMAGED);
    memcpy(copy, b.data, b.at);
    copy[24 + 24 + 5] = 0;  /* inside the name, before its NUL */
    CHECK(tb710fu_addon_find(copy, b.at, "EFI/UniXNU/firmware/Adsp/adsp.mbn", &e) == TB710FU_ADDON_DAMAGED);

    /* A name exactly at the 160-byte limit is legal; 161 is not (name_bytes > TB710FU_ADDON_PATH_MAX). */
    memset(&b, 0, sizeof b);
    b.at = 24;
    char long_path[TB710FU_ADDON_PATH_MAX];
    memset(long_path, 'a', sizeof long_path - 1);
    long_path[sizeof long_path - 1] = 0;
    entry(&b, long_path, 0644, 8, 4);
    finish(&b);
    CHECK(tb710fu_addon_find(b.data, b.at, long_path, &e) == TB710FU_ADDON_OK);

    printf("tb710fu addon host test: all checks passed\n");
    return 0;
}
