#pragma once
// polarity.ini, next to the DLL. See polarity.ini.example.

#include "mod/netplay_driver.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sfv::mod {

enum class Mode {
    Off,           // load, verify the exe, do nothing
    Probe,         // phase 1-2: log the rollback machinery, savestate hotkeys
    LearnStateMap, // record which state words hold pointers, write sfv_statemap.bin
    Netplay,       // phase 6: needs the input and tick hooks (not found yet)
    Playback,      // verify an .sfvr in local Versus: needs the same hooks
};

struct Config {
    Mode mode = Mode::Probe;

    // probe
    bool savestateHotkeys = false;   // F5/F6/F9/F10 ([probe] debug_hotkeys)
    bool forceOfflineFrames = false; // open snapshot frames offline (see probes.cpp)
    bool forceEnable = false;        // set RollbackUnit::enabled offline
    bool includeSyncCounters = true; // snapshot the battle sync frame counters too
    bool verbose = false;            // region layouts, stacks and tick summaries

    // netplay
    int localPlayer = 0;
    int inputDelay = 2;
    int predictionWindow = 8;

    // playback
    std::wstring playbackFile;
    int stressDistance = 0;

    // A noise mask file next to the DLL to use instead of the one built in
    // (for learning new words); empty: the built-in one.
    std::wstring noiseFile;

    std::wstring reportDir; // match recordings and logs; default <dll dir>\Polarity\matches (files.h)

    // [update] (updater.h): the site's update info, and whether to check at start
    std::wstring updateUrl;
    bool updateCheck = true;

    // [report] url (report_upload.h): where desync reports go
    std::wstring reportUrl;

    NetplayOptions netplay; // [netplay] and [netsim]

    int virtualPads = 0;       // XInput pads driven by sfv_cmd.txt, connected at startup
    bool alwaysActive = false; // keep reading pads while the window is in the background
};

// panelPath: the overlay's saved choices (polarity.panel[.N].ini); its
// [netplay] keys override the main file's.
Config LoadConfig(const std::wstring& path, const std::wstring& dllDir, const std::wstring& panelPath = L"");
// Writes the player's choices (pick, stage, delay, prediction window, name,
// spectators allowed, own frame rate shown) to the overlay's file. `stage`: a code, "random" or "" (as prepared).
void SaveNetplayChoices(const std::wstring& path, const NetplayOptions& n, const std::string& stage);
// My character's Random stages (`random_stages_off`, core/random_stage.h)
// into the overlay's file, nothing else (SaveNetplayChoices leaves the key
// alone: only the list's own changes write it).
void SaveRandomStagesOff(const std::wstring& path, const std::vector<std::string>& off);
// Each character's loadout as last saved with it (SaveNetplayChoices keeps the
// pick's in [loadouts] of the overlay's file), by character code.
std::map<std::string, PlayerSetup> ReadLoadouts(const std::wstring& path);
const char* ModeName(Mode mode);
// polarity.ini as LoadConfig read it, and one value written into it
// (Polarity settings' Check at start and At start).
std::wstring MainIniPath();
void SaveMainIni(const wchar_t* section, const wchar_t* key, int value);
int ReadMainIni(const wchar_t* section, const wchar_t* key, int fallback);

} // namespace sfv::mod
