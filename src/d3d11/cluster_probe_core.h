#pragma once
// The fragment-culling cluster builder's probe, pure half (built v14.30, 2026-10-13; log only, no behaviour). No Windows, no engine: the rig drives it.
//
// The function is EliteDangerous64.exe (build 332841) RVA 0x58B150 (owner of the .pdata chunks 0x58B150, 0x58B474, 0x58B530, 0x58BA2F and 0x58BDD7). Its
// arguments: rcx the cluster context (this), rdx, r8 and r9 pointers (r9 the view block: the inverse-projection rows at +0x1B0..+0x1F0 and the depth
// range at +0x234 and +0x238), then two 32-bit stack arguments, the target's width and height. The game already memoizes it: this+0x00..+0x77 holds the
// key of the last build (four 16-byte rows, two floats at +0x40 and +0x44, six 64-bit values at +0x48..+0x70: the grid, the slice count, the width,
// the height, the tile), and a call whose inputs equal that key returns at once. After a call this block is the key of THAT call, so the key before
// the call against the key after it says whether the game's own memo hit. The tables are two vectors of 32-byte elements (size at +0x90 and +0xB0,
// data at +0x98 and +0xB8): the cluster AABBs and the depth-slice AABBs.
// The probe answers: does the memo hit, and if not, which inputs changed, and is the output a function of the key (the same key, the same table).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::clusterprobe {

constexpr size_t kKeyBytes = 0x78;
constexpr size_t kRowBytes = 0x40;   // four 16-byte rows
constexpr size_t kSetCap = 256;      // distinct hashes kept per window
constexpr size_t kPairCap = 64;      // key -> output pairs kept per window

// A fast byte hash (eight bytes at a time). Not cryptographic; equality of the 64-bit result stands for equality of the bytes.
inline uint64_t hashBytes(const void* data, size_t n) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n) * 0xC2B2AE3D27D4EB4Full);
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
        p += 8;
        n -= 8;
    }
    uint64_t t = 0;
    if (n > 0) std::memcpy(&t, p, n);
    h = (h ^ t) * 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 29;
    return h;
}

// Which parts of two keys differ: the four rows (a bit each), the two floats, the six 64-bit values.
struct KeyDiff {
    uint8_t rows = 0;
    bool nearFar = false;
    bool dims = false;
    bool any() const noexcept { return rows != 0 || nearFar || dims; }
};

inline KeyDiff diffKeys(const uint8_t* a, const uint8_t* b) noexcept {
    KeyDiff d;
    for (int r = 0; r < 4; ++r) {
        if (std::memcmp(a + r * 16, b + r * 16, 16) != 0) d.rows = static_cast<uint8_t>(d.rows | (1u << r));
    }
    d.nearFar = std::memcmp(a + 0x40, b + 0x40, 8) != 0;
    d.dims = std::memcmp(a + 0x48, b + 0x48, kKeyBytes - 0x48) != 0;
    return d;
}

// One observed call.
struct CallRecord {
    uint64_t keyBefore = 0;   // hash of the key block at entry
    uint64_t keyAfter = 0;    // hash of the key block at exit (the key of this call)
    uint64_t out = 0;         // hash of the tables at exit (bounded)
    bool outHashed = false;
    KeyDiff vsPrev;           // this call's key against the previous call's
    bool havePrev = false;
    double callMs = 0.0;      // the original function's time
    double probeMs = 0.0;     // the probe's own time
    uint64_t tableBytes = 0;  // both tables' bytes
    uint64_t hashedBytes = 0; // the bytes the output hash covered
};

class Stats {
public:
    // One call. The memo hit: the key did not change across the call.
    void record(const CallRecord& c) noexcept {
        ++calls_;
        ++thisFrame_;
        const bool hit = c.keyBefore == c.keyAfter;
        if (hit) {
            ++hits_;
            sumHitMs_ += c.callMs;
        } else {
            ++misses_;
            sumMissMs_ += c.callMs;
            if (c.havePrev) {
                for (int r = 0; r < 4; ++r) {
                    if ((c.vsPrev.rows >> r) & 1u) ++missRow_[r];
                }
                if (c.vsPrev.nearFar) ++missNearFar_;
                if (c.vsPrev.dims) ++missDims_;
                if (!c.vsPrev.any()) ++missSameAsPrev_;
            }
            if (c.keyAfter == back2_ && calls_ > 2) ++missEqualTwoBack_;
        }
        back2_ = back1_;
        back1_ = c.keyAfter;
        sumProbeMs_ += c.probeMs;
        if (c.tableBytes > maxTableBytes_) maxTableBytes_ = c.tableBytes;
        if (c.hashedBytes > maxHashedBytes_) maxHashedBytes_ = c.hashedBytes;
        addDistinct(keys_, nKeys_, overflowKeys_, c.keyAfter);
        if (!c.outHashed) return;
        addDistinct(outs_, nOuts_, overflowOuts_, c.out);
        // The output as a function of the key: the same key with another output is a table the key does not determine.
        for (size_t i = 0; i < nPairs_; ++i) {
            if (pairKey_[i] == c.keyAfter) {
                ++sameKeyChecks_;
                if (pairOut_[i] != c.out) ++sameKeyMismatches_;
                return;
            }
        }
        if (nPairs_ < kPairCap) {
            pairKey_[nPairs_] = c.keyAfter;
            pairOut_[nPairs_] = c.out;
            ++nPairs_;
        }
    }

    // The frame ended: how many calls it held.
    void endFrame() noexcept {
        ++frames_;
        if (thisFrame_ >= 3) ++framesWith3_;
        else if (thisFrame_ == 2) ++framesWith2_;
        else if (thisFrame_ == 1) ++framesWith1_;
        else ++framesWith0_;
        thisFrame_ = 0;
    }

