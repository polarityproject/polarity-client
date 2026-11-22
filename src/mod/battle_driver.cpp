// The battle frame hook: the frame driver runs the battle's ticks.

#include "mod/battle_internal.h"

#include "mod/camera_rollback.h"
#include "mod/frame_meter.h"
#include "mod/hitbox_display.h"
#include "mod/hitch.h"
#include "mod/stack.h"

#include <MinHook.h>

namespace sfv::mod {

using namespace battle;

namespace {

using RecordFrameFn = uint64_t (*)(uint8_t* inputUnit, void* a2, void* a3, void* a4);

SfvGame::BattleFrameFn g_origBattleFrame = nullptr;
RecordFrameFn g_origRecordFrame = nullptr;
std::atomic<IFrameDriver*> g_driver{nullptr};

std::atomic<DWORD> g_frameThread{0};
int g_strayRecords = 0;

constexpr int kDetachWaitMs = 5000;
constexpr int kStrayRecordsLogged = 3;

void BattleFrameDetour(uint8_t* battleFrame) {
    HookGuard guard;
    g_battleFrame = battleFrame;
    g_game->OnBattleFrame(battleFrame);
    if (g_inDriver) {
        g_origBattleFrame(battleFrame);
        return;
    }
    g_frameThread = GetCurrentThreadId();
    bool handled = false;
    if (IFrameDriver* driver = g_driver) {
        g_inDriver = true;
        {
            hitch::Scope timed(hitch::Part::NetFrame);
            handled = driver->OnFrame(*g_game);
        }
        g_inDriver = false;
    }
    g_frameThread = 0;
    CameraFrameEnd();
    if (!handled) {
        hitch::Scope timed(hitch::Part::BattleFrame);
        g_origBattleFrame(battleFrame);
    }
    CollectHitboxes(*g_game);
    FrameMeterFrame(*g_game);
    BattleCineLogFrame();
    BattleCamTraceRecord(TraceAt::Frame);
}

uint64_t RecordFrameDetour(uint8_t* inputUnit, void* a2, void* a3, void* a4) {
    HookGuard guard;
    if (g_game->exclusiveTicks && g_driver && !g_inDriver) {
        if (g_strayRecords++ < kStrayRecordsLogged) LogStack("InputUnit::RecordFrame outside a driven tick (skipped)", g_imageBase);
        return 0;
    }
    g_game->OnInputUnit(inputUnit);
    return g_origRecordFrame(inputUnit, a2, a3, a4);
}

} // namespace

bool battle::Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original) {
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("battle driver: signature mismatch at %s", site.name);
        return false;
    }
    const MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("battle driver: hook %s failed: %s", site.name, MH_StatusToString(st));
        return false;
    }
    return true;
}

bool BattleInstallDriver(const GameImage& image, SfvGame* game) {
    g_game = game;
    FlowSetRegionLookup([game](const void* p) -> std::string {
        for (const StateRegion& r : game->Regions()) {
            if (p >= r.ptr && p < r.ptr + r.size) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s+0x%llx", r.name.c_str(),
                              static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - r.ptr));
                return buf;
            }
        }
        return "not in the snapshot";
    });
    g_imageBase = image.base;
    const bool ok = Hook(image, game::kBattleFrame, reinterpret_cast<void*>(&BattleFrameDetour),
                         reinterpret_cast<void**>(&g_origBattleFrame)) &&
                    Hook(image, game::kInputUnitRecord, reinterpret_cast<void*>(&RecordFrameDetour),
                         reinterpret_cast<void**>(&g_origRecordFrame));
    InstallRandomHooks(image);
    InstallPhaseHooks(image);
    InstallSoundHooks(image);
    InstallTraceHooks(image);
    game->SetBattleFrameFunction(g_origBattleFrame);
    Log("battle driver %s", ok ? "installed" : "NOT installed");
    return ok;
}

void BattleSetFrameDriver(IFrameDriver* driver) { g_driver = driver; }

void BattleSetReplayingTick(bool replaying) {
    g_replayingTick = replaying;
    CountPhaseTick();
}

bool BattleInReplayedTick() { return g_inDriver && g_replayingTick; }

void BattleDetachFrameDriver() {
    g_driver = nullptr;
    const DWORD self = GetCurrentThreadId();
    for (int waited = 0; waited < kDetachWaitMs; waited++) {
        const DWORD busy = g_frameThread;
        if (busy == 0 || busy == self) return;
        Sleep(1);
    }
    Log("battle driver: the game thread is still in a frame driver after 5 s");
}

IFrameDriver* BattleCurrentFrameDriver() { return g_driver; }

void BattleSetFreeze(bool on) {
    struct Freeze : IFrameDriver {
        bool OnFrame(SfvGame&) override { return true; }
    };

    static Freeze freeze;
    BattleSetFrameDriver(on ? &freeze : nullptr);
    Log("freeze: %s", on ? "on" : "off");
}

} // namespace sfv::mod
