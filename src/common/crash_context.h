#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

#include "dll_ledger.h"

namespace edvr::crash_context {

using Sink = void (*)(const char* line, void* context) noexcept;

namespace detail {

// Fixed storage and hand formatting: the process may already be failing.
struct Line {
    char text[192];
    size_t size = 0;
    void character(char c) noexcept {
        if (size < sizeof(text) - 1) text[size++] = c;
    }
    void append(const char* s) noexcept {
        while (*s && size < sizeof(text) - 1) character(*s++);
    }
    void hex(uint64_t v) noexcept {
        append("0x");
        bool lead = true;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const unsigned d = unsigned((v >> shift) & 15);
            if (lead && d == 0 && shift) continue;
            lead = false;
            character(char(d < 10 ? '0' + d : 'A' + d - 10));
        }
    }
    void dec(uint64_t v) noexcept {
        char digits[20];
        unsigned count = 0;
        do { digits[count++] = char('0' + v % 10); v /= 10; } while (v && count < sizeof(digits));
        while (count) character(digits[--count]);
    }
    void field(const char* name, uint64_t v) noexcept { append(name); hex(v); }
    void send(Sink sink, void* context) noexcept {
        text[size] = 0;
        sink(text, context);
    }
};

inline bool readQword(uintptr_t address, uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<const uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void stop(const char* reason, Sink sink, void* context) noexcept {
    Line line;
    line.append("crash: unwind stop=");
    line.append(reason);
    line.send(sink, context);
}

__declspec(noinline) inline void emitStack(const CONTEXT& c, Sink sink, void* context) noexcept {
    for (unsigned first = 0; first < 32; first += 4) {
        Line line;
        line.append("crash: stack");
        for (unsigned i = first; i < first + 4; ++i) {
            const uintptr_t offset = i * sizeof(uintptr_t);
            uintptr_t value = 0;
            line.field(" +", offset);
            line.append("=");
            if (c.Rsp <= UINTPTR_MAX - offset && readQword(c.Rsp + offset, value)) {
                line.hex(value);
            } else {
                line.append("<unreadable>");
            }
        }
        line.send(sink, context);
    }
}

inline void moduleName(Line& line, DWORD64 rip) noexcept {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &module)) {
        line.append(" module=unknown");
        return;
    }
    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length >= MAX_PATH) length = MAX_PATH - 1;
    DWORD base = 0;
    for (DWORD i = 0; i < length; ++i) if (path[i] == L'\\') base = i + 1;
    line.append(" module=");
    for (DWORD i = base; i < length && i - base < 64; ++i) {
        line.character(path[i] < 0x80 ? char(path[i]) : '?');
    }
    line.field(" rva=", rip - reinterpret_cast<uintptr_t>(module));
}

// Keep these buffers out of the top-level filter so its stack-overflow
// bypass does not acquire another large frame before branching.
__declspec(noinline) inline void emitUnwind(const CONTEXT& original, Sink sink,
                                          void* context) noexcept {
    CONTEXT c = original;
    for (unsigned frame = 0; frame < 8; ++frame) {
        if (!c.Rip) { stop("end", sink, context); return; }
        Line line;
        line.append("crash: unwind");
        line.field(" frame=", frame);
        line.field(" rip=", c.Rip);
        line.field(" rsp=", c.Rsp);
        moduleName(line, c.Rip);
        line.send(sink, context);

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = nullptr;
        const DWORD64 oldRip = c.Rip, oldRsp = c.Rsp;
        __try {
            function = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            stop("metadata-unreadable", sink, context); return;
        }
        if (function) {
            DWORD64 establisher = 0;
            PVOID handlerData = nullptr;
            __try {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, function,
                                 &c, &handlerData, &establisher, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                stop("frame-unreadable", sink, context); return;
            }
        } else {
            // Leaf functions have no unwind entry; their return PC is at RSP.
            uintptr_t next = 0;
            if (c.Rsp > UINTPTR_MAX - sizeof(uintptr_t) || !readQword(c.Rsp, next)) {
                stop("leaf-unreadable", sink, context); return;
            }
            c.Rip = next;
            c.Rsp += sizeof(uintptr_t);
        }
        if (c.Rsp <= oldRsp || c.Rip == oldRip) {
            stop("no-progress", sink, context); return;
        }
    }
    stop("frame-limit", sink, context);
}

