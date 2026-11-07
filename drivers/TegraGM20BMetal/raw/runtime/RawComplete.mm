// SPDX-License-Identifier: MIT
// See RawComplete.h.
#import "RawComplete.h"

#include <cstring>
#include "RawProbe.h"
#include "raw_gpu.h"

namespace raw {
namespace {

constexpr uint64_t kCacheCap = 8u << 20;

}  // namespace

Completion::Completion(Device &device, id<MTLDevice> m2v, const char *cacheDir, uint64_t stack)
    : device_(device), m2v_(m2v), stack_(stack),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.complete",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0)))
{
    compiler_ = std::make_unique<Compiler>(&cache_, [this](const CacheKey &k, RuntimeRecord &record, std::string &why) {
        return make(k, record, why);
    });
    const std::string dir = cacheDir ? cacheDir : "";
    // the directory's scan (another build's entries removed) on the queue, before any ask there
    dispatch_async(queue_, ^{
        if (!dir.empty() && !cache_.open(dir, stack_, kCacheCap))
            NSLog(@"TegraGM20BMetal: raw records kept in memory only: %s takes no entry", dir.c_str());
    });
}

Completion::~Completion()
{
    compiler_.reset();
}

// The state's key: the build, both functions' AIR (their metallib's HASH tags) and the pack key. False: a function the raw path
// cannot name the AIR of (not from a metallib file)
bool Completion::keyOf(const Ask &a, CacheKey &k)
{
    auto air = [&](const std::string &library, const std::string &name, uint8_t (&out)[32]) {
        if (library.empty())
            return false;
        auto it = airs_.find(library);
        if (it == airs_.end())
            metallibAirHashes(library, (it = airs_.emplace(library, std::map<std::string, AirHash>()).first)->second);
        const auto f = it->second.find(name);
        if (f == it->second.end())
            return false;
        memcpy(out, f->second.data(), sizeof out);
        return true;
    };
    k.stack = stack_;
    k.key = a.key;
    return air(a.vertexLibrary, a.vertexName, k.vertexAir) && air(a.fragmentLibrary, a.fragmentName, k.fragmentAir);
}

void Completion::ask(Ask a, std::function<void(Pipeline *)> ready)
{
    dispatch_async(queue_, ^{
        CacheKey k;
        if (!keyOf(a, k))
            return;
        {
            std::lock_guard<std::mutex> l(lock_);
            asked_[k.name()] = a;
        }
        const bool caTransform = a.caTransform;
        auto done = [this, k, a, ready, caTransform](const RuntimeRecord *record, const std::string &) {
            std::string why;
            if (record) {
                ready(device_.addRecord(*record, caTransform, why));
                return;
            }
            // a probe that could not run (NVK or the frontend not up to it yet) is asked again, a bounded number of times
            uint64_t us = 0;
            if (compiler_->retryAfter(k, us))
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(us) * 1000), queue_, ^{
                    ask(a, ready);
                });
            else
                ready(nullptr);
        };
        const RuntimeRecord *made = nullptr;
        const Compiler::State state = compiler_->ask(k, done, &made);
        if (state == Compiler::State::Ready && made)
            done(made, std::string());
        else if (state == Compiler::State::Refused || state == Compiler::State::Failed)
            done(nullptr, std::string());
    });
}

Compiler::Outcome Completion::make(const CacheKey &k, RuntimeRecord &record, std::string &why)
{
    Ask a;
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto it = asked_.find(k.name());
        if (it == asked_.end()) {
            why = "asked for no twin";
            return Compiler::Outcome::Failed;
        }
        a = it->second;
    }
    if (!probe_)
        probe_ = std::make_unique<Probe>(m2v_, device_.pack());
    return probe_->make(a.twin, a.descriptor, a.key, record, why);
}

Compiler::Stats Completion::stats() const
{
    return compiler_->stats();
}

}  // namespace raw
