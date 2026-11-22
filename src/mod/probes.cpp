// The start probe: RollbackUnit offline, the ring and the regions, restores and the SSE control word (docs/research.md).

#include "mod/probes.h"

#include "core/state.h"
#include "mod/battle_setup.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/lifecycle.h"
#include "mod/stack.h"
#include "mod/log.h"
#include "mod/stress_driver.h"
#include "mod/synctest.h"
#include "mod/watch.h"

#include <MinHook.h>
#include <windows.h>
#include <xmmintrin.h>

#include <atomic>
#include <vector>

namespace sfv::mod {

namespace {

constexpr uint32_t kMxcsrFlags = 0x3F, kMxcsrDefault = 0x1F80;
constexpr SHORT kKeyDown = static_cast<SHORT>(0x8000);
constexpr uint64_t kLearnEveryTicks = 30, kSaveEverySamples = 20, kSummaryEveryTicks = 300;
constexpr int kExtraStackTicks = 3;
constexpr int32_t kFrameIdMask = 0x7FFFFFFF, kProbeFrameTag = 0x5346;

using PreFn = uint64_t (*)(uint8_t* unit, void* a2, void* a3, void* a4);
using BeginFrameFn = int (*)(uint8_t* manager, const game::FrameId* id);
using StoreFn = void (*)(uint8_t* manager, int group);
using RestoreFn = int (*)(uint8_t* manager, const game::FrameId* id, int discardNewer);

struct ProbeState {
    Config config;
    GameImage image;
    SfvGame* game = nullptr;
    StateMap* stateMap = nullptr;
    std::wstring stateMapPath;

    PreFn origPre = nullptr;
    BeginFrameFn origBeginFrame = nullptr;
    StoreFn origStore = nullptr;
    RestoreFn origRestore = nullptr;

    std::atomic<uint64_t> ticks{0}, beginFrames{0}, stores{0}, restores{0};
    uint64_t lastSummaryTick = 0;
    int32_t forcedFrame = 0;
    game::FrameId lastOpenId{-1, -1};

    std::vector<uint8_t> copySave;
    uint64_t copySaveTick = 0;
    game::FrameId ringSaveId{-1, -1};

