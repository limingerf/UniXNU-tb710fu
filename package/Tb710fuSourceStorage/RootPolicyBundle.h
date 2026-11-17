// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef TB710FU_ROOT_POLICY_BUNDLE_H
#define TB710FU_ROOT_POLICY_BUNDLE_H

#include <stddef.h>
#include <stdint.h>
#include "verified_root.h"

// VROOTB01 v1 layout from UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc,
// package/SourceRootPolicies.cpp. This helper never maps or owns its input.
namespace tb710fu {

static constexpr size_t kRootPolicyLimit = 16;
static constexpr size_t kRootPolicyBundleLimit = 128U * 1024U * 1024U;
static constexpr size_t kRootPolicySignedHeaderBytes = VR_HEADER_BYTES + VR_SIGNATURE_BYTES;

struct RootPolicy {
    const uint8_t *manifest;
    uint64_t manifestBytes;
};

struct RootPolicyBundle {
    RootPolicy policies[kRootPolicyLimit];
    size_t count;
};

using RootPolicyAuthenticate = bool (*)(const uint8_t *, size_t, bool headerOnly, void *);

static inline uint64_t rootPolicyRead64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}

static inline bool rootPolicyHandoffRange(uint64_t physical, uint64_t length)
{
    return physical && !(physical & 4095) && length >= 32 &&
           length <= kRootPolicyBundleLimit && physical <= UINT64_MAX - length;
}

// The caller must keep bytes immutable for this call and the result's lifetime.
// Validate the entire table before invoking the authentication callback. No
// candidate pointers escape until every callback succeeds; failure clears out.
static inline bool openRootPolicyBundle(const uint8_t *bytes, size_t length,
                                       RootPolicyAuthenticate authenticate, void *context,
                                       RootPolicyBundle *out)
{
    if (!out) return false;
    *out = {};
    if (!bytes || !authenticate || length < 32 || length > kRootPolicyBundleLimit) return false;
    static const uint8_t magic[8] = {'V', 'R', 'O', 'O', 'T', 'B', '0', '1'};
    for (unsigned i = 0; i < sizeof(magic); ++i)
        if (bytes[i] != magic[i]) return false;
    if (rootPolicyRead64(bytes + 8) != 1 || rootPolicyRead64(bytes + 24) != length) return false;
    const uint64_t count = rootPolicyRead64(bytes + 16);
    if (!count || count > kRootPolicyLimit) return false;
    size_t cursor = 32 + static_cast<size_t>(count) * 16;
    if (cursor > length) return false;
    RootPolicyBundle candidate = {};
    for (size_t i = 0; i < count; ++i) {
        const uint64_t offset = rootPolicyRead64(bytes + 32 + i * 16);
        const uint64_t size = rootPolicyRead64(bytes + 40 + i * 16);
        // cursor <= length, so this subtraction and the subsequent addition
        // cannot wrap even when the untrusted 64-bit fields are UINT64_MAX.
        if (offset != cursor || size < kRootPolicySignedHeaderBytes || size > length - cursor)
            return false;
        candidate.policies[i] = {bytes + cursor, size};
        cursor += static_cast<size_t>(size);
    }
    if (cursor != length) return false;
    for (size_t i = 0; i < count; ++i) {
        const RootPolicy &policy = candidate.policies[i];
        if (!authenticate(policy.manifest, static_cast<size_t>(policy.manifestBytes),
                          policy.manifestBytes == kRootPolicySignedHeaderBytes, context)) return false;
    }
    candidate.count = static_cast<size_t>(count);
    *out = candidate;
    return true;
}

} // namespace tb710fu

#endif
