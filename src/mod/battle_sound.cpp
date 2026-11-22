// The battle's sounds: capturing and stopping them.

#include "mod/battle_internal.h"

#include "mod/stack.h"

#include <mutex>
#include <utility>
#include <intrin.h>

namespace sfv::mod {

using namespace battle;

namespace {

constexpr game::CodeSite kCuePlay{
    "CriWare cue play request", 0xEE7F10, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8B}};
using CuePlayFn = uint64_t (*)(void* player, int sheet, int cue, int a, int b);
CuePlayFn g_origCuePlay = nullptr;
std::atomic<int> g_soundLog{0};
std::atomic<int> g_soundStacks{0};

constexpr game::CodeSite kCueStop{
    "CriWare playback stop request", 0xEE8710, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0x39, 0x00, 0x48, 0x8B, 0xF9, 0x0F, 0x84, 0xEA}};
using CueStopFn = void (*)(void* player, int handle, bool withoutRelease);
CueStopFn g_cueStop = nullptr;
std::mutex g_captureMutex;
std::atomic<bool> g_capturing{false};
std::vector<std::pair<void*, int>> g_captured;
thread_local bool t_captureExempt = false;
constexpr int kAmbienceSheet = 58;
constexpr size_t kBattleSoundsKept = 16384;
constexpr size_t kCapturedMax = 4096;
constexpr int kNoSound = -1;
constexpr int kSoundStacksLogged = 6;
constexpr bool kWithoutRelease = true;
std::atomic<bool> g_recordBattle{false};
std::vector<std::pair<void*, int>> g_battleSounds;

uint64_t CuePlayDetour(void* player, int sheet, int cue, int a, int b) {
    const bool capture = g_capturing && !t_captureExempt && sheet != kAmbienceSheet;
    const bool keepForBattle = g_recordBattle && !t_captureExempt;
    if (g_soundLog > 0) {
        g_soundLog--;
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
        Log("sound: cue %d (sheet %d, %d %d) %s tick %u%s%s, thread %lu, from rva 0x%llX", cue, sheet, a, b,
            g_inDriver ? "in driven" : "outside driven", FlowDrivenTicks(), g_inDriver && g_replayingTick ? " replayed" : "",
            capture ? " (captured)" : "", GetCurrentThreadId(), static_cast<unsigned long long>(from));
        if (g_soundStacks > 0) {
            g_soundStacks--;
            LogStack("sound: cue play", g_imageBase);
        }
    }
    const uint64_t handle = g_origCuePlay(player, sheet, cue, a, b);
    if (const int id = static_cast<int>(handle); id != kNoSound && (capture || keepForBattle)) {
        std::lock_guard lock(g_captureMutex);
        if (capture && g_capturing && g_captured.size() < kCapturedMax) g_captured.push_back({player, id});
        if (keepForBattle && g_recordBattle) {
            if (g_battleSounds.size() >= kBattleSoundsKept) g_battleSounds.erase(g_battleSounds.begin(), g_battleSounds.begin() + kBattleSoundsKept / 2);
            g_battleSounds.push_back({player, id});
        }
    }
    return handle;
}

void StopAll(const std::vector<std::pair<void*, int>>& sounds) {
    for (const auto& [player, handle] : sounds) g_cueStop(player, handle, kWithoutRelease);
}

} // namespace

void battle::InstallSoundHooks(const GameImage& image) {
    if (!Hook(image, kCuePlay, reinterpret_cast<void*>(&CuePlayDetour), reinterpret_cast<void**>(&g_origCuePlay))) {
        Log("battle driver: soundlog unavailable");
    } else if (void* stop = image.At<void*>(kCueStop.rva); std::memcmp(stop, kCueStop.bytes, sizeof(kCueStop.bytes)) == 0) {
        g_cueStop = reinterpret_cast<CueStopFn>(stop);
    } else {
        Log("battle driver: signature mismatch at %s: sounds from muted runs may be heard after", kCueStop.name);
    }
}

void BattleBeginSoundCapture() {
    std::lock_guard lock(g_captureMutex);
    g_captured.clear();
    g_capturing = g_cueStop != nullptr;
}

int BattleStopCapturedSounds() {
    std::vector<std::pair<void*, int>> captured;
    {
        std::lock_guard lock(g_captureMutex);
        g_capturing = false;
        captured.swap(g_captured);
    }
    StopAll(captured);
    return static_cast<int>(captured.size());
}

void BattleSetSoundCaptureExempt(bool exempt) { t_captureExempt = exempt; }

void BattleRecordSounds(bool on) {
    std::lock_guard lock(g_captureMutex);
    g_recordBattle = on;
    g_battleSounds.clear();
}

int BattleStopSounds() {
    std::vector<std::pair<void*, int>> sounds, captured;
    {
        std::lock_guard lock(g_captureMutex);
        sounds.swap(g_battleSounds);
        captured.swap(g_captured);
        g_capturing = false;
    }
    if (g_cueStop) {
        StopAll(sounds);
        StopAll(captured);
    }
    return static_cast<int>(sounds.size());
}

size_t BattleSoundCaptureMark() {
    std::lock_guard lock(g_captureMutex);
    return g_captured.size();
}

void BattleKeepCapturedFrom(size_t mark) {
    std::lock_guard lock(g_captureMutex);
    if (mark < g_captured.size()) g_captured.resize(mark);
}

void BattleLogSoundPlays(int count) {
    g_soundLog = count;
    g_soundStacks = kSoundStacksLogged;
    Log("sound: logging the next %d cue plays (the first %d with stacks)", count, kSoundStacksLogged);
}

} // namespace sfv::mod
