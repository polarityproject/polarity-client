#include "mod/hud_step.h"

#include "mod/log.h"
#include "mod/ue4.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

// The manager's per-frame advance (rva 0x17CA7AC): rcx = wrapper->+0x70 (the
// GFx::Movie), xmm1 = the time, r9b = 1 (capture), r8d = 2 (catch-up frames),
// call [vtable + 0xC0].
constexpr uint32_t kAdvanceSite = 0x17CA7AC;
constexpr uint8_t kAdvanceBytes[] = {0x48, 0x8B, 0x4F, 0x70, 0x41, 0x0F, 0x28, 0xCB, 0x41, 0xB1, 0x01, 0x48, 0x8B, 0x01,
                                     0x41, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xFF, 0x90, 0xC0, 0x00, 0x00, 0x00};
// The native player's SetPause (rva 0x17C72F0): the wrapper at +0x150, its
// playing flag at +0xA6.
constexpr uint32_t kPauseSite = 0x17C72F0;
constexpr uint8_t kPauseBytes[] = {0x4C, 0x8B, 0x81, 0x50, 0x01, 0x00, 0x00, 0x4D, 0x85, 0xC0, 0x74, 0x0C, 0x84, 0xD2, 0x0F, 0x94};
// ScaleformUtilGFxPlayer: the native movie player (m_movie_player), its asset (m_playing_movie_asset).
constexpr size_t kNativePlayer = 0x398, kPlayingAsset = 0x390, kPlayerSize = 0x3A8;
constexpr size_t kWrapper = 0x150, kMovie = 0x70, kPlaying = 0xA6, kAdvanceSlot = 0xC0;

using AdvanceFn = float (*)(void* movie, float seconds, unsigned catchUp, bool capture);

bool g_ok = false;
std::atomic<int> g_on{1}; // 0 off, 1 Announce and CockpitTop, 2 Announce only (research)
uintptr_t g_base = 0;
std::vector<std::pair<uint8_t*, bool>> g_players; // this battle's Announce (true) and CockpitTop players

} // namespace

bool InstallHudStep(const GameImage& image) {
    g_base = image.base;
    g_ok = std::memcmp(image.At<const uint8_t*>(kAdvanceSite), kAdvanceBytes, sizeof(kAdvanceBytes)) == 0 &&
           std::memcmp(image.At<const uint8_t*>(kPauseSite), kPauseBytes, sizeof(kPauseBytes)) == 0;
    if (!g_ok) Log("hud step: signature mismatch: replay runs leave banners as they are");
    return g_ok;
}

void FindHudMovies() {
    g_players.clear();
    if (!g_ok || !ue4::EnsureObjects(g_base)) return;
    ue4::ForEachObject([](const void* object) {
        const std::string name = ue4::ObjectName(ue4::ClassOf(object));
        if (name != "ScaleformUtilGFxPlayer") return;
        auto* p = static_cast<uint8_t*>(const_cast<void*>(object));
        if (!IsReadable(p, kPlayerSize)) return;
        const void* asset = *reinterpret_cast<void* const*>(p + kPlayingAsset);
        const std::string movie = asset ? ue4::ObjectName(asset) : "";
        if (movie == "Announce" || movie == "CockpitTop") g_players.push_back({p, movie == "Announce"});
    });
    Log("hud step: %zu battle HUD movies", g_players.size());
}

void ForgetHudMovies() { g_players.clear(); }

namespace {
uint64_t g_steps = 0, g_stepTicks = 0; // StepHudMovies calls and their time (QPC), since the last report
} // namespace

std::string HudStepReport() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    char b[96];
    std::snprintf(b, sizeof(b), "%llu HUD steps, %.1f ms", static_cast<unsigned long long>(g_steps), g_stepTicks * 1000.0 / f.QuadPart);
    g_steps = g_stepTicks = 0;
    return b;
}

void StepHudMovies(float seconds) {
    if (!g_ok || !g_on) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    struct Count {
        LARGE_INTEGER& t0;
        LARGE_INTEGER& t1;

        ~Count() {
            QueryPerformanceCounter(&t1);
            g_steps++;
            g_stepTicks += t1.QuadPart - t0.QuadPart;
        }
    } count{t0, t1};

    const int mode = g_on;
    for (const auto& [p, announce] : g_players) {
        if (mode == 2 && !announce) continue;
        if (!IsReadable(p, kPlayerSize)) continue;
        auto* native = *reinterpret_cast<uint8_t**>(p + kNativePlayer);
        if (!native || !IsReadable(native, kWrapper + 8)) continue;
        auto* wrapper = *reinterpret_cast<uint8_t**>(native + kWrapper);
        if (!wrapper || !IsReadable(wrapper, kPlaying + 1) || !wrapper[kPlaying]) continue;
        void* movie = *reinterpret_cast<void**>(wrapper + kMovie);
        if (!movie || !IsReadable(movie, 8)) continue;
        const auto* vtable = *reinterpret_cast<const uintptr_t* const*>(movie);
        if (!IsReadable(vtable, kAdvanceSlot + 8)) continue;
        const uintptr_t fn = vtable[kAdvanceSlot / 8];
        if (fn < g_base || fn >= g_base + 0x5000000) continue;
        // No capture: the manager's own advance this frame captures for drawing.
        reinterpret_cast<AdvanceFn>(fn)(movie, seconds, 2, false);
    }
}

void SetHudStep(int mode) {
    g_on = mode;
    Log("hudstep: %s", mode == 2 ? "Announce only" : mode ? "replay runs keep the HUD's movies in step"
                                                          : "off");
}

bool HudStepOn() { return g_on != 0; }

} // namespace sfv::mod