    // The 30 s line. `seconds` is the window's length. Returns the length written.
    size_t line(char* out, size_t cap, uint64_t seconds) const noexcept {
        const double calls = calls_ > 0 ? static_cast<double>(calls_) : 1.0;
        const double frames = frames_ > 0 ? static_cast<double>(frames_) : 1.0;
        const int n = std::snprintf(
            out, cap,
            "optimized station: cluster probe last %llu s (fragment-culling cluster builder, EliteDangerous64.exe+0x58B150): %llu calls in %llu frames "
            "(%.2f a frame; frames with 0/1/2/3+ calls %llu/%llu/%llu/%llu); the game's own memo hit %llu, missed %llu (%.1f%% hit); mean ms per call %.3f "
            "on a hit, %.3f on a miss. Misses against the previous call: rows 0-3 differ %llu/%llu/%llu/%llu, depth range differs %llu, grid or size "
            "differs %llu, nothing differs %llu; equal to the key two calls back %llu. Distinct keys %llu%s, distinct tables %llu%s. Same key again: %llu "
            "checks, %llu with another table. Tables up to %llu bytes, %llu hashed. The probe's own cost %.4f ms a call.",
            static_cast<unsigned long long>(seconds), static_cast<unsigned long long>(calls_), static_cast<unsigned long long>(frames_),
            static_cast<double>(calls_) / frames, static_cast<unsigned long long>(framesWith0_), static_cast<unsigned long long>(framesWith1_),
            static_cast<unsigned long long>(framesWith2_), static_cast<unsigned long long>(framesWith3_), static_cast<unsigned long long>(hits_),
            static_cast<unsigned long long>(misses_), 100.0 * static_cast<double>(hits_) / calls, hits_ > 0 ? sumHitMs_ / static_cast<double>(hits_) : 0.0,
            misses_ > 0 ? sumMissMs_ / static_cast<double>(misses_) : 0.0, static_cast<unsigned long long>(missRow_[0]),
            static_cast<unsigned long long>(missRow_[1]), static_cast<unsigned long long>(missRow_[2]), static_cast<unsigned long long>(missRow_[3]),
            static_cast<unsigned long long>(missNearFar_), static_cast<unsigned long long>(missDims_), static_cast<unsigned long long>(missSameAsPrev_),
            static_cast<unsigned long long>(missEqualTwoBack_), static_cast<unsigned long long>(nKeys_), overflowKeys_ ? "+" : "",
            static_cast<unsigned long long>(nOuts_), overflowOuts_ ? "+" : "", static_cast<unsigned long long>(sameKeyChecks_),
            static_cast<unsigned long long>(sameKeyMismatches_), static_cast<unsigned long long>(maxTableBytes_),
            static_cast<unsigned long long>(maxHashedBytes_), sumProbeMs_ / calls);
        return n < 0 ? 0 : static_cast<size_t>(n);
    }

    uint64_t calls() const noexcept { return calls_; }
    uint64_t hits() const noexcept { return hits_; }
    uint64_t misses() const noexcept { return misses_; }
    uint64_t distinctKeys() const noexcept { return nKeys_; }
    uint64_t distinctOutputs() const noexcept { return nOuts_; }
    uint64_t sameKeyChecks() const noexcept { return sameKeyChecks_; }
    uint64_t sameKeyMismatches() const noexcept { return sameKeyMismatches_; }
    uint64_t missEqualTwoBack() const noexcept { return missEqualTwoBack_; }
    uint64_t missRow(int r) const noexcept { return missRow_[r]; }
    uint64_t missNearFar() const noexcept { return missNearFar_; }
    uint64_t missDims() const noexcept { return missDims_; }
    uint64_t framesWith2() const noexcept { return framesWith2_; }

    void clear() noexcept { *this = Stats{}; }

private:
    static void addDistinct(uint64_t* set, uint64_t& n, bool& overflow, uint64_t v) noexcept {
        for (uint64_t i = 0; i < n; ++i) {
            if (set[i] == v) return;
        }
        if (n < kSetCap) set[n++] = v;
        else overflow = true;
    }

    uint64_t calls_ = 0, hits_ = 0, misses_ = 0, frames_ = 0, thisFrame_ = 0;
    uint64_t framesWith0_ = 0, framesWith1_ = 0, framesWith2_ = 0, framesWith3_ = 0;
    uint64_t missRow_[4] = {};
    uint64_t missNearFar_ = 0, missDims_ = 0, missSameAsPrev_ = 0, missEqualTwoBack_ = 0;
    uint64_t back1_ = 0, back2_ = 0;
    double sumHitMs_ = 0.0, sumMissMs_ = 0.0, sumProbeMs_ = 0.0;
    uint64_t maxTableBytes_ = 0, maxHashedBytes_ = 0;
    uint64_t keys_[kSetCap] = {}, outs_[kSetCap] = {};
    uint64_t nKeys_ = 0, nOuts_ = 0;
    bool overflowKeys_ = false, overflowOuts_ = false;
    uint64_t pairKey_[kPairCap] = {}, pairOut_[kPairCap] = {};
    size_t nPairs_ = 0;
    uint64_t sameKeyChecks_ = 0, sameKeyMismatches_ = 0;
};

// The 16 bytes the detour pins (the function's entry, build 332841): mov rax,rsp; push rbp; push rbx; push rsi; push rdi; push r12; push r14; push r15;
// lea rbp,[rax-0x4D8]. The first 5 bytes are the instructions the detour moves.
inline constexpr uint8_t kClusterPrologue[16] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8};
constexpr uintptr_t kClusterRva = 0x58B150u;

}  // namespace edvr::clusterprobe
