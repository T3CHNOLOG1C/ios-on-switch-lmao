// Host stand-ins for what drivers/SwitchWLAN/SwitchWLANMsgbuf.cpp takes from the kernel (IOLib, the DMA area's
// IOBufferMemoryDescriptor, panic), for tests/switch_wlan_msgbuf_harness.cpp. test_switch_wlan_msgbuf.py points
// <IOKit/IOLib.h> and <IOKit/IOBufferMemoryDescriptor.h> here. The DMA area is host memory given a fake 32-bit
// physical address; every register access of the driver's Mmio goes to the harness's counter (SWLAN_MMIO_TRACE).
#ifndef IOSWITCH_TEST_SWITCH_WLAN_MSGBUF_STUBS_H
#define IOSWITCH_TEST_SWITCH_WLAN_MSGBUF_STUBS_H
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using vm_offset_t = uintptr_t;
using vm_size_t = size_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOPhysicalAddress = uint64_t;
using addr64_t = uint64_t;
constexpr IOOptionBits kIODirectionInOut = 3, kIOMemoryPhysicallyContiguous = 0x10, kIOMemoryMapperNone = 0x800;
inline void *kernel_task = nullptr;

// The driver's register accesses: the harness counts them per block (the chip's RAM is the only Mmio Msgbuf uses).
void swlan_test_mmio(const void *mmio, char kind, uint32_t offset);
#define SWLAN_MMIO_TRACE(mmio, kind, offset) swlan_test_mmio(mmio, kind, offset)

inline std::vector<std::string> gLog;
__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    std::string text = line;
    if (!text.empty() && text.back() == '\n') text.pop_back();
    gLog.push_back(text);
}
struct Panic : std::runtime_error {
    using std::runtime_error::runtime_error;
};
__attribute__((format(printf, 1, 2), noreturn)) inline void panic(const char *format, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw Panic(line);
}
inline uint64_t gDelayedUs = 0;
inline void IODelay(unsigned us)
{
    gDelayedUs += us;
}
inline void IOSleep(unsigned ms)
{
    gDelayedUs += ms * 1000ull;
}
inline void bzero(void *p, size_t n)
{
    memset(p, 0, n);
}
inline void *IOMallocZeroData(size_t n)
{
    return calloc(1, n);
}
inline void IOFree(void *p, size_t)
{
    free(p);
}
extern "C" inline vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t)
{
    return pa;
}

// Allocations of at least this many bytes fail (the large layout's fallback); 0: none fail. The last area handed out,
// for the harness's firmware model to reach by physical address.
inline size_t gFailAllocationsFrom = 0;
inline uint8_t *gAreaBytes = nullptr;
inline size_t gAreaLength = 0;
// The DMA area's fake physical address: below 4 GiB, page aligned.
constexpr uint64_t kFakeAreaPA = 0x9A000000;

class IOBufferMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(void *, IOOptionBits, size_t length, uint64_t)
    {
        if (gFailAllocationsFrom && length >= gFailAllocationsFrom) return nullptr;
        auto *m = new IOBufferMemoryDescriptor;
        m->length_ = length;
        if (posix_memalign(&m->bytes_, 16384, length)) {
            delete m;
            return nullptr;
        }
        gAreaBytes = static_cast<uint8_t *>(m->bytes_);
        gAreaLength = length;
        return m;
    }
    uint64_t getPhysicalSegment(uint64_t offset, IOByteCount *segment, IOOptionBits)
    {
        if (segment) *segment = length_ - offset;
        return kFakeAreaPA + offset;
    }
    void *getBytesNoCopy() { return bytes_; }
    void release()
    {
        free(bytes_);
        delete this;
    }

private:
    void *bytes_ = nullptr;
    size_t length_ = 0;
};
#define OSSafeReleaseNULL(p)   \
    do {                       \
        if (p) (p)->release(); \
        (p) = nullptr;         \
    } while (0)

#endif
