// Host stand-ins for the kernel interfaces drivers/SwitchMotion/SwitchMotion.cpp uses (IOKit, os_log, mach time), for
// tests/switch_motion_harness.cpp. test_switch_motion.py points <IOKit/...> and <os/log.h> here. The work loop runs
// everything at once on the caller's thread; time is a fake clock that IOSleep and IODelay advance.
#ifndef IOSWITCH_TEST_SWITCH_MOTION_STUBS_H
#define IOSWITCH_TEST_SWITCH_MOTION_STUBS_H
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;

// Fake time in nanoseconds; nanoseconds_to_absolutetime is the identity.
inline uint64_t gNow = 0;
inline uint64_t mach_absolute_time() { return gNow; }
inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs) { *abs = ns; }
inline void IODelay(unsigned us) { gNow += us * 1000ull; }
inline void IOSleep(unsigned ms) { gNow += ms * 1000000ull; }

// IOLog and os_log lines, in order, each prefixed with its channel.
inline std::vector<std::string> gLog;
inline void stubLog(const char *channel, const char *format, va_list ap)
{
    char line[1024];
    vsnprintf(line, sizeof(line), format, ap);
    std::string text = std::string(channel) + line;
    if (!text.empty() && text.back() == '\n') text.pop_back();
    gLog.push_back(text);
}
__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    stubLog("IOLog: ", format, ap);
    va_end(ap);
}
__attribute__((format(printf, 1, 2))) inline void stubOsLog(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    stubLog("os_log: ", format, ap);
    va_end(ap);
}
#define OS_LOG_DEFAULT nullptr
#define os_log(log, ...) stubOsLog(__VA_ARGS__)

struct Panic : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] __attribute__((format(printf, 1, 2))) inline void panic(const char *format, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw Panic(line);
}

#define OSDeclareDefaultStructors(cls)
#define OSDefineMetaClassAndStructors(cls, super)
#define OSTypeAlloc(cls) (new cls)
#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
#define OSSafeReleaseNULL(obj) \
    do {                       \
        if (obj) {             \
            (obj)->release();  \
            (obj) = nullptr;   \
        }                      \
    } while (0)

class OSObject {
    int refs = 1;

public:
    virtual ~OSObject() = default;
    virtual void free() {}
    void retain() { ++refs; }
    void release()
    {
        if (--refs == 0) {
            free();
            delete this;
        }
    }
};

class IOService : public OSObject {
public:
    IOService *provider = nullptr;
    bool registered = false;
    std::string imu;
    virtual bool init() { return true; }
    virtual bool start(IOService *) { return true; }
    void free() override {}
    bool attach(IOService *to)
    {
        provider = to;
        retain();
        return true;
    }
    void detach(IOService *from)
    {
        assert(provider == from);
        provider = nullptr;
        release();
    }
    void registerService() { registered = true; }
    bool setProperty(const char *key, const char *value)
    {
        if (!strcmp(key, "IMU")) imu = value;
        return true;
    }
    static IOService *getResourceService()
    {
        static IOService *resources = new IOService;
        return resources;
    }
};

class IOEventSource : public OSObject {
public:
    OSObject *owner = nullptr;
    bool enabled = true;
    void enable() { enabled = true; }
    void disable() { enabled = false; }
};

class IOWorkLoop : public OSObject {
public:
    typedef IOReturn (*Action)(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOWorkLoop *workLoop() { return new IOWorkLoop; }
    IOReturn addEventSource(IOEventSource *) { return kIOReturnSuccess; }
    IOReturn removeEventSource(IOEventSource *) { return kIOReturnSuccess; }
    IOReturn runAction(Action action, OSObject *target, void *a0 = nullptr, void *a1 = nullptr, void *a2 = nullptr,
                       void *a3 = nullptr)
    {
        return action(target, a0, a1, a2, a3);
    }
};

class IOInterruptEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *owner, IOInterruptEventSource *sender, int count);
    Action action = nullptr;
    IOService *provider = nullptr;
    static inline std::vector<IOInterruptEventSource *> all;
    static IOInterruptEventSource *interruptEventSource(OSObject *owner, Action action, IOService *provider = nullptr,
                                                       int = 0)
    {
        auto *source = new IOInterruptEventSource;
        source->owner = owner;
        source->action = action;
        source->provider = provider;
        all.push_back(source);
        return source;
    }
    // The work loop runs the action at once (a disabled source's interrupt waits; none of these tests needs that).
    void interruptOccurred(void *, IOService *, int)
    {
        if (enabled) action(owner, this, 1);
    }
};

class IOTimerEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *owner, IOTimerEventSource *sender);
    Action action = nullptr;
    bool armed = false;
    uint32_t ms = 0;
    static inline IOTimerEventSource *last = nullptr;
    static IOTimerEventSource *timerEventSource(OSObject *owner, Action action)
    {
        auto *source = new IOTimerEventSource;
        source->owner = owner;
        source->action = action;
        last = source;
        return source;
    }
    IOReturn setTimeoutMS(uint32_t timeout)
    {
        armed = true;
        ms = timeout;
        return kIOReturnSuccess;
    }
    void cancelTimeout() { armed = false; }
    void fire()
    {
        assert(armed);
        armed = false;
        action(owner, this);
    }
};
#endif
