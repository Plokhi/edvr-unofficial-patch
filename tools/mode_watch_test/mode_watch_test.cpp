// mode_watch_test -- the mode watch's decisions (src\d3d11\mode_watch_logic.h): a change waits for its confirming read 250 ms later, a
// half-written file is ignored, a flip-flop drops, a live rise posts Vr from a flat commit, and the toggles post in order. Built from
// the repo root by build.bat's :rig_mode_watch_test. Windows-free: it includes only the pure headers. --dry-run writes nothing;
// --self-test runs the checks.
#include "../../src/d3d11/mode_watch_logic.h"
#include "../../src/common/install_scope.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace edvr;
using namespace edvr::modewatch;

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

// A read of a mode, as readGameMode makes it from Settings.xml's text.
ModeRead readOf(GameMode mode) {
    return readGameMode(mode == GameMode::Flat ? "<StereoscopicMode>0</StereoscopicMode>" : "<StereoscopicMode>4</StereoscopicMode>");
}

ModeRead unknownRead() { return readGameMode("<Settings></Settings>"); }

bool outcomeIs(const Step& s, Outcome o) { return s.outcome == o; }

void writeThenConfirm() {
    Watch w(GameMode::Flat);
    const Step first = w.onRead(1000, readOf(GameMode::Vr));
    check(outcomeIs(first, Outcome::Pending) && first.mode == GameMode::Vr && w.confirmDueMs() == 1250,
        "flat launch, write 0 -> 4: pending Vr, confirm at +250");
    const Step hint = w.onRead(1100, readOf(GameMode::Vr));
    check(outcomeIs(hint, Outcome::Same) && w.pending() && w.committed() == GameMode::Flat,
        "flat launch, read at +100 still 4: same, still pending");
    const Step confirm = w.onRead(1250, readOf(GameMode::Vr));
    check(outcomeIs(confirm, Outcome::Posted) && confirm.mode == GameMode::Vr && w.committed() == GameMode::Vr && !w.pending(),
        "flat launch, read at +250 still 4: posted Vr");
}

void halfWrittenDuringPending() {
    Watch w(GameMode::Flat);
    w.onRead(0, readOf(GameMode::Vr));
    const Step half = w.onRead(100, unknownRead());
    check(outcomeIs(half, Outcome::Ignored) && w.pending() && w.confirmDueMs() == 250,
        "half-written file during pending: ignored, pending and due kept");
    const Step confirm = w.onRead(250, readOf(GameMode::Vr));
    check(outcomeIs(confirm, Outcome::Posted) && confirm.mode == GameMode::Vr,
        "half-written file during pending: posted Vr at the confirm");
}

void flipFlop() {
    Watch w(GameMode::Flat);
    w.onRead(0, readOf(GameMode::Vr));
    const Step back = w.onRead(100, readOf(GameMode::Flat));
    check(outcomeIs(back, Outcome::Dropped) && !w.pending() && w.committed() == GameMode::Flat,
        "flip-flop 0 -> 4 -> 0 within 250 ms: dropped, flat stays committed");
    const Step later = w.onRead(300, readOf(GameMode::Flat));
    check(outcomeIs(later, Outcome::Same) && w.committed() == GameMode::Flat,
        "flip-flop: nothing posted after the drop");
}

void confirmDisagrees() {
    Watch w(GameMode::Flat);
    w.onRead(0, readOf(GameMode::Vr));
    const Step confirm = w.onRead(250, readOf(GameMode::Flat));
    check(outcomeIs(confirm, Outcome::Dropped) && w.committed() == GameMode::Flat && !w.pending(),
        "confirm read disagrees (flat at +250): dropped");
}

void hintNoChange() {
    Watch vr(GameMode::Vr);
    check(outcomeIs(vr.onRead(0, readOf(GameMode::Vr)), Outcome::Same) && !vr.pending(),
        "hint read with no change from a Vr launch: same");
    Watch flat(GameMode::Flat);
    check(outcomeIs(flat.onRead(0, readOf(GameMode::Flat)), Outcome::Same) && !flat.pending() && flat.confirmDueMs() == 0,
        "hint read with no change from a flat launch: same, nothing due");
}

void liveRise() {
    Watch flat(GameMode::Flat);
    const Step rise = flat.onLiveRise(0);
    check(outcomeIs(rise, Outcome::Posted) && rise.mode == GameMode::Vr && flat.committed() == GameMode::Vr,
        "live rise from a flat launch: posted Vr");

    Watch vr(GameMode::Vr);
    check(outcomeIs(vr.onLiveRise(0), Outcome::Nothing) && vr.committed() == GameMode::Vr,
        "live rise from a Vr launch: nothing");

    Watch after(GameMode::Vr);
    after.onRead(0, readOf(GameMode::Flat));
    after.onRead(250, readOf(GameMode::Flat));
    check(after.committed() == GameMode::Flat, "live rise after a posted Flat: flat committed first");
    const Step again = after.onLiveRise(300);
    check(outcomeIs(again, Outcome::Posted) && again.mode == GameMode::Vr && after.committed() == GameMode::Vr,
        "live rise after a posted Flat: posted Vr");
}

