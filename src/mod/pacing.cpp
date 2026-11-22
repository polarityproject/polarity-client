// Frame pacing: time sync's stretch waited out before the pads are read (docs/netplay.md).

#include "mod/pacing.h"

#include "mod/hitch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace sfv::mod {

namespace {

constexpr double kPeriodMs = 1000.0 / 60.0;
constexpr double kResetAfterMs = 100;
constexpr ULONGLONG kLapseMs = 250;
constexpr double kTimerUnitsPerMs = 10000;

std::atomic<float> g_ahead{0};
std::atomic<ULONGLONG> g_leadAt{0};
std::atomic<uint32_t> g_leads{0};
std::mutex g_statsMutex;
PaceStats g_stats;

double NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

void WaitUntil(double deadline) {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (double now = NowMs(); now < deadline; now = NowMs()) {
        const double rest = deadline - now;
        if (rest > 1.5 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((rest - 1.0) * kTimerUnitsPerMs);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, static_cast<DWORD>(rest) + 1);
        } else {
            YieldProcessor();
        }
    }
}

} // namespace

float PaceStretchFor(float ahead) {
    if (ahead <= 0.25f) return 0.0f;
    const float s = ahead <= 3.0f ? ahead * 0.01f : 0.03f + (ahead - 3.0f) * 0.03f;
    return std::min(0.25f, s);
}

void PaceLead(float ahead) {
    g_ahead = ahead;
    g_leadAt = GetTickCount64();
    g_leads++;
}

void PaceFrame() {
    static double last = 0;
    static uint32_t pacedLead = 0;
    if (GetTickCount64() - g_leadAt.load() > kLapseMs) {
        last = 0;
        return;
    }
    const uint32_t leads = g_leads.load();
    if (leads == pacedLead) return;
    pacedLead = leads;
    const float stretch = PaceStretchFor(g_ahead);
    const double now = NowMs();
    const bool reset = last == 0 || now - last > kResetAfterMs;
    const double deadline = last + kPeriodMs * (1.0 + stretch);
    const bool late = !reset && stretch > 0 && now >= deadline;
    if (!reset && stretch > 0) WaitUntil(deadline);
    last = NowMs();
    const double waited = last - now;
    hitch::Add(hitch::Part::Pacing, static_cast<int64_t>(waited * 1000));
    std::lock_guard lock(g_statsMutex);
    g_stats.paced++;
    g_stats.late += late;
    g_stats.resets += reset;
    if (stretch > 0) {
        g_stats.stretched++;
        g_stats.stretchSum += stretch;
    }
    g_stats.waitedMs += waited;
}

PaceStats PaceTakeStats() {
    std::lock_guard lock(g_statsMutex);
    const PaceStats s = g_stats;
    g_stats = {};
    return s;
}

} // namespace sfv::mod
