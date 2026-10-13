#include "../../src/common/crash_context.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static unsigned checks = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
struct Lines { std::vector<std::string> values; };
static void collect(const char* line, void* opaque) noexcept {
    static_cast<Lines*>(opaque)->values.emplace_back(line);
}
static bool has(const Lines& lines, const char* needle) {
    for (const auto& line : lines.values) if (line.find(needle) != std::string::npos) return true;
    return false;
}
static void validLines(const Lines& lines) {
    check(!lines.values.empty(), "report emitted no lines");
    check(lines.values.size() <= 30, "report exceeds bounded line count");
    for (const auto& line : lines.values) check(line.size() < 192, "line exceeds breadcrumb bound");
}
static std::vector<DWORD64> unwindPCs(const Lines& lines) {
    std::vector<DWORD64> pcs;
    for (const auto& line : lines.values) {
        if (line.find("crash: unwind frame=") != 0) continue;
        auto offset = line.find(" rip=0x");
        check(offset != std::string::npos, "unwind PC present");
        pcs.push_back(std::strtoull(line.c_str() + offset + 7, nullptr, 16));
    }
    return pcs;
}
static bool inFunction(DWORD64 pc, const void* function) {
    DWORD64 base = 0;
    auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(function), &base, nullptr);
    return entry && pc >= base + entry->BeginAddress && pc < base + entry->EndAddress;
}

__declspec(noinline) static void captureLargeFrame(Lines* lines) {
    volatile unsigned char padding[32768];
    for (unsigned i = 0; i < sizeof(padding); i += 4096) padding[i] = 1;
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_POINTERS pointers{nullptr, &context};
    edvr::crash_context::report(&pointers, collect, lines);
    // Retain the large frame around the captured context and prevent tail calls.
    check(padding[0] + padding[28672] == 2, "large stack frame remains live");
}
__declspec(noinline) static void captureCaller(Lines* lines) {
    captureLargeFrame(lines);
    check(!lines->values.empty(), "capture returned through caller");
}

