"""Compile the actual service against a narrow host lifecycle test double.

Checks the open/answer boundaries, not real IOKit registration. Without sheng-aks every open is refused (closed boundary);
with it the user client answers the handshake/bookkeeping selectors, get_system (14), lock state (7), device state (17, 35;
decoded below the way libaks decodes it), wrap/unwrap (10, 11; argument checks and dispatch, with stand-in crypto: the real
key wrap is checked by test_keywrap.py), bag UUID (23), not-found configuration/memento (31, 139) and rejects the rest.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / "IOKit").mkdir()
    (work / "IOKit/IOService.h").write_text(r'''
#pragma once
#include <cstdint>
#include <cstddef>
using UInt32 = uint32_t;
using task_t = void *;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNotReady = -7;
constexpr IOReturn kIOReturnBadArgument = -8;
constexpr IOReturn kIOReturnNoSpace = -9;
constexpr IOReturn kIOReturnNoMemory = -10;
constexpr IOReturn kIOReturnError = -11;
class OSDictionary {};
class OSObject { public: virtual ~OSObject() = default; void release() { delete this; } };
class IOMemoryDescriptor {
public:
    uint8_t bytes[512]; size_t length = 512;
    size_t getLength() const { return length; }
    size_t writeBytes(size_t offset, const void *p, size_t n) { for (size_t i = 0; i < n; ++i) bytes[offset + i] = ((const uint8_t *)p)[i]; return n; }
};
struct IOExternalMethodArguments {
    const uint64_t *scalarInput; uint32_t scalarInputCount; uint64_t *scalarOutput; uint32_t scalarOutputCount;
    void *structureOutput; uint32_t structureOutputSize;
    IOMemoryDescriptor *structureOutputDescriptor = nullptr; uint32_t structureOutputDescriptorSize = 0;
    const void *structureInput = nullptr; uint32_t structureInputSize = 0; IOMemoryDescriptor *structureInputDescriptor = nullptr;
};
struct IOExternalMethodDispatch {};
class IOService;
class IOUserClient : public OSObject {
public:
    bool terminated = false, started = false;
    virtual bool initWithTask(task_t, void *, UInt32, OSDictionary *) { return true; }
    bool attach(IOService *) { return true; }
    void detach(IOService *) {}
    virtual bool start(IOService *) { started = true; return true; }
    void terminate() { terminated = true; }
    virtual IOReturn clientClose() { return kIOReturnSuccess; }
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch * = nullptr,
                                    OSObject * = nullptr, void * = nullptr) = 0;
};
#define OSDeclareDefaultStructors(name)
#define OSDefineMetaClassAndStructors(name, base)
class IOService : public OSObject {
public:
    bool baseOK = true;
    unsigned properties = 0, failProperty = 0, publications = 0, stops = 0;
    virtual bool start(IOService *) { return baseOK; }
    virtual void stop(IOService *) { ++stops; }
    void setName(const char *) {}
    bool setProperty(const char *, bool) { return ++properties != failProperty; }
    bool setProperty(const char *, const char *) { return ++properties != failProperty; }
    void registerService() { ++publications; }
    virtual IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) = 0;
    virtual IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) = 0;
};
''')
    (work / "IOKit/IOUserClient.h").write_text("#pragma once\n#include <IOKit/IOService.h>\n")
    (work / "test.cpp").write_text(r'''
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
static bool enabled;
static unsigned refused, opened, calls;
static uint32_t lockState;
extern "C" bool native_aks_state_enabled() { return enabled; }
extern "C" uint32_t native_aks_lock_state() { return lockState; }
extern "C" void native_keyevents_open_refused() { ++refused; }
extern "C" void native_aks_opened() { ++opened; }
extern "C" void native_aks_call(uint32_t, int32_t) { ++calls; }
#include <IOKit/IOService.h>
#include "AKSKeyWrap.h"
// Stand-in crypto (AKSDeviceKey.cpp is kernel code): wrap prefixes a class-tagged 8-byte header, unwrap checks it.
static unsigned keyStarts, unsupported;
extern "C" void native_aks_device_key_start() { ++keyStarts; }
extern "C" IOReturn native_aks_wrap(uint32_t, uint32_t keyClass, const uint8_t *key, size_t length, uint8_t *out, size_t capacity,
                                    size_t *written) {
    if (!aks::wrapLengthValid(length)) return (IOReturn)aks::kReturnBadArgument;
    if (capacity < length + 8) return (IOReturn)aks::kReturnNoSpace;
    memset(out, 0xa0 + (int)keyClass, 8); memcpy(out + 8, key, length); *written = length + 8; return kIOReturnSuccess;
}
extern "C" IOReturn native_aks_unwrap(uint32_t, uint32_t keyClass, const uint8_t *blob, size_t length, uint8_t *out,
                                      size_t capacity, size_t *written) {
    if (!aks::unwrappedLengthValid(length)) return (IOReturn)aks::kReturnBadArgument;
    if (capacity < length - 8) return (IOReturn)aks::kReturnNoSpace;
    for (int i = 0; i < 8; ++i) if (blob[i] != 0xa0 + (int)keyClass) return (IOReturn)aks::kReturnError;
    memcpy(out, blob + 8, length - 8); *written = length - 8; return kIOReturnSuccess;
}
extern "C" IOReturn native_aks_bag_uuid(uint32_t, uint8_t uuid[16]) { memset(uuid, 0x5a, 16); return kIOReturnSuccess; }
extern "C" void native_aks_unsupported(uint32_t) { ++unsupported; }
#include "NativeKeyStoreEvents.cpp"

// The decoder libaks uses (_der_dict_iterate/_der_get_number): SET { SEQUENCE { UTF8String key, INTEGER } ... }.
static bool decode(const uint8_t *p, size_t n, std::map<std::string, int64_t> &out) {
    size_t i = 0;
    auto len = [&](size_t &l) { if (i >= n) return false; uint8_t b = p[i++]; if (b < 0x80) { l = b; return true; }
        if (b != 0x81 || i >= n) return false; l = p[i++]; return true; };
    if (n < 2 || p[i++] != 0x31) return false;
    size_t total; if (!len(total) || i + total != n) return false;
    while (i < n) {
        if (p[i++] != 0x30) return false;
        size_t sl; if (!len(sl) || i + sl > n) return false;
        size_t end = i + sl;
        if (p[i++] != 0x0c) return false;
        size_t kl; if (!len(kl) || i + kl > end) return false;
        std::string key((const char *)p + i, kl); i += kl;
        if (p[i++] != 0x02) return false;
        size_t il; if (!len(il) || i + il != end || il == 0 || il > 8) return false;
        int64_t v = (int8_t)p[i]; for (size_t k = 1; k < il; ++k) v = (v << 8) | p[i + k];
        i = end; out[key] = v;
    }
    return true;
}
int main() {
    auto *device = static_cast<AppleKeyStore *>(native_keyevents_allocate());
    IOService *base = device;
    assert(base->start(nullptr) && device->publications == 1);
    IOUserClient sentinel2 = *(IOUserClient *)nullptr;
}
'''.replace('    IOUserClient sentinel2 = *(IOUserClient *)nullptr;\n', '''
    struct Probe : IOUserClient { IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) override { return 0; } };
    Probe sentinel;
    // Closed boundary (default): every open is refused and clears the output.
    for (UInt32 type : {0u, 1u, 7u, 0xffffffffu}) {
        IOUserClient *out = &sentinel;
        assert(base->newUserClient(nullptr, nullptr, type, nullptr, &out) == kIOReturnNotReady && !out);
        out = &sentinel;
        assert(base->newUserClient(nullptr, nullptr, type, &out) == kIOReturnNotReady && !out);
        assert(base->newUserClient(nullptr, nullptr, type, nullptr, nullptr) == kIOReturnNotReady);
    }
    assert(refused == 12 && !opened);
    // State mode: the client opens and answers only the state queries.
    enabled = true;
    IOUserClient *client = nullptr;
    assert(base->newUserClient(nullptr, nullptr, 0, nullptr, &client) == kIOReturnSuccess && client && client->started);
    assert(opened == 1);
    uint64_t in[1] = {0}, outScalar[1] = {0};
    IOExternalMethodArguments a{in, 1, outScalar, 1, nullptr, 0};
    assert(client->externalMethod(7, &a) == kIOReturnSuccess && outScalar[0] == 4);
    a.scalarInputCount = 2; assert(client->externalMethod(7, &a) == kIOReturnBadArgument);
    // get_system hands out the system bag handle 0; handshake and bookkeeping selectors succeed without outputs.
    outScalar[0] = 99; IOExternalMethodArguments sys{in, 0, outScalar, 1, nullptr, 0};
    assert(client->externalMethod(14, &sys) == kIOReturnSuccess && outScalar[0] == 0 && sys.scalarOutputCount == 1);
    IOExternalMethodArguments nosys{in, 0, nullptr, 0, nullptr, 0};
    assert(client->externalMethod(14, &nosys) == kIOReturnBadArgument);
    for (uint32_t sel : {0u, 4u, 5u, 9u, 12u, 13u, 26u, 27u, 55u, 101u, 102u}) {
        IOExternalMethodArguments h{nullptr, 0, nullptr, 0, nullptr, 0};
        assert(client->externalMethod(sel, &h) == kIOReturnSuccess);
    }
    uint8_t buffer[0x1000];
    for (uint32_t want : {0u, 3u}) {
        lockState = want;
        for (uint32_t sel : {17u, 35u}) {
            IOExternalMethodArguments s{in, 1, nullptr, 0, buffer, sizeof buffer};
            assert(client->externalMethod(sel, &s) == kIOReturnSuccess && s.structureOutputSize > 20 && s.structureOutputSize < 200);
            std::map<std::string, int64_t> state;
            assert(decode(buffer, s.structureOutputSize, state));
            assert(state.size() == 10 && state["sls"] == want && state["ss"] == 4 && state["bh"] == 0 && state.count("sgpe") && state.count("srcd"));
            // Large outputs come through a descriptor.
            IOMemoryDescriptor descriptor;
            IOExternalMethodArguments d{in, 1, nullptr, 0, nullptr, 0, &descriptor, 0};
            assert(client->externalMethod(sel, &d) == kIOReturnSuccess && d.structureOutputDescriptorSize == s.structureOutputSize);
            std::map<std::string, int64_t> viaDescriptor;
            assert(decode(descriptor.bytes, d.structureOutputDescriptorSize, viaDescriptor) && viaDescriptor == state);
            IOMemoryDescriptor tiny; tiny.length = 10;
            IOExternalMethodArguments td{in, 1, nullptr, 0, nullptr, 0, &tiny, 0};
            assert(client->externalMethod(sel, &td) == kIOReturnNoSpace);
            IOExternalMethodArguments none{nullptr, 0, nullptr, 0, buffer, sizeof buffer};
            assert(client->externalMethod(sel, &none) == kIOReturnSuccess);
            IOExternalMethodArguments small{in, 1, nullptr, 0, buffer, 10};
            assert(client->externalMethod(sel, &small) == kIOReturnNoSpace);
            IOExternalMethodArguments bad{in, 1, outScalar, 1, buffer, sizeof buffer};
            assert(client->externalMethod(sel, &bad) == kIOReturnBadArgument);
        }
    }
    // Wrap/unwrap: [bag, class] in, key or blob inline; class echoed back by wrap; malformed requests rejected, not answered.
    uint8_t key[32], blob[64], back[64];
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)(i * 3);
    uint64_t bagClass[2] = {0, 11}, cls[1] = {0};
    IOExternalMethodArguments w{bagClass, 2, cls, 1, blob, sizeof blob};
    w.structureInput = key; w.structureInputSize = sizeof key;
    assert(client->externalMethod(10, &w) == kIOReturnSuccess && w.structureOutputSize == 40 && cls[0] == 11);
    IOExternalMethodArguments u{bagClass, 2, nullptr, 0, back, sizeof back};
    u.structureInput = blob; u.structureInputSize = 40;
    assert(client->externalMethod(11, &u) == kIOReturnSuccess && u.structureOutputSize == 32 && !memcmp(back, key, 32));
    uint64_t otherClass[2] = {0, 6};
    IOExternalMethodArguments wrongClass{otherClass, 2, nullptr, 0, back, sizeof back};
    wrongClass.structureInput = blob; wrongClass.structureInputSize = 40;
    assert(client->externalMethod(11, &wrongClass) == (IOReturn)0xe00002bc);
    uint64_t otherBag[2] = {7, 11}, badBag[2] = {(uint64_t)-1, 11}, noClass[2] = {0, 0}, bigClass[2] = {0, 32};
    for (auto *scalars : {otherBag, badBag, noClass, bigClass}) {
        IOExternalMethodArguments r{scalars, 2, cls, 1, blob, sizeof blob};
        r.structureInput = key; r.structureInputSize = sizeof key;
        assert(client->externalMethod(10, &r) == (scalars == otherBag ? (IOReturn)0xe00002f0 : (IOReturn)0xe00002c2));
    }
    for (uint32_t length : {0u, 8u, 20u, 520u}) {
        IOExternalMethodArguments r{bagClass, 2, cls, 1, buffer, sizeof buffer};
        r.structureInput = buffer; r.structureInputSize = length;
        assert(client->externalMethod(10, &r) == (IOReturn)0xe00002c2);
    }
    IOExternalMethodArguments shortOut{bagClass, 2, cls, 1, blob, 39};
    shortOut.structureInput = key; shortOut.structureInputSize = sizeof key;
    assert(client->externalMethod(10, &shortOut) == (IOReturn)0xe00002db);
    IOExternalMethodArguments noInput{bagClass, 2, cls, 1, blob, sizeof blob};
    assert(client->externalMethod(10, &noInput) == (IOReturn)0xe00002c2);
    IOExternalMethodArguments unwrapScalarOut{bagClass, 2, cls, 1, back, sizeof back};
    unwrapScalarOut.structureInput = blob; unwrapScalarOut.structureInputSize = 40;
    assert(client->externalMethod(11, &unwrapScalarOut) == (IOReturn)0xe00002c2);
    // Bag UUID for the system bag only; configuration and memento state are honestly absent.
    uint8_t uuid[16] = {};
    IOExternalMethodArguments g{in, 1, nullptr, 0, uuid, sizeof uuid};
    assert(client->externalMethod(23, &g) == kIOReturnSuccess && g.structureOutputSize == 16 && uuid[0] == 0x5a);
    uint64_t bagSeven[1] = {7};
    IOExternalMethodArguments g7{bagSeven, 1, nullptr, 0, uuid, sizeof uuid};
    assert(client->externalMethod(23, &g7) == (IOReturn)0xe00002f0);
    for (uint32_t sel : {31u, 139u}) {
        IOExternalMethodArguments c{in, 1, nullptr, 0, buffer, sizeof buffer};
        assert(client->externalMethod(sel, &c) == (IOReturn)0xe00002f0);
        IOExternalMethodArguments cb{in, 2, nullptr, 0, buffer, sizeof buffer};
        assert(client->externalMethod(sel, &cb) == (IOReturn)0xe00002c2);
    }
    // Everything else is unsupported and reported once per call to the console hook; a missing argument block is an error.
    unsupported = 0;
    for (uint32_t sel : {1u, 2u, 3u, 6u, 15u, 16u, 18u, 36u, 73u, 1000u}) {
        IOExternalMethodArguments x{in, 1, outScalar, 1, buffer, sizeof buffer};
        assert(client->externalMethod(sel, &x) == (IOReturn)0xe00002c7);
    }
    assert(unsupported == 10);
    assert(client->externalMethod(7, nullptr) == (IOReturn)0xe00002bc);
    assert(client->clientClose() == kIOReturnSuccess && static_cast<IOUserClient *>(client)->terminated);
    assert(calls > 10);
    client->release();
    delete device;
    AppleKeyStore failed;
    failed.baseOK = false;
    assert(!failed.start(nullptr) && !failed.publications && !failed.properties);
    for (unsigned which : {1u, 2u, 3u}) {
        AppleKeyStore propertyFailure;
        propertyFailure.failProperty = which;
        assert(!propertyFailure.start(nullptr));
        assert(!propertyFailure.publications && propertyFailure.stops == 1);
    }
'''))
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(work), "-I", str(root), str(work / "test.cpp"),
                    "-o", str(work / "test")], check=True)
    subprocess.run([str(work / "test")], check=True)
print("PASS: closed boundary by default; state mode answers state, bookkeeping, wrap/unwrap, bag UUID and not-found selectors and rejects every other selector")
