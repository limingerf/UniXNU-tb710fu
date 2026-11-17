// SPDX-License-Identifier: BSD-2-Clause-Patent
// Narrow host model of the IOKit ownership/matching calls used by the binding.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using binding_dev_t = int32_t;
#define dev_t binding_dev_t
#define major(x) ((int32_t)(((uint32_t)(x) >> 24) & 0xff))
#define minor(x) ((int32_t)((x) & 0xffffff))
#define makedev(x, y) ((dev_t)(((x) << 24) | (y)))
#define NODEV ((dev_t)-1)
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSSafeReleaseNULL(object) \
    do { if (object) { (object)->release(); (object) = nullptr; } } while (0)

constexpr const char *kIOBSDMajorKey = "major";
constexpr const char *kIOBSDMinorKey = "minor";
constexpr const char *kIOBSDNameKey = "name";
const void *gIOServicePlane = nullptr;

struct IOService;
namespace model {
int liveObjects = 0;
int failAllocationAfter = -1;
int failSetAfter = -1;
bool invalidIterator = false;
std::vector<IOService *> registry;
std::function<void()> onAuthenticate;

bool fail(int &remaining)
{
    if (remaining < 0) return false;
    if (remaining == 0) { remaining = -1; return true; }
    --remaining;
    return false;
}
} // namespace model

struct OSObject {
    int refs = 1;
    OSObject() { ++model::liveObjects; }
    virtual ~OSObject() { --model::liveObjects; }
    void retain() { assert(refs > 0); ++refs; }
    void release() { assert(refs > 0); if (!--refs) delete this; }
};

struct OSString : OSObject {
    std::string value;
    explicit OSString(std::string text) : value(std::move(text)) {}
    static OSString *withCString(const char *text)
    {
        return model::fail(model::failAllocationAfter) ? nullptr : new OSString(text);
    }
    size_t getLength() { return value.size(); }
    const char *getCStringNoCopy() { return value.c_str(); }
};

struct OSNumber : OSObject {
    uint64_t value;
    explicit OSNumber(uint64_t number) : value(number) {}
    static OSNumber *withNumber(uint64_t number, unsigned)
    {
        return model::fail(model::failAllocationAfter) ? nullptr : new OSNumber(number);
    }
    uint64_t unsigned64BitValue() { return value; }
    uint32_t unsigned32BitValue() { return uint32_t(value); }
};

struct OSDictionary : OSObject {
    std::map<std::string, OSObject *> values;
    ~OSDictionary() { for (auto &entry : values) entry.second->release(); }
    static OSDictionary *withCapacity(unsigned)
    {
        return model::fail(model::failAllocationAfter) ? nullptr : new OSDictionary;
    }
    bool setObject(const char *key, OSObject *value)
    {
        if (!value || model::fail(model::failSetAfter)) return false;
        value->retain();
        auto &previous = values[key];
        if (previous) previous->release();
        previous = value;
        return true;
    }
    OSObject *get(const char *key)
    {
        auto entry = values.find(key);
        return entry == values.end() ? nullptr : entry->second;
    }
};

struct IORegistryEntry : OSObject {
    OSDictionary *properties = new OSDictionary;
    IORegistryEntry *parent = nullptr; // Fixture owns the graph; copied parents retain.
    ~IORegistryEntry() { properties->release(); }
    OSObject *copyProperty(const char *key)
    {
        auto property = properties->get(key);
        if (property) property->retain();
        return property;
    }
    IORegistryEntry *copyParentEntry(const void *)
    {
        if (parent) parent->retain();
        return parent;
    }
};

struct OSIterator : OSObject {
    std::vector<OSObject *> objects;
    size_t next = 0;
    bool valid = true;
    explicit OSIterator(std::vector<OSObject *> entries) : objects(std::move(entries))
    {
        for (auto object : objects) object->retain();
    }
    ~OSIterator() { for (auto object : objects) object->release(); }
    OSObject *getNextObject() { return next < objects.size() ? objects[next++] : nullptr; }
    bool isValid() { return valid; }
};

struct IOService : IORegistryEntry {
    bool inactive = false;
    int auth = -1;
    bool isInactive() { return inactive; }
    static OSDictionary *serviceMatching(const char *) { return OSDictionary::withCapacity(1); }
    static OSIterator *getMatchingServices(OSDictionary *);
};

static bool modelEqual(OSObject *left, OSObject *right)
{
    auto ls = dynamic_cast<OSString *>(left), rs = dynamic_cast<OSString *>(right);
    auto ln = dynamic_cast<OSNumber *>(left), rn = dynamic_cast<OSNumber *>(right);
    return ls && rs ? ls->value == rs->value : ln && rn && ln->value == rn->value;
}

OSIterator *IOService::getMatchingServices(OSDictionary *matching)
{
    // Matching dictionaries remain owned by the caller in the pinned XNU API.
    auto wanted = dynamic_cast<OSDictionary *>(matching->get("IOPropertyMatch"));
    assert(wanted);
    if (model::fail(model::failAllocationAfter)) return nullptr;
    std::vector<OSObject *> found;
    for (auto media : model::registry) {
        bool matches = true;
        for (auto &entry : wanted->values)
            if (!modelEqual(media->properties->get(entry.first.c_str()), entry.second)) matches = false;
        if (matches) found.push_back(media);
    }
    auto result = new OSIterator(std::move(found));
    result->valid = !model::invalidIterator;
    return result;
}
