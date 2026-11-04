#pragma once
// The room screen (custom-rooms-plan.md M3; mock-up board "Room"): the
// tables on the left (four in view; seats, queues and matches come with M4),
// the players and the room chat on the right, the code with Copy code beside
// the title. B asks before leaving ("Leave the room?", saying who gets the
// room when the owner leaves). While the keeper is gone the room is paused:
// the tables dim, chat carries on.
//
// Chat: Enter (or a click on the box) starts typing, Enter sends, Escape
// stops; 200 characters, one a second (keeper_protocol.h ChatLimiter).

#include "mod/hub_screen.h"
#include "mod/rooms_client.h"

#include <string>

namespace sfv::mod {

struct RoomScreenInput {
    bool leave = false;       // confirmed
    std::string chat;         // a message to send
    bool copiedCode = false;  // the code went to the clipboard
};

void ResetRoomScreen(); // a new room: the dialog shut, nothing typed
void DrawRoomScreen(const HubFonts& fonts, const RoomsView& view, bool takeInput, RoomScreenInput* input);

} // namespace sfv::mod
