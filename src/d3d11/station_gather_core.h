#pragma once
// The station's lock-free hand-offs (built v14.26, 2026-10-13). Pure: no Windows, no engine, so the rig can drive them from several threads.
//   GatherBuffers<E, kSlots, kCap>: the store callbacks (any game thread) append the entries the tick needs to their own thread's slot, in one of two
//     parities. The tick flips the parity, waits for the writers still inside the old one (a busy count per slot), and visits the old entries in slot
//     order. A full slot, or a thread past the last slot, drops the entry and counts it; the tick then keeps every record of that frame.
//   RigPubTable: the rig verdicts and collections the tick publishes. A callback looks a rig up in the latest publication. The run of vetoes per rig
//     is an atomic counter moved by compare-and-swap, so a rig's veto sequence is the one the locked version gave, one call at a time.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "optimized_station_core.h"
#include "station_lockfree_core.h"

namespace edvr::sealed {

// The rig verdict codes the publication carries (the station's own values: unknown 0, mixed 1, outside 2).
constexpr uint8_t kRigVerdictUnknown = 0;
constexpr uint8_t kRigVerdictMixed = 1;
constexpr uint8_t kRigVerdictOutside = 2;

template <typename E, size_t kSlots, size_t kCap>
class GatherBuffers {
public:
    GatherBuffers() noexcept : id_(nextId().fetch_add(1, std::memory_order_relaxed)) {}
    GatherBuffers(const GatherBuffers&) = delete;
    GatherBuffers& operator=(const GatherBuffers&) = delete;

    // Any thread. False when this thread has no slot or its slot is full: the entry is lost and counted.
    bool push(const E& e) noexcept {
        Slot* s = slotOfThisThread();
        if (s == nullptr) {
            lost_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        s->busy.fetch_add(1, std::memory_order_seq_cst);
        const uint32_t p = parity_.load(std::memory_order_seq_cst) & 1u;
        const uint32_t n = s->count[p].load(std::memory_order_relaxed);
        bool ok = false;
        if (n < kCap) {
            s->items[p][n] = e;
            s->count[p].store(n + 1, std::memory_order_relaxed);
            ok = true;
        } else {
            lost_.fetch_add(1, std::memory_order_relaxed);
        }
        s->busy.fetch_sub(1, std::memory_order_release);
        return ok;
    }

    // Any thread: the entries lost since the last drain.
    uint64_t lostSinceDrain() const noexcept { return lost_.load(std::memory_order_relaxed); }

    // The tick only (one thread). Flips the parity, waits for the writers still in the old parity, visits the old entries in slot order, and clears
    // them. Returns the entries lost since the previous drain.
    template <typename F>
    uint64_t drain(F&& visit) {
        const uint32_t old = parity_.fetch_add(1, std::memory_order_seq_cst) & 1u;
        for (size_t i = 0; i < kSlots; ++i) {
            Slot& s = slots_[i];
            while (s.busy.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
            const uint32_t n = s.count[old].load(std::memory_order_relaxed);
            for (uint32_t k = 0; k < n; ++k) visit(s.items[old][k]);
            s.count[old].store(0, std::memory_order_relaxed);
        }
        return lost_.exchange(0, std::memory_order_relaxed);
    }

private:
    struct alignas(64) Slot {
        std::atomic<uint32_t> busy{0};
        std::atomic<uint32_t> count[2] = {};
        E items[2][kCap] = {};
    };

    static std::atomic<uint64_t>& nextId() noexcept {
        static std::atomic<uint64_t> next{1};
        return next;
    }

    // A thread claims a slot the first time it appends to this buffer. A thread that alternates between buffers claims again on each switch.
    Slot* slotOfThisThread() noexcept {
        thread_local uint64_t tlsOwner = 0;
        thread_local uint32_t tlsSlot = 0;
        if (tlsOwner != id_) {
            tlsOwner = id_;
            tlsSlot = claimed_.fetch_add(1, std::memory_order_relaxed);
        }
        return tlsSlot < kSlots ? &slots_[tlsSlot] : nullptr;
    }

    const uint64_t id_;
    std::atomic<uint32_t> parity_{0};
    std::atomic<uint32_t> claimed_{0};
    std::atomic<uint64_t> lost_{0};
    Slot slots_[kSlots];
};

// The veto's outcome for one call against a rig's verdict.
enum class RigOutcome : uint8_t { kUnknown, kMixed, kVetoed, kRefresh };

// An outside rig is vetoed kRigRefreshN calls in a row, then let through once (the refresh) and the run starts again. A mixed or unknown rig resets the
// run. The run moves by compare-and-swap, so concurrent calls count exactly as serial calls would.
inline RigOutcome rigOutcomeOf(uint8_t verdict, std::atomic<uint32_t>& run) noexcept {
    if (verdict == kRigVerdictOutside) {
        uint32_t cur = run.load(std::memory_order_relaxed);
        for (;;) {
            if (cur >= kRigRefreshN) {
                if (run.compare_exchange_weak(cur, 0, std::memory_order_relaxed)) return RigOutcome::kRefresh;
            } else if (run.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed)) {
                return RigOutcome::kVetoed;
            }
        }
    }
    run.store(0, std::memory_order_relaxed);
    return verdict == kRigVerdictMixed ? RigOutcome::kMixed : RigOutcome::kUnknown;
}

// The rig verdicts of one publication. The tick (one thread) begins a publication and inserts each rig it publishes. A callback looks a rig up in the
// latest publication. A rig the publication does not hold forwards, as a rig the tick never bounded did. Each cell's verdict and collection are one
// seqlock payload (four words), so a lookup never pairs a verdict with another publication's collection.
class RigPubTable {
public:
    static constexpr size_t kCells = 4096;

