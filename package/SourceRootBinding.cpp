// SPDX-License-Identifier: BSD-2-Clause-Patent
// Source: UniXNU/UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc,
// package/SourceRootBinding.cpp (read with git show at that exact revision).
// TB710FU changes: validate bounded identities and BSD numbers; retain registry
// properties and parents while inspecting them; reject ambiguous device/UUID
// matches and clear all failed
// outputs. Authentication and the SHA-384 manifest remain NativeUfs operations,
// not registry assertions or substitute security providers.
#include <IOKit/IOService.h>
#include <IOKit/IOBSD.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <sys/types.h>
#include <sys/param.h>

extern "C" {
int native_ufs_block_authenticates(IOService *provider, const char *uuid, uint8_t *digest);
int native_verified_block_root_authorized(dev_t device);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);
}

namespace {
constexpr size_t kDigestBytes = 48;
constexpr size_t kUUIDBytes = 37;
constexpr size_t kNameBytes = 58; // Preserve the reference's maximum 57-byte BSD name.
constexpr size_t kPathBytes = 64;
constexpr unsigned kProviderLimit = 64;

struct MediaIdentity {
    char uuid[kUUIDBytes];
    char name[kNameBytes];
    dev_t device;
};

// Bound the scan even for a malformed caller string or an embedded-NUL OSString.
static bool validUUID(const char *uuid)
{
    if (!uuid) return false;
    for (size_t i = 0; i != kUUIDBytes - 1; ++i) {
        const char c = uuid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return uuid[kUUIDBytes - 1] == 0;
}

static bool copyString(IOService *media, const char *key, char *out, size_t size, bool uuid)
{
    OSObject *property = media->copyProperty(key);
    OSString *text = OSDynamicCast(OSString, property);
    const size_t length = text ? text->getLength() : 0;
    const char *bytes = text ? text->getCStringNoCopy() : nullptr;
    bool valid = bytes && length && length < size;
    if (valid && uuid) valid = length == kUUIDBytes - 1 && validUUID(bytes);
    if (valid && !uuid) {
        // A BSD node is a single leaf, never a path, traversal or truncated name.
        for (size_t i = 0; i != length; ++i) {
            const char c = bytes[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) {
                valid = false;
                break;
            }
        }
        if (valid) valid = bytes[length] == 0;
    }
    if (valid) memcpy(out, bytes, length + 1);
    OSSafeReleaseNULL(property);
    return valid;
}

static bool copyIdentity(IOService *media, MediaIdentity &identity)
{
    if (media->isInactive() || !copyString(media, "UUID", identity.uuid, sizeof(identity.uuid), true) ||
        !copyString(media, kIOBSDNameKey, identity.name, sizeof(identity.name), false)) return false;
    OSObject *majorProperty = media->copyProperty(kIOBSDMajorKey);
    OSObject *minorProperty = media->copyProperty(kIOBSDMinorKey);
    OSNumber *mjr = OSDynamicCast(OSNumber, majorProperty);
    OSNumber *mnr = OSDynamicCast(OSNumber, minorProperty);
    const bool valid = mjr && mnr &&
                       mjr->unsigned64BitValue() <= 255 && mnr->unsigned64BitValue() <= 0xffffff;
    if (valid) {
        // Unsigned operands avoid a signed left shift in the makedev macro.
        identity.device = makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue());
    }
    OSSafeReleaseNULL(minorProperty);
    OSSafeReleaseNULL(majorProperty);
    return valid && identity.device != NODEV;
}

// Returns one retained service, or NULL for no match, ambiguity or invalidation.
// getMatchingServices does not consume `matching`; release it exactly once here.
static IOService *uniqueMedia(OSDictionary *properties)
{
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    const bool ready = matching && properties && matching->setObject("IOPropertyMatch", properties);
    OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!iterator) return nullptr;
    IOService *found = nullptr;
    bool ambiguous = false;
    while (OSObject *object = iterator->getNextObject()) {
        IOService *media = OSDynamicCast(IOService, object);
        if (!media || found) { ambiguous = true; break; }
        media->retain();
        found = media;
    }
    if (ambiguous || !iterator->isValid()) OSSafeReleaseNULL(found);
    iterator->release();
    return found;
}

static IOService *mediaForDevice(dev_t device)
{
    if (device == NODEV) return nullptr;
    OSDictionary *properties = OSDictionary::withCapacity(2);
    const uint32_t raw = static_cast<uint32_t>(device);
    OSNumber *mjr = OSNumber::withNumber(static_cast<unsigned>(major(raw)), 32);
    OSNumber *mnr = OSNumber::withNumber(static_cast<unsigned>(minor(raw)), 32);
    const bool ready = properties && mjr && mnr && properties->setObject(kIOBSDMajorKey, mjr) &&
                       properties->setObject(kIOBSDMinorKey, mnr);
    IOService *media = ready ? uniqueMedia(properties) : nullptr;
    OSSafeReleaseNULL(mnr);
    OSSafeReleaseNULL(mjr);
    OSSafeReleaseNULL(properties);
    return media;
}

