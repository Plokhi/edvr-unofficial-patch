#pragma once
// The sun-shadow skip, pure half (built v14.31, 2026-10-13; v14.30 counted, v14.31 skips). While the station's sun skip is on, the sun's shadow cascade
// map is drawn for nothing: the v14.30 flight saw 240 to 293 draws a frame into the 2048x1024 D16 map with no colour target, and no shader of any
// pixel-shader slot 0 to 3 or any UAV bound it. This skips those draws, with a fail-safe: for the first kScanFrames frames of each sun-skip window the
// draws still run while a sampled scan enumerates the shader-resource slots of every stage (vertex, geometry, hull, domain, pixel, compute) for the map;
// a window with no consumer then skips, and a consumer found at any time (the continuing, cheaper scan or the pixel-shader slots checked on every draw)
// stands the skip down for the rest of the window and says why. The 256x256 map and every eye-size pass are not touched: the map is found by its
// shape (a 2048x1024 texture of R16-typeless or D16) as a depth target with no colour target.
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::sunskip {

constexpr uint32_t kCascadeWidth = 2048;
constexpr uint32_t kCascadeHeight = 1024;
constexpr uint32_t kDxgiR16Typeless = 53;
constexpr uint32_t kDxgiD16Unorm = 55;

// A resolved resource is the cascade map: a 2048x1024 texture of D16, or of the typeless 16-bit format a D16 view is made from.
inline bool isCascadeMap(bool isTexture2D, uint32_t width, uint32_t height, uint32_t format) noexcept {
    return isTexture2D && width == kCascadeWidth && height == kCascadeHeight && (format == kDxgiR16Typeless || format == kDxgiD16Unorm);
}

enum class Stage : uint8_t { kVs, kGs, kHs, kDs, kPs, kCs, kCsUav, kCount };
inline const char* stageName(Stage s) noexcept {
    switch (s) {
        case Stage::kVs: return "vertex shader";
        case Stage::kGs: return "geometry shader";
        case Stage::kHs: return "hull shader";
        case Stage::kDs: return "domain shader";
        case Stage::kPs: return "pixel shader";
        case Stage::kCs: return "compute shader";
        case Stage::kCsUav: return "compute shader UAV";
        default: return "?";
    }
}

enum class Phase : uint8_t { kIdle, kScanning, kSkipping, kStoodDown };
inline const char* phaseName(Phase p) noexcept {
    switch (p) {
        case Phase::kIdle: return "idle (the sun skip is off)";
        case Phase::kScanning: return "scanning";
        case Phase::kSkipping: return "skipping";
        default: return "stood down";
    }
}

constexpr uint32_t kScanFrames = 90;            // frames of a sun-skip window the draws still run while the scan looks for a consumer
constexpr uint32_t kScanDrawStride = 64;        // while scanning: one draw in this many is enumerated (the offset moves every frame)
constexpr uint32_t kWatchDrawStride = 128;      // once skipping: the continuing scan (about ten draws a frame; every position in about 1.4 s)
constexpr uint32_t kScanDispatchStride = 1;     // while scanning: every dispatch
constexpr uint32_t kWatchDispatchStride = 8;    // once skipping

// The scan's stride in draws and in dispatches for a phase, and whether the ordinal-th draw (or dispatch) of this frame is enumerated: the offset moves
// with the frame, so every position in the draw order is reached over a window.
inline uint32_t drawStride(Phase p) noexcept { return p == Phase::kScanning ? kScanDrawStride : kWatchDrawStride; }
inline uint32_t dispatchStride(Phase p) noexcept { return p == Phase::kScanning ? kScanDispatchStride : kWatchDispatchStride; }
inline bool takeScan(uint32_t ordinal, uint32_t frameNo, uint32_t stride) noexcept { return (ordinal % stride) == (frameNo % stride); }

struct Consumer {
    Stage stage = Stage::kPs;
    uint32_t slot = 0;
    uint64_t vs = 0, ps = 0, cs = 0;   // the shaders bound when it was found
};

// The state machine and the counts. One owner (the draw thread and the tick, serialized by the caller).
class SkipGate {
public:
    Phase phase() const noexcept { return phase_; }
    bool skipping() const noexcept { return phase_ == Phase::kSkipping; }
    bool scanning() const noexcept { return phase_ == Phase::kScanning || phase_ == Phase::kSkipping; }

    // The tick: the sun skip's state this frame. Returns true when the phase changed to skipping (the caller logs it once).
    bool frame(bool sunSkipped) noexcept {
        ++frameNo_;
        if (!sunSkipped) {
            if (phase_ != Phase::kIdle) {
                phase_ = Phase::kIdle;
                windowFrames_ = 0;
                windowDraws_ = windowDispatches_ = 0;
            }
            return false;
        }
        ++sunFrames_;
        if (phase_ == Phase::kIdle) {
            phase_ = Phase::kScanning;
            windowFrames_ = 0;
            windowDraws_ = windowDispatches_ = 0;
        }
        ++windowFrames_;
        if (phase_ == Phase::kScanning && windowFrames_ >= kScanFrames) {
            phase_ = Phase::kSkipping;
            return true;
        }
        return false;
    }

    // The scan enumerated a draw, or a dispatch (the caller took it by takeScan).
    void noteScannedDraw() noexcept {
        ++scannedDraws_;
        ++windowDraws_;
    }
    void noteScannedDispatch() noexcept {
        ++scannedDispatches_;
        ++windowDispatches_;
    }

    // A consumer of the map was found. Returns true the first time in the window (the caller logs why).
    bool consumer(const Consumer& c) noexcept {
        ++consumers_;
        ++byStage_[static_cast<size_t>(c.stage)];
        if (phase_ != Phase::kScanning && phase_ != Phase::kSkipping) return false;
        phase_ = Phase::kStoodDown;
        ++standDowns_;
        last_ = c;
        return true;
    }

    // Draws into the map that were skipped, and that were seen (skipped or not).
    void addSkipped(uint64_t n) noexcept { skipped_ += n; }
    void addInto(uint64_t n) noexcept { into_ += n; }

    uint64_t skipped() const noexcept { return skipped_; }
    uint64_t into() const noexcept { return into_; }
    uint64_t sunFrames() const noexcept { return sunFrames_; }
    uint32_t frameNo() const noexcept { return frameNo_; }
    uint64_t scannedDraws() const noexcept { return scannedDraws_; }
    uint64_t scannedDispatches() const noexcept { return scannedDispatches_; }
    uint64_t windowDraws() const noexcept { return windowDraws_; }
    uint64_t windowDispatches() const noexcept { return windowDispatches_; }
    uint64_t consumers() const noexcept { return consumers_; }
    uint64_t standDowns() const noexcept { return standDowns_; }
    const Consumer& lastConsumer() const noexcept { return last_; }

    // The 30 s line's counts start afresh (the phase and the window's scan do not).
    void clearReport() noexcept {
        skipped_ = into_ = sunFrames_ = scannedDraws_ = scannedDispatches_ = consumers_ = standDowns_ = 0;
        for (uint64_t& b : byStage_) b = 0;
    }

    // The 30 s line.
    size_t line(char* out, size_t cap, uint64_t seconds) const noexcept {
        char why[200] = {};
        if (consumers_ > 0) {
            std::snprintf(why, sizeof(why), "; last consumer: %s slot %u (VS %016llX PS %016llX CS %016llX)", stageName(last_.stage),
                          static_cast<unsigned>(last_.slot), static_cast<unsigned long long>(last_.vs), static_cast<unsigned long long>(last_.ps),
                          static_cast<unsigned long long>(last_.cs));
        }
        const double frames = sunFrames_ > 0 ? static_cast<double>(sunFrames_) : 1.0;
        const int n = std::snprintf(
            out, cap,
            "optimized station: sun shadow map last %llu s (the sun skip was on for %llu frames): state %s; draws into the 2048x1024 D16 map with no colour "
            "target %llu seen, %llu skipped (%.1f a frame skipped); consumers of the map found %llu (vertex/geometry/hull/domain/pixel/compute/UAV "
            "%llu/%llu/%llu/%llu/%llu/%llu/%llu), stood down %llu times%s; the scan enumerated %llu draws and %llu dispatches (shader-resource slots 0 to 7 "
            "of every stage, compute UAVs 0 to 3).",
            static_cast<unsigned long long>(seconds), static_cast<unsigned long long>(sunFrames_), phaseName(phase_), static_cast<unsigned long long>(into_),
            static_cast<unsigned long long>(skipped_), static_cast<double>(skipped_) / frames, static_cast<unsigned long long>(consumers_),
            static_cast<unsigned long long>(byStage_[0]), static_cast<unsigned long long>(byStage_[1]), static_cast<unsigned long long>(byStage_[2]),
            static_cast<unsigned long long>(byStage_[3]), static_cast<unsigned long long>(byStage_[4]), static_cast<unsigned long long>(byStage_[5]),
            static_cast<unsigned long long>(byStage_[6]), static_cast<unsigned long long>(standDowns_), why,
            static_cast<unsigned long long>(scannedDraws_), static_cast<unsigned long long>(scannedDispatches_));
        return n < 0 ? 0 : static_cast<size_t>(n);
    }

private:
    Phase phase_ = Phase::kIdle;
    uint32_t windowFrames_ = 0;
    uint32_t frameNo_ = 0;
    uint64_t windowDraws_ = 0, windowDispatches_ = 0;
    uint64_t skipped_ = 0, into_ = 0, sunFrames_ = 0, scannedDraws_ = 0, scannedDispatches_ = 0, consumers_ = 0, standDowns_ = 0;
    uint64_t byStage_[static_cast<size_t>(Stage::kCount)] = {};
    Consumer last_;
};

}  // namespace edvr::sunskip
