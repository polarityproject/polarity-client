#pragma once
// The room chat while watching a table's set (a spectator in a custom room):
// a see-through box at the bottom center of the fight, between the two
// players' V-gauges, with the room's last lines; typing into it sends to the
// room as the room screen does.
//
// Keys: a tap of Back (pad) or Tab (keyboard) shows or hides it; Back + Y
// (pad; Steam + X brings up a Deck's keyboard) or T (keyboard) starts typing,
// Enter sends, Escape (or B) stops. Hidden, it draws nothing at all.
//
// Render thread, inside an ImGui frame.

#include "mod/hub_screen.h"
#include "mod/rooms_client.h"

#include <string>

namespace sfv::mod {

struct WatchChatInput {
    std::string send; // a line to send to the room
};

// A Back tap and Back + Y as the overlay's pad reading saw them (any thread).
void WatchChatToggle();
void WatchChatStartTyping();
bool WatchChatShown();
// Testing (`watchchat type|say <text>`): typing starts with `text` in the
// field; with `send`, it is sent as Enter would.
void WatchChatTestText(const std::string& text, bool send);

// `takeInput`: nothing else over the fight takes the input (the watch panel,
// the leave modal).
void WatchChatDraw(const HubFonts& fonts, const RoomsView& rooms, bool takeInput, WatchChatInput* input);

} // namespace sfv::mod
