// Frame drops: where each slow frame went (docs/research.md).

#include "mod/hitch.h"

#include "mod/log.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstdio>
#include <string>
#include <windows.h>

namespace sfv::mod::hitch {

namespace {

constexpr int64_t kPauseMicros = 2000000, kFrameMicros = 16667;
constexpr int64_t kWindowMs = 1000;
constexpr size_t kCapturedReserve = 4096;

constexpr int kParts = static_cast<int>(Part::Count);
std::atomic<int64_t> g_parts[kParts];
std::atomic<int> g_ticks{0};
std::atomic<int> g_replayed{0};

struct Frame {
    int64_t micros = 0;
    int64_t parts[kParts] = {};
    int ticks = 0;
    int replayed = 0;
};

ULONGLONG g_windowStart = 0;
int g_slow = 0;
int64_t g_slowExtraMicros = 0;
Frame g_slowest;

std::string Describe(const Frame& f) {
    auto ms = [&](Part p) { return f.parts[static_cast<int>(p)] / 1000.0; };
    const double game = ms(Part::NetFrame) + ms(Part::BattleFrame) + ms(Part::Pacing);
    const double render = ms(Part::Overlay) + ms(Part::Present);
    const double elsewhere = f.micros / 1000.0 - (game > render ? game : render);
    char buf[340];
    std::snprintf(buf, sizeof(buf),
                  "%.0f ms: netplay %.1f ms (%d ticks, %d replayed), battle %.1f ms, pacing %.1f ms, transport %.1f ms "
                  "(lock wait %.1f), overlay %.1f ms, present %.1f ms, elsewhere %.0f ms",
                  f.micros / 1000.0, ms(Part::NetFrame), f.ticks, f.replayed, ms(Part::BattleFrame), ms(Part::Pacing),
                  ms(Part::Transport), ms(Part::TransportWait), ms(Part::Overlay), ms(Part::Present),
                  elsewhere > 0 ? elsewhere : 0.0);
    return buf;
}

} // namespace

void Add(Part part, int64_t micros) {
    g_parts[static_cast<int>(part)].fetch_add(micros, std::memory_order_relaxed);
}

void AddTick(bool replayed) {
    g_ticks.fetch_add(1, std::memory_order_relaxed);
    if (replayed) g_replayed.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<bool> g_capturing{false};
std::mutex g_captureMutex;
std::vector<CapturedFrame> g_captured;

void BeginCapture() {
    std::lock_guard lock(g_captureMutex);
    g_captured.clear();
    g_captured.reserve(kCapturedReserve);
    g_capturing = true;
}

std::vector<CapturedFrame> EndCapture() {
    g_capturing = false;
    std::lock_guard lock(g_captureMutex);
    return std::move(g_captured);
}

void FramePresented(int64_t frameMicros, const char* where) {
    Frame f;
    f.micros = frameMicros;
    for (int i = 0; i < kParts; i++) f.parts[i] = g_parts[i].exchange(0, std::memory_order_relaxed);
    f.ticks = g_ticks.exchange(0, std::memory_order_relaxed);
    f.replayed = g_replayed.exchange(0, std::memory_order_relaxed);
    if (g_capturing) {
        CapturedFrame c;
        c.micros = frameMicros;
        c.net = f.parts[static_cast<int>(Part::NetFrame)];
        c.battle = f.parts[static_cast<int>(Part::BattleFrame)];
        c.draw = f.parts[static_cast<int>(Part::Overlay)] + f.parts[static_cast<int>(Part::Present)];
        std::lock_guard lock(g_captureMutex);
        g_captured.push_back(c);
    }
    const ULONGLONG now = GetTickCount64();
    if (!where || frameMicros > kPauseMicros) return;
    const bool menus = std::strcmp(where, "menus") == 0;
    const double ms = frameMicros / 1000.0;
    if (ms >= kHitchMs) Log("hitch%s: one frame took %s", menus ? " (menus)" : "", Describe(f).c_str());
    if (ms >= kSlowMs) {
        g_slow++;
        g_slowExtraMicros += frameMicros - kFrameMicros;
        if (f.micros > g_slowest.micros) g_slowest = f;
    }
    if (now - g_windowStart >= kWindowMs) {
        if (g_slow > 0) {
            Log("hitch%s: %d slow frames in the last second (%.0f ms lost), the slowest %s", menus ? " (menus)" : "", g_slow,
                g_slowExtraMicros / 1000.0, Describe(g_slowest).c_str());
        }
        g_windowStart = now;
        g_slow = 0;
        g_slowExtraMicros = 0;
        g_slowest = Frame{};
    }
}

} // namespace sfv::mod::hitch