static IOService *mediaForUUID(const char *uuid)
{
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *wanted = OSString::withCString(uuid);
    const bool ready = properties && wanted && properties->setObject("UUID", wanted);
    IOService *media = ready ? uniqueMedia(properties) : nullptr;
    OSSafeReleaseNULL(wanted);
    OSSafeReleaseNULL(properties);
    return media;
}

static bool authenticates(IOService *media, const char *uuid, uint8_t *digest)
{
    // Read-only storage is enforced by the real NativeUfsBlock parent:
    // reportWriteProtection=true, write/format requests return NotWritable.
    // Do not infer authentication from an APFS IOMedia's registry properties.
    // Owner-root mount callers separately require MNT_RDONLY after mounting.
    IOService *provider = media;
    provider->retain();
    bool authorized = false;
    for (unsigned depth = 0; provider && depth != kProviderLimit; ++depth) {
        if (provider->isInactive()) break;
        const int result = native_ufs_block_authenticates(provider, uuid, digest);
        if (result >= 0) { authorized = result == 1; break; }
        IORegistryEntry *parent = provider->copyParentEntry(gIOServicePlane);
        provider->release();
        provider = OSDynamicCast(IOService, parent);
        if (!provider) OSSafeReleaseNULL(parent);
        // A successful cast carries copyParentEntry's reference into the next step.
    }
    OSSafeReleaseNULL(provider);
    return authorized;
}

static bool sameIdentity(const MediaIdentity &a, const MediaIdentity &b)
{
    return a.device == b.device && !strcmp(a.uuid, b.uuid) && !strcmp(a.name, b.name);
}

static int verifiedVolumeQuery(dev_t device, const char *expectedUUID, uint8_t *manifestDigest,
                               char *bsdName = nullptr)
{
    if (manifestDigest) bzero(manifestDigest, kDigestBytes);
    IOService *media = mediaForDevice(device);
    if (!media) return 0;
    MediaIdentity identity = {}, checked = {};
    bool authorized = copyIdentity(media, identity) && identity.device == device &&
                      (!expectedUUID || !strcmp(expectedUUID, identity.uuid));
    // An authenticated UUID must identify the same unique media as its BSD number.
    IOService *byUUID = authorized ? mediaForUUID(identity.uuid) : nullptr;
    authorized = authorized && byUUID == media;
    OSSafeReleaseNULL(byUUID);
    uint8_t digest[kDigestBytes] = {};
    authorized = authorized && authenticates(media, identity.uuid, manifestDigest ? digest : nullptr) &&
                 copyIdentity(media, checked) && sameIdentity(identity, checked);
    // These are bounded registry snapshots, not a lock against detach/rematching.
    // Recheck both indices after authenticating; mount callers must reauthorize
    // the actual mounted device as the owner-root patches already require.
    IOService *byDevice = authorized ? mediaForDevice(device) : nullptr;
    byUUID = authorized ? mediaForUUID(identity.uuid) : nullptr;
    authorized = authorized && byDevice == media && byUUID == media && !media->isInactive();
    OSSafeReleaseNULL(byUUID);
    OSSafeReleaseNULL(byDevice);
    media->release();
    if (authorized) {
        if (manifestDigest) memcpy(manifestDigest, digest, kDigestBytes);
        if (bsdName) memcpy(bsdName, identity.name, strlen(identity.name) + 1);
    }
    bzero(digest, sizeof(digest));
    return authorized ? 1 : 0;
}
} // namespace

extern "C" int native_verified_block_root_authorized(dev_t device)
{
    return verifiedVolumeQuery(device, nullptr, nullptr);
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest) return 0;
    bzero(digest, kDigestBytes);
    if (!validUUID(uuid)) return 0;
    return verifiedVolumeQuery(device, uuid, digest);
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
    if (device) *device = NODEV;
    if (path && size) path[0] = 0;
    if (!validUUID(uuid) || !device || !path || size < 6) return 0;
    IOService *media = mediaForUUID(uuid);
    if (!media) return 0;
    MediaIdentity identity = {}, checked = {};
    bool authorized = copyIdentity(media, identity) && !strcmp(identity.uuid, uuid);
    char name[kNameBytes] = {};
    authorized = authorized && verifiedVolumeQuery(identity.device, uuid, nullptr, name) &&
                 !strcmp(name, identity.name) && copyIdentity(media, checked) &&
                 sameIdentity(identity, checked);
    IOService *byDevice = authorized ? mediaForDevice(identity.device) : nullptr;
    IOService *byUUID = authorized ? mediaForUUID(uuid) : nullptr;
    authorized = authorized && byDevice == media && byUUID == media && !media->isInactive();
    OSSafeReleaseNULL(byUUID);
    OSSafeReleaseNULL(byDevice);
    media->release();
    if (!authorized || strlen(name) + 6 > size) return 0;
    char candidatePath[kPathBytes] = {};
    const int written = snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(candidatePath) ||
        static_cast<size_t>(written) >= size) return 0;
    memcpy(path, candidatePath, static_cast<size_t>(written) + 1);
    *device = identity.device;
    return 1;
}
