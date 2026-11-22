#pragma once
// Updates from the project site, asking first. At start the mod reads the
// site's update info (core/update_info.h) and checks its signature; when it
// names a newer version, the panel offers it (Settings, Updates, and a
// toast). If the player agrees, the files are downloaded into Polarity\update,
// checked against their sizes and SHA-256, and swapped in: each file the
// game has (even the loaded loader, xinput1_3.dll) is renamed to
// <name>.<n>.old, the new one moved into its place. The mod runs from a copy
// (proxy/load_mod.h), so the new build takes over at the next launch, which
// also deletes the .old files. The player's ini files are never touched.
//
// The site's address: [update] url in polarity.ini, else kDefaultUpdateUrl.

#include <cstdint>
#include <string>

namespace sfv::mod {

// The update channel this build follows, and its update info on the project
// site (polarity-web on Cloudflare Pages): main builds read update.json, the
// rooms branch's builds rooms/update.json (docs/custom-rooms-plan.md). The
// channel is part of the build, so 0.3.0, built from main, moves branch
// installs back by itself. release.ps1 checks it against -Channel.
constexpr const char* kUpdateChannel = "main";
constexpr const char* kDefaultUpdateUrl = "https://polaritymod.link/update.json";

struct UpdateView {
    enum class State {
        Off,  // no site set, or no signing key in this build
        Idle, // not checked yet
        Checking,
        UpToDate,
        Available,   // `latest` is newer: the player may install it
        Downloading, // `done` of `total` bytes
        Ready,       // installed: the next launch runs `latest`
        Failed,      // `message` says why; checking again is allowed
    };
    State state = State::Off;
    std::string current; // this build's version
    std::string latest;  // the site's
    std::string published;
    std::string notes;
    std::string message; // what failed, or why updates are off
    std::string url;
    uint64_t done = 0, total = 0;
    int64_t checkedAtMs = -1; // GetTickCount64 when the last check ended (any outcome); -1 never
};

// From the mod's start: `gameDir` is where the game's files are (the DLL's
// folder). With `checkNow`, checks in the background at once.
void UpdateStart(const std::wstring& gameDir, const std::string& currentVersion, const std::string& url, bool checkNow);
UpdateView UpdateViewNow();
// Both run in the background; ignored while a check or download runs.
void UpdateCheck();
void UpdateInstall();
// For tests: another site (empty: the configured one).
void UpdateSetUrl(const std::string& url);
// Checks unless a check ended within `maxAgeMs` (or one runs, or an update
// is already found): what Custom Rooms asks before letting a player in, so
// only the latest client plays there.
bool UpdateCheckIfStale(int64_t maxAgeMs); // true: a check started (the state is Checking already)
// An update found, being downloaded or installed: Custom Rooms waits for it.
bool UpdatePending(const UpdateView& v);
// For tests: this build reports `version` (the updater compares with it,
// rooms show it).
void UpdatePretendVersion(const std::string& version);
// Once an update is installed: starts the game again (the new build). A new
// game process is started with POLARITY_WAIT_PID (and SFVR_WAIT_PID) set (the loader waits for this
// one to end first: the game allows one copy of itself), then `window` is
// asked to close. False (and why) if the new process could not start; this
// game then keeps running.
bool UpdateRestartGame(void* window, std::string* error);

} // namespace sfv::mod
