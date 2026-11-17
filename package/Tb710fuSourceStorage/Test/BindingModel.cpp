// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "BindingModel.h"
// The runner generates this from the current production file, dropping includes only.
#include "SourceRootBinding.model.inc"

namespace {
constexpr const char *uuid = "12345678-1234-1234-1234-123456789ABC";
constexpr const char *otherUUID = "12345678-1234-1234-1234-123456789ABD";
const dev_t device = makedev(6U, 17U);
std::vector<IOService *> owned;

void set(IOService *service, const char *key, OSObject *property)
{
    assert(service->properties->setObject(key, property));
    property->release();
}

IOService *node()
{
    auto service = new IOService;
    owned.push_back(service);
    return service;
}

IOService *media(const char *identity = uuid, uint64_t majorValue = 6, uint64_t minorValue = 17)
{
    auto service = node();
    set(service, "UUID", new OSString(identity));
    set(service, kIOBSDNameKey, new OSString("disk6s17"));
    set(service, kIOBSDMajorKey, new OSNumber(majorValue));
    set(service, kIOBSDMinorKey, new OSNumber(minorValue));
    model::registry.push_back(service);
    return service;
}

IOService *window(IOService *child)
{
    auto parent = node();
    parent->auth = 1;
    child->parent = parent;
    return parent;
}

void clear()
{
    model::onAuthenticate = {};
    model::registry.clear();
    for (auto service : owned) {
        assert(service->refs == 1); // No leaked retain after success or any refusal.
        service->release();
    }
    owned.clear();
    model::invalidIterator = false;
    model::failAllocationAfter = model::failSetAfter = -1;
    assert(model::liveObjects == 0);
}

void rejectDigest(dev_t number = device, const char *identity = uuid)
{
    uint8_t digest[48];
    memset(digest, 0xee, sizeof digest);
    assert(!native_verified_block_manifest_sha384(number, identity, digest));
    for (auto byte : digest) assert(byte == 0);
}

void rejectVolume(const char *identity = uuid, size_t size = 64)
{
    dev_t number = 0;
    char path[64] = "previous output";
    assert(!native_verified_block_volume_device(identity, &number, path, size));
    assert(number == NODEV && !path[0]);
}

void successAndBounds()
{
    auto child = media();
    window(child);
    uint8_t digest[48];
    assert(native_verified_block_root_authorized(device));
    assert(native_verified_block_manifest_sha384(device, uuid, digest));
    for (auto byte : digest) assert(byte == 0xa7);
    dev_t number = 0;
    char path[64];
    assert(native_verified_block_volume_device(uuid, &number, path, sizeof path));
    assert(number == device && !strcmp(path, "/dev/disk6s17"));
    for (unsigned i = 0; i != 50; ++i) assert(native_verified_block_root_authorized(device));
    rejectVolume(uuid, 6);
    rejectDigest(NODEV);
    rejectDigest(device, "short");
    rejectDigest(device, otherUUID);
    rejectVolume(nullptr);
    assert(!native_verified_block_manifest_sha384(device, uuid, nullptr));
    set(child, "UUID", new OSString(std::string("12345678-1234-1234-1234-123456789AB\0", 36)));
    rejectDigest();
    clear();

    for (const auto &name : {std::string("../disk"), std::string(58, 'a'), std::string("disk\0s1", 7)}) {
        child = media();
        window(child);
        set(child, kIOBSDNameKey, new OSString(name));
        rejectDigest();
        clear();
    }
    for (uint64_t value : {256ULL, 0x100000006ULL}) {
        window(media(uuid, value, 17));
        rejectVolume();
        clear();
    }
    for (uint64_t value : {0x1000000ULL, 0x100000011ULL}) {
        window(media(uuid, 6, value));
        rejectVolume();
        clear();
    }
}

void refusalsAndOwnership()
{
    window(media());
    media(otherUUID); // Same BSD number, different UUID.
    rejectDigest();
    rejectVolume();
    clear();
    window(media());
    media(uuid, 6, 18); // Same UUID, different BSD number.
    rejectDigest();
    rejectVolume();
    clear();
    auto child = media();
    auto parent = window(child);
    parent->auth = 0;
    rejectDigest(); // Authenticator writes a digest before refusing; output must stay zero.
    clear();
    child = media();
    window(child);
    child->inactive = true;
    rejectDigest();
    clear();
    child = media();
    parent = window(child);
    parent->inactive = true;
    rejectDigest();
    clear();

    for (unsigned steps : {63U, 64U}) {
        child = media();
        parent = child;
        for (unsigned i = 0; i != steps; ++i) {
            auto next = node();
            parent->parent = next;
            parent = next;
        }
        parent->auth = 1;
        assert(bool(native_verified_block_root_authorized(device)) == (steps == 63));
        clear();
    }
    child = media();
    parent = node();
    child->parent = parent;
    parent->parent = child;
    rejectDigest(); // Bounded cycle traversal.
    clear();

    for (bool allocation : {true, false}) {
        // Refuse each factory/setObject call reached by all three interfaces.
        for (int failure = 0; failure != 32; ++failure) {
            for (unsigned operation = 0; operation != 3; ++operation) {
                window(media());
                (allocation ? model::failAllocationAfter : model::failSetAfter) = failure;
                uint8_t digest[48];
                memset(digest, 0xee, sizeof digest);
                dev_t number = 0;
                char path[64] = "previous output";
                int accepted = operation == 0 ? native_verified_block_root_authorized(device) :
                    operation == 1 ? native_verified_block_manifest_sha384(device, uuid, digest) :
                    native_verified_block_volume_device(uuid, &number, path, sizeof path);
                if ((allocation ? model::failAllocationAfter : model::failSetAfter) < 0)
                    assert(!accepted); // Every injected dependency failure must fail closed.
                if (!accepted && operation == 1) for (auto byte : digest) assert(byte == 0);
                if (!accepted && operation == 2) assert(number == NODEV && !path[0]);
                clear();
            }
        }
    }
}

void mutations()
{
    auto child = media();
    window(child);
    model::onAuthenticate = [child] { set(child, kIOBSDNameKey, new OSString("disk6s18")); };
    rejectDigest();
    clear();
    window(media());
    model::invalidIterator = true;
    rejectDigest();
    clear();
    window(media());
    model::onAuthenticate = [] { model::invalidIterator = true; };
    rejectDigest(); // Iterator invalidation after authentication.
    clear();
    window(media());
    model::onAuthenticate = [] { media(uuid, 6, 18); };
    rejectDigest(); // Duplicate UUID appears while authentication runs.
    clear();
    window(media());
    model::onAuthenticate = [] { media(otherUUID); };
    rejectDigest(); // Duplicate BSD number appears while authentication runs.
    clear();
    child = media();
    window(child);
    model::onAuthenticate = [child] { child->inactive = true; };
    rejectVolume(); // The retained object can become inactive without disappearing.
    clear();
}
} // namespace

extern "C" int native_ufs_block_authenticates(IOService *provider, const char *identity, uint8_t *digest)
{
    if (provider->auth < 0) return -1;
    if (digest) memset(digest, 0xa7, 48);
    if (model::onAuthenticate) model::onAuthenticate();
    return provider->auth == 1 && !strcmp(identity, uuid) ? 1 : 0;
}

int main()
{
    successAndBounds();
    refusalsAndOwnership();
    mutations();
    puts("PASS: production binding model; identities, failure outputs, retained lifetimes, "
         "allocation faults, provider bounds and authentication-time mutation");
}
