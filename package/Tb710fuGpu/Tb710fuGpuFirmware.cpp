// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware provenance for the gen70900 images (see Tb710fuGpuFirmware.h).
//
// This file never stages a byte into a GPU region: the GMU's own SMMU context
// and the SM8650 RSC/PDC programming are the preconditions of the cold start
// (stage 7), and neither exists yet. What it does prove on hardware is that the
// payload carries exactly the pinned images, which every later stage depends on.
#include "Tb710fuGpuFirmware.h"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <string.h>

extern "C" {
/* package/Tb710fuAdsp/Tb710fuAdspFirmware.c: vnode and addon readers. */
int tb710fu_fw_open(const char *path, void **handle, uint64_t *size);
int tb710fu_fw_open_addon_path(const char *path, void **handle, uint64_t *size);
int tb710fu_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void tb710fu_fw_close(void *handle);
}

namespace {
const char kDataVolume[] = "/private/var/ngpu/Gpu/Firmware/";
const char kAddonDirectory[] = "Gpu/Firmware/";

/* One-shot SHA-256, adapted from the framework's NativeAdrenoSha256.cpp (MIT),
 * which itself is the public-domain reference compression. */
const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t rotr(uint32_t value, unsigned count) { return (value >> count) | (value << (32 - count)); }

void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256(const uint8_t *bytes, uint32_t length, uint8_t digest[32])
{
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint32_t at = 0;
    while (length - at >= 64) {
        compress(state, bytes + at);
        at += 64;
    }
    uint8_t tail[64];
    const uint32_t rest = length - at;
    memset(tail, 0, sizeof(tail));
    if (rest) {
        memcpy(tail, bytes + at, rest);
    }
    tail[rest] = 0x80;
    if (rest >= 56) {
        compress(state, tail);
        memset(tail, 0, sizeof(tail));
    }
    const uint64_t bits = uint64_t(length) * 8;
    for (unsigned i = 0; i < 8; ++i) {
        tail[63 - i] = uint8_t(bits >> (8 * i));
    }
    compress(state, tail);
    for (unsigned i = 0; i < 8; ++i) {
        digest[i * 4] = uint8_t(state[i] >> 24);
        digest[i * 4 + 1] = uint8_t(state[i] >> 16);
        digest[i * 4 + 2] = uint8_t(state[i] >> 8);
        digest[i * 4 + 3] = uint8_t(state[i]);
    }
}

bool joinPath(char *destination, unsigned size, const char *directory, const char *name)
{
    if (!destination || size == 0 || !directory || !name) {
        return false;
    }
    unsigned at = 0;
    while (*directory && at + 1 < size) {
        destination[at++] = *directory++;
    }
    while (*name && at + 1 < size) {
        destination[at++] = *name++;
    }
    destination[at] = 0;
    return *directory == 0 && *name == 0;
}

int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool digestMatches(const uint8_t digest[32], const char *hex)
{
    if (!hex || strlen(hex) != 64) {
        return false;
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int hi = hexNibble(hex[i * 2]);
        const int lo = hexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0 || digest[i] != uint8_t((hi << 4) | lo)) {
            return false;
        }
    }
    return true;
}
}  // namespace

extern "C" int tb710fu_gpu_firmware_verify(const char *name, uint64_t expected_bytes,
                                            const char *expected_hex, struct Tb710fuGpuFirmware *out)
{
    if (!name || !expected_hex || !out || !expected_bytes) {
        return -5;
    }
    memset(out, 0, sizeof(*out));
    char path[192];
    void *handle = nullptr;
    uint64_t size = 0;
    uint32_t source = 1;
    /* Addon first: it is what the loader proved and what the payload ships. */
    if (!joinPath(path, sizeof(path), kAddonDirectory, name) ||
        tb710fu_fw_open_addon_path(path, &handle, &size) != 0) {
        source = 2;
        if (!joinPath(path, sizeof(path), kDataVolume, name) ||
            tb710fu_fw_open(path, &handle, &size) != 0) {
            return -1;
        }
    }
    int result = 0;
    if (size != expected_bytes) {
        result = -2;
    } else {
        uint8_t *image = static_cast<uint8_t *>(IOMallocData((vm_size_t)size));
        if (!image) {
            result = -4;
        } else {
            if (tb710fu_fw_read(handle, 0, image, (size_t)size) != 0) {
                result = -2;
            } else {
                sha256(image, (uint32_t)size, out->digest);
                if (!digestMatches(out->digest, expected_hex)) {
                    result = -3;
                }
            }
            IOFreeData(image, (vm_size_t)size);
        }
    }
    out->bytes = size;
    out->source = source;
    tb710fu_fw_close(handle);
    return result;
}