int main(int argc, char** argv) {
    if (argc > 1 && (argc != 2 || std::string(argv[1]) != "--self-test")) return 2;
    alignas(16) uintptr_t stack[32]{};
    for (unsigned i = 0; i < 32; ++i) stack[i] = 0x100000000ull + i;
    CONTEXT context{};
    context.Rax = 0x11; context.Rbx = 0x22; context.Rcx = 0x33; context.Rdx = 0x44;
    context.Rsi = 0x55; context.Rdi = 0x66; context.Rbp = 0x77;
    context.Rsp = reinterpret_cast<DWORD64>(stack); context.R8 = 0x88; context.R9 = 0x99;
    context.R10 = 0xAA; context.R11 = 0xBB; context.R12 = 0xCC; context.R13 = 0xDD;
    context.R14 = 0xEE; context.R15 = 0xFF; context.Rip = 0x12345678; context.EFlags = 0x202;
    EXCEPTION_RECORD access{};
    access.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    access.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
    access.NumberParameters = 2;
    access.ExceptionInformation[0] = 1;
    access.ExceptionInformation[1] = 0xDEADBEEF;
    EXCEPTION_POINTERS pointers{&access, &context};
    Lines lines;
    auto run = [&] {
        lines.values.clear();
        edvr::crash_context::report(&pointers, collect, &lines);
        validLines(lines);
    };
    run();
    check(has(lines, "op=write") && has(lines, "address=0xDEADBEEF"), "write address");
    const char* expected[] = {"rax=0x11", "rbx=0x22", "rcx=0x33", "rdx=0x44",
                             "rsi=0x55", "rdi=0x66", "rbp=0x77", "rsp=0x",
                             "r8=0x88", "r9=0x99", "r10=0xAA", "r11=0xBB",
                             "r12=0xCC", "r13=0xDD", "r14=0xEE", "r15=0xFF",
                             "rip=0x12345678", "eflags=0x202"};
    for (auto* value : expected) check(has(lines, value), value);
    for (unsigned i = 0; i < 32; ++i) {
        char value[80];
        std::snprintf(value, sizeof(value), " +0x%X=0x%llX", i * 8, stack[i]);
        check(has(lines, value), "stack offset and value");
    }
    check(!has(lines, " +0x100="), "stack capture ends at 256 bytes");
    check(unwindPCs(lines).size() == 8 && has(lines, "stop=frame-limit"), "leaf unwind bounded");
    check(has(lines, "crash: fault-page state=free type=none"), "a wild address reports a free page");
    check(has(lines, "crash: ledger events=") && has(lines, "covered-by=none"), "a wild address is covered by no image");
    check(!has(lines, "crash: cell"), "a data fault does not look for a table cell");

    for (ULONG_PTR op : {ULONG_PTR(0), ULONG_PTR(8), ULONG_PTR(7)}) {
        access.ExceptionInformation[0] = op; run();
        check(has(lines, op == 0 ? "op=read" : op == 8 ? "op=execute" : "op=unknown"), "access operation");
        check(has(lines, "crash: cell none") == (op == 8), "only a jump to a bad address looks for its cell");
    }
    access.NumberParameters = 1; run();
    check(has(lines, "params=truncated") && !has(lines, "address="), "missing address not fabricated");
    access.ExceptionCode = EXCEPTION_IN_PAGE_ERROR; access.NumberParameters = 2; run();
    check(has(lines, "status=truncated"), "truncated in-page parameters");
    access.NumberParameters = 3; access.ExceptionInformation[2] = 0xC000009C; run();
    check(has(lines, "status=0xC000009C"), "in-page status");
    pointers.ContextRecord = nullptr; run();
    check(lines.values.size() == 1, "absent context emits only exception");
    pointers.ExceptionRecord = nullptr; run();
    check(has(lines, "code=0x0"), "absent record");

    pointers.ContextRecord = &context;
    context.Rax = context.Rbx = context.Rcx = context.Rdx = UINT64_MAX;
    context.Rsi = context.Rdi = context.Rbp = context.Rsp = UINT64_MAX;
    context.R8 = context.R9 = context.R10 = context.R11 = UINT64_MAX;
    context.R12 = context.R13 = context.R14 = context.R15 = context.Rip = UINT64_MAX;
    context.EFlags = UINT32_MAX; run();
    check(has(lines, "r15=0xFFFFFFFFFFFFFFFF") && has(lines, "eflags=0xFFFFFFFF"), "max-width registers");
    check(has(lines, "<unreadable>") && has(lines, "stop=leaf-unreadable"), "overflowing stack address");

    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto* guarded = static_cast<uint8_t*>(VirtualAlloc(nullptr, system.dwPageSize * 2,
                                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(guarded != nullptr, "guarded allocation");
    DWORD old = 0;
    check(VirtualProtect(guarded + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &old) != 0,
          "unreadable page");
    auto* last = reinterpret_cast<uintptr_t*>(guarded + system.dwPageSize - sizeof(uintptr_t));
    *last = 0xAABBCCDD;
    context.Rip = 0; context.Rsp = reinterpret_cast<DWORD64>(last); run();
    check(has(lines, "+0x0=0xAABBCCDD") && has(lines, "+0x8=<unreadable>"), "page boundary guarded");
    check(has(lines, "+0xF8=<unreadable>"), "unreadable tail retained explicitly");
    VirtualFree(guarded, 0, MEM_RELEASE);

    lines.values.clear();
    edvr::crash_context::report(nullptr, collect, &lines);
    validLines(lines);
    check(lines.values.size() == 1, "null pointers handled");
    edvr::crash_context::report(nullptr, nullptr);

    lines.values.clear();
    captureCaller(&lines);
    validLines(lines);
    auto pcs = unwindPCs(lines);
    check(pcs.size() >= 2, "large frame has caller");
    check(inFunction(pcs[0], reinterpret_cast<void*>(&captureLargeFrame)), "frame zero is captured function");
    check(inFunction(pcs[1], reinterpret_cast<void*>(&captureCaller)), "caller recovered across 32KB frame");
    check(has(lines, "module=crash_context_test.exe rva=0x"), "module identity and RVA retained");

    // The shutdown crash's shape, rebuilt: an image the loader mapped and then
    // freed, a table cell that still holds an address inside it, and a jump
    // through that cell. The ledger must name the image and how long ago it
    // left, the page must read as free, and the cell scan must find the
    // register that points at the cell, its distance from another register,
    // and one junk cell among 255 image pointers.
    {
        using namespace edvr;
        dll_ledger::clearForTest();
        check(dll_ledger::install() && dll_ledger::install(), "the ledger installs, and installing twice is harmless");
        HMODULE victim = LoadLibraryW(L"dbghelp.dll");
        check(victim != nullptr, "a DLL to load and free");
        const uintptr_t base = reinterpret_cast<uintptr_t>(victim);
        check(FreeLibrary(victim) != 0 && GetModuleHandleW(L"dbghelp.dll") == nullptr, "the DLL is really unmapped");
        dll_ledger::Entry hit;
        check(dll_ledger::covering(base + 0xF780, hit) && hit.unloaded && hit.base == base &&
                  std::string(hit.name) == "dbghelp.dll",
              "the ledger answers for an address inside the freed image");
        check(!dll_ledger::covering(0x1234, hit), "and for no address outside any image");

        alignas(16) uintptr_t table[300];
        for (auto& cell : table) cell = reinterpret_cast<uintptr_t>(&captureCaller);
        table[150] = base + 0xF780;
        alignas(16) uintptr_t shapeStack[32]{};
        CONTEXT crash{};
        crash.Rax = reinterpret_cast<DWORD64>(&table[150]);
        crash.Rcx = reinterpret_cast<DWORD64>(&table[0]);
        crash.Rsp = reinterpret_cast<DWORD64>(shapeStack);
        crash.Rip = base + 0xF780;
        EXCEPTION_RECORD jump{};
        jump.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        jump.ExceptionAddress = reinterpret_cast<PVOID>(crash.Rip);
        jump.NumberParameters = 2;
        jump.ExceptionInformation[0] = 8;
        jump.ExceptionInformation[1] = base + 0xF780;
        EXCEPTION_POINTERS shape{&jump, &crash};
        lines.values.clear();
        edvr::crash_context::report(&shape, collect, &lines);
        validLines(lines);
        check(has(lines, "state=free type=none"), "the freed image's address reads as a free page");
        check(has(lines, "covered-by-unloaded=dbghelp.dll"), "the crash line names the image that was freed");
        check(has(lines, "crash: ledger newest-unloads ") && has(lines, ".dll@"),
              "and the newest unloads are listed with their ages");
        check(has(lines, "crash: cell reg=rax") && has(lines, "rcx+0x4B0"),
              "the cell is found through rax, 0x4B0 bytes past rcx");
        check(has(lines, "crash: cell window cells=256 image=255 zero=0 other=1"),
              "one junk cell among 255 image pointers is counted");
        check(has(lines, "crash: cell first-other [0]=0x"), "and listed at its own offset");

        const uint32_t before = dll_ledger::events();
        dll_ledger::remove();
        HMODULE other = LoadLibraryW(L"winmm.dll");
        if (other) FreeLibrary(other);
        check(dll_ledger::events() == before, "a removed ledger records nothing more");
    }

    std::printf("crash context self-test: %u checks PASS\n", checks);
    return 0;
}
