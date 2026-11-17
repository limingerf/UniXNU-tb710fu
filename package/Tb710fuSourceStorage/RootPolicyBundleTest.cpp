// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "RootPolicyBundle.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
int vr_host_open(const uint8_t *, size_t, const uint8_t *, uint64_t, struct vr_view *);
int vr_host_open_header(const uint8_t *, size_t, const uint8_t *, uint64_t, struct vr_view *);
}

using Bytes = std::vector<uint8_t>;

static void put64(Bytes &bytes, size_t offset, uint64_t value)
{
    assert(offset <= bytes.size() && bytes.size() - offset >= 8);
    for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

static Bytes bundle(const std::vector<Bytes> &policies)
{
    size_t cursor = 32 + policies.size() * 16;
    Bytes bytes(cursor);
    memcpy(bytes.data(), "VROOTB01", 8);
    put64(bytes, 8, 1);
    put64(bytes, 16, policies.size());
    for (size_t i = 0; i < policies.size(); ++i) {
        put64(bytes, 32 + i * 16, cursor);
        put64(bytes, 40 + i * 16, policies[i].size());
        bytes.insert(bytes.end(), policies[i].begin(), policies[i].end());
        cursor += policies[i].size();
    }
    put64(bytes, 24, cursor);
    return bytes;
}

static Bytes read(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    assert(stream.is_open());
    return Bytes(std::istreambuf_iterator<char>(stream), {});
}

static void empty(const tb710fu::RootPolicyBundle &result)
{
    assert(!result.count);
    for (const auto &policy : result.policies) assert(!policy.manifest && !policy.manifestBytes);
}

struct LayoutAuthentication {
    size_t calls = 0;
    size_t headerCalls = 0;
    size_t failAt = SIZE_MAX;
};

// Used only for layout fault tests; real policies below use VerifiedRoot's
// existing OpenSSL provider and never this callback.
static bool layoutAuthentication(const uint8_t *, size_t, bool headerOnly, void *opaque)
{
    auto *state = static_cast<LayoutAuthentication *>(opaque);
    state->headerCalls += headerOnly;
    return state->calls++ != state->failAt;
}

static void badLayout(const Bytes &bytes)
{
    LayoutAuthentication state;
    tb710fu::RootPolicyBundle result = {};
    result.count = 1;
    assert(!tb710fu::openRootPolicyBundle(bytes.data(), bytes.size(), layoutAuthentication, &state, &result));
    empty(result);
    assert(!state.calls); // The whole table must be valid before any signature work.
}

struct RealAuthentication {
    const uint8_t *key;
    size_t calls = 0;
    size_t headerCalls = 0;
    size_t fullCalls = 0;
};

static bool realAuthentication(const uint8_t *bytes, size_t length, bool headerOnly, void *opaque)
{
    auto *state = static_cast<RealAuthentication *>(opaque);
    ++state->calls;
    state->headerCalls += headerOnly;
    state->fullCalls += !headerOnly;
    struct vr_view view = {};
    return (headerOnly ? vr_host_open_header(bytes, length, state->key, 1, &view) :
                         vr_host_open(bytes, length, state->key, 1, &view)) == VR_OK;
}

static void realPolicies(const std::string &directory, const char *fullAppPath)
{
    const Bytes key = read(directory + "/OS/trusted-key.bin");
    assert(key.size() == VR_KEY_BYTES);
    std::vector<Bytes> headers;
    for (const char *name : {"OS", "SystemOS", "App"}) {
        assert(read(directory + "/" + name + "/trusted-key.bin") == key);
        headers.push_back(read(directory + "/" + name + "/manifest.header.bin"));
        assert(headers.back().size() == tb710fu::kRootPolicySignedHeaderBytes);
    }
    Bytes bytes = bundle(headers);
    RealAuthentication state{key.data()};
    tb710fu::RootPolicyBundle result = {};
    assert(tb710fu::openRootPolicyBundle(bytes.data(), bytes.size(), realAuthentication, &state, &result));
    assert(result.count == 3 && state.calls == 3 && state.headerCalls == 3 && !state.fullCalls);
    for (size_t i = 0; i < 3; ++i) {
        assert(result.policies[i].manifest == bytes.data() + 80 + i * 544);
        assert(result.policies[i].manifestBytes == headers[i].size());
    }
    // Refuse each signed header/signature mutation, including a failure after
    // earlier genuine signatures have succeeded; no partial result is returned.
    for (size_t policy = 0; policy < 3; ++policy) for (size_t field : {size_t(40), size_t(160)}) {
        Bytes changed = bytes;
        changed[80 + policy * 544 + field] ^= 1;
        RealAuthentication failed{key.data()};
        assert(!tb710fu::openRootPolicyBundle(changed.data(), changed.size(), realAuthentication, &failed, &result));
        assert(failed.calls == policy + 1);
        empty(result);
    }
    Bytes otherKey = key;
    otherKey[9] ^= 1;
    RealAuthentication wrong{otherKey.data()};
    assert(!tb710fu::openRootPolicyBundle(bytes.data(), bytes.size(), realAuthentication, &wrong, &result));
    empty(result);
    if (fullAppPath) {
        const Bytes app = read(fullAppPath);
        assert(app.size() > headers[2].size());
        assert(!memcmp(app.data(), headers[2].data(), headers[2].size()));
        bytes = bundle({headers[0], headers[1], app});
        RealAuthentication full{key.data()};
        assert(tb710fu::openRootPolicyBundle(bytes.data(), bytes.size(), realAuthentication, &full, &result));
        assert(result.count == 3 && full.headerCalls == 2 && full.fullCalls == 1);
        // The App envelope includes its actual signed hash table. A table byte
        // mutation must be refused by vr_open, after the other policies pass.
        bytes[80 + 2 * 544 + app.size() - 1] ^= 1;
        RealAuthentication changed{key.data()};
        assert(!tb710fu::openRootPolicyBundle(bytes.data(), bytes.size(), realAuthentication, &changed, &result));
        assert(changed.calls == 3 && changed.fullCalls == 1);
        empty(result);
    }
    puts("PASS: existing OS/SystemOS/App RSA-3072 headers, independent key, signature mutations and no partial policy result");
    if (fullAppPath) puts("PASS: existing full App policy and its signed table; mutated table refused by vr_open");
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    assert(tb710fu::rootPolicyHandoffRange(0x1000, 32));
    assert(tb710fu::rootPolicyHandoffRange(0x1000, tb710fu::kRootPolicyBundleLimit));
    assert(!tb710fu::rootPolicyHandoffRange(0, 32));
    assert(!tb710fu::rootPolicyHandoffRange(0x1001, 32));
    assert(!tb710fu::rootPolicyHandoffRange(0x1000, 31));
    assert(!tb710fu::rootPolicyHandoffRange(0x1000, tb710fu::kRootPolicyBundleLimit + 1));
    assert(!tb710fu::rootPolicyHandoffRange(UINT64_MAX - 4095, 4096));

    const Bytes header(tb710fu::kRootPolicySignedHeaderBytes);
    const Bytes valid = bundle({header, header, header});
    LayoutAuthentication state;
    tb710fu::RootPolicyBundle result = {};
    assert(tb710fu::openRootPolicyBundle(valid.data(), valid.size(), layoutAuthentication, &state, &result));
    assert(result.count == 3 && state.calls == 3 && state.headerCalls == 3);
    for (size_t i = 3; i < tb710fu::kRootPolicyLimit; ++i)
        assert(!result.policies[i].manifest && !result.policies[i].manifestBytes);
    for (size_t n = 0; n < 32; ++n) {
        assert(!tb710fu::openRootPolicyBundle(valid.data(), n, layoutAuthentication, &state, &result));
        empty(result);
    }
    assert(!tb710fu::openRootPolicyBundle(nullptr, valid.size(), layoutAuthentication, &state, &result));
    assert(!tb710fu::openRootPolicyBundle(valid.data(), valid.size(), nullptr, &state, &result));
    assert(!tb710fu::openRootPolicyBundle(valid.data(), valid.size(), layoutAuthentication, &state, nullptr));
    assert(!tb710fu::openRootPolicyBundle(valid.data(), tb710fu::kRootPolicyBundleLimit + 1,
                                         layoutAuthentication, &state, &result));
    for (size_t i = 0; i < 8; ++i) { Bytes b = valid; b[i] ^= 1; badLayout(b); }
    for (uint64_t version : {uint64_t(0), uint64_t(2), UINT64_MAX}) {
        Bytes b = valid; put64(b, 8, version); badLayout(b);
    }
    for (uint64_t count : {uint64_t(0), uint64_t(17), UINT64_MAX}) {
        Bytes b = valid; put64(b, 16, count); badLayout(b);
    }
    for (uint64_t length : {uint64_t(0), uint64_t(valid.size() - 1), uint64_t(valid.size() + 1), UINT64_MAX}) {
        Bytes b = valid; put64(b, 24, length); badLayout(b);
    }
    { Bytes b(32); memcpy(b.data(), valid.data(), 32); put64(b, 24, b.size()); badLayout(b); }
    for (size_t i = 0; i < 3; ++i) {
        for (uint64_t offset : {uint64_t(0), uint64_t(80 + i * 544 - 1), uint64_t(80 + i * 544 + 1), UINT64_MAX}) {
            Bytes b = valid; put64(b, 32 + i * 16, offset); badLayout(b);
        }
        for (uint64_t size : {uint64_t(0), uint64_t(543), uint64_t(valid.size()), UINT64_MAX}) {
            Bytes b = valid; put64(b, 40 + i * 16, size); badLayout(b);
        }
    }
    { Bytes b = valid; b.push_back(0); put64(b, 24, b.size()); badLayout(b); }
    { Bytes b = valid; b.pop_back(); put64(b, 24, b.size()); badLayout(b); }
    for (size_t failAt = 0; failAt < 3; ++failAt) {
        LayoutAuthentication failed;
        failed.failAt = failAt;
        assert(!tb710fu::openRootPolicyBundle(valid.data(), valid.size(), layoutAuthentication, &failed, &result));
        assert(failed.calls == failAt + 1);
        empty(result);
    }
    { // The protocol's actual upper policy count is accepted without wrap.
        Bytes sixteen = bundle(std::vector<Bytes>(16, header));
        LayoutAuthentication maximum;
        assert(tb710fu::openRootPolicyBundle(sixteen.data(), sixteen.size(), layoutAuthentication, &maximum, &result));
        assert(result.count == 16 && maximum.calls == 16);
    }
    { // Header-only/full-envelope dispatch belongs to each record's exact size.
        Bytes mixed = bundle({header, Bytes(576), header});
        LayoutAuthentication dispatch;
        assert(tb710fu::openRootPolicyBundle(mixed.data(), mixed.size(), layoutAuthentication, &dispatch, &result));
        assert(dispatch.calls == 3 && dispatch.headerCalls == 2);
    }
    puts("PASS: handoff range, VROOTB01 v1 count/length/offset/size/overflow/truncation/trailing-data faults and authentication failure");
    realPolicies(argv[1], argc == 3 ? argv[2] : nullptr);
}
