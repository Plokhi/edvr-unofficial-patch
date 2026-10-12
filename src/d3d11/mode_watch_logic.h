// The mode watch's decisions: when a read of Settings.xml posts a 3D mode change, and when a live runtime posts Vr. Pure and free of any
// Windows API, so tools\mode_watch_test drives every rule here; mode_watch.cpp owns the files, the threads and the log. Phase 1 only
// logs what these decisions say (docs\design-unified-install-2026-10-12.md section 3.2). The one-read-later rule follows the Bindings
// poll's two-check rule (device_hook.cpp).
#pragma once

#include <cstdint>

#include "../common/install_scope.h"

namespace edvr::modewatch {

// ignored: the read did not name a mode (absent, half-written, out of range). same: nothing changed, or a pending mode is still waiting.
// pending: a change to mode was seen and waits for its confirming read. posted: the committed mode changed. dropped: the pending mode
// did not survive its confirming read. nothing: a live rise that changes nothing.
enum class Outcome { Nothing, Ignored, Same, Pending, Posted, Dropped };

// mode is the pending or posted class for Pending and Posted; for the other outcomes it is not meaningful.
struct Step {
    Outcome outcome;
    GameMode mode;
};

// Two classes are kept. fileMode_ is the last class Settings.xml was confirmed to hold: a read is compared with it, so a read posts only
// when the file changes. committed_ is the class EDVR runs. They differ after a live rise: Epic's "Launch in VR" goes live with
// Settings.xml still saying flat, and a later read of that unchanged file must not post Flat. The live flag is never used for the fall
// (design section 3.2), and neither may a stale file be.
class Watch {
public:
    // fileMode is the launch file's class; effectiveMode is the class the launch runs, which a /vr override makes Vr over a flat file
    // (design section 3.1). A read is compared with fileMode, and EDVR runs effectiveMode from the start, so a /vr launch over a flat
    // file does not post the file's flat when the file is read unchanged. An unknown launch file already reads as Vr (design Q2).
    Watch(GameMode fileMode, GameMode effectiveMode) : committed_(effectiveMode), fileMode_(fileMode) {}

    Step onRead(uint64_t nowMs, const ModeRead& r) {
        if (!r.known) return Step{Outcome::Ignored, r.mode};
        if (!pending_) {
            if (r.mode == fileMode_) return Step{Outcome::Same, committed_};
            pending_ = true;
            pendingMode_ = r.mode;
            due_ = nowMs + kConfirmMs;
            return Step{Outcome::Pending, pendingMode_};
        }
        if (nowMs < due_) {
            if (r.mode == pendingMode_) return Step{Outcome::Same, pendingMode_};
            pending_ = false;
            return Step{Outcome::Dropped, r.mode};
        }
        const bool confirmed = r.mode == pendingMode_;
        const GameMode mode = pendingMode_;
        pending_ = false;
        if (!confirmed) return Step{Outcome::Dropped, r.mode};
        fileMode_ = mode;
        // A live rise may already have posted this class; then the confirmed file changes nothing EDVR runs.
        if (mode == committed_) return Step{Outcome::Same, committed_};
        committed_ = mode;
        return Step{Outcome::Posted, committed_};
    }

    // Touches committed_ only. A pending file change stays pending, and fileMode_ is not changed.
    Step onLiveRise(uint64_t /*nowMs*/) {
        if (committed_ != GameMode::Flat) return Step{Outcome::Nothing, committed_};
        committed_ = GameMode::Vr;
        return Step{Outcome::Posted, committed_};
    }

    // The time the pending mode is confirmed, or 0 when nothing is pending.
    uint64_t confirmDueMs() const { return pending_ ? due_ : 0; }
    bool pending() const { return pending_; }
    GameMode committed() const { return committed_; }

    static constexpr uint64_t kConfirmMs = 250;

private:
    GameMode committed_;
    GameMode fileMode_;
    bool pending_ = false;
    GameMode pendingMode_ = GameMode::Vr;
    uint64_t due_ = 0;
};

}  // namespace edvr::modewatch
