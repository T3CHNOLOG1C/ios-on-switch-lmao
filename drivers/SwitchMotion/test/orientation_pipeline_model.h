// SPDX-License-Identifier: BSD-2-Clause-Patent
// What 21H461 does with the device orientation service's reports, from its binaries (kernelcache.release.ipad7
// IOHIDFamily, the dyld shared cache's CoreMotion; addresses are theirs, unslid):
//   * IOHIDFamily: the report descriptor's Input elements (HID 1.11 short items; Var only); per report, each element's
//     value, the value it had in the report before, and the report's time once its value changed.
//   * IOHIDEventDriver::handleInterruptReport (0xfffffff005af71f4): the child vendor phase first (handleVendorMessageReport
//     0xfffffff005af73cc, phase 0: every element whose parent collection is AppleVendor 0xff00 / Message 0x23, Physical
//     or Application, and whose report ID matches, becomes vendorDefinedEvent(time, page, usage, 0, its bytes), held
//     until the next dispatch), then handleOrientationReport (0xfffffff005af91b4: an AppleVendorMotion 0xff0c usage
//     0x64..0x6a element of this report whose time is the report's, previous value 0 and value not 0 makes an orientation
//     event), then the primary phase, which dispatches children still held as events of their own. dispatchEvent
//     (0xfffffff005aface8) appends the held children to the event it dispatches.
//   * CoreMotion CLOrientationNotifier::onIohidEvent (0x1941e155c): type 10 only ("Received wrong event type"), usage
//     0x64..0x6a; the sample time from child 0 (0x194241298: no children, "Event has no children", 0; else page 0xff15,
//     usage 2, 8 data bytes, their u64); 0x1941e5564: dropped unless above the last time taken ("Skipping stale
//     orientation (N @ T)"), else taken and delivered as orientation usage - 0x64.
#ifndef SWITCH_TEST_ORIENTATION_PIPELINE_MODEL_H
#define SWITCH_TEST_ORIENTATION_PIPELINE_MODEL_H
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace orientation_model {

struct Collection {
    uint32_t page = 0;
    uint32_t usage = 0;
    uint8_t type = 0;  // 0 Physical, 1 Application
};

struct Element {
    uint32_t page = 0;
    uint32_t usage = 0;
    uint8_t reportID = 0;
    unsigned bit = 0;  // after the report ID byte
    unsigned size = 0;
    Collection parent;
    uint64_t value = 0;
    uint64_t previous = 0;
    uint64_t time = 0;
};

struct Descriptor {
    std::vector<Element> elements;
    std::vector<Collection> topLevel;
    unsigned inputBits[256] = {};  // per report ID
};

[[noreturn]] inline void fail(const std::string &why)
{
    throw std::runtime_error("HID descriptor: " + why);
}

class Parser {
public:
    Descriptor parse(const uint8_t *d, size_t n)
    {
        for (size_t i = 0; i < n;) {
            const uint8_t prefix = d[i++];
            if (prefix == 0xfe) {
                fail("long item");
            }
            const unsigned size = (prefix & 3u) == 3u ? 4u : (prefix & 3u);
            if (i + size > n) {
                fail("item past the end");
            }
            uint32_t data = 0;
            for (unsigned b = 0; b < size; ++b) {
                data |= static_cast<uint32_t>(d[i + b]) << (8u * b);
            }
            i += size;
            item((prefix >> 2u) & 3u, prefix >> 4u, size, data);
        }
        if (!stack.empty()) {
            fail("open collection");
        }
        return out;
    }

private:
    Descriptor out;
    uint32_t page = 0;
    uint32_t reportSize = 0;
    uint32_t reportCount = 0;
    uint8_t reportID = 0;
    std::vector<uint32_t> usages;
    uint32_t usageMin = 0;
    uint32_t usageMax = 0;
    bool range = false;
    std::vector<Collection> stack;

    void item(unsigned type, unsigned tag, unsigned size, uint32_t data)
    {
        if (type == 1) {
            global(tag, data);
        } else if (type == 2) {
            local(tag, size == 4 ? data : (page << 16u | data));
        } else if (type == 0) {
            mainItem(tag, data);
            usages.clear();
            range = false;
        }
    }

    void global(unsigned tag, uint32_t data)
    {
        switch (tag) {
        case 0: page = data; break;
        case 7: reportSize = data; break;
        case 8: reportID = static_cast<uint8_t>(data); break;
        case 9: reportCount = data; break;
        case 10:
        case 11: fail("push/pop");
        default: break;
        }
    }

    void local(unsigned tag, uint32_t usage)
    {
        if (tag == 0) {
            usages.push_back(usage);
        } else if (tag == 1) {
            usageMin = usage;
            range = true;
        } else if (tag == 2) {
            usageMax = usage;
            range = true;
        }
    }

    void mainItem(unsigned tag, uint32_t data)
    {
        if (tag == 0xa) {
            const uint32_t usage = usages.empty() ? 0 : usages.front();
            const Collection c{usage >> 16u, usage & 0xffffu, static_cast<uint8_t>(data)};
            if (stack.empty()) {
                out.topLevel.push_back(c);
            }
            stack.push_back(c);
        } else if (tag == 0xc) {
            if (stack.empty()) {
                fail("end collection without collection");
            }
            stack.pop_back();
        } else if (tag == 8) {
            input(data);
        } else if (tag == 9 || tag == 0xb) {
            fail("output or feature item");
        }
    }

    uint32_t usageAt(uint32_t k) const
    {
        if (range) {
            return usageMin + k <= usageMax ? usageMin + k : usageMax;
        }
        if (usages.empty()) {
            fail("input without a usage");
        }
        return usages[k < usages.size() ? k : usages.size() - 1];
    }

    void input(uint32_t flags)
    {
        if (stack.empty()) {
            fail("input outside a collection");
        }
        unsigned &bits = out.inputBits[reportID];
        const bool constant = (flags & 1u) != 0;
        if (!constant && (flags & 2u) == 0) {
            fail("array input");
        }
        if (!constant && (reportSize == 0 || reportSize > 64)) {
            fail("element size");
        }
        for (uint32_t k = 0; !constant && k < reportCount; ++k) {
            const uint32_t usage = usageAt(k);
            Element e;
            e.page = usage >> 16u;
            e.usage = usage & 0xffffu;
            e.reportID = reportID;
            e.bit = bits + k * reportSize;
            e.size = reportSize;
            e.parent = stack.back();
            out.elements.push_back(e);
        }
        bits += reportSize * reportCount;
    }
};

inline Descriptor parse(const uint8_t *d, size_t n)
{
    return Parser().parse(d, n);
}

struct Event {
    uint32_t type = 0;  // 10 orientation, 1 vendor-defined
    uint64_t time = 0;
    uint32_t usage = 0;  // orientation: CMUsage; vendor-defined: usage, with `page`
    uint32_t page = 0;
    std::vector<uint8_t> data;
    std::vector<Event> children;
};

inline bool isVendorMessageChild(const Element &e)
{
    return e.parent.page == 0xff00 && e.parent.usage == 0x23 && (e.parent.type == 0 || e.parent.type == 1);
}

inline bool isOrientation(const Element &e)
{
    return e.page == 0xff0c && e.usage >= 0x64 && e.usage <= 0x6a;
}

struct EventDriver {
    Descriptor descriptor;
    std::vector<Event> dispatched;
    std::vector<Event> held;

    explicit EventDriver(const Descriptor &d) : descriptor(d) {}

    void dispatch(Event event)
    {
        for (const Event &child : held) {
            event.children.push_back(child);
        }
        held.clear();
        dispatched.push_back(event);
    }

    void report(const uint8_t *bytes, size_t length, uint64_t time)
    {
        if (length == 0) {
            fail("empty report");
        }
        const uint8_t id = bytes[0];
        if ((length - 1) * 8 < descriptor.inputBits[id]) {
            fail("report shorter than its descriptor");
        }
        for (Element &e : descriptor.elements) {
            if (e.reportID == id) {
                update(e, bytes + 1, time);
            }
        }
        for (const Element &e : descriptor.elements) {
            if (e.reportID == id && isVendorMessageChild(e)) {
                held.push_back(vendorEvent(e, time));
            }
        }
        for (const Element &e : descriptor.elements) {
            if (e.reportID == id && isOrientation(e) && e.time == time && e.previous == 0 && e.value != 0) {
                Event event;
                event.type = 10;
                event.time = time;
                event.usage = e.usage;
                dispatch(event);
            }
        }
        std::vector<Event> leftover;
        leftover.swap(held);
        for (const Event &child : leftover) {
            dispatched.push_back(child);
        }
    }

private:
    static void update(Element &e, const uint8_t *data, uint64_t time)
    {
        uint64_t v = 0;
        for (unsigned b = 0; b < e.size; ++b) {
            const unsigned at = e.bit + b;
            v |= static_cast<uint64_t>((data[at / 8] >> (at % 8)) & 1u) << b;
        }
        e.previous = e.value;
        if (v != e.value) {
            e.time = time;
        }
        e.value = v;
    }

    static Event vendorEvent(const Element &e, uint64_t time)
    {
        Event child;
        child.type = 1;
        child.time = time;
        child.page = e.page;
        child.usage = e.usage;
        for (unsigned b = 0; b < (e.size + 7) / 8; ++b) {
            child.data.push_back(static_cast<uint8_t>(e.value >> (8 * b)));
        }
        return child;
    }
};

struct Notifier {
    uint64_t last = 0;
    std::vector<uint8_t> delivered;
    std::vector<uint64_t> times;
    unsigned wrongType = 0;
    unsigned noChildren = 0;
    unsigned badChild = 0;
    unsigned stale = 0;
    std::vector<std::string> log;

    void onEvent(const Event &event)
    {
        if (event.type != 10) {
            ++wrongType;
            log.push_back("Received wrong event type " + std::to_string(event.type));
            return;
        }
        if (event.usage < 0x64 || event.usage > 0x6a) {
            log.push_back("Received invalid orientation " + std::to_string(event.usage));
            return;
        }
        const uint64_t time = sampleTime(event);
        const unsigned orientation = event.usage - 0x64;
        if (last >= time) {
            ++stale;
            log.push_back("Skipping stale orientation (" + std::to_string(orientation) + " @ " + std::to_string(time) +
                          ")");
            return;
        }
        last = time;
        delivered.push_back(static_cast<uint8_t>(orientation));
        times.push_back(time);
    }

private:
    uint64_t sampleTime(const Event &event)
    {
        if (event.children.empty()) {
            ++noChildren;
            log.emplace_back("Event has no children");
            return 0;
        }
        const Event &child = event.children.front();
        if (child.usage != 2 || child.page != 0xff15 || child.data.size() != 8) {
            ++badChild;
            return 0;
        }
        uint64_t time = 0;
        memcpy(&time, child.data.data(), sizeof(time));
        return time;
    }
};

}  // namespace orientation_model
#endif
