#pragma once
#include <atomic>
#include <cstdint>

namespace edvr { namespace flatdiag {
// Temporary flat-profile isolation controls. Normal mode is exactly mask 0.
// An explicit session also provides an all-on, zero-jitter/reset-history control.
enum Bit : uint32_t { Motion=1, Preparation=2, Composition=4, Backend=8, Session=16 };
inline uint32_t selection(bool session,bool motion,bool preparation,bool composition,bool backend) {
    return session ? Session | (motion?0u:Motion) | (preparation?0u:Preparation) |
        (composition?0u:Composition) | (backend?0u:Backend) : 0u;
}
inline std::atomic<uint32_t> mask{0}, epoch{1};
inline bool active() { return (mask.load(std::memory_order_relaxed)&Session)!=0; }
inline bool enabled(Bit b) { return (mask.load(std::memory_order_relaxed)&b)==0; }
// Called only at the immediate-context frame boundary. No mid-draw switch.
inline bool publish(uint32_t next) {
    if(mask.load(std::memory_order_relaxed)==next)return false;
    epoch.fetch_add(1,std::memory_order_relaxed);
    mask.store(next,std::memory_order_release);return true;
}
inline bool foregroundEnabled() { return enabled(Motion) && enabled(Composition); }
enum Work : unsigned { Resolve, Prepared, Evaluated, Spatial, Producer, Coverage, Domain, Replay, Foreground, Finished, Neutral, WorkCount };
// The flat owner thread owns these counters, as it owns all immediate-context work.
inline thread_local uint64_t counts[WorkCount]{};
inline void note(Work w) { if(active())++counts[w]; }
inline void clearCounts() { for(auto& n:counts)n=0; }
}} // edvr::flatdiag