// ---- what the faulting address is --------------------------------------
//
// An access violation names an address and the module list says "no module".
// These lines say what that address is: the memory manager's view of it, which
// image the loader mapped over it and freed (dll_ledger.h), and -- for a jump
// to a bad address -- the table cell that held it. Each answers one fork of
// the shutdown crash: freed image, freed block, or a cell written with junk.

inline const char* stateName(DWORD state) noexcept {
    return state == MEM_COMMIT ? "commit" : state == MEM_RESERVE ? "reserve"
         : state == MEM_FREE ? "free" : "other";
}

inline const char* typeName(DWORD type) noexcept {
    return type == MEM_IMAGE ? "image" : type == MEM_MAPPED ? "mapped"
         : type == MEM_PRIVATE ? "private" : "none";
}

__declspec(noinline) inline void emitFaultPage(uintptr_t address, Sink sink, void* context) noexcept {
    Line line;
    line.append("crash: fault-page");
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info)) {
        line.append(" query=failed");
    } else {
        line.append(" state=");
        line.append(stateName(info.State));
        line.append(" type=");
        line.append(typeName(info.Type));
        line.field(" protect=", info.Protect);
        line.field(" allocbase=", reinterpret_cast<uintptr_t>(info.AllocationBase));
        line.field(" allocprotect=", info.AllocationProtect);
        line.field(" region=", info.RegionSize);
    }
    line.send(sink, context);
}

__declspec(noinline) inline void emitLedger(uintptr_t address, Sink sink, void* context) noexcept {
    const uint64_t now = GetTickCount64();
    {
        Line line;
        line.append("crash: ledger events=");
        line.dec(dll_ledger::events());
        dll_ledger::Entry hit;
        if (dll_ledger::covering(address, hit)) {
            line.append(hit.unloaded ? " covered-by-unloaded=" : " covered-by-loaded=");
            line.append(hit.name);
            line.field(" base=", hit.base);
            line.field(" size=", hit.size);
            line.append(" age_ms=");
            line.dec(now >= hit.tick ? now - hit.tick : 0);
        } else {
            line.append(" covered-by=none");
        }
        line.send(sink, context);
    }
    Line line;
    line.append("crash: ledger newest-unloads");
    // name@age_ms. A freed DLL takes its private dependencies with it, so the
    // newest few are often the tail of one chain; the "covered-by" line above
    // is the answer for the fault address, this is the surrounding picture.
    unsigned named = 0;
    dll_ledger::Entry e;
    for (unsigned back = 0; back < 64 && named < 5 && dll_ledger::recent(back, e); ++back) {
        if (!e.unloaded) continue;
        line.append(" ");
        line.append(e.name);
        line.append("@");
        line.dec(now >= e.tick ? now - e.tick : 0);
        ++named;
    }
    if (!named) line.append(" none");
    line.send(sink, context);
}

inline bool pointerLike(uint64_t value) noexcept {
    return value >= 0x10000 && value < 0x800000000000ull;
}

inline bool inLoadedImage(uintptr_t address) noexcept {
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) && module;
}

