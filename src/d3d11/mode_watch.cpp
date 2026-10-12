// The mode watch's worker and its commit (mode_watch.h says what the API is; mode_watch_logic.h holds the rules). The worker owns the
// files, the wait and the log lines; the rules see none of it. Design: docs\design-unified-install-2026-10-12.md sections 3.2 and 3.3.
#include "mode_watch.h"

#include "flat_runtime.h"       // flatRuntimeResize: the flat teardown, run before a commit leaves flat
#include "mode_watch_logic.h"

#include "../common/install_scope.h"
#include "../common/log.h"
#include "../common/native_frame.h"           // nativeFrameLatchedFrames: the eye-submit count the watch samples
#include "../common/native_render_settings.h"  // edvrQueryNativeRenderSizing: the runtime's live VR session
#include "../common/runtime_profile.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace edvr {
namespace {

using modewatch::Outcome;
using modewatch::Step;
using modewatch::Watch;

constexpr DWORD kMaxWaitMs = 1000;     // the stat backup and the openvr check run at this interval
constexpr uint64_t kRetryMs = 100;     // a read that cannot open its file is retried this often
constexpr int kMaxRetries = 20;        // per write; then the watch waits for the next event
constexpr size_t kMaxSettingsBytes = 1024 * 1024;

// The posted mode, packed so one exchange takes all of it: bit 0 posted, bit 1 Vr (clear is Flat), bit 2 from a live rise (clear is
// the file), and the time from bit 8 up.
constexpr uint64_t kPostedBit = 1, kVrBit = 2, kLiveBit = 4;
constexpr unsigned kTimeShift = 8;
std::atomic<uint64_t> g_posted{0};
std::atomic<uint32_t> g_hints{0};
std::atomic<HANDLE> g_hintEvent{nullptr};
std::atomic<bool> g_startCalled{false};
std::atomic<bool> g_started{false};

// The toast the menu shows once per commit. Written on the render thread, read by the menu's poll.
std::mutex g_noticeMutex;
std::string g_notice;
std::atomic<bool> g_noticeDue{false};

uint64_t nowMs() { return GetTickCount64(); }

const char* profileWord(RuntimeProfile p) {
    switch (p) {
    case RuntimeProfile::Vr: return "vr";
    case RuntimeProfile::Flat: return "flat";
    case RuntimeProfile::Passthrough: return "passthrough";
    default: return "invalid";
    }
}

void setNotice(const char* text) {
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    g_notice = text;
    g_noticeDue.store(true, std::memory_order_release);
}

void postMode(uint64_t t, GameMode mode, bool live) {
    const uint64_t bits = kPostedBit | (mode == GameMode::Vr ? kVrBit : 0) | (live ? kLiveBit : 0);
    g_posted.store((t << kTimeShift) | bits, std::memory_order_release);
}

// Settings.xml's last write time, or 0 when the file is absent.
uint64_t writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// The worker. One per process, pinned with the DLL, so it is never freed and has no shutdown path (module_pin.h).
class Worker {
public:
    Worker(GameMode launch, GameMode effective, std::wstring folder, std::wstring file, HANDLE hint)
        : watch_(launch, effective), folder_(std::move(folder)), file_(std::move(file)), hint_(hint) {}

