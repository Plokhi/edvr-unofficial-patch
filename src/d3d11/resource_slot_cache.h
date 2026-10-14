#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// Positive identity-to-slot hints for a table of unique retained resources.
// Every hit reads the current slot identity; eviction, pointer reuse and
// in-place metadata changes need no notification. Never cache misses or data.
// The last hit stays one comparison; alternating resources use a bounded
// two-way index instead of scanning the table. Collisions fall back to a scan.
// Like the owning camera/projection tables, this cache is owner-thread only.
template <std::size_t Capacity>
class ResourceSlotCache {
    static_assert(Capacity > 0, "a resource table needs at least one slot");
    static constexpr std::size_t kBuckets = Capacity * 2;
public:
    void reset() noexcept {
        hint_ = Capacity;
        for (auto& bucket : index_) bucket[0] = bucket[1] = 0;
        for (auto& next : next_) next = 0;
    }

    template <typename Entry, typename Resource, typename ResourceOf>
    Entry* find(Entry (&entries)[Capacity], Resource resource, ResourceOf resourceOf,
                std::size_t active = Capacity) noexcept {
        if (!resource) return nullptr;
        if (active > Capacity) active = Capacity;
        if (hint_ < active && resourceOf(entries[hint_]) == resource)
            return &entries[hint_];

        std::uintptr_t key = reinterpret_cast<std::uintptr_t>(resource) >> 4;
        key ^= key >> 17; key ^= key >> 9;
        const std::size_t bucket = key % kBuckets;
        for (std::size_t encoded : index_[bucket]) {
            if (!encoded) continue;
            const std::size_t slot = encoded - 1;
            if (slot < active && slot != hint_ && resourceOf(entries[slot]) == resource) {
                hint_ = slot;
                return &entries[slot];
            }
        }
        for (std::size_t i = 0; i < active; ++i) {
            if (resourceOf(entries[i]) == resource) {
                hint_ = i;
                // Index+1 keeps empty buckets zero-initialized. Store only a
                // slot, with the same pointer mixing as EDHDR's heal memo.
                if (index_[bucket][0] != i + 1 && index_[bucket][1] != i + 1) {
                    index_[bucket][next_[bucket]] = i + 1;
                    next_[bucket] ^= 1;
                }
                return &entries[i];
            }
        }
        return nullptr;
    }

private:
    std::size_t hint_ = Capacity;
    std::size_t index_[kBuckets][2]{};
    unsigned char next_[kBuckets]{};
};

} // namespace edvr
