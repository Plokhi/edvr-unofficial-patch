#pragma once

#include <cstdint>

namespace edvr::openxr {

// The headset while flat (docs/design-unified-install-2026-10-12.md section 12, N1). While the graphics half says the game
// is not in its VR profile, the headset keeps the last frame the game submitted, so the runtime runs empty frames of its
// own. The game's calls always win: a frame it holds open, or any call inside the quiet window, stops them.
//
// Owner-thread policy only: no clock and no API calls. The owner passes in what it observed on this pump.
class FlatClearState final {
 public:
  static constexpr uint64_t kQuietMs = 250;

  enum class Stop { None, VrProfile, GameFrame, Session };

  struct Step {
    bool run = false;      // run one empty frame now
    bool started = false;  // the first empty frame of a stretch
    bool stopped = false;  // this pump ends a stretch
    Stop reason = Stop::None;
  };

  Step decide(bool vrProfileActive, bool gameFrameOpen, bool sessionRunning, uint64_t quietMs) {
    Step step;
    step.run = !vrProfileActive && !gameFrameOpen && sessionRunning && quietMs >= kQuietMs;
    step.started = step.run && !active_;
    step.stopped = !step.run && active_;
    if (step.stopped)
      step.reason = vrProfileActive ? Stop::VrProfile : !sessionRunning ? Stop::Session : Stop::GameFrame;
    active_ = step.run;
    return step;
  }

  bool active() const { return active_; }

 private:
  bool active_ = false;
};

}  // namespace edvr::openxr
