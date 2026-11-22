// The DLL's entry points: start, exit and hot reload (docs/startup.md).

#include "mod/battle_driver.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/crash_report.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/overlay.h"
#include "mod/watch.h"

#include <MinHook.h>
#include <windows.h>

namespace sfv::mod {
DWORD WINAPI ModMain(LPVOID self);
}

namespace {

constexpr int kDrainWaitMs = 5000, kDrainPollMs = 10;
constexpr DWORD kDetourTailMs = 50;

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, sfv::mod::ModMain, module, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        sfv::mod::CrashOnProcessExit("exit");
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool SfvCanUnload() {
    using namespace sfv::mod;
    const NetView::Phase phase = NetViewNow().phase;
    if (phase != NetView::Phase::Idle && phase != NetView::Phase::Ended) {
        Log("hot reload refused: a netplay session is on (leave it first)");
        return false;
    }
    if (BattleCurrentFrameDriver()) {
        Log("hot reload refused: the battle's frames are still driven (wait for the menus)");
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) void SfvShutdown() {
    using namespace sfv::mod;
    g_stopping = true;
    NetLeave();
    CrashRemoveHandler();
    WatchRemoveHandler();
    InputRestoreWindowProc();
    OverlayUnhookPresentTable();
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < kDrainWaitMs / kDrainPollMs && (g_inDetour > 0 || g_threads > 0); i++) Sleep(kDrainPollMs);
    Sleep(kDetourTailMs);
    OverlayShutdown();
    MH_Uninitialize();
    Log("shut down for reload (%d detours, %d threads still active)", g_inDetour.load(), g_threads.load());
    LogClose();
}
