#include "../flat_temporal_test/flat_cpu_tests.h"
#include "proxy_passthrough_tests.h"
#include "resource_slot_cache_tests.h"
#include "constant_batch_tests.h"
#include "flat_diagnostic_tests.h"
#include <cstdio>
#include <cstring>
#include <chrono>

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
}
void profileTests() {
    using namespace edvr;
    check(!cpuProfilePolicy(nullptr,nullptr), "profiling default is off");
    check(cpuProfilePolicy("1",nullptr), "profiling explicit opt-in");
    check(cpuProfilePolicy(nullptr,"1"), "legacy explicit clock opt-in preserved");
    check(!cpuProfilePolicy("0","1"), "new off switch overrides legacy on");
    check(cpuProfilePolicy("1","0"), "new on switch overrides legacy off");
    check(!cpuProfilePolicy("11",nullptr), "profiling rejects malformed prefix");
    check(cpuSamplePeriodPolicy(nullptr)==32 && cpuSamplePeriodPolicy("99999999999")==32,
          "missing and overflowing sample periods keep safe default");
    check(cpuSamplePeriodPolicy("64")==64 && cpuSamplePeriodPolicy("1024")==1024,
          "sparse sample periods accepted");
    check(cpuSamplePeriodPolicy("1")==32 && cpuSamplePeriodPolicy("33")==32,
          "dense or irregular periods rejected");

    // Exercise the actual forwarding helper with the actual instrument: the
    // game call is never counted as EDVR, and an inactive call takes no clock.
    for (bool sampled : {false,true}) for (bool capture : {false,true}) {
        flatcpu::resetForTest();
        flat_cpu_rig::fake(100);
        if (!sampled) emcpu::g_gate.store(0);
        unsigned forwards = 0;
        int64_t elapsed = -1;
        {
            emcpu::Scope scope(emcpu::kJobs);
            const auto result = cpuTimedForward(scope,capture,[] { return flat_cpu_rig::now(); },
                [&] { ++forwards; flat_cpu_rig::t_now += 100; return 49u; },elapsed);
            check(result==49 && forwards==1, "forwarded result and exactly one call preserved");
            flat_cpu_rig::t_now += 7;
        }
        check(elapsed == (sampled || capture ? 100 : 0), "capture and sampled forward elapsed");
        if (!sampled && !capture)
            check(flat_cpu_rig::t_reads==0 && emcpu::slotCount()==0,
                  "inactive job forward reads no clock and registers no timer slot");
        emcpu::FrameCut cut;
        emcpu::cutFrame(cut);
        check(cut.renderTicks[emcpu::kJobs] == (sampled ? 7u : 0u),
              "downstream work excluded from EDVR job timing");
    }
    flat_cpu_rig::t_fake = false;
    flatcpu::WindowReport r;
    r.valid=true; r.frames=r.renderClockedFrames=1;
    r.renderMs[flatcpu::kReduce]=2;
    r.renderMs[flatcpu::kSubmission]=50;
    r.renderMs[flatcpu::kPresent]=100;
    r.renderMs[flatcpu::kResourceSubmission]=200;
    flatcpu::Lines lines;
    flatcpu::formatWindow(r,&lines);
    bool separated=false;
    for (int i=0;i<lines.count;++i)
        separated |= std::strstr(lines.line[i],"EDVR per frame total 2.000 ms")!=nullptr;
    check(separated, "downstream draw and Present time excluded from EDVR total");
}
void drawAttributionTests() {
    using namespace edvr;
    for (bool sampled : {false, true}) {
        flatcpu::resetForTest();
        flat_cpu_rig::fake(100);
        if (!sampled) flatcpu::g_gate.store(0);
        unsigned forwards = 0, reissues = 0;
        {
            flatcpu::Scope hook(flatcpu::kOther);
            flat_cpu_rig::t_now += 5; // setup before the existing runtime scope
            { flatcpu::Scope setup(flatcpu::kOther); flat_cpu_rig::t_now += 7; }
            hook.timeChild(flatcpu::kSubmission, [&] {
                ++forwards; flat_cpu_rig::t_now += 100;
            });
            flat_cpu_rig::t_now += 11; // actual-draw cleanup
            { flatcpu::Scope ui(flatcpu::kUiComposition); ++reissues; flat_cpu_rig::t_now += 13; }
            flat_cpu_rig::t_now += 17; // runtime destructor/restore
        }
        { flatcpu::Scope prep(flatcpu::kPreparation); flat_cpu_rig::t_now += 23; }
        check(forwards == 1 && reissues == 1, "draw and UI reissue still execute exactly once");
        check(sampled || (flat_cpu_rig::t_reads == 0 && flatcpu::slotCount() == 0),
              "disabled draw partition registers no slots and reads no clocks");
        flatcpu::FrameCut cut;
        flatcpu::cutFrame(cut);
        check(cut.renderTicks[flatcpu::kOther] == (sampled ? 40u : 0u),
              "setup, cleanup and restore counted once despite nested runtime scope");
        check(cut.renderTicks[flatcpu::kSubmission] == (sampled ? 100u : 0u),
              "only the original game draw belongs to downstream submission");
        check(cut.renderTicks[flatcpu::kUiComposition] == (sampled ? 13u : 0u),
              "UI reissue belongs to EDVR processing");
        check(cut.renderTicks[flatcpu::kPreparation] == (sampled ? 23u : 0u),
              "temporal preparation has an independent measured family");
        uint64_t total = 0;
        for (unsigned f = 0; f < flatcpu::kFamilies; ++f) total += cut.renderTicks[f];
        check(total == (sampled ? 176u : 0u), "exclusive families partition elapsed time without overlap");
        // A child shares its parent's clocking decision and preserves return values.
        int value;
        { flatcpu::Scope hook(flatcpu::kOther); value = hook.timeChild(flatcpu::kSubmission, [] { return 49; }); }
        check(value == 49, "timed child forwards its result");
    }
    flat_cpu_rig::t_fake = false;
}
void benchmarkSlots() {
    struct Entry { const void* resource; unsigned value; } entries[64];
    for (unsigned i=0;i<64;++i) entries[i]={reinterpret_cast<const void*>(uintptr_t(i+1)),i};
    edvr::ResourceSlotCache<64> hint;
    const auto key=[](const Entry& e) { return e.resource; };
    volatile uintptr_t request=64;
    volatile uint64_t sink=0;
    constexpr unsigned iterations=5000000;
    const auto start=std::chrono::steady_clock::now();
    for (unsigned n=0;n<iterations;++n) {
        const void* wanted=reinterpret_cast<const void*>(request);
        Entry* result=nullptr;
        for (auto& e:entries) if (e.resource==wanted) { result=&e; break; }
        if (result) sink+=result->value;
    }
    const auto middle=std::chrono::steady_clock::now();
    for (unsigned n=0;n<iterations;++n) {
        const void* wanted=reinterpret_cast<const void*>(request);
        Entry* result=hint.find(entries,wanted,key);
        if (result) sink+=result->value;
    }
    const auto end=std::chrono::steady_clock::now();
    const double linear=std::chrono::duration<double,std::nano>(middle-start).count()/iterations;
    const double cached=std::chrono::duration<double,std::nano>(end-middle).count()/iterations;
    std::printf("native helper benchmark: %u warm last-slot lookups; linear %.2f ns, hint %.2f ns; checksum %llu\n",
        iterations,linear,cached,static_cast<unsigned long long>(sink));
    std::puts("CPU helper only; excludes D3D, engine, GPU and rendering. No FPS inference.");
}
}
int main(int argc, char** argv) {
    if (argc==2 && !std::strcmp(argv[1],"--benchmark")) { benchmarkSlots(); return 0; }
    if (argc==3 && !std::strcmp(argv[1],"--check-profile")) {
        const bool expected = !std::strcmp(argv[2],"on");
        return edvr::emcpu::profileWanted()==expected && edvr::flatcpu::clocksWanted()==expected ? 0 : 1;
    }
    if (argc>1 && !std::strcmp(argv[1],"--dry-run")) return 0;
    if (argc!=2 || std::strcmp(argv[1],"--self-test")) {
        std::puts("usage: edvr_perf_test --self-test | --dry-run | --benchmark"); return 2;
    }
    proxy_passthrough_tests::run(check);
    resource_slot_cache_tests::run(check);
    constant_batch_tests::run(check);
    flat_diagnostic_tests::run(check);
    profileTests();
    drawAttributionTests();
    failures += static_cast<unsigned>(flatCpuTests());
    std::printf("%s: performance policies (%u targeted checks), existing flat CPU rig\n",
                failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
