// The mod's start: the instance, the log, the settings and the hooks (docs/startup.md).

#include "core/version.h"
#include "builtin_noise.h"
#include "core/replay_file.h"
#include "core/state.h"
#include "mod/battle_data.h"
#include "mod/battle_driver.h"
#include "mod/camera_rollback.h"
#include "mod/hud_step.h"
#include "mod/battle_setup.h"
#include "mod/boot.h"
#include "mod/config.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/mod_main.h"
#include "mod/crash_report.h"
#include "mod/flow.h"
#include "mod/overlay.h"
#include "mod/image.h"
#include "mod/netplay_driver.h"
#include "mod/lifecycle.h"
#include "mod/files.h"
#include "mod/log.h"
#include "mod/migrate.h"
#include "mod/probes.h"
#include "mod/sfv_game.h"
#include "mod/report_upload.h"
#include "mod/entitlements.h"
#include "mod/updater.h"
#include "mod/status.h"
#include "mod/stress_driver.h"
#include "mod/rooms_client.h"
#include "mod/rooms_screen.h"
#include "mod/weyve_service.h"
#include "mod/ui_sound.h"

#include <windows.h>

#include <thread>

#include <cstring>
#include <string>

namespace sfv::mod {

namespace {

constexpr const char* kModVersion = kPolarityVersion;
constexpr int kSteamWaitMs = 120000, kSteamPollMs = 100;

std::wstring DirectoryOf(HMODULE module) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::string Narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    if (!s.empty()) s.pop_back();
    return s;
}

} // namespace