    static DWORD WINAPI threadMain(LPVOID p) { return static_cast<Worker*>(p)->run(); }

private:
    DWORD run() {
        HANDLE handles[2] = {hint_, INVALID_HANDLE_VALUE};
        DWORD count = 1;
        HANDLE change = FindFirstChangeNotificationW(folder_.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
        const bool haveFolder = change != INVALID_HANDLE_VALUE;
        if (haveFolder) {
            handles[count++] = change;
        } else {
            Log::get().note("mode watch: no folder notification (error %lu); polling once a second", GetLastError());
        }
        lastWrite_ = writeTime(file_);
        submitsAtSample_ = nativeFrameLatchedFrames();
        nextSampleMs_ = nowMs() + kMaxWaitMs;

        for (;;) {
            // A timed read (a confirm or a retry) runs as soon as it is due; the wait below ends no later than the next one, and no
            // later than the next eye-submit sample.
            const uint64_t at = timedReadAt();
            if (at && nowMs() >= at) readFile(retryAt_ != 0);
            uint64_t wake = nextSampleMs_;
            const uint64_t next = timedReadAt();
            if (next && next < wake) wake = next;
            const uint64_t now = nowMs();
            DWORD timeout = kMaxWaitMs;
            if (wake) timeout = wake > now ? static_cast<DWORD>(std::min<uint64_t>(kMaxWaitMs, wake - now)) : 0;

            const DWORD r = WaitForMultipleObjects(count, handles, FALSE, timeout);
            if (r == WAIT_OBJECT_0) {
                handleHints();
            } else if (haveFolder && r == WAIT_OBJECT_0 + 1) {
                if (!FindNextChangeNotification(change))
                    Log::get().note("mode watch: folder notification re-arm failed (error %lu)", GetLastError());
                checkWrite(false);
            } else if (r == WAIT_TIMEOUT) {
                // A hint can land before the event exists or be missed while the worker is between waits; the bits are read here too.
                if (g_hints.load(std::memory_order_acquire)) handleHints();
                sampleSubmits(nowMs());
                checkWrite(true);
                checkOpenVr();
            } else {
                Log::get().note("mode watch: wait failed (error %lu); the watch stopped", GetLastError());
                return 1;
            }
        }
    }

    // The next time a read is due by the clock: a pending mode's confirm, or a retry. Zero when none is.
    uint64_t timedReadAt() const {
        if (gaveUp_) return 0;
        uint64_t at = watch_.pending() ? watch_.confirmDueMs() : 0;
        if (retryAt_ > at) at = retryAt_;
        return at;
    }

    void handleHints() {
        const uint32_t bits = g_hints.exchange(0, std::memory_order_acq_rel);
        const uint64_t now = nowMs();
        if (bits & static_cast<uint32_t>(ModeHint::Setter))
            Log::get().note("mode watch: hint setter t=%llu", static_cast<unsigned long long>(now));
        if (bits & static_cast<uint32_t>(ModeHint::Window))
            Log::get().note("mode watch: hint window t=%llu", static_cast<unsigned long long>(now));
        if (bits & static_cast<uint32_t>(ModeHint::LiveRise)) {
            Log::get().note("mode watch: hint live-rise t=%llu", static_cast<unsigned long long>(now));
            // A live runtime with no file write (Epic's "Launch in VR") posts Vr here, before the read below.
            const Step s = watch_.onLiveRise(now);
            if (s.outcome == Outcome::Posted) {
                postMode(now, GameMode::Vr, true);
                Log::get().note("mode watch: live-rise t=%llu -> posted vr", static_cast<unsigned long long>(now));
            } else {
                Log::get().note("mode watch: live-rise t=%llu -> nothing", static_cast<unsigned long long>(now));
            }
        }
        readFile(false);
    }

    // The folder's notification or the stat backup: a write is logged once per change of the file's write time.
    void checkWrite(bool fromStat) {
        const uint64_t wt = writeTime(file_);
        if (wt == lastWrite_) return;
        lastWrite_ = wt;
        char suffix[64];
        Log::get().note("mode watch: write t=%llu%s%s", static_cast<unsigned long long>(nowMs()), fromStat ? " (stat)" : "",
                        submitSuffix(suffix, sizeof(suffix)));
        readFile(false);
    }

    // The eye-submit rate: the frame pairs the game submitted in the last full second, sampled on the timeout path once a second. It is
    // file-independent, so it says whether the game still reaches the VR runtime when Settings.xml does not change. Logs only when the
    // state changes: "stopped" when a second with none follows one with some, "resumed" for the reverse.
    void sampleSubmits(uint64_t now) {
        if (now < nextSampleMs_) return;
        const uint64_t total = nativeFrameLatchedFrames();
        lastSecondSubmits_ = total - submitsAtSample_;
        submitsAtSample_ = total;
        nextSampleMs_ = now + kMaxWaitMs;
        const bool flowing = lastSecondSubmits_ > 0;
        if (flowing && !flowing_)
            Log::get().note("mode watch: eye submits resumed t=%llu (%llu/s)", static_cast<unsigned long long>(now),
                            static_cast<unsigned long long>(lastSecondSubmits_));
        else if (!flowing && flowing_)
            Log::get().note("mode watch: eye submits stopped t=%llu", static_cast<unsigned long long>(now));
        flowing_ = flowing;
    }

    // The suffix every read and write line carries: the last full second's eye submits (zero until the first sample completes).
    const char* submitSuffix(char* buf, size_t size) const {
        std::snprintf(buf, size, " (eye submits last 1 s: %llu)", static_cast<unsigned long long>(lastSecondSubmits_));
        return buf;
    }

    void checkOpenVr() {
        const bool loaded = GetModuleHandleW(L"openvr_api.dll") != nullptr;
        if (loaded == openvrLoaded_) return;
        openvrLoaded_ = loaded;
        Log::get().note("mode watch: openvr_api.dll %s t=%llu", loaded ? "loaded" : "unloaded",
                        static_cast<unsigned long long>(nowMs()));
    }

    // Reads Settings.xml and feeds the watch. A retry is a read the timer asked for; any other read starts a new attempt count.
    void readFile(bool retry) {
        const uint64_t now = nowMs();
        if (!retry) {
            retries_ = 0;
            gaveUp_ = false;
        }
        ModeRead r{};
        DWORD error = 0;
        char suffix[64];
        if (!readSettings(&r, &error)) {
            retryAt_ = 0;
            if (retries_ < kMaxRetries) {
                ++retries_;
                retryAt_ = now + kRetryMs;
                Log::get().note("mode watch: read failed t=%llu (error %lu); retry in 100 ms%s",
                                static_cast<unsigned long long>(now), error, submitSuffix(suffix, sizeof(suffix)));
            } else {
                gaveUp_ = true;
                Log::get().note("mode watch: read failed t=%llu (error %lu); giving up on this write after %d retries%s",
                                static_cast<unsigned long long>(now), error, kMaxRetries, submitSuffix(suffix, sizeof(suffix)));
            }
            return;
        }
        retryAt_ = 0;
        const Step s = watch_.onRead(now, r);

        char mode[16];
        if (r.known) std::snprintf(mode, sizeof(mode), "%d", r.value);
        else std::snprintf(mode, sizeof(mode), "unknown");
        char result[96];
        switch (s.outcome) {
        case Outcome::Ignored: std::snprintf(result, sizeof(result), "ignored"); break;
        case Outcome::Same: std::snprintf(result, sizeof(result), "same"); break;
        case Outcome::Pending:
            std::snprintf(result, sizeof(result), "pending %s confirm at t=%llu", gameModeName(s.mode),
                          static_cast<unsigned long long>(watch_.confirmDueMs()));
            break;
        case Outcome::Dropped: std::snprintf(result, sizeof(result), "dropped"); break;
        case Outcome::Posted:
            std::snprintf(result, sizeof(result), "posted %s", gameModeName(s.mode));
            break;
        default: std::snprintf(result, sizeof(result), "nothing"); break;
        }
        Log::get().note("mode watch: read t=%llu mode %s -> %s%s", static_cast<unsigned long long>(now), mode, result,
                        submitSuffix(suffix, sizeof(suffix)));
        if (s.outcome == Outcome::Posted) postMode(now, s.mode, false);
    }

    // False when the file could not be opened or read; the error is then the Windows error. An absent file reads as no mode, and so
    // does one too large to be Settings.xml.
    bool readSettings(ModeRead* out, DWORD* error) {
        HANDLE file = CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
                *out = readGameMode(nullptr);
                return true;
            }
            *error = e;
            return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) {
            *error = GetLastError();
            CloseHandle(file);
            return false;
        }
        if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(kMaxSettingsBytes)) {
            CloseHandle(file);
            *out = readGameMode("");
            return true;
        }
        std::string text(static_cast<size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        const bool ok = text.empty() || (ReadFile(file, &text[0], static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size());
        const DWORD e = GetLastError();
        CloseHandle(file);
        if (!ok) {
            *error = e;
            return false;
        }
        *out = readGameMode(text.c_str());
        return true;
    }

    Watch watch_;
    std::wstring folder_;
    std::wstring file_;
    HANDLE hint_;
    uint64_t lastWrite_ = 0;
    uint64_t retryAt_ = 0;
    int retries_ = 0;
    bool gaveUp_ = false;
    bool openvrLoaded_ = false;
    // The eye-submit sampler: the counter at the last sample, the next sample's time, the last full second's count, and whether that
    // second had any. flowing_ starts false, so the first second with submits logs "resumed".
    uint64_t submitsAtSample_ = 0;
    uint64_t nextSampleMs_ = 0;
    uint64_t lastSecondSubmits_ = 0;
    bool flowing_ = false;
};

// The render thread's commit: the posted mode becomes the profile it selects, or a log line and a notice when this build does not
// switch live. Phase 1 never moves into the other profile; it drops to Passthrough and asks for a restart.
void commitPosted(uint64_t packed) {
    const GameMode mode = (packed & kVrBit) ? GameMode::Vr : GameMode::Flat;
    const bool live = (packed & kLiveBit) != 0;
    const unsigned long long t = static_cast<unsigned long long>(packed >> kTimeShift);
    const char* cls = gameModeName(mode);
    const char* from = live ? "live-rise" : "file";
    const RuntimeProfile target = activeProfileFor(runtimeInstallScope(), mode);
    RuntimeProfile cur = runtimeProfileValue();
    if (cur == RuntimeProfile::LegacyVr) cur = RuntimeProfile::Vr;

    char line[400];
    if (cur == RuntimeProfile::Passthrough) {
        std::snprintf(line, sizeof(line), "profile switch: none, passthrough until restart (posted %s t=%llu from %s)", cls, t, from);
    } else if (target == cur) {
        std::snprintf(line, sizeof(line), "profile switch: none, stays %s (posted %s t=%llu from %s)", profileWord(cur), cls, t, from);
    } else {
        // Flat's teardown runs while the flat predicate still answers true, before the profile flips (design section 3.3).
        if (cur == RuntimeProfile::Flat) flatRuntimeResize();
        runtimeProfileSet(RuntimeProfile::Passthrough);
        std::snprintf(line, sizeof(line),
                      "profile switch: %s -> passthrough (posted %s t=%llu from %s; this build does not switch live: restart Elite to run "
                      "the %s profile)",
                      profileWord(cur), cls, t, from, profileWord(target));
        if (target == RuntimeProfile::Passthrough) setNotice("EDVR is paused until Elite restarts");
        else if (target == RuntimeProfile::Vr) setNotice("Elite's 3D mode changed: restart Elite for EDVR's VR fixes");
        else setNotice("Elite's 3D mode changed: restart Elite for EDVR's flat fixes");
    }
    Log::get().note("%s", line);
}

}  // namespace