    bool keyDown[256] = {};
    uint64_t learnSamples = 0;
    uint32_t mxcsr = ~0u;
    SyncTest syncTest;
    int stackTicks = 0;
};

ProbeState g;

bool Pressed(int vk) {
    bool foreground = false;
    if (HWND w = InputRealForegroundWindow()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        foreground = pid == GetCurrentProcessId();
    }
    const bool down = foreground && (GetAsyncKeyState(vk) & kKeyDown) != 0;
    const bool edge = down && !g.keyDown[vk];
    g.keyDown[vk] = down;
    return edge;
}

void HandleHotkeys() {
    const auto& regions = g.game->Regions();
    const GameRequest request = ControlTakeGameRequest();
    const GameCommand cmd = request.command;
    if (cmd == GameCommand::SyncTest) g.syncTest.Start(request.arg0, static_cast<uint64_t>(request.arg1));
    if (cmd == GameCommand::Watch) {
        if (request.arg0 >= 0 && size_t(request.arg0) < regions.size() &&
            size_t(request.arg1) + request.arg2 <= regions[request.arg0].size) {
            ArmWatch(0, regions[request.arg0].ptr + request.arg1, request.arg2, g.image.base);
        } else {
            Log("watch: no region %d with offset 0x%x", request.arg0, request.arg1);
        }
    }
    if (cmd == GameCommand::WatchAddr) {
        ArmWatch(0, reinterpret_cast<const void*>(request.address), request.arg2, g.image.base, request.flag);
    }
    if (cmd == GameCommand::WatchReport) WatchReport();
    if (cmd == GameCommand::Stress || cmd == GameCommand::LearnNoise) {
        RequestStressTest(request.arg0, request.arg1, static_cast<uint64_t>(request.arg2),
                          cmd == GameCommand::LearnNoise);
    }
    if (cmd == GameCommand::Unwatch) DisarmWatches();
    const bool keys = g.config.savestateHotkeys;
    const bool save = (keys && Pressed(VK_F5)) || cmd == GameCommand::Save;
    const bool load = (keys && Pressed(VK_F9)) || cmd == GameCommand::Load;
    const bool ringSave = (keys && Pressed(VK_F6)) || cmd == GameCommand::RingSave;
    const bool ringLoad = (keys && Pressed(VK_F10)) || cmd == GameCommand::RingLoad;
    if (cmd == GameCommand::Layout) g.game->LogLayout();

    if (save) {
        g.copySave.resize(StateSize(regions));
        SaveRegions(regions, g.copySave.data());
        g.copySaveTick = g.ticks;
        Log("F5: copied %zu regions (%zu bytes) at tick %llu", regions.size(), g.copySave.size(),
            static_cast<unsigned long long>(g.ticks.load()));
    }
    if (load) {
        if (g.copySave.size() == StateSize(regions) && !g.copySave.empty()) {
            LoadRegions(regions, g.copySave.data());
            g.game->OnStateLoaded(static_cast<int>(g.copySaveTick));
            Log("F9: restored the copy from tick %llu", static_cast<unsigned long long>(g.copySaveTick));
        } else {
            Log("F9: no copy for this battle's layout");
        }
    }
    if (ringSave) {
        g.ringSaveId = g.lastOpenId;
        Log("F6: remembered ring frame %d:%d", g.ringSaveId.lo, g.ringSaveId.hi);
    }
    if (ringLoad) {
        const bool ok = g.ringSaveId.lo >= 0 && g.game->RestoreRingFrame(g.ringSaveId, true);
        Log("F10: game Restore of ring frame %d:%d %s", g.ringSaveId.lo, g.ringSaveId.hi,
            ok ? "succeeded" : "failed (not in the ring any more?)");
    }
}

void LogSummary(uint8_t* unit) {
    const auto inv = g.game->Inspect();
    Log("tick %llu: enabled=%u open=%u flags=%u/%u interval=%d/%d lastId=%d:%d | ring %llu, current %d:%d | "
        "since last: %llu BeginFrame, %llu Store, %llu Restore",
        static_cast<unsigned long long>(g.ticks.load()), unit[game::unit::kEnabled], unit[game::unit::kFrameOpen],
        unit[game::unit::kFlag109], unit[game::unit::kFlag10A],
        *reinterpret_cast<int32_t*>(unit + game::unit::kInterval),
        *reinterpret_cast<int32_t*>(unit + game::unit::kIntervalBase),
        reinterpret_cast<game::FrameId*>(unit + game::unit::kLastFrameId)->lo,
        reinterpret_cast<game::FrameId*>(unit + game::unit::kLastFrameId)->hi,
        static_cast<unsigned long long>(inv.ringCount), inv.currentId.lo, inv.currentId.hi,
        static_cast<unsigned long long>(g.beginFrames.exchange(0)),
        static_cast<unsigned long long>(g.stores.exchange(0)), static_cast<unsigned long long>(g.restores.exchange(0)));
}

uint64_t PreDetour(uint8_t* unit, void* a2, void* a3, void* a4) {
    HookGuard guard;
    if (const uint32_t csr = _mm_getcsr() & ~kMxcsrFlags; csr != g.mxcsr) {
        Log("MXCSR control %08X%s", csr, csr == kMxcsrDefault ? " (default)" : " (NOT the default 1F80: float results may differ)");
        g.mxcsr = csr;
    }

    g.game->OnRollbackUnitTick(unit);
    if (g.game->RefreshRegions()) {
        if (g.config.verbose) {
            g.game->LogLayout();
            LogStack("RollbackUnit::Pre", g.image.base);
            g.stackTicks = kExtraStackTicks;
        }
        LogBattleSetup("at battle start");
    }

    if (g.config.mode == Mode::Probe) HandleHotkeys();
    g.syncTest.OnTick(*g.game);
    if (g.stackTicks > 0 && --g.stackTicks >= 0) LogStack("RollbackUnit::Pre", g.image.base);

    if (g.config.mode == Mode::LearnStateMap && g.ticks % kLearnEveryTicks == 0) {
        g.game->LearnPointers(*g.stateMap);
        if (++g.learnSamples % kSaveEverySamples == 0) {
            const bool saved = g.stateMap->Save(g.stateMapPath);
            Log("state map: %llu samples, %s", static_cast<unsigned long long>(g.learnSamples),
                saved ? "saved" : "could not save");
        }
    }

    if (g.config.forceEnable) unit[game::unit::kEnabled] = 1;
    const uint64_t result = g.origPre(unit, a2, a3, a4);

    if (g.config.forceOfflineFrames && unit[game::unit::kEnabled] && !unit[game::unit::kFrameOpen]) {
        game::FrameId id{g.forcedFrame++ & kFrameIdMask, kProbeFrameTag};
        if (uint8_t* m = g.game->Manager(); m && g.origBeginFrame(m, &id)) {
            unit[game::unit::kFrameOpen] = 1;
            *reinterpret_cast<game::FrameId*>(unit + game::unit::kLastFrameId) = id;
        }
    }
    if (unit[game::unit::kFrameOpen]) g.lastOpenId = *reinterpret_cast<game::FrameId*>(unit + game::unit::kLastFrameId);

    const uint64_t tick = ++g.ticks;
    if (g.config.verbose && (tick == 1 || tick - g.lastSummaryTick >= kSummaryEveryTicks)) {
        g.lastSummaryTick = tick;
        LogSummary(unit);
    }
    return result;
}

int BeginFrameDetour(uint8_t* manager, const game::FrameId* id) {
    HookGuard guard;
    if (g.beginFrames++ < 3) Log("BeginFrame(%p, %d:%d)", manager, id->lo, id->hi);
    return g.origBeginFrame(manager, id);
}

void StoreDetour(uint8_t* manager, int group) {
    HookGuard guard;
    g.stores++;
    g.origStore(manager, group);
}

int RestoreDetour(uint8_t* manager, const game::FrameId* id, int discardNewer) {
    HookGuard guard;
    g.restores++;
    const int ok = g.origRestore(manager, id, discardNewer);
    Log("Restore(%p, %d:%d, discard=%d) = %d", manager, id->lo, id->hi, discardNewer, ok);
    return ok;
}

template <typename Fn>
bool Hook(const GameImage& image, const game::CodeSite& site, Fn detour, Fn* original) {
    void* target = image.At<void*>(site.rva);
    const MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    if (st != MH_OK) {
        Log("hook %s failed: %s", site.name, MH_StatusToString(st));
        return false;
    }
    return true;
}

} // namespace

bool ProbesInstall(const GameImage& image, const Config& config, SfvGame* game, StateMap* stateMap,
                   const std::wstring& stateMapPath) {
    g.config = config;
    g.image = image;
    g.game = game;
    g.stateMap = stateMap;
    g.stateMapPath = stateMapPath;

    if (MH_Initialize() != MH_OK) {
        Log("MinHook init failed");
        return false;
    }
    bool ok = Hook(image, game::kRollbackUnitPre, &PreDetour, &g.origPre);
    ok = ok && Hook(image, game::kManagerBeginFrame, &BeginFrameDetour, &g.origBeginFrame);
    ok = ok && Hook(image, game::kManagerStore, &StoreDetour, &g.origStore);
    ok = ok && Hook(image, game::kManagerRestore, &RestoreDetour, &g.origRestore);
    game->includeSyncCounters = config.includeSyncCounters;
    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        Log("probes not installed");
        return false;
    }
    Log("probes installed (hotkeys %s, force_offline_frames %d, force_enable %d)",
        config.savestateHotkeys ? "on" : "off", config.forceOfflineFrames, config.forceEnable);
    return true;
}

} // namespace sfv::mod