DWORD WINAPI ModMain(LPVOID self) {
    ThreadGuard running;
    const std::wstring dir = DirectoryOf(static_cast<HMODULE>(self));
    wchar_t instanceText[8] = L"0";
    GetEnvironmentVariableW(L"SFV_INSTANCE", instanceText, 8);
    const int instance = _wtoi(instanceText);
    const std::wstring suffix = instance == 0 ? L"" : L"." + std::to_wstring(instance);
    const MigrateResult migrated = MigrateFromSfvr(dir);
    FilesSetDataDir(dir + L"\\Polarity");
    FilesEnsureDirectory(FilesLogsDir());
    FilesEnsureDirectory(FilesMatchesDir());
    const std::string lastRun = CrashCheckPreviousRun(suffix, kModVersion);
    FilesRotateLog(FilesLogsDir(), L"polarity" + suffix);
    LogOpen(FilesLogsDir() + L"\\polarity" + suffix + L".log");
    for (const std::string& line : migrated.lines) Log("%s", line.c_str());
    if (migrated.waited) Log("migrate: waited for another game moving SFVR's files");
    if (!lastRun.empty()) Log("%s", lastRun.c_str());
    CrashInstallHandler(suffix, kModVersion);
    std::wstring iniPath = dir + L"\\polarity" + suffix + L".ini";
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) iniPath = dir + L"\\polarity.ini";
    const std::wstring panelPath = dir + L"\\polarity.panel" + suffix + L".ini";
    const Config config = ConfigLoad(iniPath, dir, panelPath);
    Log("Polarity %s, instance %d, mode %s", kModVersion, instance, ConfigModeName(config.mode));
    if (GetEnvironmentVariableW(L"POLARITY_RESTARTED", nullptr, 0)) {
        SetEnvironmentVariableW(L"POLARITY_RESTARTED", nullptr);
        Log("started again by the last run's restart (after an update)");
    }
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModMain), &module);
        Log("module at %p (for scripts/symbolize.py)", static_cast<void*>(module));
    }
    if (config.mode == Mode::Off) return 0;

    for (int i = 0; i < kSteamWaitMs / kSteamPollMs && !GetModuleHandleW(L"steam_api64.dll"); i++) Sleep(kSteamPollMs);
    if (!GetModuleHandleW(L"steam_api64.dll")) {
        Log("steam_api64.dll never loaded; not hooking");
        return 0;
    }

    static GameImage image;
    std::string why;
    if (!OpenGameImage(&image, &why)) {
        Log("not hooking: %s", why.c_str());
        return 0;
    }
    Log("game image at %p verified", reinterpret_cast<void*>(image.base));
    StatusSetInstance(instance);

    static StateMap stateMap;
    const std::wstring stateMapPath = dir + L"\\sfv_statemap.bin";
    const bool haveMap = config.mode != Mode::LearnStateMap && stateMap.Load(stateMapPath);
    Log("state map: %s", haveMap ? "loaded" : config.mode == Mode::LearnStateMap ? "learning"
                                                                                 : "none (hashes include pointers)");

    static SfvGame game(image);
    if (haveMap) game.SetStateMap(&stateMap);
    const std::wstring noisePath = dir + L"\\" + (config.noiseFile.empty() ? L"sfv_noise.txt" : config.noiseFile);
    StressSetNoisePath(noisePath);
    if (!config.noiseFile.empty() && game.LoadNoise(noisePath)) {
        Log("noise mask: %s (%zu words; not the built-in one: peers need the same file)", Narrow(config.noiseFile).c_str(),
            game.NoiseWords());
    } else {
        if (!config.noiseFile.empty()) Log("noise mask: cannot read %s; the built-in one instead", Narrow(config.noiseFile).c_str());
        game.LoadNoiseText(reinterpret_cast<const char*>(kBuiltinNoise), sizeof(kBuiltinNoise));
        Log("noise mask: built in (%zu words)", game.NoiseWords());
    }
    Log("game files: %s", FilesGameText().c_str());
    std::thread([] {
        ThreadGuard running;
        Log("mods: %s", BattleDataScanModPaks().Text().c_str());
    }).detach();

    if (!ProbesInstall(image, config, &game, &stateMap, stateMapPath)) return 0;
    EntitlementsInstall(image.base, iniPath);
    CrashHookProcessEnd();
    InputInstall(config.virtualPads, config.alwaysActive);
    ControlInstall(dir + L"\\sfv_cmd" + suffix + L".txt");
    OverlayInstall(panelPath);
    UiSoundInstall(image);
    OverlaySetBuild(std::string("Polarity ") + kModVersion);
    if (migrated.moved) OverlaySetMigratedNotice();
    UpdateStart(dir, kModVersion, Narrow(config.updateUrl), config.updateCheck);
    BattleSetupInstall(image);
    BootInstallProbes(image, config.verbose);
    BootInstallFreePlayGuard(image);
    FlowInstallProbes(image);
    BootWatchLoginStatus();
    BattleInstallDriver(image, &game);
    CameraInstall(image);
    HudStepInstall(image);
    CameraSetRollback(config.netplay.cameraRollback);
    if (config.netplay.skipLoadCallbacks) GameSetSkippedLoadCallbacks(GameDefaultSkippedLoadCallbacks());
    GameSetLeanResim(config.netplay.leanResim);
    if (!config.netplay.leanResim) FlowSetResimTaskSkip({});
    NetInstall(&game, config.reportDir, kModVersion, config.netplay);
    WeyveService::SetHideCountry(!config.netplay.shareCountry);
    RoomsScreenSetStreamerMode(config.netplay.streamerMode);
    RoomsSetWordFilter(config.netplay.wordFilter);
    ReportsInstall(config.reportDir, Narrow(config.reportUrl), kModVersion, config.netplay.uploadReports);

    switch (config.mode) {
        case Mode::Netplay:
        case Mode::Playback: {
            if (config.mode == Mode::Playback) {
                ReplayFile rf;
                std::string error;
                if (LoadReplay(FilesUtf8Path(config.playbackFile), &rf, &error)) {
                    Log("playback: %s, %d frames, build %s (this build %s)", Narrow(config.playbackFile).c_str(),
                        rf.FrameCount(), rf.gameBuild.c_str(), game.BuildId().c_str());
                } else {
                    Log("playback: %s", error.c_str());
                }
            }
            Log("%s needs the phase 3 input and tick hooks; running probes only", ConfigModeName(config.mode));
            break;
        }
        default:
            break;
    }
    return 0;
}

} // namespace sfv::mod
