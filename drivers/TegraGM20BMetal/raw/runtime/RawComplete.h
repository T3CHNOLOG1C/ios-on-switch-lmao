// SPDX-License-Identifier: MIT
// The raw device's completion of the states its pack lacks (RawMetal.mm), of any library's functions: each asked once per
// process and per build, off the asker's thread. A state is first looked up in the record cache under the process's shader cache directory, else probed in
// process on the m2v device (RawProbe.h), its record kept, and handed to the raw device; until then, and for a state it refuses,
// m2v draws it.
#pragma once
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "raw_cache.h"
#include "raw_metallib.h"

namespace raw {

class Device;
class Probe;
struct Pipeline;

class Completion {
public:
    // `cacheDir`: the process's own shader cache directory (null or empty: records kept in memory only)
    Completion(Device &device, id<MTLDevice> m2v, const char *cacheDir, uint64_t stack);
    ~Completion();

    struct Ask {
        id<MTLRenderPipelineState> twin;
        MTLRenderPipelineDescriptor *descriptor;  // the twin's: its functions are m2v's, whose metallibs name their AIR
        std::string key;                          // the pack key
        bool caTransform = false;                 // Pipeline::caTransform
        // its functions are not a framework's default library's: no pack names the key, so the record is kept under the
        // key and both functions' AIR (two apps' alike-named functions are not one state)
        bool own = false;
    };

    // The state's pipeline handed to `ready` once made (on the compile queue; at once if it was before), or null once the raw
    // path will not draw it (refused, its probe failed past its retries, or no room for it)
    void ask(Ask a, std::function<void(Pipeline *)> ready);

    Compiler::Stats stats() const;

private:
    Device &device_;
    id<MTLDevice> m2v_;
    RecordCache cache_;
    std::unique_ptr<Compiler> compiler_;
    std::unique_ptr<Probe> probe_;
    uint64_t stack_;
    dispatch_queue_t queue_;
    std::mutex lock_;
    std::map<uint64_t, Ask> asked_;  // by CacheKey::name, what make() probes

    bool keyOf(Ask &a, CacheKey &k);
    Compiler::Outcome make(const CacheKey &k, RuntimeRecord &record, std::string &why);
};

}  // namespace raw