// A live rise does not cancel a pending file change; the confirm of that change finds the class already posted.
void liveRiseDuringPending() {
    Watch w(GameMode::Flat);
    w.onRead(0, readOf(GameMode::Vr));
    const Step rise = w.onLiveRise(100);
    check(outcomeIs(rise, Outcome::Posted) && rise.mode == GameMode::Vr && w.committed() == GameMode::Vr,
        "live rise while a Vr file change is pending: posted Vr");
    check(w.pending() && w.confirmDueMs() == 250, "live rise while pending: the pending change is kept, due unchanged");
    const Step confirm = w.onRead(250, readOf(GameMode::Vr));
    check(outcomeIs(confirm, Outcome::Same) && w.committed() == GameMode::Vr && !w.pending(),
        "live rise during pending: the confirm returns same (already Vr)");
}

// Epic's "Launch in VR": the live rise posts Vr while Settings.xml still says flat. The unchanged file must not post Flat; a later
// real change of the file is still posted in each direction.
void liveRiseThenFileChanges() {
    Watch w(GameMode::Flat);
    const Step rise = w.onLiveRise(0);
    check(outcomeIs(rise, Outcome::Posted) && rise.mode == GameMode::Vr, "live rise with the file still flat: posted Vr");
    const Step unchanged = w.onRead(100, readOf(GameMode::Flat));
    check(outcomeIs(unchanged, Outcome::Same) && !w.pending() && w.committed() == GameMode::Vr,
        "a read of the unchanged flat file after a live rise: same, nothing posted");
    const Step toVr = w.onRead(200, readOf(GameMode::Vr));
    check(outcomeIs(toVr, Outcome::Pending) && toVr.mode == GameMode::Vr, "file 0 -> 4 after a live rise: pending Vr");
    const Step toVrConfirm = w.onRead(450, readOf(GameMode::Vr));
    check(outcomeIs(toVrConfirm, Outcome::Same) && w.committed() == GameMode::Vr && !w.pending(),
        "file 0 -> 4 confirmed after a live rise: same (already Vr)");
    const Step toFlat = w.onRead(500, readOf(GameMode::Flat));
    check(outcomeIs(toFlat, Outcome::Pending) && toFlat.mode == GameMode::Flat, "file 4 -> 0: pending Flat");
    const Step toFlatConfirm = w.onRead(750, readOf(GameMode::Flat));
    check(outcomeIs(toFlatConfirm, Outcome::Posted) && toFlatConfirm.mode == GameMode::Flat && w.committed() == GameMode::Flat,
        "file 4 -> 0 confirmed: posted Flat");
}

void threeToggles() {
    Watch w(GameMode::Vr);
    std::vector<GameMode> posted;
    uint64_t t = 0;
    const GameMode reads[] = {GameMode::Flat, GameMode::Vr, GameMode::Flat};
    for (GameMode m : reads) {
        const Step pending = w.onRead(t, readOf(m));
        if (!outcomeIs(pending, Outcome::Pending)) break;
        const Step confirm = w.onRead(t + 250, readOf(m));
        if (outcomeIs(confirm, Outcome::Posted)) posted.push_back(confirm.mode);
        t += 1000;
    }
    const bool order = posted.size() == 3 && posted[0] == GameMode::Flat && posted[1] == GameMode::Vr && posted[2] == GameMode::Flat;
    check(order, "three toggles off-on-off from a Vr launch: posted Flat, Vr, Flat in order");
}

void unknownLaunch() {
    const ModeRead launch = unknownRead();
    Watch w(launch.mode);
    check(w.committed() == GameMode::Vr, "unknown launch file counts as Vr");
    const ModeRead three = readGameMode("<StereoscopicMode>3</StereoscopicMode>");
    check(outcomeIs(w.onRead(0, three), Outcome::Same) && !w.pending(),
        "unknown launch, then a read of 3: same");
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun && !selfTest) {
        std::printf("mode_watch_test: dry run, nothing written\n");
        return 0;
    }
    writeThenConfirm();
    halfWrittenDuringPending();
    flipFlop();
    confirmDisagrees();
    hintNoChange();
    liveRise();
    liveRiseDuringPending();
    liveRiseThenFileChanges();
    threeToggles();
    unknownLaunch();
    if (g_fails) {
        std::printf("FAIL: mode_watch_test %d of %d checks failed\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: mode_watch_test %d checks (confirm at 250 ms, half-written reads ignored, flip-flop dropped, live rise, toggles in "
                "order, unknown launch)\n", g_checks);
    return 0;
}
