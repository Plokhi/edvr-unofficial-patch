// Test rig for the optimized station's pure half (src/d3d11/optimized_station_core.h): the seal trigger (the state machine that seals, releases
// and re-seals the station), the release rules (setting off, undocked, Explorer Cam, the camera leaving the bay, the pad moving against the hangar
// shell, a hangar change, launch), the pad-local frame's round trip and the release's threshold, the parse of the per-size floors, the own-ship
// prefix and sphere, the bay's volume, and the unreadable rule. No DLL and no game.
#include "../../src/d3d11/optimized_station_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace se = edvr::sealed;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// One tick of the seal trigger: the inputs the engine gathers, with the defaults of a docked, still, lowered pad in the hangar.
se::SealInputs quiet(uint64_t now) {
    se::SealInputs in;
    in.setting = true;
    in.docked = true;
    in.pickOk = true;
    in.along = 10.0f;
    in.lowest = 10.0f;
    in.nowMs = now;
    return in;
}

// Runs the trigger for ticks from t0 to t1 (every 100 ms) with the same inputs; returns the first engage request, or 0 when none came.
uint64_t firstEngage(se::SealState* st, se::SealInputs in, uint64_t t0, uint64_t t1) {
    se::ReleaseWhy why = se::ReleaseWhy::kNone;
    for (uint64_t t = t0; t <= t1; t += 100) {
        in.nowMs = t;
        if (se::sealStep(in, st, &why) == se::SealAction::kRequestEngage) return t;
    }
    return 0;
}

