#pragma once
// The custom rooms screen (custom-rooms-plan.md M3; mock-up board 7): one
// screen to browse, create and join. On the left, Create a room and Join with
// a code, then the public rooms (six in view, scrolling with the focus,
// updating by themselves); on the right a panel that follows the focused row:
// a room's summary, the create form (name preset, visibility, tables), or the
// code slots and keypad. B goes back to the hub.
//
// Render thread, inside an ImGui frame. It draws from a RoomsView and says
// what the player asked for; the overlay hands that to rooms_client.

#include "core/keeper_protocol.h"
#include "mod/hub_screen.h"
#include "mod/rooms_client.h"

#include <imgui.h>

#include <string>

namespace sfv::mod {

struct RoomsScreenInput {
    bool back = false;          // to the hub
    bool create = false;        // ask the keeper for a room with `settings`
    keeper::RoomSettings settings;
    std::string join;           // join this room code
    bool clearNotice = false;   // "Try again" after a failed join
    std::string focusedRoom;    // the public room in focus (its detail is asked for: RoomsPeek)
    bool settings_ = false;     // open SFVR settings (wrong build, no server)
};

void OpenRoomsScreen();
void CloseRoomsScreen();
bool RoomsScreenOpen();
void DrawRoomsScreen(const HubFonts& fonts, const RoomsView& view, bool takeInput, RoomsScreenInput* input);

// Shared with the room screen: a character's face in a slanted frame, the
// owner's crown, the player glyph.
void DrawFace(ImDrawList* d, const std::string& chara, float x, float y, float w, float h);
// A room's title as this game shows it: the game's own Battle Lounge comment
// (its language; the keeper's English until the table is read) with the
// characters' names; no title: "<owner>'s room".
std::string RoomTitleText(const keeper::RoomTitle& title, const std::string& owner);
void DrawCrown(ImDrawList* d, float x, float y, float scale, ImU32 col);
// Text that shrinks to `maxW` (design units): its size down to `minSize`,
// then cut with an ellipsis. The width drawn.
// `stage`: text with nothing behind it (the light drop shadow under it).
float FitText(ImDrawList* d, ImFont* f, float size, float minSize, float maxW, ImVec2 at, ImU32 c, std::string t, float align = 0,
              bool stage = false);
// Streamer mode (SFVR settings): room codes are not shown on the rooms
// screens (Copy code still copies it). Any thread.
void SetStreamerMode(bool on);
bool StreamerMode();
// In place of a room code at `size` (the text's design size, from its top
// at y): six dots. The width (d null: only measured).
float DrawHiddenCode(ImDrawList* d, float x, float y, float size, ImU32 col);
// A player's country flag (the game's own, CommonAsset/CountryFlags), `h`
// tall from (x, y) in design units; a country the game has no flag for shows
// its two letters. Nothing for "" (unknown, or the player hides it). The
// width drawn: kFlagAspect x h for a flag, a little more for the letters; 0
// for nothing.
constexpr float kFlagAspect = 1.5f;
float DrawFlag(ImDrawList* d, ImFont* tagFont, const std::string& country, float x, float y, float h, float alpha = 1);
void DrawPerson(ImDrawList* d, float x, float y, float size, ImU32 col);

} // namespace sfv::mod
