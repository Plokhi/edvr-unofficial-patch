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

// Built v14.29: a caller can also pin with a per-thread hazard cell (pinThread): the cell is the thread's own cache line, so a pin writes no line another
// thread touches. The thread keeps its slot pinned until its next pinThread, so a batch of calls on one published slot costs two loads of the published
// index. The publisher skips a slot that a hazard cell names. A thread past the last cell pins by the shared count (pin / unpin), as before. A ring that
// hazard cells hold needs N above the number of threads that can each hold a different old slot (the station uses 40 for 16 gather slots).
template <typename T, size_t N, size_t kHazards = 48>
class SnapshotRing {
public:
    SnapshotRing() noexcept : id_(nextId().fetch_add(1, std::memory_order_relaxed)) {}
    // The publisher's slot, free for writing: the first slot after the published one that no caller holds (by count or by hazard cell). Single publisher only.
    T& beginWrite() noexcept {
        for (;;) {
            const size_t c = cur_.load(std::memory_order_seq_cst);
            for (size_t k = 1; k < N; ++k) {
                const size_t cand = (c + k) % N;
                if (refs_[cand].load(std::memory_order_seq_cst) == 0 && !hazarded(cand)) {
                    next_ = cand;
                    return slots_[cand];
                }
            }
        }
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

    // The per-thread pin. Returns the published slot, valid until this thread's next pinThread. *counted is 0 when the thread's hazard cell holds the
    // slot (nothing to release), or idx + 1 when the thread had no cell and the shared count holds it (release with unpin(*counted - 1)).
    const T* pinThread(size_t* counted) noexcept {
        Hazard* h = hazardOfThisThread();
        if (h == nullptr) {
            size_t idx = 0;
            const T* p = pin(&idx);
            *counted = idx + 1;
            return p;
        }
        *counted = 0;
        for (;;) {
            const size_t i = cur_.load(std::memory_order_seq_cst);
            if (h->idx.load(std::memory_order_relaxed) != i) h->idx.store(static_cast<uint32_t>(i), std::memory_order_seq_cst);
            if (cur_.load(std::memory_order_seq_cst) == i) return &slots_[i];
        }
    }
    uint64_t publishes() const noexcept { return publishes_.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
    struct alignas(64) Hazard {
        std::atomic<uint32_t> idx{kNoSlot};
    };
    static std::atomic<uint64_t>& nextId() noexcept {
        static std::atomic<uint64_t> next{1};
        return next;
    }
    bool hazarded(size_t slot) const noexcept {
        const uint32_t used = claimed_.load(std::memory_order_seq_cst);
        const uint32_t n = used < kHazards ? used : static_cast<uint32_t>(kHazards);
        for (uint32_t i = 0; i < n; ++i) {
            if (haz_[i].idx.load(std::memory_order_seq_cst) == slot) return true;
        }
        return false;
    }
    Hazard* hazardOfThisThread() noexcept {
        thread_local uint64_t tlsOwner = 0;
        thread_local uint32_t tlsCell = 0;
        if (tlsOwner != id_) {
            tlsOwner = id_;
            tlsCell = claimed_.fetch_add(1, std::memory_order_seq_cst);
        }
        return tlsCell < kHazards ? &haz_[tlsCell] : nullptr;
    }

    const uint64_t id_;
    T slots_[N] = {};
    std::atomic<size_t> cur_{0};
    size_t next_ = 1;
    std::atomic<uint32_t> refs_[N] = {};
    std::atomic<uint32_t> claimed_{0};
    Hazard haz_[kHazards];
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

// Built v14.26: the same cell on exact uint32 words (a verdict and a collection pointer are not floats). The float forms below are wrappers.
inline bool seqCellWriteWords(SeqCell& c, uintptr_t rec, uint32_t gen, const uint32_t w[4]) noexcept {
    uint32_t s = c.seq.load(std::memory_order_acquire);
    if ((s & 1u) != 0) return false;
    if (!c.seq.compare_exchange_strong(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    c.rec.store(rec, std::memory_order_relaxed);
    c.gen.store(gen, std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) c.word[k].store(w[k], std::memory_order_relaxed);
    c.seq.store(s + 2, std::memory_order_release);
    return true;
}

inline bool seqCellReadWords(const SeqCell& c, uintptr_t* rec, uint32_t* gen, uint32_t w[4]) noexcept {
    const uint32_t s1 = c.seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0) return false;
    *rec = c.rec.load(std::memory_order_relaxed);
    *gen = c.gen.load(std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) w[k] = c.word[k].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return c.seq.load(std::memory_order_relaxed) == s1;
}

// The table: `cap` cells, bounded probing (kTableProbeMax), a generation per frame. A cell of an earlier generation is free. Returns false when the
// bounded probe finds no cell (the caller counts it and runs uncached). indexOut, when given, is the cell's index on success.
inline bool seqTableInsertWords(SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, const uint32_t w[4], uint32_t* probes,
                                size_t* indexOut = nullptr) noexcept {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const ProbeResult p = boundedProbe(
            home, cap, kTableProbeMax,
            [&](size_t i) {
                uintptr_t r = 0;
                uint32_t g = 0;
                uint32_t tmp[4];
                return seqCellReadWords(cells[i], &r, &g, tmp) && g == gen && r == rec;
            },
            [&](size_t i) {
                uintptr_t r = 0;
                uint32_t g = 0;
                uint32_t tmp[4];
                return seqCellReadWords(cells[i], &r, &g, tmp) && g != gen;
            });
        if (probes != nullptr) *probes = p.probes;
        if (p.exhausted) return false;
        if (p.found || p.empty) {
            if (seqCellWriteWords(cells[p.index], rec, gen, w)) {
                if (indexOut != nullptr) *indexOut = p.index;
                return true;
            }
            // another writer holds that cell: try again (the cell may now hold another record, so the probe runs again)
        }
    }
    return false;
}

// The cell for `rec` in this generation, if cached. False when it is not (or a writer is in the way).
inline bool seqTableFindWords(const SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, uint32_t w[4], uint32_t* probes,
                              size_t* indexOut = nullptr) noexcept {
    const ProbeResult p = boundedProbe(
        home, cap, kTableProbeMax,
        [&](size_t i) {
            uintptr_t r = 0;
            uint32_t g = 0;
            uint32_t tmp[4];
            return seqCellReadWords(cells[i], &r, &g, tmp) && g == gen && r == rec;
        },
        [&](size_t i) {
            uintptr_t r = 0;
            uint32_t g = 0;
            uint32_t tmp[4];
            return seqCellReadWords(cells[i], &r, &g, tmp) && g != gen;
        });
    if (probes != nullptr) *probes = p.probes;
    if (!p.found) return false;
    uintptr_t r = 0;
    uint32_t g = 0;
    if (!seqCellReadWords(cells[p.index], &r, &g, w) || r != rec || g != gen) return false;
    if (indexOut != nullptr) *indexOut = p.index;
    return true;
}

// The float payload forms (the producer cells): the same table on the words of the floats.
inline bool seqTableInsert(SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, const float v[4], uint32_t* probes) noexcept {
    const uint32_t w[4] = {floatBits(v[0]), floatBits(v[1]), floatBits(v[2]), floatBits(v[3])};
    return seqTableInsertWords(cells, cap, home, rec, gen, w, probes, nullptr);
}

inline bool seqTableFind(const SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, float v[4], uint32_t* probes) noexcept {
    uint32_t w[4] = {};
    if (!seqTableFindWords(cells, cap, home, rec, gen, w, probes, nullptr)) return false;
    for (size_t k = 0; k < 4; ++k) v[k] = bitsFloat(w[k]);
    return true;
}

}  // namespace edvr::sealed