void modeWatchStart() {
    if (runtimeProfileValue() == RuntimeProfile::Invalid) return;
    if (g_startCalled.exchange(true)) return;

    const ModeRead launch = runtimeLaunchRead();
    const std::wstring& path = runtimeSettingsPathWide();
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        Log::get().note("mode watch: not started (no Settings.xml path; LOCALAPPDATA is not set)");
        return;
    }
    const std::wstring folder = path.substr(0, slash);
    HANDLE hint = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hint) {
        Log::get().note("mode watch: not started (event error %lu)", GetLastError());
        return;
    }
    g_hintEvent.store(hint, std::memory_order_release);

    char launchMode[16];
    if (launch.known) std::snprintf(launchMode, sizeof(launchMode), "%d", launch.value);
    else std::snprintf(launchMode, sizeof(launchMode), "unknown");
    const GameMode effective = runtimeLaunchEffective();
    Log::get().note("mode watch: started on %S; launch file mode %s (%s); launch mode %s; scope %s", folder.c_str(), launchMode,
                    gameModeName(launch.mode), gameModeName(effective), scopeName(runtimeInstallScope()));

    Worker* worker = new Worker(launch.mode, effective, folder, path, hint);
    const HANDLE thread = CreateThread(nullptr, 0, Worker::threadMain, worker, 0, nullptr);
    if (!thread) {
        Log::get().note("mode watch: worker thread not started (error %lu)", GetLastError());
        return;
    }
    CloseHandle(thread);
    g_started.store(true, std::memory_order_release);
}

void modeWatchHint(ModeHint h) {
    g_hints.fetch_or(static_cast<uint32_t>(h), std::memory_order_acq_rel);
    const HANDLE e = g_hintEvent.load(std::memory_order_acquire);
    if (e) SetEvent(e);
}

void modeWatchFrameBoundary() {
    if (!g_started.load(std::memory_order_acquire)) return;
    const uint64_t posted = g_posted.exchange(0, std::memory_order_acq_rel);
    if (posted) commitPosted(posted);

    // The runtime's live flag, sampled once a frame; a rise asks the worker for a re-read (a VR start may write no file).
    static bool lastLive = false;
    EdvrNativeRenderSizing sizing{};
    const bool live = edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, sizeof(sizing), &sizing) && sizing.valid == 1;
    if (live && !lastLive) modeWatchHint(ModeHint::LiveRise);
    lastLive = live;
}

bool modeWatchNoticeDue(char* out, size_t size) {
    if (!out || size == 0 || !g_noticeDue.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    if (!g_noticeDue.load(std::memory_order_acquire)) return false;
    std::snprintf(out, size, "%s", g_notice.c_str());
    g_noticeDue.store(false, std::memory_order_release);
    return true;
}

}  // namespace edvr
