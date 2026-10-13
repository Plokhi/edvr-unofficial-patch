#pragma once
// The sun-shadow probe, pure half (built v14.30, 2026-10-13; log only, no behaviour). While the station's sun skip is on, does anything still sample the
// sun's shadow cascade map, and how much is drawn into it? The map is the 2048x1024 depth texture (D16, typeless R16 as the resource) used as a depth
// target with no colour target. The probe counts, only while the sun is skipped: the draws into it, the draws that bind it as a pixel-shader input in
// slots 0 to 3 (the only slots the binding shadow keeps), the dispatches that bind it as a UAV, and the vertex and pixel shader pairs of the samplers.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::sunprobe {

constexpr uint32_t kCascadeWidth = 2048;
constexpr uint32_t kCascadeHeight = 1024;
constexpr uint32_t kDxgiR16Typeless = 53;
constexpr uint32_t kDxgiD16Unorm = 55;

// A resolved resource is the cascade map: a 2048x1024 texture of D16, or of the typeless 16-bit format a D16 view is made from.
inline bool isCascadeMap(bool isTexture2D, uint32_t width, uint32_t height, uint32_t format) noexcept {
    return isTexture2D && width == kCascadeWidth && height == kCascadeHeight && (format == kDxgiR16Typeless || format == kDxgiD16Unorm);
}

constexpr size_t kPairCap = 32;
constexpr size_t kTopShown = 8;

struct ShaderPair {
    uint64_t a = 0;   // the vertex shader hash (the compute shader hash for a dispatch)
    uint64_t b = 0;   // the pixel shader hash (0 for a dispatch)
    uint64_t n = 0;
};

class Counts {
public:
    // A frame in which the sun skip was on.
    void noteSkippedFrame() noexcept { ++skippedFrames_; }
    // A draw into the cascade map (depth target, no colour target).
    void noteIntoCascade() noexcept { ++into_; }
    // A draw that binds the map as a pixel-shader input.
    void noteSampler(uint64_t vs, uint64_t ps) noexcept {
        ++samplers_;
        add(samplerPairs_, nSamplerPairs_, vs, ps);
    }
    // A dispatch that binds the map as a UAV.
    void noteDispatchBound(uint64_t cs) noexcept {
        ++dispatchBound_;
        add(dispatchPairs_, nDispatchPairs_, cs, 0);
    }
    // A distinct resource seen as the map.
    void noteResource(bool newResource) noexcept {
        if (newResource) ++resources_;
    }

    uint64_t skippedFrames() const noexcept { return skippedFrames_; }
    uint64_t into() const noexcept { return into_; }
    uint64_t samplers() const noexcept { return samplers_; }
    uint64_t dispatchBound() const noexcept { return dispatchBound_; }
    uint64_t resources() const noexcept { return resources_; }

    // The top pairs by count, most first (up to kTopShown), written to out. Returns how many.
    size_t top(const ShaderPair* pairs, size_t n, ShaderPair* out) const noexcept {
        ShaderPair tmp[kPairCap];
        for (size_t i = 0; i < n; ++i) tmp[i] = pairs[i];
        std::sort(tmp, tmp + n, [](const ShaderPair& x, const ShaderPair& y) { return x.n > y.n; });
        const size_t m = n < kTopShown ? n : kTopShown;
        for (size_t i = 0; i < m; ++i) out[i] = tmp[i];
        return m;
    }
    size_t topSamplers(ShaderPair* out) const noexcept { return top(samplerPairs_, nSamplerPairs_, out); }
    size_t topDispatches(ShaderPair* out) const noexcept { return top(dispatchPairs_, nDispatchPairs_, out); }

    // The 30 s line. `sunSkipped`: whether the sun skip is on now. Returns the length written.
    size_t line(char* out, size_t cap, uint64_t seconds) const noexcept {
        char samp[420] = {};
        char disp[200] = {};
        format(samp, sizeof(samp), samplerPairs_, nSamplerPairs_, true);
        format(disp, sizeof(disp), dispatchPairs_, nDispatchPairs_, false);
        const double frames = skippedFrames_ > 0 ? static_cast<double>(skippedFrames_) : 1.0;
        const int n = std::snprintf(
            out, cap,
            "optimized station: sun shadow probe last %llu s (the sun skip was on for %llu frames): draws into the 2048x1024 D16 map with no colour target "
            "%llu (%.1f a frame, the saving if they were skipped); draws that sample it through a pixel-shader slot 0 to 3 %llu%s; dispatches that bind it "
            "as a UAV %llu%s; resources seen as the map %llu. Vertex-shader and compute shader-resource slots are not shadowed, so a sampler there is not "
            "counted.",
            static_cast<unsigned long long>(seconds), static_cast<unsigned long long>(skippedFrames_), static_cast<unsigned long long>(into_),
            static_cast<double>(into_) / frames, static_cast<unsigned long long>(samplers_), samp, static_cast<unsigned long long>(dispatchBound_), disp,
            static_cast<unsigned long long>(resources_));
        return n < 0 ? 0 : static_cast<size_t>(n);
    }

    void clear() noexcept { *this = Counts{}; }

private:
    static void add(ShaderPair* set, size_t& n, uint64_t a, uint64_t b) noexcept {
        for (size_t i = 0; i < n; ++i) {
            if (set[i].a == a && set[i].b == b) {
                ++set[i].n;
                return;
            }
        }
        if (n < kPairCap) {
            set[n].a = a;
            set[n].b = b;
            set[n].n = 1;
            ++n;
        } else {
            // the table is full: the pair is counted in the total only
        }
    }
    void format(char* out, size_t cap, const ShaderPair* pairs, size_t n, bool pixel) const noexcept {
        out[0] = '\0';
        if (n == 0) return;
        ShaderPair t[kTopShown];
        const size_t m = top(pairs, n, t);
        size_t used = 0;
        used += static_cast<size_t>(std::snprintf(out + used, cap - used, " (top by shader:"));
        for (size_t i = 0; i < m && used < cap; ++i) {
            const int w = pixel ? std::snprintf(out + used, cap - used, " VS %016llX PS %016llX x%llu;", static_cast<unsigned long long>(t[i].a),
                                                static_cast<unsigned long long>(t[i].b), static_cast<unsigned long long>(t[i].n))
                                : std::snprintf(out + used, cap - used, " CS %016llX x%llu;", static_cast<unsigned long long>(t[i].a),
                                                static_cast<unsigned long long>(t[i].n));
            if (w < 0 || static_cast<size_t>(w) >= cap - used) break;
            used += static_cast<size_t>(w);
        }
        if (used + 2 < cap) {
            out[used] = ')';
            out[used + 1] = '\0';
        }
    }

    uint64_t skippedFrames_ = 0, into_ = 0, samplers_ = 0, dispatchBound_ = 0, resources_ = 0;
    ShaderPair samplerPairs_[kPairCap] = {};
    ShaderPair dispatchPairs_[kPairCap] = {};
    size_t nSamplerPairs_ = 0, nDispatchPairs_ = 0;
};

}  // namespace edvr::sunprobe
