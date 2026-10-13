#pragma once
// The lock-free pieces of the station's per-record path (built v14.25, 2026-10-13). Pure: no Windows, no engine, so the rig can drive them from
// several threads. Two primitives:
//   SnapshotRing<T, N>: the tick (the single publisher, under the station's state lock) publishes an immutable value; callers on any thread pin the
//     published slot, read it, and unpin. A slot is rewritten only when no caller holds it (a per-slot reference count), so nothing is freed or torn
//     under a reader, and no reader takes a lock.
//   SeqCell: one cached record (its record address, its generation and four floats) under a seqlock. Writers claim the cell with a compare-and-swap
//     on the sequence word and retry if another writer holds it; readers copy it and check the sequence, so a torn copy is never returned.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "optimized_station_core.h"

namespace edvr::sealed {

template <typename T, size_t N>
class SnapshotRing {
public:
    // The publisher's slot, free for writing: the slot after the published one, once no caller holds it. Single publisher only.
    T& beginWrite() noexcept {
        next_ = (cur_.load(std::memory_order_seq_cst) + 1) % N;
        while (refs_[next_].load(std::memory_order_seq_cst) != 0) {
        }
        return slots_[next_];
    }
    // Makes the slot from beginWrite the published one.
    void publish() noexcept {
        cur_.store(next_, std::memory_order_seq_cst);
        publishes_.fetch_add(1, std::memory_order_relaxed);
    }
    // A caller's view of the published value: pinned until unpin(idx). Never blocks.
    const T* pin(size_t* idx) noexcept {
        for (;;) {
            const size_t i = cur_.load(std::memory_order_seq_cst);
            refs_[i].fetch_add(1, std::memory_order_seq_cst);
            if (cur_.load(std::memory_order_seq_cst) == i) {
                *idx = i;
                return &slots_[i];
            }
            refs_[i].fetch_sub(1, std::memory_order_seq_cst);
        }
    }
    void unpin(size_t idx) noexcept { refs_[idx].fetch_sub(1, std::memory_order_seq_cst); }
    uint64_t publishes() const noexcept { return publishes_.load(std::memory_order_relaxed); }

private:
    T slots_[N] = {};
    std::atomic<size_t> cur_{0};
    size_t next_ = 1;
    std::atomic<uint32_t> refs_[N] = {};
    std::atomic<uint64_t> publishes_{0};
};

// One cached record: the record's address, the frame generation it belongs to, and four floats (centre x, y, z and radius).
struct SeqCell {
    std::atomic<uint32_t> seq{0};   // even: stable; odd: a writer holds the cell
    std::atomic<uintptr_t> rec{0};
    std::atomic<uint32_t> gen{0};
    std::atomic<uint32_t> word[4] = {};
};

inline uint32_t floatBits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
inline float bitsFloat(uint32_t u) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// Claims the cell and writes it. False when another writer holds the cell right now (the caller retries the insert).
inline bool seqCellWrite(SeqCell& c, uintptr_t rec, uint32_t gen, const float v[4]) noexcept {
    uint32_t s = c.seq.load(std::memory_order_acquire);
    if ((s & 1u) != 0) return false;
    if (!c.seq.compare_exchange_strong(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    c.rec.store(rec, std::memory_order_relaxed);
    c.gen.store(gen, std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) c.word[k].store(floatBits(v[k]), std::memory_order_relaxed);
    c.seq.store(s + 2, std::memory_order_release);
    return true;
}

// Copies the cell. False when a writer holds it or wrote it during the copy (the caller treats that as no entry).
inline bool seqCellRead(const SeqCell& c, uintptr_t* rec, uint32_t* gen, float v[4]) noexcept {
    const uint32_t s1 = c.seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0) return false;
    *rec = c.rec.load(std::memory_order_relaxed);
    *gen = c.gen.load(std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) v[k] = bitsFloat(c.word[k].load(std::memory_order_relaxed));
    std::atomic_thread_fence(std::memory_order_acquire);
    return c.seq.load(std::memory_order_relaxed) == s1;
}

// The table: `cap` cells, bounded probing (kTableProbeMax), a generation per frame. A cell of an earlier generation is free. Returns false when the
// bounded probe finds no cell (the caller counts it and runs uncached).
inline bool seqTableInsert(SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, const float v[4], uint32_t* probes) noexcept {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const ProbeResult p = boundedProbe(
            home, cap, kTableProbeMax,
            [&](size_t i) {
                uintptr_t r = 0;
                uint32_t g = 0;
                float tmp[4];
                return seqCellRead(cells[i], &r, &g, tmp) && g == gen && r == rec;
            },
            [&](size_t i) {
                uintptr_t r = 0;
                uint32_t g = 0;
                float tmp[4];
                return seqCellRead(cells[i], &r, &g, tmp) && g != gen;
            });
        if (probes != nullptr) *probes = p.probes;
        if (p.exhausted) return false;
        if (p.found || p.empty) {
            if (seqCellWrite(cells[p.index], rec, gen, v)) return true;
            // another writer holds that cell: try again (the cell may now hold another record, so the probe runs again)
        }
    }
    return false;
}

// The cell for `rec` in this generation, if cached. False when it is not (or a writer is in the way).
inline bool seqTableFind(const SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, float v[4], uint32_t* probes) noexcept {
    const ProbeResult p = boundedProbe(
        home, cap, kTableProbeMax,
        [&](size_t i) {
            uintptr_t r = 0;
            uint32_t g = 0;
            float tmp[4];
            return seqCellRead(cells[i], &r, &g, tmp) && g == gen && r == rec;
        },
        [&](size_t i) {
            uintptr_t r = 0;
            uint32_t g = 0;
            float tmp[4];
            return seqCellRead(cells[i], &r, &g, tmp) && g != gen;
        });
    if (probes != nullptr) *probes = p.probes;
    if (!p.found) return false;
    uintptr_t r = 0;
    uint32_t g = 0;
    return seqCellRead(cells[p.index], &r, &g, v) && r == rec && g == gen;
}

}  // namespace edvr::sealed