    // The tick only: starts a publication. The cells of earlier publications are free.
    void begin() noexcept { gen_.store(gen_.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst); }

    // The tick only: publishes one rig's verdict and collection in the current publication. False when the bounded probe finds no cell (the rig then
    // forwards). The cell's run of vetoes starts at zero.
    bool insert(uintptr_t rig, size_t home, uint8_t verdict, uintptr_t coll, size_t* indexOut) noexcept {
        const uint32_t gen = gen_.load(std::memory_order_relaxed);
        const uint32_t w[4] = {verdict, static_cast<uint32_t>(static_cast<uint64_t>(coll) & 0xFFFFFFFFu),
                               static_cast<uint32_t>(static_cast<uint64_t>(coll) >> 32), 0u};
        size_t idx = 0;
        if (!seqTableInsertWords(cells_, kCells, home, rig, gen, w, nullptr, &idx)) return false;
        run_[idx].store(0, std::memory_order_relaxed);
        if (indexOut != nullptr) *indexOut = idx;
        return true;
    }

    // Any thread: the rig's verdict and collection in the latest publication, and its cell index. False: not published.
    bool lookup(uintptr_t rig, size_t home, uint8_t* verdict, uintptr_t* coll, size_t* index) const noexcept {
        uint32_t w[4] = {};
        size_t idx = 0;
        if (!seqTableFindWords(cells_, kCells, home, rig, gen_.load(std::memory_order_seq_cst), w, nullptr, &idx)) return false;
        *verdict = static_cast<uint8_t>(w[0]);
        *coll = static_cast<uintptr_t>(static_cast<uint64_t>(w[1]) | (static_cast<uint64_t>(w[2]) << 32));
        *index = idx;
        return true;
    }

    // Any thread: the veto decision for a found rig; records the stamp of its last call (the tally's "matched").
    RigOutcome decide(size_t idx, uint8_t verdict, uint32_t stamp) noexcept {
        evalStamp_[idx].store(stamp, std::memory_order_relaxed);
        return rigOutcomeOf(verdict, run_[idx]);
    }

    // Any thread: records the stamp of a call that did not decide (the association moved).
    void touch(size_t idx, uint32_t stamp) noexcept { evalStamp_[idx].store(stamp, std::memory_order_relaxed); }

    // Any thread: true for the first caller of this stamp on this cell (one association request per rig per frame).
    bool askOnce(size_t idx, uint32_t stamp) noexcept { return askStamp_[idx].exchange(stamp, std::memory_order_relaxed) != stamp; }

    // The stamp of the rig's last call in the publication.
    uint32_t evalStampAt(size_t idx) const noexcept { return evalStamp_[idx].load(std::memory_order_relaxed); }

    // The tick only: the rig's run of vetoes starts afresh (its bounds were forgotten).
    void resetRun(size_t idx) noexcept { run_[idx].store(0, std::memory_order_relaxed); }

private:
    SeqCell cells_[kCells];
    std::atomic<uint32_t> gen_{0};
    std::atomic<uint32_t> run_[kCells] = {};
    std::atomic<uint32_t> evalStamp_[kCells] = {};
    std::atomic<uint32_t> askStamp_[kCells] = {};
};

}  // namespace edvr::sealed
