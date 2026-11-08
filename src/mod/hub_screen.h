#pragma once
// The SFVR hub (docs/online-lobby.md "The SFVR hub and settings";
// custom-rooms-plan.md M3; mock-up board 1): the mod's front screen after the
// title, a grid like the game's own main menu. The left column holds this
// PC's own things (Play Offline; the Battle Log after 0.3.0), the panel at the
// top right shows the focused entry (its character over a stage, the title,
// a live line), and the online modes sit under it (Custom Rooms; the Global
// Lobby and Quick Match later). Start opens SFVR settings; Back + Start in
// the game's menus comes back. It fades in at launch in place of the game's
// main menu.
//
// Laid out from a list of entries and frames, so later modes are entries,
// not a new screen. Render thread, inside an ImGui frame.

#include "mod/updater.h"

#include <string>

struct ImFont;

namespace sfv::mod {

struct HubFonts {
    ImFont* display = nullptr; // Archivo Black: titles
    ImFont* strong = nullptr;  // Barlow SemiBold: info lines, the player's name
    ImFont* body = nullptr;    // Barlow: the description, prompts
    ImFont* bold = nullptr;    // Barlow Bold: names, buttons, focused rows (the 1d look)
};

// What the hub shows that changes.
struct HubLive {
    std::string player;     // the name others see
    std::string server;     // "Public server" or the server's address
    int serverState = 0;    // 0 not connected, 1 connecting, 2 connected, 3 failed
    std::string version;    // "SFVR 0.3.0-rooms.1"
    std::string roomsInfo;  // Custom Rooms' live line ("3 public rooms")
    std::string roomsState; // on the Custom Rooms tile's strip: "9 public rooms · 31 players", or what is wrong
    // The pictures: Play Offline shows the player's favorite (Fighter
    // Profile), Custom Rooms its own.
    std::string offlineChara, offlineStage, roomsChara, roomsStage;
    // Why Custom Rooms cannot be used now (picking it shows this in a modal
    // instead of a screen that can do nothing), "" when it can.
    // CheckingUpdate: the check Custom Rooms asks for first; UpdatePending:
    // an update is out (only the latest client plays there).
    enum class RoomsProblem { None, Connecting, Unreachable, UpdateNeeded, Offline, CheckingUpdate, UpdatePending };
    RoomsProblem roomsProblem = RoomsProblem::None;
    std::string roomsServer;  // the address, for the modal
    std::string roomsBuild;   // UpdateNeeded: the build the server runs
    UpdateView update;        // the update modal's
    std::string restartError; // the last restart that failed, why
};

// RetryRooms: the modal's Try again (connect to the rooms server now).
// UpdateInstall / UpdateRestart / UpdateRetry: the update modal's buttons
// (download and install; start the game again; check again after a failure).
enum class HubAction { None, PlayOffline, CustomRooms, Settings, RetryRooms, UpdateInstall, UpdateRestart, UpdateRetry };

// `fromBlack`: at launch, in place of the game's main menu: it fades in
// from black (the menu never shows), else over whatever is there.
void OpenHub(bool fromBlack = false);
void CloseHub();
// Custom Rooms' modal on the hub (HubLive::roomsProblem): the rooms screen
// lost its server, so it gave way to the hub saying why.
void ShowRoomsProblem();
// The update modal: the found update, its download, then Restart.
void OpenUpdateModal();
bool UpdateModalOpen();
bool HubOpen();
// Draws the hub (fading in when opened) and, with `takeInput`, returns what
// the player chose this frame.
HubAction DrawHub(const HubFonts& fonts, const HubLive& live, bool takeInput);
// Its pictures, asked for ahead (they load behind whatever is drawn).
void WarmHubTextures(const HubLive& live);

} // namespace sfv::mod
