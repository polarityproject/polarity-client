// Hardware data breakpoints on every thread, for research (docs/research.md).

#include "mod/watch.h"

#include "mod/log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace sfv::mod {

namespace {

constexpr int kMaxSites = 32;

constexpr int kStackDepth = 12;
constexpr int kSlots = 4;
constexpr uint64_t kDr6Hits = 0xF;
constexpr uint64_t kDr7LocalEnable = 1, kDr7SlotEnableMask = 3, kDr7Write = 1, kDr7ReadWrite = 3, kDr7SlotFieldMask = 0xF;
constexpr int kDr7ControlShift = 16, kDr7LengthShift = 2;
constexpr uint64_t kLength1 = 0, kLength2 = 1, kLength8 = 2, kLength4 = 3;
constexpr DWORD kTrapDrainMs = 250;
constexpr uintptr_t kImageSpan = 0x5000000;

struct Site {
    std::atomic<uintptr_t> rip{0};
    std::atomic<uint64_t> hits{0};
    uintptr_t stack[kStackDepth] = {};
};

void UnwindFrom(CONTEXT c, uintptr_t* out) {
    for (int i = 0; i < kStackDepth; i++) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
        } else {
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        out[i] = static_cast<uintptr_t>(c.Rip);
    }
}

Site g_sites[kMaxSites];
std::atomic<uint64_t> g_dropped{0};
uintptr_t g_imageBase = 0;
PVOID g_handler = nullptr;
std::atomic<uint64_t> g_armed{0};
uint64_t g_everArmed = 0;

void Record(uintptr_t rip, const CONTEXT& ctx) {
    for (auto& s : g_sites) {
        uintptr_t cur = s.rip.load();
        if (cur == rip) {
            s.hits++;
            return;
        }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (s.rip.compare_exchange_strong(expected, rip)) {
                UnwindFrom(ctx, s.stack);
                s.hits++;
                return;
            }
            if (expected == rip) {
                s.hits++;
                return;
            }
        }
    }
    g_dropped++;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    if (!(ctx->Dr6 & kDr6Hits)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_armed == 0) ctx->Dr7 &= ~g_everArmed;
    else Record(static_cast<uintptr_t>(ctx->Rip), *ctx);
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

template <typename F>
int ForEachThread(F update) {
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    int done = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                update(ctx);
                if (SetThreadContext(t, &ctx)) done++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return done;
}

} // namespace

bool WatchArm(int slot, const void* address, int length, uintptr_t imageBase, bool readsToo) {
    if (slot < 0 || slot >= kSlots) return false;
    uint64_t len;
    switch (length) {
        case 1: len = kLength1; break;
        case 2: len = kLength2; break;
        case 4: len = kLength4; break;
        case 8: len = kLength8; break;
        default: return false;
    }
    const auto addr = reinterpret_cast<uintptr_t>(address);
    if (addr % length) {
        Log("watch: %p is not aligned to %d", address, length);
        return false;
    }
    g_imageBase = imageBase;
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, Handler);
    for (auto& s : g_sites) {
        s.rip = 0;
        s.hits = 0;
    }

    const uint64_t enable = kDr7LocalEnable << (slot * 2);
    const int field = kDr7ControlShift + slot * 4;
    const uint64_t control = ((readsToo ? kDr7ReadWrite : kDr7Write) | (len << kDr7LengthShift)) << field;
    const uint64_t mask = (kDr7SlotEnableMask << (slot * 2)) | (kDr7SlotFieldMask << field);
    const int threads = ForEachThread([&](CONTEXT& ctx) {
        (&ctx.Dr0)[slot] = addr;
        ctx.Dr7 = (ctx.Dr7 & ~mask) | enable | control;
    });
    g_armed |= mask;
    g_everArmed |= mask;
    Log("watch: slot %d on %p (%d bytes, %s), %d threads", slot, address, length, readsToo ? "read/write" : "write",
        threads);
    return threads > 0;
}

void WatchDisarmAll() {
    const uint64_t armed = g_armed.exchange(0);
    if (armed) ForEachThread([&](CONTEXT& ctx) { ctx.Dr7 &= ~armed; });
}

void WatchRemoveHandler() {
    WatchDisarmAll();
    if (!g_handler) return;
    Sleep(kTrapDrainMs);
    RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
}

void WatchReport() {
    int n = 0;
    for (const auto& s : g_sites) {
        const uintptr_t rip = s.rip;
        if (!rip) continue;
        const bool inImage = rip >= g_imageBase && rip < g_imageBase + kImageSpan;
        Log("watch: write before rip %s0x%llX, %llu hits", inImage ? "rva " : "",
            static_cast<unsigned long long>(inImage ? rip - g_imageBase : rip),
            static_cast<unsigned long long>(s.hits.load()));
        std::string callers;
        for (uintptr_t a : s.stack) {
            if (!a) break;
            char buf[24];
            if (a >= g_imageBase && a < g_imageBase + 0x5000000) std::snprintf(buf, sizeof(buf), " %llX", static_cast<unsigned long long>(a - g_imageBase));
            else std::snprintf(buf, sizeof(buf), " ext");
            callers += buf;
        }
        Log("watch:   called from%s", callers.c_str());
        n++;
    }
    Log("watch: %d sites, %llu dropped", n, static_cast<unsigned long long>(g_dropped.load()));
}

} // namespace sfv::mod