// A jump to a bad address through a table: the register that points at a cell
// holding exactly the fault address (the live-mode stub is `mov rax, &cell;
// jmp [rax]`, so it is rax there), that cell's distance from another register
// (the D3D11 context object keeps its method table inline, so the cell is the
// context plus 8 plus 8 times the slot), and what the 256 cells around it
// hold. One junk cell among images is a write to that slot; a whole table of
// non-images is the table having been torn down.
__declspec(noinline) inline void emitCell(uintptr_t address, const CONTEXT& c, Sink sink,
                                          void* context) noexcept {
    static const char* const names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    const DWORD64 values[16] = {c.Rax, c.Rcx, c.Rdx, c.Rbx, c.Rsp, c.Rbp, c.Rsi, c.Rdi,
                                c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15};
    for (unsigned i = 0; i < 16; ++i) {
        const uintptr_t cell = static_cast<uintptr_t>(values[i]);
        uintptr_t held = 0;
        if (i == 4 || !pointerLike(cell) || (cell & 7) || !readQword(cell, held) || held != address) continue;
        Line line;
        line.append("crash: cell reg=");
        line.append(names[i]);
        line.field(" at=", cell);
        unsigned related = 0;
        for (unsigned j = 0; j < 16 && related < 2; ++j) {
            const uint64_t base = values[j];
            if (j == i || j == 4 || !pointerLike(base) || base >= cell || cell - base > 0x1000 ||
                ((cell - base) & 7)) continue;
            line.append(" ");
            line.append(names[j]);
            line.field("+", cell - base);
            ++related;
        }
        line.send(sink, context);

        unsigned images = 0, zeros = 0, others = 0, listed = 0;
        Line other;
        other.append("crash: cell first-other");
        for (int k = -128; k < 128; ++k) {
            uintptr_t value = 0;
            const uintptr_t at = cell + static_cast<uintptr_t>(static_cast<intptr_t>(k) * 8);
            if (!readQword(at, value)) { ++others; continue; }
            if (value == 0) { ++zeros; continue; }
            if (pointerLike(value) && inLoadedImage(value)) { ++images; continue; }
            ++others;
            if (listed < 3) {
                other.append(" [");
                if (k < 0) other.character('-');
                other.dec(static_cast<uint64_t>(k < 0 ? -k : k));
                other.append("]=");
                other.hex(value);
                ++listed;
            }
        }
        Line window;
        window.append("crash: cell window cells=256 image=");
        window.dec(images);
        window.append(" zero=");
        window.dec(zeros);
        window.append(" other=");
        window.dec(others);
        window.send(sink, context);
        if (listed) other.send(sink, context);
        return;
    }
    Line none;
    none.append("crash: cell none (no register points at a cell holding the fault address)");
    none.send(sink, context);
}

} // namespace detail

__declspec(noinline) inline void report(EXCEPTION_POINTERS* info, Sink sink,
                                       void* context = nullptr) noexcept {
    if (!sink) return;
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    const CONTEXT* c = info ? info->ContextRecord : nullptr;
    detail::Line line;
    line.append("crash:");
    line.field(" thread=", GetCurrentThreadId());
    line.field(" code=", record ? record->ExceptionCode : 0);
    if (record && (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                   record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)) {
        if (record->NumberParameters < 2) {
            line.append(" params=truncated");
        } else {
            const ULONG_PTR op = record->ExceptionInformation[0];
            line.append(" op=");
            line.append(op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "unknown");
            line.field(" address=", record->ExceptionInformation[1]);
            if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
                if (record->NumberParameters < 3) line.append(" status=truncated");
                else line.field(" status=", record->ExceptionInformation[2]);
            }
        }
    }
    line.send(sink, context);
    if (!c) return;
    const char* names[] = {" rax=", " rbx=", " rcx=", " rdx=", " rsi=", " rdi=",
                          " rbp=", " rsp=", " r8=", " r9=", " r10=", " r11=",
                          " r12=", " r13=", " r14=", " r15=", " rip=", " eflags="};
    const DWORD64 values[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi,
                             c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11,
                             c->R12, c->R13, c->R14, c->R15, c->Rip, c->EFlags};
    for (unsigned first = 0; first < 18; first += 4) {
        detail::Line regs;
        regs.append("crash: regs");
        for (unsigned i = first; i < first + 4 && i < 18; ++i) regs.field(names[i], values[i]);
        regs.send(sink, context);
    }
    if (record && (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                   record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        const uintptr_t address = static_cast<uintptr_t>(record->ExceptionInformation[1]);
        detail::emitFaultPage(address, sink, context);
        detail::emitLedger(address, sink, context);
        if (record->ExceptionInformation[0] == 8) detail::emitCell(address, *c, sink, context);
    }
    detail::emitStack(*c, sink, context);
    detail::emitUnwind(*c, sink, context);
}

} // namespace edvr::crash_context
