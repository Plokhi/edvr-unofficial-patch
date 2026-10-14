#pragma once

#include "../../src/d3d11/resource_slot_cache.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace resource_slot_cache_tests {

struct ModelEntry {
    const void* resource = nullptr;
    uint32_t metadata = 0;
};

struct ResourceRead {
    uint32_t* comparisons = nullptr;
    const void* operator()(const ModelEntry& entry) const {
        if (comparisons) ++*comparisons;
        return entry.resource;
    }
};

template <std::size_t N>
ModelEntry* linearFind(ModelEntry (&entries)[N], const void* resource) {
    if (!resource) return nullptr;
    for (std::size_t i = 0; i < N; ++i)
        if (entries[i].resource == resource) return &entries[i];
    return nullptr;
}

template <typename Check>
void run(Check check) {
    alignas(16) static const unsigned char ids[8]{};
    const void* a = &ids[0];
    const void* b = &ids[1];
    const void* c = &ids[2];
    const void* d = &ids[3];
    const void* missing = &ids[4];

    ModelEntry entries[4]{{a, 10}, {b, 20}, {c, 30}, {d, 40}};
    edvr::ResourceSlotCache<4> cache;
    uint32_t comparisons = 0;
    ResourceRead read{&comparisons};

    auto agrees = [&](const void* resource, const char* label) {
        ModelEntry* expected = linearFind(entries, resource);
        ModelEntry* actual = cache.find(entries, resource, read);
        check(actual == expected, label);
        if (actual && expected) check(actual->metadata == expected->metadata, "resource cache: metadata matches oracle");
    };

    agrees(c, "resource cache: cold hit matches linear oracle");
    const uint32_t afterCold = comparisons;
    agrees(c, "resource cache: repeated hit matches linear oracle");
    check(comparisons - afterCold == 1, "resource cache: last hit takes one comparison");

    agrees(a, "resource cache: alternating first-slot hit matches oracle");
    agrees(d, "resource cache: alternating last-slot hit matches oracle");
    agrees(b, "resource cache: alternating middle-slot hit matches oracle");

    const uint32_t beforeMiss = comparisons;
    agrees(missing, "resource cache: miss matches linear oracle");
    check(comparisons - beforeMiss >= 5 && comparisons - beforeMiss <= 7,
          "resource cache: miss performs hint/index checks then the full scan");
    const uint32_t beforeAfterMiss = comparisons;
    agrees(b, "resource cache: miss does not replace last-hit hint");
    check(comparisons - beforeAfterMiss == 1, "resource cache: positive hint survives a miss");

    const uint32_t beforeNull = comparisons;
    agrees(nullptr, "resource cache: null matches linear oracle");
    check(comparisons == beforeNull, "resource cache: null performs no slot reads");
    const uint32_t beforeNullFollowup = comparisons;
    agrees(b, "resource cache: null does not replace last-hit hint");
    check(comparisons - beforeNullFollowup == 1, "resource cache: positive hint survives null");

    cache.reset();
    const uint32_t afterReset = comparisons;
    agrees(b, "resource cache: reset hit matches linear oracle");
    check(comparisons - afterReset == 2, "resource cache: reset forces the ordinary scan");

    // Reusing the hinted slot must return its current entry, never the old
    // entry's metadata. A miss for the evicted identity remains a miss.
    entries[2] = ModelEntry{missing, 900};
    agrees(missing, "resource cache: slot reuse finds current resource");
    check(cache.find(entries, missing, read)->metadata == 900,
          "resource cache: slot reuse returns current metadata");
    agrees(c, "resource cache: evicted resource stays absent");

    // Pointer reuse represents a new retained object at the same address. The
    // lookup must return the current slot contents; the cache stores no metadata.
    entries[2] = ModelEntry{c, 901};
    agrees(c, "resource cache: reused pointer matches current slot");
    check(cache.find(entries, c, read)->metadata == 901,
          "resource cache: reused pointer cannot expose stale metadata");

    // Updating metadata in-place must be visible even on the one-comparison hit.
    entries[2].metadata = 902;
    const uint32_t beforeChangedMetadata = comparisons;
    agrees(c, "resource cache: changed metadata matches oracle");
    check(comparisons - beforeChangedMetadata == 1,
          "resource cache: changed metadata keeps last-hit complexity");

    // A miss must not suppress a later admission; active-count shrink must
    // reject an indexed entry outside the live table even if its pointer stays.
    entries[3] = ModelEntry{missing, 903};
    agrees(missing, "resource cache: a previously missing identity is admitted");
    check(cache.find(entries, missing, read, 3) == nullptr,
          "resource cache: count shrink rejects a cached slot outside the live range");
    check(cache.find(entries, missing, read, 4) == &entries[3],
          "resource cache: count growth finds the current entry");
    cache.reset();
    check(cache.find(entries, missing, read, 0) == nullptr,
          "resource cache: an empty table returns no cached entry");

    // All these byte-sized identities share at most two hash buckets. Cycling
    // more than two per bucket forces index displacement and linear fallback.
    for (unsigned turn = 0; turn < 100; ++turn) {
        const void* wanted = entries[turn % 4].resource;
        agrees(wanted, "resource cache: collision displacement agrees with the oracle");
    }

    // Production-like interleaved writes/prepares into two retained buffers
    // near the end of a full bank defeat the previous last-hit-only hint.
    struct alignas(16) Identity { unsigned char bytes[16]{}; } bankIds[64];
    ModelEntry bank[64]{};
    for (unsigned i = 0; i < 64; ++i) bank[i] = ModelEntry{&bankIds[i], i};
    edvr::ResourceSlotCache<64> indexed;
    uint32_t indexedReads = 0, oldReads = 0;
    ResourceRead bankRead{&indexedReads};
    size_t oldHint = 64;
    bool equal = true;
    constexpr unsigned lookups = 20000;
    for (unsigned turn = 0; turn < lookups; ++turn) {
        const unsigned slot = 62 + turn % 2;
        const void* wanted = bank[slot].resource;
        ModelEntry* old = nullptr;
        if (oldHint < 64) { ++oldReads; if (bank[oldHint].resource == wanted) old = &bank[oldHint]; }
        if (!old) for (unsigned i = 0; i < 64; ++i) {
            ++oldReads;
            if (bank[i].resource == wanted) { oldHint = i; old = &bank[i]; break; }
        }
        ModelEntry* now = indexed.find(bank, wanted, bankRead);
        equal &= now == old && now && now->metadata == slot;
    }
    check(equal, "resource index: interleaved retained-buffer lookups preserve every answer");
    check(indexedReads * 10 < oldReads,
          "resource index: interleaving eliminates over 90 percent of the old table comparisons");
    const uint32_t interleavedReads = indexedReads;
    bank[0] = ModelEntry{missing, 904};
    const uint32_t beforeUnrelatedReuse = indexedReads;
    check(indexed.find(bank, bank[62].resource, bankRead) == &bank[62] &&
          indexedReads - beforeUnrelatedReuse <= 3,
          "resource index: an unrelated slot replacement preserves other warm identities");
    std::printf("resource index operations: %u interleaved lookups, old hint %u identity reads, indexed %u\n",
                lookups, oldReads, interleavedReads);

}

} // namespace resource_slot_cache_tests
