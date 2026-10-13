// Test rig for the optimized station's pure half (src/d3d11/optimized_station_core.h): the seal trigger (the state machine that seals, releases
// and re-seals the station), the release rules (setting off, undocked, Explorer Cam, the camera leaving the bay, the pad moving against the hangar
// shell, a hangar change, launch), the pad-local frame's round trip and the release's threshold, the parse of the per-size floors, the own-ship
// prefix and sphere, the bay's volume, and the unreadable rule. No DLL and no game.
#include "../../src/d3d11/optimized_station_core.h"
#include "../../src/d3d11/explorer_cam_core.h"
#include "../../src/d3d11/station_lockfree_core.h"

#include <atomic>
#include <thread>
#include <vector>   // the build-332841 PE identity constants (store hook gate)

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

// The seal's inputs for a docked station whose own pad the game says is in the hangar (r1 1, fresh), with the camera in the bay.
se::SealInputs inHangar(uint64_t now) {
    se::SealInputs in;
    in.setting = true;
    in.docked = true;
    in.ownKnown = true;
    in.r1Fresh = true;
    in.inHangar = true;
    in.pickOk = true;
    in.cameraInBox = true;
    in.pad = 0x1000;
    in.nowMs = now;
    return in;
}

// Runs the trigger from t0 to t1 every 100 ms with the same inputs; returns the first engage request's time, or 0 when none came.
uint64_t firstEngage(se::SealState* st, se::SealInputs in, uint64_t t0, uint64_t t1) {
    se::ReleaseWhy why = se::ReleaseWhy::kNone;
    for (uint64_t t = t0; t <= t1; t += 100) {
        in.nowMs = t;
        if (se::sealStep(in, st, &why) == se::SealAction::kRequestEngage) return t;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: optimized_station_test --self-test\n");
        return 2;
    }

    // ---- Built v14.23 (2026-10-13): the seal is the game's pad state (r1 1 on the own pad). The geometric trigger has no tests any more ---------------
    {
        // Load-in: the game says the own pad is in the hangar (r1 1, fresh) and the camera is in the bay: the seal comes on the first eligible frame.
        se::SealState st;
        check(firstEngage(&st, inHangar(0), 100, 10000) == 100, "v14.23 load-in: the own pad in the hangar by the game (r1 1) seals on the first eligible frame (100 ms)");
    }
    {
        // The surface of an outpost: r0 0 and r1 0. The pad never seals (no fallback), and the sun stays on.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.inHangar = false;
        check(firstEngage(&st, in, 0, 60000) == 0, "v14.23 surface: r0 0 and r1 0 (outside the hangar) never seals, with no fallback");
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 0;
        si.r1Fresh = true;
        si.inHangar = false;
        si.relOk = true;
        si.rel = -53.5f;
        const char* why = "";
        check(!se::sunStep(si, &sun, &why) && !sun.off, "v14.23 surface: the sun stays on at the outpost surface (r0 0)");
    }
    {
        // A descent: r0 3 (the descent has started) turns the sun off; the pad is not yet in the bay, so no seal.
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 3;
        si.r1Fresh = true;
        si.inHangar = false;
        si.relOk = true;
        si.rel = -20.0f;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off && std::strcmp(why, "pad moving in or locked in (r0)") == 0,
              "v14.23 descent: r0 3 turns the sun off");
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.inHangar = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "v14.23 descent: r1 0 during the descent: no seal");
    }
    {
        // The lock: r1 turns 1 (the game says the pad is in the hangar): the seal comes at once; r0 1 turns the sun off.
        se::SealState st;
        check(firstEngage(&st, inHangar(0), 4000, 10000) == 4000, "v14.23 lock: r1 1 on the own pad seals at once (the first eligible frame)");
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 1;
        si.r1Fresh = true;
        si.inHangar = true;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 lock: r0 1 (moving in) turns the sun off");
    }
    {
        // Toggle off and on while r1 is 1: the bay re-seals at once (no edge is needed for a setting release).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(100);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 toggle: the seal in the hangar");
        in.held = true;
        in.setting = false;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kSettingOff, "v14.23 toggle: the setting off releases");
        in.held = false;
        in.setting = true;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 toggle: the setting on again, with r1 1, re-seals at once");
    }
    {
        // A mechanism release (the launch) while r1 stays 1: no re-seal for 2 s of r1 1; r1 0 then 1 (the fresh edge) re-seals.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 mechanism: the setup seal");
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism, "v14.23 mechanism: the bay mechanism moving releases");
        in.held = false;
        in.mechanismMoved = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 2500; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.23 mechanism: with r1 still 1 after the launch release, nothing re-seals (2 s)");
        in.inHangar = false;
        in.nowMs = 2600;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "v14.23 mechanism: r1 0 is the first half of the fresh edge (no seal)");
        in.inHangar = true;
        in.nowMs = 2700;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 mechanism: r1 back to 1 after the 0 re-seals");
    }
    {
        // An r1 1 to 0 edge (the pad leaves the bay) releases the seal.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.inHangar = false;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kInHangarLeft, "v14.23 edge: r1 1 to 0 releases the seal");
    }
    {
        // The camera leaves and re-enters the bay: the seal re-takes without any in-hangar edge (r1 stayed 1).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.cameraOutside = true;
        bool left = false;
        for (uint64_t t = 100; t <= 1000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kCameraLeft) left = true;
        }
        check(left, "v14.23 camera: the camera outside the box for kCameraFrames releases the seal");
        in.held = false;
        in.cameraOutside = false;
        in.cameraInBox = false;
        in.nowMs = 1100;
        se::sealStep(in, &st, &w);
        uint64_t back = 0;
        for (uint64_t t = 1200; t <= 4000; t += 100) {
            in.cameraInBox = true;
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && back == 0) back = t;
        }
        check(back != 0, "v14.23 camera: the camera back in the box re-seals with r1 still 1 (no edge needed)");
    }
    {
        // A request made for one pad is never admitted for another; the trigger asks again for the new pad.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs a = inHangar(0);
        a.pad = 0x1000;
        se::sealStep(a, &st, &w);
        se::EngageRequest reqA{true, 0x1000, 0x5000};
        se::SealInputs b = inHangar(100);
        b.pad = 0x2000;
        const bool askedB = se::sealStep(b, &st, &w) == se::SealAction::kRequestEngage;
        se::engageTrigger(&reqA, st);
        const se::EngageRequest reqB{true, 0x2000, 0x5000};
        check(askedB && !se::engageAdmitted(reqA, st, 0x2000, 0x5000, true) && se::engageAdmitted(reqB, st, 0x2000, 0x5000, true),
              "v14.23 engage: a request bound to pad A is never admitted for pad B; the new request for B is");
    }
    {
        // The own pad is unknown (no r0 on this dock): no seal, and the waiting line says so.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.ownKnown = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "v14.23 unknown: no own pad from the game (no r0 this dock): no seal");
        se::GameView w;
        w.dockedKnown = true;
        w.docked = true;
        w.cameraKnown = true;
        w.cameraIn = true;
        w.cellBuilt = true;
        char line[480] = {};
        se::gameWaitingLine(w, line, sizeof(line));
        check(std::strstr(line, "game pad state unknown") != nullptr, "v14.23 unknown: the waiting line says game pad state unknown");
        se::GameView k = w;
        k.ownKnown = true;
        k.own = 0xABC40;
        k.r0 = 1;
        k.r1 = 1;
        k.r0Fresh = true;
        k.r1Fresh = true;
        se::gameWaitingLine(k, line, sizeof(line));
        check(std::strstr(line, "own pad @0abc40, r0=1 r1=1, in hangar=yes") != nullptr, "v14.23 waiting line: the own pad's key and codes, and in hangar yes");
    }
    {
        // The own pad: the object r0 was last called on this dock; a reset at undock or a hangar change; an r0 from before the reset names nothing.
        se::GameTrack tr;
        check(tr.observe(false, 0, 0) == se::GameTrackEvent::kNone && !tr.have, "v14.23 own: no r0 yet: no own pad");
        check(tr.observe(true, 0xA000, 100) == se::GameTrackEvent::kFirst && tr.own == 0xA000, "v14.23 own: the first r0 this dock names the own pad");
        check(tr.observe(true, 0xA000, 200) == se::GameTrackEvent::kNone, "v14.23 own: the same object again: no event");
        check(tr.observe(true, 0xB000, 300) == se::GameTrackEvent::kSwitched && tr.own == 0xB000,
              "v14.23 own: r0 on another object switches the own pad (one event)");
        tr.reset(400);
        check(!tr.have, "v14.23 own: undock or a hangar change resets the own pad");
        check(tr.observe(true, 0xB000, 350) == se::GameTrackEvent::kNone && !tr.have,
              "v14.23 own: an r0 call from before the reset names no own pad");
        check(tr.observe(true, 0xB000, 450) == se::GameTrackEvent::kFirst, "v14.23 own: an r0 call after the reset names the own pad");
    }
    {
        // The sun after a lock: r0 1 is off; the pad in the bay (r1 1) holds it; out and raised turns it on; undock turns it on.
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 1;
        si.r1Fresh = true;
        si.inHangar = true;
        si.relOk = true;
        si.rel = -45.0f;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: r0 1 turns the skip on");
        si.r0 = 0;
        check(!se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: the pad still in the bay (r1 1) holds the skip after r0 stops");
        si.inHangar = false;
        si.rel = 5.0f;
        check(se::sunStep(si, &sun, &why) && !sun.off && std::strcmp(why, "pad raised (launch)") == 0,
              "v14.23 sun: the pad out of the bay (r1 0, r0 not 1 or 3) and raised turns the sun back on");
        si.r0 = 1;
        si.inHangar = false;
        si.rel = -45.0f;
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: moving again turns it off");
        si.docked = false;
        check(se::sunStep(si, &sun, &why) && !sun.off && std::strcmp(why, "undocked") == 0, "v14.23 sun: undock turns it on");
    }
    // The pure helpers that are not about the seal trigger (kept): the latch's identity, the bounded probe, the rig's epoch rule, the view's centre.
    {
        se::LatchIdentity id;
        id.bind(0xB0, 0x5000);
        check(!id.step(false, 0, true, 0x5000), "v14.20 latch: a sample that misses the pad keeps the identity: no change");
        check(id.step(true, 0xA0, true, 0x5000) && id.pad == 0xA0, "v14.20 latch: pad B replaced by pad A is a genuine change: rebuild");
        se::LatchSet ls;
        ls.n = 2;
        ls.latched = true;
        ls.recs[0].movedRun = 2;
        ls.recs[1].movedRun = 3;
        ls.reset();
        check(ls.movedIndex() == -1 && ls.recs[1].movedRun == 0, "v14.20 latch: a reset clears the records' motion references");
        const uint8_t outside = static_cast<uint8_t>(se::RigVerdict::kOutside);
        check(se::rigMayVeto(3, 3, true, outside) && !se::rigMayVeto(3, 4, true, outside) && !se::rigMayVeto(4, 4, false, outside),
              "v14.20 rig: an outside bound from the current epoch may veto; an old epoch or an unbounded slot forwards");
        check(se::viewCentreReady(true, false, false) && !se::viewCentreReady(false, true, false) && se::viewCentreReady(false, true, true),
              "v14.20 view: the origin is the centre; an eye pose alone is not");
        se::SealState s2;
        check(s2.reseal == 0 && !s2.requested, "v14.23: a fresh trigger holds no re-seal and requests nothing");
    }
    {
        // The bounded probe: at the bound, a full run is exhausted (no walk of the table); a stale slot is free.
        struct Slot {
            uintptr_t key = 0;
            uint32_t gen = 0;
        };
        constexpr size_t kCap = 64;
        Slot table[kCap] = {};
        uint32_t cur = 1;
        auto matchesKey = [&](size_t i, uintptr_t key) { return table[i].gen == cur && table[i].key == key; };
        auto staleSlot = [&](size_t i) { return table[i].gen != cur; };
        for (uintptr_t k = 1; k <= se::kTableProbeMax; ++k) {
            const se::ProbeResult p = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, k); }, staleSlot);
            if (p.empty) {
                table[p.index].key = k;
                table[p.index].gen = cur;
            }
        }
        const se::ProbeResult miss = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, 99); }, staleSlot);
        check(miss.exhausted && miss.probes == se::kTableProbeMax, "v14.20 table: at capacity a missing key is a miss after the bound (32 probes)");
        cur = 2;
        const se::ProbeResult fresh = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, 99); }, staleSlot);
        check(fresh.empty && fresh.probes == 1, "v14.20 table: a slot of an earlier epoch is free");
    }
    {
        // The store hook's identity decision (kept): the PE pair first, then the pin.
        const uint8_t pin[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFE, 0xFF};
        const uint32_t wantStamp = edvr::ecm::kExpectedTimestamp, wantSize = edvr::ecm::kExpectedImageSize;
        check(se::storeInstallDecision(true, wantStamp, wantSize, wantStamp, wantSize, pin, pin, 16) == se::StoreInstall::kInstall,
              "v14.20 store hook: build 332841's identity and pin install the detour");
        check(se::storeInstallDecision(true, wantStamp + 1, wantSize, wantStamp, wantSize, pin, pin, 16) == se::StoreInstall::kWrongIdentity,
              "v14.20 store hook: another stamp is refused");
    }
    {
        // The shell frame (kept): the pad 3 m along x and 4 m along y from the shell, 45 m below it; forward +y against +x: yaw +90 deg about up.
        const float shellO[3] = {0.0f, 0.0f, 0.0f};
        const float upZ[3] = {0.0f, 0.0f, 1.0f};
        const float shFwdX[3] = {1.0f, 0.0f, 0.0f};
        const float padP[3] = {3.0f, 4.0f, -45.0f};
        const float padFwdY[3] = {0.0f, 1.0f, 0.0f};
        const se::ShellFrame sf = se::shellFrameOf(padP, shellO, upZ, shFwdX, padFwdY);
        check(sf.ok && std::fabs(sf.z + 45.0f) < 1e-4f && std::fabs(sf.lateral - 5.0f) < 1e-4f && sf.yawOk && std::fabs(sf.yawDeg - 90.0f) < 1e-3f,
              "v14.17 shell frame: z is the rel reading, lateral is the distance from the shaft, and yaw is +90 deg");
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
        check(c3.has && c3.pad == 0x1000, "v14.8: the B platform is not picked while the A platform of its size is within 60 m (the nearer B loses)");

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


    // ---- Built v14.8: the lift platform B, the cell validity, the ship part count, the still clock across a source change ----------------
    {
        se::PadObs pa{};
        pa.addr = 0x1000;
        pa.centre[0] = -20.0f;
        pa.radius = 30.0f;
        pa.size = se::PadSize::kMedium;
        std::snprintf(pa.name, sizeof(pa.name), "%s", "LandingPadMediumA");
        se::PadObs pb{};
        pb.addr = 0x2000;
        pb.centre[0] = 20.0f;
        pb.radius = 30.0f;
        pb.size = se::PadSize::kMedium;
        std::snprintf(pb.name, sizeof(pb.name), "%s", "LandingPadMediumB");
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        const float camB[3] = {10.0f, 0.0f, 0.0f};   // B 10 m, A 30 m
        se::PadObs ordAB[2] = {pa, pb};
        se::OwnCell c;
        se::pickOwnPad(ordAB, 2, camB, radii, &c);
        check(c.has && c.pad == 0x1000, "v14.8: the nearer B platform loses to the A platform of its size within 60 m");
        se::PadObs farA = pa;
        farA.centre[0] = -80.0f;   // A 90 m from the camera
        se::PadObs ordFar[2] = {farA, pb};
        se::pickOwnPad(ordFar, 2, camB, radii, &c);
        check(c.has && c.pad == 0x2000, "v14.8: beyond 60 m the A platform does not count: B is picked");
        se::OwnPadSticky st;
        se::OwnCell s;
        se::PadObs onlyB[1] = {pb};
        se::pickOwnPadSticky(onlyB, 1, camB, radii, &st, &s);
        check(s.has && s.pad == 0x2000, "v14.8 sticky: B picked while no A is in the store");
        se::pickOwnPadSticky(ordAB, 2, camB, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "v14.8 sticky: a held B gives way to the A platform within 60 m at once");

        se::OwnCell none;
        check(se::cellInvalidWhy(none) != nullptr, "v14.8 cell: no pad, no seal");
        se::OwnCell noTarget;
        noTarget.has = true;
        check(se::cellInvalidWhy(noTarget) != nullptr, "v14.8 cell: a pad with no shaft target does not seal");
        se::OwnCell zero = noTarget;
        zero.capsule = true;
        zero.box = true;   // the zero box the flight sealed with
        check(se::cellInvalidWhy(zero) != nullptr, "v14.8 cell: a zero box never seals (the flight's 0 x 0 x 0)");
        se::OwnCell ok = zero;
        ok.boxHalf[0] = ok.boxHalf[1] = ok.boxHalf[2] = 12.0f;
        check(se::cellInvalidWhy(ok) == nullptr, "v14.8 cell: a box with every half-extent at least 10 m is a valid cell");
        se::OwnCell thin = ok;
        thin.boxHalf[1] = 5.0f;
        check(se::cellInvalidWhy(thin) != nullptr, "v14.8 cell: a box under 10 m on any axis does not seal");
    }
    {
        // The ship's part count: a real ship has many records near the camera; a boarding volume or a GUI record does not.
        check(se::shipPrefixNamesShip("LakonMiner_", 10), "v14.8 ship: a prefix with ten records near the camera names a ship");
        check(!se::shipPrefixNamesShip("LakonMiner_", 9), "v14.8 ship: nine records are not enough");
        check(!se::shipPrefixNamesShip("ShipBoardingVolume_", 50), "v14.8 ship: a BoardingVolume prefix never names a ship");
        check(!se::shipPrefixNamesShip("GUI_", 50) && !se::shipPrefixNamesShip("GUI_Panel", 50), "v14.8 ship: a GUI_ prefix never names a ship");
        check(!se::shipPrefixNamesShip("", 50) && !se::shipPrefixNamesShip(nullptr, 50), "v14.8 ship: no prefix, no ship");
    }

    // ---- Built v14.10: the bay settles (the watch's motion, the continuous movers, the fallback, the frame shift) --------------------------------
    {
        // The watch's motion test: a frame shift of the record and the shell together is not motion; a rotation over 0.5 deg is; so is a 3 cm move.
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float prev[3] = {5.0f, 6.0f, -45.0f};
        check(!se::poseMoved(prev, prev, a0, a0, a2, a2), "v14.10 watch: a record that does not move is not moved");
        const float v[3] = {123.0f, -40.0f, 900.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float recC[3] = {5.0f, 6.0f, -45.0f};
        const float shellS[3] = {shell[0] + v[0], shell[1] + v[1], shell[2] + v[2]};
        const float recS[3] = {recC[0] + v[0], recC[1] + v[1], recC[2] + v[2]};
        const float rel0[3] = {recC[0] - shell[0], recC[1] - shell[1], recC[2] - shell[2]};
        const float rel1[3] = {recS[0] - shellS[0], recS[1] - shellS[1], recS[2] - shellS[2]};
        check(!se::poseMoved(rel0, rel1, a0, a0, a2, a2), "v14.10 watch: a frame shift of the record and the shell together is not motion");
        const float tilt1[3] = {std::cos(1.0f * 0.0174533f), std::sin(1.0f * 0.0174533f), 0.0f};
        const float tilt03[3] = {std::cos(0.3f * 0.0174533f), std::sin(0.3f * 0.0174533f), 0.0f};
        check(se::poseMoved(prev, prev, a0, tilt1, a2, a2), "v14.10 watch: a 1 deg rotation of an axis is motion");
        check(!se::poseMoved(prev, prev, a0, tilt03, a2, a2), "v14.10 watch: a 0.3 deg rotation is not motion");
        const float near3[3] = {prev[0] + 0.02f, prev[1], prev[2]};
        const float far3[3] = {prev[0] + 0.05f, prev[1], prev[2]};
        check(!se::poseMoved(prev, near3, a0, a0, a2, a2) && se::poseMoved(prev, far3, a0, a0, a2, a2),
              "v14.10 watch: a 2 cm centre move is not motion; a 5 cm move is");
    }

    // ---- Built v14.12: the latch (selection, cap, the by-address still test), the lift sequence, the held pad absence ----------------------
    {
        // Selection: the own pad (A, medium, at the origin); the other medium pad 40 m lateral; a small pad 10 m lateral (no); a medium pad 70 m lateral
        // (no); a Lift within the span (yes); a Lift beyond the span (no); a BlastShieldBase (yes); a HangarGate_Ramp (yes); a container (no); a guide
        // marker pad (no).
        se::LatchCand cands[12] = {};
        auto put = [](se::LatchCand& cd, uintptr_t addr, const char* nm, float x, float y, float z) {
            cd.addr = addr;
            std::snprintf(cd.name, sizeof(cd.name), "%s", nm);
            cd.centre[0] = x;
            cd.centre[1] = y;
            cd.centre[2] = z;
        };
        put(cands[0], 0x2, "LandingPadMediumB", 0.0f, 40.0f, -20.0f);
        put(cands[1], 0x3, "LandingPadSmallA", 10.0f, 0.0f, 0.0f);
        put(cands[2], 0x4, "LandingPadMediumB", 70.0f, 0.0f, 0.0f);
        put(cands[3], 0x5, "Lift_1", 20.0f, 0.0f, -30.0f);
        put(cands[4], 0x6, "Lift_2", 20.0f, 0.0f, 100.0f);
        put(cands[5], 0x7, "BlastShieldBase", 0.0f, 30.0f, -10.0f);
        put(cands[6], 0x8, "HangarGate_Ramp1_0", 50.0f, 0.0f, -5.0f);
        put(cands[7], 0x9, "Container_1", 5.0f, 0.0f, -5.0f);
        put(cands[8], 0xA, "LandingPadMediumGuideMarkers", 5.0f, 0.0f, 0.0f);
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        const size_t k = se::selectMechanism(cands, 9, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -90.0f, 40.0f, out);
        bool hasB = false, hasLift1 = false, hasShield = false, hasRamp = false, hasLift2 = false, hasSmall = false;
        for (size_t i = 0; i < k; ++i) {
            hasB = hasB || std::strcmp(out[i].name, "LandingPadMediumB") == 0 && out[i].addr == 0x2;
            hasLift1 = hasLift1 || std::strcmp(out[i].name, "Lift_1") == 0;
            hasShield = hasShield || std::strcmp(out[i].name, "BlastShieldBase") == 0;
            hasRamp = hasRamp || std::strcmp(out[i].name, "HangarGate_Ramp1_0") == 0;
            hasLift2 = hasLift2 || std::strcmp(out[i].name, "Lift_2") == 0;
            hasSmall = hasSmall || std::strcmp(out[i].name, "LandingPadSmallA") == 0;
        }
        check(k == 5 && out[0].addr == 0x1 && hasB && hasLift1 && hasShield && hasRamp && !hasLift2 && !hasSmall,
              "v14.12 latch: the own pad, the medium B 40 m lateral, Lift_1, BlastShieldBase, HangarGate_Ramp within the span; not the small pad, the far pad, the lift beyond the span, the container or the guide markers");
        se::LatchCand many[40] = {};
        for (int i = 0; i < 30; ++i) {
            char nm[48] = {};
            std::snprintf(nm, sizeof(nm), "Lift_%d", i);
            put(many[i], static_cast<uintptr_t>(0x100 + i), nm, 5.0f, 0.0f, 0.0f);
        }
        const size_t kc = se::selectMechanism(many, 30, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -90.0f, 40.0f, out);
        check(kc == se::kLatchCap, "v14.12 latch: at most kLatchCap (16) records: the own pad and fifteen others");
    }
    {
        // By address: a still record is not moved; 5 cm is; a missing frame re-bases without counting; a frame shift of the record and the shell
        // together is not motion (the relative centre is the same).
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float r0[3] = {1.0f, 2.0f, -45.0f};
        const float r1[3] = {1.0f, 2.05f, -45.0f};
        se::LatchRec lr;
        lr.step(10, r0, a0, a2, 1, 1, 1000);
        check(!lr.step(11, r0, a0, a2, 1, 1, 1100) && lr.lastMovedMs == 0, "v14.12 by address: a still record is not moved");
        check(lr.step(12, r1, a0, a2, 1, 1, 1200) && lr.lastMovedMs == 1200, "v14.12 by address: a 5 cm move is a move");
        check(!lr.step(14, r0, a0, a2, 1, 1, 1400) && lr.lastMovedMs == 1200, "v14.12 by address: a missing frame re-bases without counting");
        const float v[3] = {123.0f, -40.0f, 900.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float rec[3] = {5.0f, 6.0f, -45.0f};
        const float relA[3] = {rec[0] - shell[0], rec[1] - shell[1], rec[2] - shell[2]};
        const float relB[3] = {rec[0] + v[0] - (shell[0] + v[0]), rec[1] + v[1] - (shell[1] + v[1]), rec[2] + v[2] - (shell[2] + v[2])};
        se::LatchRec fs;
        fs.step(1, relA, a0, a2, 1, 1, 100);
        check(!fs.step(2, relB, a0, a2, 1, 1, 200) && fs.lastMovedMs == 0, "v14.12 by address: a frame shift of the record and the shell together is not motion");
    }

    // ---- Built v14.16: a release by motion waits the lowered pad and the stillness; the mechanism set (no TurboLift, doors or decals; the cap) ----
    {
        // The mechanism set: no TurboLift*, doors or decals; Lift and Lift_* yes, LiftDoor no; the cap of 16 with the refusals logged.
        check(se::latchClass("TurboLiftDoorLeft") < 0 && se::latchClass("TurboLift_Blocker") < 0 && se::latchClass("DecalB") < 0 &&
                  se::latchClass("LiftDoor") < 0 && se::latchClass("Lift") == 2 && se::latchClass("Lift_3") == 2 && se::latchClass("BlastShieldBase") == 2 &&
                  se::latchClass("HangarGate_Ramp1_0") == 3 && se::latchClass("LandingPadMediumB") == 1,
              "v14.16 set: TurboLift, doors and decals are never mechanism records; Lift and Lift_* are; the pads are the first class");
        se::LatchCand many[40] = {};
        for (int i = 0; i < 30; ++i) {
            char nm[48] = {};
            std::snprintf(nm, sizeof(nm), "Lift_%d", i);
            many[i].addr = static_cast<uintptr_t>(0x300 + i);
            std::snprintf(many[i].name, sizeof(many[i].name), "%s", nm);
            many[i].centre[0] = 5.0f;
        }
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        se::LatchReject rej[10];
        size_t rejN = 0;
        const size_t kc = se::selectMechanism(many, 30, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, 300.0f, out, rej, 10, &rejN);
        check(kc == se::kLatchCap && rejN == 10 && std::strstr(rej[0].why, "cap") != nullptr,
              "v14.16 set: at the cap (16) the rest are refused with 'over the latch cap' (the first ten logged)");
        se::LatchCand door[2] = {};
        std::snprintf(door[0].name, sizeof(door[0].name), "%s", "TurboLiftDoorLeft");
        door[0].addr = 0x401;
        std::snprintf(door[1].name, sizeof(door[1].name), "%s", "DecalB");
        door[1].addr = 0x402;
        size_t rejN2 = 0;
        const size_t kd = se::selectMechanism(door, 2, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, 300.0f, out, rej, 10, &rejN2);
        check(kd == 1 && rejN2 == 2, "v14.16 set: the elevator doors and decals are refused, the set is the own pad only");
    }

    // ---- Built v14.15: the load-in latch, the reject log, the bay's span, and the own pad's 3D move while sealed ----------------------------
    {
        // The span: a BlastShield 268 m along the shaft with no shaft target built (capLen 0) is kept; one 70 m lateral is refused with its reason.
        se::LatchCand cands[3] = {};
        std::snprintf(cands[0].name, sizeof(cands[0].name), "%s", "BlastShield_A");
        cands[0].addr = 0x21;
        cands[0].centre[0] = 41.0f;
        cands[0].centre[2] = 268.0f;
        std::snprintf(cands[1].name, sizeof(cands[1].name), "%s", "BlastShieldBase_B");
        cands[1].addr = 0x22;
        cands[1].centre[0] = 70.0f;
        cands[1].centre[2] = 100.0f;
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        se::LatchReject rej[10];
        size_t rejN = 0;
        const float span = std::max(0.0f + 10.0f, se::kLatchSpanM);
        const size_t k = se::selectMechanism(cands, 1, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, span, out, rej, 10, &rejN);
        check(k == 2 && rejN == 0 && std::strcmp(out[1].name, "BlastShield_A") == 0,
              "v14.15: a BlastShield 41 m lateral and 268 m along (no target built yet) is latched");
        size_t rejN2 = 0;
        se::selectMechanism(cands + 1, 1, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, span, out, rej, 10, &rejN2);
        check(rejN2 == 1 && std::strstr(rej[0].why, "lateral") != nullptr && std::strcmp(rej[0].name, "BlastShieldBase_B") == 0,
              "v14.15: a BlastShield 70 m lateral is refused, with its reason logged");
    }


    // ---- Built v14.13: the sun inside the hangar (its own state), the hangar watch, the no-hangar pad, the names --------------------------
    {
        // A docked pad with no sized shell: the note is due once; the give-up after kNoShellGiveUpFrames; a shell resets the count; undock starts over.
        se::HangarWatch hw;
        int notes = 0;
        for (uint32_t k = 0; k < se::kHangarNoteFrames + 20; ++k) notes += hw.step(false, true, false) ? 1 : 0;
        check(notes == 1 && hw.logged && !hw.giveUp, "v14.13 hangar: the no-hangar note is due once per dock, at 60 frames");
        for (uint32_t k = 0; k < se::kNoShellGiveUpFrames; ++k) hw.step(false, true, false);
        check(hw.giveUp, "v14.13 hangar: after 300 frames without a shell the gather of the shell parts stops");
        se::HangarWatch found;
        for (uint32_t k = 0; k < 100; ++k) found.step(false, true, false);
        found.step(true, true, false);
        check(found.missing == 0 && !found.giveUp, "v14.13 hangar: a shell found resets the count (no give-up)");
        found.step(false, false, false);
        check(!found.logged && found.missing == 0, "v14.13 hangar: undock starts the dock over (the note is due again)");
    }
    {
        // Names: the hangar-like filter (hangars, pads, lifts, doors and bays), not a plain structure.
        check(se::hangarLikeName("HangerSmall") && se::hangarLikeName("LandingPadSmallA") && se::hangarLikeName("Elevator_1") &&
                  se::hangarLikeName("BayDoor_2") && !se::hangarLikeName("Container_1"),
              "v14.13 names: hangar, pad, elevator and bay names are noted; a container is not");
        // A part with no size word is not the shell (outposts and carriers have none of the sized kind).
        se::ExplicitObs ex[1] = {};
        ex[0].addr = 1;
        std::snprintf(ex[0].name, sizeof(ex[0].name), "%s", "HangarXYZ");
        const float centre[3] = {0.0f, 0.0f, 0.0f};
        check(se::pickShell(ex, 1, centre, se::PadSize::kMedium, 150.0f) == 1,
              "v14.13 shell: a hangar part with no size word is not the reference");
    }


    // ---- Built v14.6: a ship change (the own ship\'s type prefix) resets the trigger; a flicker is not a change -----------------------------
    {
        se::ShipWatch w;
        check(!w.step("Anaconda"), "v14.6: the first reading sets the own ship's prefix: no change");
        bool any = false;
        for (int k = 0; k < 20; ++k) any = any || w.step("Anaconda");
        check(!any, "v14.6: the same prefix every tick is no change");
        any = false;
        for (int k = 0; k < 9; ++k) any = any || w.step("Cobra");
        check(!any, "v14.6: a different prefix for nine readings is not yet a ship change");
        check(!w.step("Anaconda") && std::strcmp(w.prefix, "Anaconda") == 0, "v14.6: a flicker back to the old prefix resets the count");
        any = false;
        for (int k = 0; k < 9; ++k) any = any || w.step("Asp");
        check(!any, "v14.6: nine readings of the new prefix: still not a change");
        check(w.step("Asp") && std::strcmp(w.prefix, "Asp") == 0, "v14.6: the tenth reading of the new prefix is the ship change (the prefix is now Asp)");
        check(!w.step(nullptr) && !w.step(""), "v14.6: a missing reading is no change");
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
    check(se::kPadMoveReleaseM == 2.0f && se::kCameraLeaveM == 2.0f && se::kBayStillMs == 2000,
          "the constants: the pad-move release 2 m, the camera 2 m, the latch movers window 2 s");
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


    // ---- Restored (test-only, 2026-10-13): retained behaviour whose tests the v14.23 prune removed, rewritten against the current APIs --------------
    {
        // v14.9, the absent-pad hold: a pad absent for a few frames keeps the trigger's request; a longer absence clears it.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        check(se::sealStep(inHangar(0), &st, &w) == se::SealAction::kRequestEngage, "v14.9 hold: the setup request");
        se::SealInputs held = inHangar(100);
        held.pickOk = false;
        held.padHeld = true;
        check(se::sealStep(held, &st, &w) == se::SealAction::kNone && st.requested,
              "v14.9 hold: a pad absent for fewer than kHeldLostFrames keeps the request (no release, no new request)");
        held.nowMs = 200;
        check(se::sealStep(held, &st, &w) == se::SealAction::kNone && st.requested, "v14.9 hold: the same short absence again keeps it");
        se::SealInputs lost = inHangar(300);
        lost.pickOk = false;
        lost.padHeld = false;
        check(se::sealStep(lost, &st, &w) == se::SealAction::kNone && !st.requested, "v14.9 hold: a pad absent past the hold clears the request");
        check(!se::heldCellDrops(29, 0, se::kHeldLostFrames) && se::heldCellDrops(30, 0, se::kHeldLostFrames),
              "v14.9 hold: the held cell drops at kHeldLostFrames (30) absent frames, and not before");
    }
    {
        // The hangar change releases a held seal, with its own reason (the ship swap and the held pad gone both come here).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.hangarChanged = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kHangarChanged,
              "hangar change: a held seal is released by the hangar change");
    }
    {
        // The pad moved against the shell (ShellMotion) releases a held seal; the next seal needs the r1 0 then 1 edge.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.padMoved = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kPadMoved, "pad moved: a held seal is released by the pad moving");
        in.held = false;
        in.padMoved = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 2000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "pad moved: with r1 still 1, the pad's move does not re-seal until the r1 edge (2 s)");
    }
    {
        // The mechanism latch (v14.14-16): a latched record that moves on two compared frames in a row is the launch; one move is not; a source change
        // re-bases without counting; the own pad is index 0 and counts the same way.
        se::LatchSet ls;
        ls.n = 1;
        ls.latched = true;
        const float r0[3] = {0.0f, 0.0f, 0.0f};
        const float r1[3] = {0.0f, 0.0f, 0.5f};
        const float r2[3] = {0.0f, 0.0f, 1.0f};
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        ls.recs[0].step(1, r0, a0, a2, 1, 2, 100);
        ls.recs[0].step(2, r1, a0, a2, 1, 2, 200);
        check(ls.movedIndex() == -1, "latch: one compared move is not a release");
        ls.recs[0].step(3, r2, a0, a2, 1, 2, 300);
        check(ls.movedIndex() == 0, "latch: a record that moves on two compared frames in a row is the launch (the own pad is index 0)");
        ls.recs[0].step(4, r2, a0, a2, 1, 2, 400);
        check(ls.movedIndex() == -1, "latch: a still frame ends the run");
        ls.recs[0].step(5, r0, a0, a2, 3, 2, 500);   // another centre source: a re-base, never a move
        check(ls.movedIndex() == -1 && ls.recs[0].movedRun == 0, "latch: a change of centre source re-bases without counting as a move");
        // The identity (review 2): a shell replacement is a genuine change too.
        se::LatchIdentity id;
        id.bind(0xA0, 0x5000);
        check(id.step(true, 0xA0, true, 0x6000) && id.shell == 0x6000, "latch identity: a shell replacement is a change (rebuild the latch)");
    }
    {
        // The camera (v14.7): out for kCameraFrames frames releases the seal (the 10th frame); back inside it re-seals after kCameraFrames
        // frames inside the bay, with no in-hangar edge (r1 stayed 1).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.cameraOutside = true;
        int outTicks = 0;
        for (int k = 1; k <= 12 && outTicks == 0; ++k) {
            in.nowMs = 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kCameraLeft) outTicks = k;
        }
        check(outTicks == 10, "camera: out of the box for kCameraFrames (10) frames releases the seal, on the 10th frame");
        in.held = false;
        in.cameraOutside = false;
        in.cameraInBox = false;
        in.nowMs = 2000;
        se::sealStep(in, &st, &w);
        int inTicks = 0;
        for (int k = 1; k <= 12 && inTicks == 0; ++k) {
            in.cameraInBox = true;
            in.nowMs = 2000 + 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage) inTicks = k;
        }
        check(inTicks == 10, "camera: back inside the bay the re-seal waits kCameraFrames (10) frames, and needs no in-hangar edge");
    }
    {
        // Review 1: a request whose pad or shell changed before its box was valid is never admitted to freeze a cell.
        se::SealState st;
        st.requested = true;
        st.camInRun = se::kCameraFrames;
        const se::EngageRequest r{true, 0x1000, 0x5000};
        check(!se::engageAdmitted(r, st, 0x1000, 0x5000, false), "review 1: an invalid box at the request does not freeze a cell");
        check(!se::engageAdmitted(r, st, 0x1000, 0x6000, true), "review 1: the shell replaced before the box was valid: no freeze");
        check(!se::engageAdmitted(r, st, 0x2000, 0x5000, true), "review 1: the pad replaced before the box was valid: no freeze");
        check(se::engageAdmitted(r, st, 0x1000, 0x5000, true), "review 1: control: the same pad and shell with a valid box freeze the cell");
    }
    {
        // Store mode (v14.1): the store observer does nothing with the setting off or undocked and unsealed; gathers while docked and unsealed; judges
        // while sealed. The promote is due when the frame stamp has moved on.
        check(se::storeMode(false, false, true) == se::StoreMode::kNone && se::storeMode(true, false, false) == se::StoreMode::kNone,
              "v14.1 store: setting off, or undocked and unsealed, the store observer does nothing");
        check(se::storeMode(true, false, true) == se::StoreMode::kGather, "v14.1 store: docked and unsealed, the observer gathers (no judgement)");
        check(se::storeMode(true, true, false) == se::StoreMode::kJudge && se::storeMode(true, true, true) == se::StoreMode::kJudge,
              "v14.1 store: sealed, the observer judges, docked or not");
        check(se::framePromoteDue(3, 4) && !se::framePromoteDue(4, 4),
              "v14.2 promote: the frame's cell is promoted once per frame stamp (due when the stamp moved on)");
    }
    {
        // v14.1: a pad that was not gathered (no pick) never requests the seal, however docked; the pad in the hangar with a pick does.
        se::SealState st;
        se::SealInputs noPick = inHangar(0);
        noPick.pickOk = false;
        check(firstEngage(&st, noPick, 0, 20000) == 0, "v14.1: no pad gathered (pickOk false): the seal is never requested");
    }

    // ---- Built v14.24 (2026-10-13, the review of the 013c6122 flight) -----------------------------------------------------------------------------
    {
        // Item 1: a change of rig A's association forgets A's bounds and verdict only; rig B keeps its bound and its verdict (no whole-epoch bump).
        bool aCur = true, aLast = true, bCur = true, bLast = true;
        uint8_t aV = 2, bV = 2;
        uint32_t aRun = 3, bRun = 3;
        se::rigForgetBounds(&aCur, &aLast, &aV, &aRun);
        check(!aCur && !aLast && aV == 0 && aRun == 0, "v14.24 rig: a change of rig A's collection forgets rig A's bounds and verdict");
        check(bCur && bLast && bV == 2 && bRun == 3 && se::rigMayVeto(4, 4, bCur, bV),
              "v14.24 rig: rig B keeps its bound and its outside verdict, and may still veto (no whole-epoch bump)");
        check(se::collAssociationIs(0x500, 0x500) && !se::collAssociationIs(0x600, 0x500) && !se::collAssociationIs(0, 0),
              "v14.24 rig: a table entry names its rig only while the rig still holds that collection");
    }
    {
        // Item 3: the cost summary's percentiles, and the change-only gate.
        double v[100];
        for (int i = 0; i < 100; ++i) v[i] = static_cast<double>(100 - i);   // 100 down to 1
        double p50 = 0.0, p95 = 0.0, mx = 0.0;
        se::samplePercentiles(v, 100, &p50, &p95, &mx);
        check(p50 == 50.0 && p95 == 95.0 && mx == 100.0, "v14.24 cost: p50, p95 and max of 1..100 are 50, 95 and 100");
        se::samplePercentiles(v, 0, &p50, &p95, &mx);
        check(p50 == 0.0 && p95 == 0.0 && mx == 0.0, "v14.24 cost: no samples read as zeros");
        se::ChangeGate g;
        check(g.changed(7) && !g.changed(7) && g.changed(8) && !g.changed(8), "v14.24 log: a change-only gate writes a key once per change");
        check(se::textKey("pad") == se::textKey("pad") && se::textKey("pad") != se::textKey("pad "),
              "v14.24 log: a text key is stable for the same text and differs for another");
        se::SealState st;
        check(!st.mechanism && st.poseRun == 0, "v14.24: a fresh trigger holds no mechanism release");
    }
    {
        // Item 2: the pose of a latched record against its pose at the seal.
        const float rel0[3] = {0.0f, 0.0f, 0.0f};
        const float relOff[3] = {0.0f, 0.0f, 0.3f};
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float c2 = std::cos(2.0f * 3.14159265f / 180.0f), s2 = std::sin(2.0f * 3.14159265f / 180.0f);
        const float a0Turn[3] = {c2, s2, 0.0f};
        float dM = -1.0f, dDeg = -1.0f;
        se::latchPoseDelta(rel0, rel0, a0, a0, a2, a2, &dM, &dDeg);
        check(dM == 0.0f && dDeg == 0.0f && se::latchPoseWithin(dM, dDeg), "v14.24 pose: a record at its sealed pose is within tolerance");
        se::latchPoseDelta(relOff, rel0, a0, a0, a2, a2, &dM, &dDeg);
        check(std::fabs(dM - 0.3f) < 1e-5f && !se::latchPoseWithin(dM, dDeg), "v14.24 pose: a 0.3 m offset is outside the 0.25 m tolerance");
        se::latchPoseDelta(rel0, rel0, a0Turn, a0, a2, a2, &dM, &dDeg);
        check(std::fabs(dDeg - 2.0f) < 1e-3f && !se::latchPoseWithin(dM, dDeg), "v14.24 pose: a 2 degree turn is outside the 1 degree tolerance");
    }
    {
        // Item 2, the re-seal on the poses: a Lift that returns to its sealed pose re-seals after kPoseBackFrames frames with r1 1.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.24 pose: the setup seal");
        in.held = true;
        in.mechanismMoved = true;
        in.poseBack = false;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism, "v14.24 pose: the Lift moving releases the seal");
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = true;
        int frames = 0;
        for (int k = 1; k <= 60 && frames == 0; ++k) {
            in.nowMs = 100 + 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage) frames = k;
        }
        check(frames == static_cast<int>(se::kPoseBackFrames),
              "v14.24 pose: a Lift that returns to its sealed pose re-seals after 30 frames (r1 1, camera in)");
    }
    {
        // A Lift that stays away does not re-seal, however long r1 stays 1.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 10000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.24 pose: a Lift that stays away does not re-seal (100 s with r1 1, the pose never back)");
    }
    {
        // r1 1 to 0 while away keeps the edge rule: the pose does not count while the pad is out; the next seal is the r1 0 to 1 edge.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = false;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "v14.24 edge: with the pose away and r1 1, no re-seal");
        in.inHangar = false;
        in.poseBack = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.reseal == 2 && !st.mechanism,
              "v14.24 edge: r1 1 to 0 moves the re-seal to the edge rule and drops the pose count");
        in.inHangar = true;
        in.poseBack = false;
        in.nowMs = 400;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.24 edge: r1 back to 1 is the edge: it re-seals at once");
    }
    {
        // The pose rule belongs to the mechanism release only: a pad-moved release does not re-seal on poses.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.padMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.padMoved = false;
        in.poseBack = true;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 4000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.24 pose: a pad-moved release does not re-seal on the poses (it waits for the r1 edge)");
    }
    {
        // Item 3: the waiting reason carries no per-frame counter: two frames of an absent pad give the same text.
        se::OwnPadSticky sticky;
        sticky.pad = 0x1000;
        const float camera[3] = {0.0f, 0.0f, 0.0f};
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        se::OwnCell first, second;
        se::pickOwnPadSticky(nullptr, 0, camera, radii, &sticky, &first);
        se::pickOwnPadSticky(nullptr, 0, camera, radii, &sticky, &second);
        check(std::strcmp(first.reason, second.reason) == 0 && std::strstr(first.reason, "held for up to 30 frames") != nullptr,
              "v14.24 log: the held-pad reason is the same on every frame (no counter, no line per frame)");
    }

    // ---- Built v14.25 (2026-10-13): the lock-free pieces, driven from several threads ---------------------------------------------------------------
    {
        // The snapshot ring, one thread: the value pinned is the one published.
        struct Snap {
            uint64_t seq = 0;
            uint64_t a = 0;
            uint64_t b = 0;
            uint64_t check = 0;
        };
        se::SnapshotRing<Snap, 4> ring;
        Snap& w = ring.beginWrite();
        w.seq = 1;
        w.a = 3;
        w.b = 5;
        w.check = 9;
        ring.publish();
        size_t idx = 0;
        const Snap* p = ring.pin(&idx);
        check(p->seq == 1 && p->check == 9, "v14.25 ring: the value pinned is the value published");
        ring.unpin(idx);
    }
    {
        // Eight readers and one publisher. A pinned value is never torn (check = seq + a + b), a reader's sequence never goes back, and the last
        // publish is the one read at the end (no lost epoch).
        struct Snap {
            uint64_t seq = 0;
            uint64_t a = 0;
            uint64_t b = 0;
            uint64_t check = 0;
        };
        se::SnapshotRing<Snap, 4> ring;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0};
        std::atomic<uint64_t> backwards{0};
        std::atomic<uint64_t> reads{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 8; ++t) {
            readers.emplace_back([&] {
                uint64_t last = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    size_t idx2 = 0;
                    const Snap* q = ring.pin(&idx2);
                    if (q->check != q->seq + q->a + q->b) torn.fetch_add(1);
                    if (q->seq < last) backwards.fetch_add(1);
                    last = q->seq;
                    ring.unpin(idx2);
                    reads.fetch_add(1);
                }
            });
        }
        const uint64_t kPublishes = 20000;
        while (reads.load() == 0) std::this_thread::yield();   // the readers are running before the first publish
        for (uint64_t k = 1; k <= kPublishes; ++k) {
            Snap& wr = ring.beginWrite();
            wr.seq = k;
            wr.a = 3 * k;
            wr.b = 5 * k;
            wr.check = k + 3 * k + 5 * k;
            ring.publish();
        }
        stop.store(true);
        for (auto& th : readers) th.join();
        check(torn.load() == 0, "v14.25 ring, threads: no torn snapshot across 8 readers and 20000 publishes");
        check(backwards.load() == 0, "v14.25 ring, threads: no reader sees the epoch go backwards");
        size_t idx3 = 0;
        const Snap* last = ring.pin(&idx3);
        check(last->seq == kPublishes && ring.publishes() == kPublishes, "v14.25 ring, threads: the last publish is the one read (no lost epoch)");
        ring.unpin(idx3);
        check(reads.load() > 0, "v14.25 ring, threads: the readers did read");
    }
    {
        // The seqlock table: a single-threaded reference, then eight writers inserting the same records concurrently into a fresh table. Every
        // record must read back its exact payload in both; readers running during the writes never see a torn payload.
        constexpr size_t kCap = 1024;
        static se::SeqCell ref[kCap];
        static se::SeqCell par[kCap];
        const size_t kRecs = 160;
        auto homeOf = [&](uintptr_t rec) { return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCap - 1); };
        auto valueOf = [](uintptr_t rec, float out[4]) {
            out[0] = static_cast<float>(rec % 997);
            out[1] = static_cast<float>(rec % 31);
            out[2] = static_cast<float>(rec % 17);
            out[3] = static_cast<float>(rec % 11) + 1.0f;
        };
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float v[4];
            valueOf(rec, v);
            se::seqTableInsert(ref, kCap, homeOf(rec), rec, 1, v, nullptr);
        }
        size_t refFound = 0;
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float got[4] = {};
            float want[4];
            valueOf(rec, want);
            if (se::seqTableFind(ref, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) == 0) ++refFound;
        }
        check(refFound == kRecs, "v14.25 table, reference: every record reads back its payload (single thread)");
        std::atomic<uint64_t> badHits{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 8; ++t) {
            writers.emplace_back([&, t] {
                for (size_t k = 0; k < kRecs; ++k) {
                    const size_t j = (k * 7 + static_cast<size_t>(t) * 13) % kRecs;   // each thread inserts all records, in its own order
                    const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(j);
                    float v[4];
                    valueOf(rec, v);
                    se::seqTableInsert(par, kCap, homeOf(rec), rec, 1, v, nullptr);
                }
            });
        }
        std::thread checker([&] {
            for (int round = 0; round < 200; ++round) {
                for (size_t k = 0; k < kRecs; ++k) {
                    const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
                    float got[4] = {};
                    float want[4];
                    valueOf(rec, want);
                    if (se::seqTableFind(par, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) != 0) badHits.fetch_add(1);
                }
            }
        });
        for (auto& th : writers) th.join();
        checker.join();
        size_t parFound = 0;
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float got[4] = {};
            float want[4];
            valueOf(rec, want);
            if (se::seqTableFind(par, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) == 0) ++parFound;
        }
        check(badHits.load() == 0, "v14.25 table, threads: a reader during concurrent inserts never sees a torn payload");
        check(parFound == refFound && parFound == kRecs,
              "v14.25 table, threads: eight writers inserting the same records give the same decisions as the single-thread reference");
        float got[4] = {};
        check(!se::seqTableFind(par, kCap, homeOf(0x7000), 0x7000, 2, got, nullptr), "v14.25 table: a record of another generation is not found");
    }

    std::printf("optimized_station_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