// Built v14.3: a real descent this session. The pad is first seen 4 m up, then at its lowest from t = 100 ms (the still period starts there).
se::SealState descended() {
    se::SealState st;
    se::ReleaseWhy w = se::ReleaseWhy::kNone;
    se::SealInputs a = quiet(0);
    a.along = 14.0f;
    a.lowest = 10.0f;
    se::sealStep(a, &st, &w);
    se::sealStep(quiet(100), &st, &w);
    return st;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: optimized_station_test --self-test\n");
        return 2;
    }

    // ---- The seal trigger -----------------------------------------------------------------------------------------------------------------------
    {
        // Built v14.3, the load-in rule: a pad first seen at rest with no descent seals on the first eligible frame.
        se::SealState st;
        const uint64_t t = firstEngage(&st, quiet(0), 100, 10000);
        check(t == 100, "load-in: a docked pad first seen at its lowest, with no descent, seals on the first eligible frame (100 ms)");
        check(st.immediate && firstEngage(&st, quiet(0), 100, 10000) == 0, "one engage per seal: the request is not repeated while the seal stands");
    }
    {
        // After an observed descent, the still delay still applies: 6 s from the pad settling at its lowest (100 ms).
        se::SealState st = descended();
        check(firstEngage(&st, quiet(0), 200, 20000) == 100 + se::kSealDelayMs,
              "after an observed descent the delay applies: the seal comes 6 s after the pad settles (6100 ms)");
    }
    {
        // A different own pad starts its own motion memory: at rest, it seals on the first eligible frame.
        se::SealState st = descended();
        se::SealInputs other = quiet(30000);
        other.pad = 0x3000;
        check(firstEngage(&st, other, 30000, 30000) == 30000, "a different own pad: its motion memory starts fresh and it seals at once at rest");
    }
    {
        se::SealState st;
        se::SealInputs in = quiet(0);
        in.docked = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "not docked: no seal, however still the pad is");
    }
    {
        se::SealState st;
        se::SealInputs in = quiet(0);
        in.explorerOn = true;
        check(firstEngage(&st, in, 0, 10000) == 0, "Explorer Cam on: no seal");
    }
    {
        se::SealState st;
        se::SealInputs in = quiet(0);
        in.pickOk = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "no own pad found: no seal");
    }
    {
        se::SealState st;
        se::SealInputs in = quiet(0);
        in.along = 10.6f;   // 0.6 m above its lowest seen height: not at the lowest
        check(firstEngage(&st, in, 0, 10000) == 0, "a pad above its lowest seen height (by more than 0.2 m) does not seal");
    }
    {
        se::SealState st;
        // The pad moves 0.5 m at 3 s: the still period restarts there, so the seal comes 6 s after 3 s, not 6 s after the start.
        se::SealInputs in = quiet(0);
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        uint64_t sealedAt = 0;
        for (uint64_t t = 0; t <= 12000; t += 100) {
            in.nowMs = t;
            in.along = t == 0 ? 14.0f : ((t >= 3000 && t < 3100) ? 10.5f : 10.0f);   // first seen 4 m up, then descends
            if (se::sealStep(in, &st, &why) == se::SealAction::kRequestEngage && sealedAt == 0) sealedAt = t;
        }
        check(sealedAt == 9100, "a move of 0.5 m restarts the still period: the seal comes 6 s after the pad settles (9.1 s), not at 6 s");
    }
    {
        se::SealState st;
        se::SealInputs in = quiet(0);
        in.along = 10.1f;   // within 0.2 m of its lowest and still: seals
        in.lowest = 10.0f;
        check(firstEngage(&st, in, 100, 10000) == 100, "load-in: a pad first seen within 0.2 m of its lowest, with no descent, seals on the first eligible frame");
    }
    {
        se::SealState st;
        se::SealInputs in = quiet(0);
        in.setting = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "the setting off: no seal");
    }

    // ---- The releases, and the re-seal ------------------------------------------------------------------------------------------------------------
    struct Case {
        const char* name;
        se::SealInputs inputs;
        se::ReleaseWhy want;
    };
    se::SealInputs base = quiet(0);
    base.held = true;
    std::vector<Case> cases;
    {
        Case c{"setting off", base, se::ReleaseWhy::kSettingOff};
        c.inputs.setting = false;
        cases.push_back(c);
    }
    {
        Case c{"undocked", base, se::ReleaseWhy::kUndocked};
        c.inputs.docked = false;
        cases.push_back(c);
    }
    {
        Case c{"Explorer Cam on", base, se::ReleaseWhy::kExplorerCam};
        c.inputs.explorerOn = true;
        cases.push_back(c);
    }
    {
        Case c{"the camera left the bay", base, se::ReleaseWhy::kCameraLeft};
        c.inputs.cameraOutside = true;
        cases.push_back(c);
    }
    {
        Case c{"the pad moved against the shell", base, se::ReleaseWhy::kPadMoved};
        c.inputs.padMoved = true;
        cases.push_back(c);
    }
    {
        Case c{"the hangar changed (a ship swap)", base, se::ReleaseWhy::kHangarChanged};
        c.inputs.hangarChanged = true;
        cases.push_back(c);
    }
    {
        Case c{"launch", base, se::ReleaseWhy::kLaunch};
        c.inputs.launched = true;
        cases.push_back(c);
    }
    for (const Case& c : cases) {
        se::SealState st;
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        const se::SealAction a = se::sealStep(c.inputs, &st, &why);
        check(a == se::SealAction::kRelease && why == c.want, c.name);
    }
    {
        // Nothing held, nothing to release: a held-only rule does not fire while sealed-off.
        se::SealState st;
        se::SealInputs in = base;
        in.held = false;
        in.padMoved = true;
        in.cameraOutside = true;
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        const se::SealAction a = se::sealStep(in, &st, &why);
        check(a != se::SealAction::kRelease && why == se::ReleaseWhy::kNone,
              "the release rules do not fire while not sealed (a pad at rest may request the load-in seal)");
    }
    {
        // Priority: the first rule that holds names the release (undocking while the pad moves says undocked).
        se::SealState st;
        se::SealInputs in = base;
        in.docked = false;
        in.padMoved = true;
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        se::sealStep(in, &st, &why);
        check(why == se::ReleaseWhy::kUndocked, "two rules at once: the first in the brief's order names the release");
    }
    {
        // Ship swap: released by the hangar change, then the new pad must be lowered and still for the whole delay before a re-seal.
        se::SealState st;
        se::SealInputs held = base;
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        se::sealStep(held, &st, &why);   // held: a non-release tick
        se::SealInputs swap = base;
        swap.hangarChanged = true;
        check(se::sealStep(swap, &st, &why) == se::SealAction::kRelease && why == se::ReleaseWhy::kHangarChanged,
              "ship swap: the hangar change releases");
        // The new pad, shut in a closed bay, at rest: the load-in rule seals it on the first eligible frame.
        se::SealInputs fresh = quiet(20000);
        fresh.pad = 0x2000;
        check(firstEngage(&st, fresh, 20000, 30000) == 20000,
              "ship swap into a closed bay: the new pad at rest seals on the first eligible frame (load-in)");
        // The new pad that then descends: the delay applies.
        se::SealState st2;
        se::ReleaseWhy w2 = se::ReleaseWhy::kNone;
        se::SealInputs d0 = quiet(20000);
        d0.pad = 0x2000;
        d0.along = 14.0f;
        se::sealStep(d0, &st2, &w2);
        se::SealInputs d1 = quiet(20100);
        d1.pad = 0x2000;
        se::sealStep(d1, &st2, &w2);
        se::SealInputs d2 = quiet(20200);
        d2.pad = 0x2000;
        check(firstEngage(&st2, d2, 20200, 40000) == 20100 + se::kSealDelayMs,
              "ship swap, then the new pad descends: the delay applies after the descent settles");
    }
    {
        // Re-seal after a release: the pad is still at the lowest for the delay, the station seals again.
        se::SealState st;
        se::SealInputs in = quiet(0);
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        se::sealStep(in, &st, &why);
        se::SealInputs rel = quiet(1000);
        rel.held = true;
        rel.padMoved = true;
        se::sealStep(rel, &st, &why);   // released: the state clears
        const uint64_t again = firstEngage(&st, quiet(0), 2000, 12000);
        check(again == 2000 + se::kSealDelayMs, "re-seal: the same pad, still at its lowest, seals again after the full delay from the release");
    }

    // ---- Built v14: the own pad's near-tie and its stickiness (fix 1); the shell named once, never a gate, ramp or skirt (fix 2) ----------------
    {
        se::PadObs pa{};
        pa.addr = 0x1000;
        pa.centre[0] = -20.0f;
        pa.radius = 30.0f;
        pa.size = se::PadSize::kMedium;
        pa.up[2] = -1.0f;
        pa.upOk = true;
        std::snprintf(pa.name, sizeof(pa.name), "%s", "LandingPadMediumA");
        se::PadObs pb{};
        pb.addr = 0x2000;
        pb.centre[0] = 20.0f;
        pb.radius = 30.0f;
        pb.size = se::PadSize::kMedium;
        pb.up[2] = 1.0f;
        pb.upOk = true;
        std::snprintf(pb.name, sizeof(pb.name), "%s", "LandingPadMediumB");
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        const float camMid[3] = {0.0f, 0.0f, 0.0f};    // 20 m from each: an exact tie
        const float camB[3] = {10.0f, 0.0f, 0.0f};     // B 10 m, A 30 m: not a tie
        const float camNearB[3] = {4.0f, 0.0f, 0.0f};  // B 16 m, A 24 m: B nearer by 8 m, not a tie
        se::PadObs ordAB[2] = {pa, pb};
        se::PadObs ordBA[2] = {pb, pa};
        se::OwnCell c1, c2, c3;
        se::pickOwnPad(ordAB, 2, camMid, radii, &c1);
        se::pickOwnPad(ordBA, 2, camMid, radii, &c2);
        check(c1.has && c1.pad == 0x1000 && c2.has && c2.pad == 0x1000, "fix 1: a tie within 5 m goes to the platform named A, whatever the record order");
        se::pickOwnPad(ordAB, 2, camB, radii, &c3);
        check(c3.has && c3.pad == 0x2000, "fix 1: no tie (B nearer by 20 m): the nearest pad wins, not the one named A");

        se::OwnPadSticky st;
        se::OwnCell s;
        se::pickOwnPadSticky(ordAB, 2, camMid, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "fix 1: the first pick, from the tie, is A");
        se::pickOwnPadSticky(ordBA, 2, camNearB, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "fix 1: sticky: A stays the own pad while its record is seen, though B is now nearer");
        se::PadObs onlyB[1] = {pb};
        bool anyPick = false;
        for (int f = 1; f <= 29; ++f) {
            se::pickOwnPadSticky(onlyB, 1, camNearB, radii, &st, &s);
            if (s.has) anyPick = true;
        }
        check(!anyPick, "fix 1: sticky: A unseen for 29 frames: no other pad is picked yet");
        se::pickOwnPadSticky(onlyB, 1, camNearB, radii, &st, &s);
        check(s.has && s.pad == 0x2000, "fix 1: sticky: A unseen for kHeldLostFrames (30): B takes over");

        se::ExplicitObs ex[4] = {};
        ex[0].addr = 1;
        ex[0].centre[0] = 2.0f;
        std::snprintf(ex[0].name, sizeof(ex[0].name), "%s", "MediumHangerSkirt");
        ex[1].addr = 2;
        ex[1].centre[0] = 3.0f;
        std::snprintf(ex[1].name, sizeof(ex[1].name), "%s", "HangarGate_Ramp1_1");
        ex[2].addr = 3;
        ex[2].centre[0] = 40.0f;
        std::snprintf(ex[2].name, sizeof(ex[2].name), "%s", "HangerMedium");
        ex[3].addr = 4;
        ex[3].centre[0] = 60.0f;
        std::snprintf(ex[3].name, sizeof(ex[3].name), "%s", "HangerLarge");
        const float padC[3] = {0.0f, 0.0f, 0.0f};
        check(!se::shellNameUsable("MediumHangerSkirt") && !se::shellNameUsable("HangarGate_Ramp1_1") && se::shellNameUsable("HangerMedium"),
              "fix 2: a skirt, a gate and a ramp are never the shell; a hanger is");
        check(se::pickShell(ex, 4, padC, se::PadSize::kMedium, 150.0f) == 2, "fix 2: the medium pad's HangerMedium, though the skirt and the gate are nearer");
        check(se::pickShell(ex, 4, padC, se::PadSize::kSmall, 150.0f) == 2, "fix 2: a small pad with no HangerSmall takes the nearest other sized hanger");
        check(se::pickShell(ex, 2, padC, se::PadSize::kMedium, 150.0f) == 2, "fix 2: with only the skirt and the gate there is no shell (n)");
    }

    // ---- Built v14.1: the unsealed gathering path (the store observer while docked) and the waiting line ----------------------------------
    {
        check(se::storeMode(false, false, true) == se::StoreMode::kNone && se::storeMode(true, false, false) == se::StoreMode::kNone,
              "v14.1: setting off, or undocked and unsealed, the store observer does nothing");
        check(se::storeMode(true, false, true) == se::StoreMode::kGather, "v14.1: docked and unsealed, the store observer gathers (no judgement)");
        check(se::storeMode(true, true, false) == se::StoreMode::kJudge && se::storeMode(true, true, true) == se::StoreMode::kJudge,
              "v14.1: sealed, the store observer judges, docked or not");

        // The v14 flight's chicken and egg: with no pad gathered, pickOk stays false and the seal is never requested.
        se::SealState stNoPad;
        se::SealInputs noPad = quiet(0);
        noPad.pickOk = false;
        check(firstEngage(&stNoPad, noPad, 0, 20000) == 0, "v14.1: no pad gathered (pickOk false): the seal is never requested");
        se::SealState stGathered;
        check(firstEngage(&stGathered, quiet(0), 100, 20000) == 100,
              "v14.1: with the pad gathered while docked, the pad at rest requests the load-in seal on the first eligible frame");

        char line[320] = {};
        se::WaitView unknownNoPad;
        se::waitingLine(unknownNoPad, 6.0f, line, sizeof(line));
        check(std::strstr(line, "docked=unknown") != nullptr && std::strstr(line, "pad=none") != nullptr && std::strstr(line, "cell=failed:") != nullptr,
              "v14.1: the waiting line names an unknown docked state, no pad, and a failed cell");
        se::WaitView v;
        v.dockedKnown = true;
        v.docked = true;
        v.havePad = true;
        std::snprintf(v.padName, sizeof(v.padName), "%s", "LandingPadMediumA");
        v.atLowest = true;
        v.stillS = 2.5f;
        v.cellBuilt = true;
        se::waitingLine(v, 6.0f, line, sizeof(line));
        check(std::strstr(line, "docked=yes") != nullptr && std::strstr(line, "pad=LandingPadMediumA") != nullptr &&
                  std::strstr(line, "at lowest=yes") != nullptr && std::strstr(line, "still for 2.5 of 6 s") != nullptr &&
                  std::strstr(line, "cell=built") != nullptr,
              "v14.1: the waiting line names docked, the pad, at lowest, the still time of the delay, and the cell");
        se::WaitView v2 = v;
        v2.stillS = 3.0f;
        check(!se::waitingKeyChanged(v, v2), "v14.1: the still time alone is not a state change (no line per frame)");
        v2 = v;
        v2.atLowest = false;
        check(se::waitingKeyChanged(v, v2), "v14.1: at lowest changing is a state change");
        v2 = v;
        v2.docked = false;
        check(se::waitingKeyChanged(v, v2), "v14.1: docked changing is a state change");
        v2 = v;
        v2.cellBuilt = false;
        std::snprintf(v2.cellReason, sizeof(v2.cellReason), "%s", "no pad height");
        se::waitingLine(v2, 6.0f, line, sizeof(line));
        check(se::waitingKeyChanged(v, v2) && std::strstr(line, "cell=failed:no pad height") != nullptr,
              "v14.1: a cell that fails names its reason, and is a state change");

        // Built v14.2: the evidence in the line (records gathered, camera, the pads with distance and containment).
        se::WaitView ev = v;
        ev.gathered = 5;
        ev.cameraKnown = true;
        ev.padCount = 1;
        std::snprintf(ev.padEv[0].name, sizeof(ev.padEv[0].name), "%s", "LandingPadLargeA");
        ev.padEv[0].d = 140.2f;
        ev.padEv[0].contains = true;
        se::waitingLine(ev, 6.0f, line, sizeof(line));
        check(std::strstr(line, "gathered=5") != nullptr && std::strstr(line, "camera=yes") != nullptr &&
                  std::strstr(line, "LandingPadLargeA d=140.2 contains=yes") != nullptr,
              "v14.2: the waiting line names the records gathered, the camera, and each pad's distance and containment");
        se::WaitView none = v;
        none.padCount = 0;
        none.cameraKnown = false;
        se::waitingLine(none, 6.0f, line, sizeof(line));
        check(std::strstr(line, "camera=no") != nullptr && std::strstr(line, "pads=0 [none]") != nullptr,
              "v14.2: with no pads and no camera the line says so");

        // The v14.1 flight's fault: the frame stamp never advanced, so no promote ever ran. The stamp must advance each tick.
        {
            uint32_t cellStampStuck = 0, frameStuck = 0;
            int promotesStuck = 0;
            for (int r = 0; r < 3; ++r) {
                if (se::framePromoteDue(cellStampStuck, frameStuck)) {
                    cellStampStuck = frameStuck;
                    ++promotesStuck;
                }
            }
            check(promotesStuck == 0, "v14.2: with the stamp never advanced (the flight's fault), no promote is ever due");
            uint32_t cellStamp = 0, frame = 0;
            int promotes = 0;
            for (int f = 0; f < 3; ++f) {
                ++frame;   // the tick advances the stamp
                for (int r = 0; r < 4; ++r) {
                    if (se::framePromoteDue(cellStamp, frame)) {
                        cellStamp = frame;
                        ++promotes;
                    }
                }
            }
            check(promotes == 3, "v14.2: the stamp advanced once per tick: one promote per frame, at its first record");
        }
    }

    // ---- Built v14.4: the flicker fix (the reference, the hysteresis, the sequence after a motion release, the safety net) ----------------------
    {
        // (1) A static pad, its reference read from one source. A later read of the same pad from another source is a different centre (a constant
        // offset in the two computations): it is not compared, and it never releases. The same source, unchanged, never releases.
        se::ShellMotion sm;
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float padAt[3] = {0.0f, 0.0f, 10.0f};
        const float padOff[3] = {0.0f, 0.0f, 7.6f};   // the other source reads 2.4 m lower: the observed -2.4 m constant
        check(!sm.sample(padAt, 1, shell, 2, up) && sm.captured, "v14.4: the reference is captured at the first sample");
        bool released = false;
        for (int k = 0; k < 200; ++k) released = released || sm.sample(padAt, 1, shell, 2, up);
        check(!released && sm.delta == 0.0f, "v14.4: a static pad read from the same source never releases (200 samples)");
        released = false;
        for (int k = 0; k < 200; ++k) released = released || sm.sample(padOff, 0, shell, 2, up);
        check(!released, "v14.4: a static pad read from another source (a constant -2.4 m computation difference) never releases");
    }
    {
        // (2) Hysteresis: a pad 3 m off its reference releases only on the tenth consecutive sample; a single sample never releases.
        se::ShellMotion sm;
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float padAt[3] = {0.0f, 0.0f, 10.0f};
        const float padMoved[3] = {0.0f, 0.0f, 13.0f};
        sm.sample(padAt, 1, shell, 2, up);
        bool early = false;
        for (int k = 0; k < 9; ++k) early = early || sm.sample(padMoved, 1, shell, 2, up);
        check(!early, "v14.4: 3 m off the reference for nine samples does not release");
        check(sm.sample(padMoved, 1, shell, 2, up), "v14.4: 3 m off the reference for ten consecutive samples releases");
        se::ShellMotion one;
        one.sample(padAt, 1, shell, 2, up);
        check(!one.sample(padMoved, 1, shell, 2, up) && !one.sample(padAt, 1, shell, 2, up), "v14.4: a single sample off the reference never releases");
    }
    {
        // (3) The sequence: load-in seal -> motion release -> no immediate re-seal. The release runs sealStep (which records the motion) and then
        // clearTrigger, as the engine's releaseSeal does; the next frame must wait the delay.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        check(se::sealStep(quiet(100), &st, &w) == se::SealAction::kRequestEngage && st.immediate,
              "v14.4 sequence: the pad first seen at rest seals at load-in");
        se::SealInputs rel = quiet(150);
        rel.held = true;
        rel.padMoved = true;
        check(se::sealStep(rel, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kPadMoved, "v14.4 sequence: the pad moved: released");
        se::clearTrigger(&st);
        check(st.moved && st.seen, "v14.4 sequence: the motion memory survives the release");
        check(se::sealStep(quiet(200), &st, &w) == se::SealAction::kNone && !st.immediate,
              "v14.4 sequence: no immediate re-seal after a motion release (the first frame after it waits)");
        check(firstEngage(&st, quiet(0), 200, 20000) == 200 + se::kSealDelayMs && !st.immediate,
              "v14.4 sequence: the re-seal after a motion release comes after the full delay");
    }
    {
        // (4) The safety net: more than six seals in a minute trips it; spaced seals never do; a reset (a hangar change or a setting toggle) clears it.
        se::SealRateGuard g;
        bool any = false;
        for (uint64_t i = 0; i < 6; ++i) any = any || g.note(i * 1000);
        check(!any && !g.tripped, "v14.4 safety net: six seals in a minute do not trip it");
        check(g.note(6000) && g.tripped, "v14.4 safety net: the seventh seal in a minute trips it");
        se::SealRateGuard h;
        bool spaced = false;
        for (uint64_t i = 0; i < 12; ++i) spaced = spaced || h.note(i * 70000);
        check(!spaced, "v14.4 safety net: seals spaced over a minute apart never trip it");
        g = se::SealRateGuard{};
        check(!g.tripped && !g.note(7000), "v14.4 safety net: a reset clears it");
    }

    // ---- The rules the seal relies on -------------------------------------------------------------------------------------------------------------
    check(se::kPadMoveReleaseM == 2.0f && se::kCameraLeaveM == 2.0f && se::kSealStillM == 0.2f && se::kSealDelayMs == 6000,
          "the constants: the pad-move release 2 m, the camera 2 m, the still 0.2 m, the seal delay 6 s");
    {
        float v = -1.0f;
        check(se::parseBelowM("", se::kBelowDefaultsM[0], &v) && v == 85.0f && se::kBelowDefaultsM[0] == 85.0f,
              "the small floor defaults to 85 m (the lift doors at -72 m)");
        check(se::belowForSize(se::PadSize::kMedium, se::kBelowDefaultsM, false, 0.0f) == 120.0f &&
                  se::belowForSize(se::PadSize::kLarge, se::kBelowDefaultsM, false, 0.0f) == 145.0f,
              "the medium floor is 120 m and the large 145 m (the measured doors plus margin)");
        check(se::parseBelowM("45", 85.0f, &v) && v == 45.0f && !se::parseBelowM("sixty", 85.0f, &v) && !se::parseBelowM("201", 85.0f, &v),
              "a floor parses as metres in [0, 200]; text and 201 are refused");
    }
    {
        const float rawOk[3] = {10.0f, 2.0f, -3.0f}, rawZero[3] = {0.0f, 0.0f, 0.0f};
        check(se::rawCentreUsable(rawOk, 5.0f) && !se::rawCentreUsable(rawZero, 5.0f) && !se::rawCentreUsable(rawOk, 0.0f),
              "an unreadable centre (the origin) is not usable: such a record is kept, never judged");
    }
    check(se::ownPadCandidate(50.0f, 60.0f) && se::ownPadCandidate(39.0f, 0.0f) && !se::ownPadCandidate(41.0f, 0.0f),
          "a pad is an own pad candidate when the camera is inside its sphere or its centre is within 40 m");
    check(!se::heldCellDrops(29, 29, se::kHeldLostFrames) && se::heldCellDrops(30, 0, se::kHeldLostFrames) &&
              se::heldCellDrops(0, 30, se::kHeldLostFrames),
          "the held cell drops at 30 consecutive frames of absence or of another own pad");
    {
        char stripped[48] = {};
        check(se::strippedPrefix("CobraMkV_", stripped, sizeof(stripped)) && std::strcmp(stripped, "CobraMkV") == 0 &&
                  !se::strippedPrefix("CobraMkV", stripped, sizeof(stripped)),
              "the own record's name is the prefix without its underscore");
        check(se::ownRecordMatch("CobraMkV", "CobraMkV") && !se::ownRecordMatch("CobraMkV_Door", "CobraMkV"),
              "the own record matches its exact name only");
        const float oc[3] = {0.0f, 0.0f, 0.0f}, in1[3] = {34.0f, 0.0f, 0.0f}, in2[3] = {36.0f, 0.0f, 0.0f};
        check(se::ownSphereKeeps(in1, oc, 30.0f, 5.0f) && !se::ownSphereKeeps(in2, oc, 30.0f, 5.0f),
              "a record within the own record's live sphere plus 5 m is kept; 36 m from a 30 m sphere is not");
    }
    {
        const float o[3] = {0.0f, 0.0f, 0.0f}, u[3] = {0.0f, 0.0f, 1.0f};
        const float p1[3] = {0.0f, 0.0f, 50.0f}, p2[3] = {61.0f, 0.0f, 50.0f}, p3[3] = {60.0f, 0.0f, 50.0f};
        const float p4[3] = {0.0f, 0.0f, -90.0f}, p5[3] = {0.0f, 0.0f, -80.0f};
        check(se::inBayVolume(p1, o, u, 100.0f, 85.0f) && !se::inBayVolume(p2, o, u, 100.0f, 85.0f) && se::inBayVolume(p3, o, u, 100.0f, 85.0f),
              "the bay: on the shaft is in; 61 m lateral is out; 60 m lateral is in");
        check(!se::inBayVolume(p4, o, u, 100.0f, 85.0f) && se::inBayVolume(p5, o, u, 100.0f, 85.0f),
              "the bay: 90 m below the pad is out of an 85 m floor; 80 m below is in");
        check(se::bayPartName("Hangar_Small") && se::bayPartName("TurboLiftDoor") && se::bayPartName("LandingPad_Large") &&
                  !se::bayPartName("CobraMkV_") && !se::bayPartName("SocialSpace"),
              "the bay keeps the hangar's part names (Hangar, LandingPad, lift doors, ...) and not a ship's or the concourse's");
    }

    // ---- The pad-local frame and the release by the shell
    {
        // Built v13.7 (a): the pad frame. A rotated pad at (10, 2, 3): a point taken to the pad's frame and back is itself; the cell placed back
        // after a frame shift moves by exactly the shift; a pad turned about up turns the box's directions with it, and a point at a fixed place on
        // the pad keeps that place on the pad.
        const float th = 0.5235988f;   // 30 degrees
        const float axesA[3][3] = {{std::cos(th), std::sin(th), 0.0f}, {-std::sin(th), std::cos(th), 0.0f}, {0.0f, 0.0f, 1.0f}};
        const float padC[3] = {10.0f, 2.0f, 3.0f};
        const float p0[3] = {14.0f, -5.0f, 20.0f};
        float loc0[3] = {}, back0[3] = {};
        se::pointToLocal(p0, padC, axesA, loc0);
        se::pointToWorld(loc0, padC, axesA, back0);
        check(std::fabs(back0[0] - p0[0]) < 1e-3f && std::fabs(back0[1] - p0[1]) < 1e-3f && std::fabs(back0[2] - p0[2]) < 1e-3f,
              "a point taken to the rotated pad's frame and back is the same point");
        check(std::fabs(loc0[2] - 17.0f) < 1e-3f, "a point 17 m above the pad's centre is 17 m along the pad's up axis");

        // A cell with a shaft, a box, frozen at engage; then the pad's live frame is shifted by T (the record frame moves).
        se::OwnCell cellW;
        cellW.has = true;
        cellW.centre[0] = padC[0]; cellW.centre[1] = padC[1]; cellW.centre[2] = padC[2];
        cellW.capA[0] = padC[0]; cellW.capA[1] = padC[1]; cellW.capA[2] = padC[2];
        cellW.capB[0] = 12.0f; cellW.capB[1] = 40.0f; cellW.capB[2] = 90.0f;
        cellW.capsule = true;
        cellW.box = true;
        cellW.boxCentre[0] = 9.0f; cellW.boxCentre[1] = 3.0f; cellW.boxCentre[2] = 60.0f;
        cellW.boxU[0] = std::cos(th); cellW.boxU[1] = std::sin(th);
        cellW.boxV[0] = -std::sin(th); cellW.boxV[1] = std::cos(th);
        cellW.boxW[2] = 1.0f;
        cellW.boxHalf[0] = 33.0f; cellW.boxHalf[1] = 14.0f; cellW.boxHalf[2] = 60.0f;
        se::OwnCell cellL;
        se::localizeCell(cellW, padC, axesA, &cellL);
        const float T[3] = {-5.18f, -2.99f, 34.0f};   // the v13.6 flight's shift
        const float padS[3] = {padC[0] + T[0], padC[1] + T[1], padC[2] + T[2]};
        se::OwnCell cellS = cellW;
        se::worldizeCell(cellL, padS, axesA, &cellS);
        check(std::fabs(cellS.centre[0] - padS[0]) < 1e-3f && std::fabs(cellS.centre[2] - padS[2]) < 1e-3f,
              "the pad-frame cell's centre follows the pad's live centre after a frame shift");
        check(std::fabs(cellS.capB[0] - (cellW.capB[0] + T[0])) < 1e-3f && std::fabs(cellS.capB[1] - (cellW.capB[1] + T[1])) < 1e-3f &&
                  std::fabs(cellS.capB[2] - (cellW.capB[2] + T[2])) < 1e-3f,
              "the shaft target moves with the frame shift, by the same offset as the pad");
        check(std::fabs(cellS.boxCentre[2] - (cellW.boxCentre[2] + T[2])) < 1e-3f && std::fabs(cellS.boxU[0] - cellW.boxU[0]) < 1e-5f &&
                  std::fabs(cellS.boxHalf[2] - 60.0f) < 1e-6f,
              "the box moves by the shift; its axes and half-extents do not change under a pure shift");

        // A pad turned 90 degrees about up: the box's length axis (world +x at 30 degrees, which is the pad's own axis 0) turns with the pad to
        // world +y; the shaft target keeps its height along the pad's up axis.
        const float axesR[3][3] = {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        se::OwnCell cellT = cellW;
        se::worldizeCell(cellL, padC, axesR, &cellT);
        check(std::fabs(cellT.boxU[0]) < 1e-3f && std::fabs(cellT.boxU[1] - 1.0f) < 1e-3f,
              "a pad turned 90 degrees turns the box's length axis with it (to world +y)");
        const float rotCap[3] = {cellT.capB[0] - padC[0], cellT.capB[1] - padC[1], cellT.capB[2] - padC[2]};
        check(std::fabs(rotCap[2] - cellL.capB[2]) < 1e-3f,
              "the shaft target keeps its height along the pad's up axis when the pad turns about up");

        // Built v13.7 (b): the release by the pad's own motion along the shaft, against the hangar shell. A shift of the whole frame (pad and shell
        // alike) changes nothing; the pad rising 6 m relative to the shell releases; rising 4 m or 5 m does not (the limit is more than 5 m).
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shellE[3] = {0.0f, 0.0f, 50.0f}, padE[3] = {0.0f, 0.0f, 10.0f};
        const float relE = se::shellRelative(padE, shellE, up);
        const float shellS[3] = {T[0], T[1], 50.0f + T[2]}, padSh[3] = {T[0], T[1], 10.0f + T[2]};
        check(!se::padMovedAway(relE, se::shellRelative(padSh, shellS, up)),
              "a frame shift that moves the pad and the shell alike does not release the cell");
        const float padUp6[3] = {0.0f, 0.0f, 13.0f}, padUp4[3] = {0.0f, 0.0f, 11.5f}, padUp5[3] = {0.0f, 0.0f, 12.0f};
        check(se::padMovedAway(relE, se::shellRelative(padUp6, shellE, up)) && !se::padMovedAway(relE, se::shellRelative(padUp4, shellE, up)) &&
                  !se::padMovedAway(relE, se::shellRelative(padUp5, shellE, up)),
              "the pad rising 3 m against the shell releases the cell (the limit is 2 m); 1.5 m and exactly 2 m do not");
        check(!se::padMovedAway(NAN, 0.0f), "an unreadable reference never releases");
    }

    std::printf("optimized_station_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
