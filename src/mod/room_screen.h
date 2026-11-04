#pragma once
// The room screen (custom-rooms-plan.md M3/M4; mock-up board "Room"): the
// tables on the left (four in view), the players and the room chat on the
// right, the code with Copy code beside the title.
//
// Tables (M4): A on a table sits you (left and right pick P1 or P2 when both
// seats are open), A again stands you up; on a busy table A joins its queue
// (or leaves it). When both seats are taken the your-turn card comes up for
// the two players: Ready (A), the input delay for this match (left and
// right, preset from SFVR settings), Leave queue (B); 8 s to answer. Then a
// connecting notice while P1 opens the match room. Start (owner) opens the
// table rules; Y opens My character.
//
// Players (M6): RB moves the cursor to the player list; A mutes or unmutes a
// player's chat (this session), X (owner) removes them from the room.
//
// B asks before leaving the room. While the keeper is gone the room is
// paused: the tables dim, chat carries on. Chat: Enter (or Back + Y on a pad)
// starts typing, Enter sends, Escape stops.

#include "mod/hub_screen.h"
#include "mod/rooms_client.h"

#include <string>
#include <vector>

namespace sfv::mod {

struct RoomScreenInput {
    bool leave = false;       // confirmed
    std::string chat;         // a message to send
    bool copiedCode = false;  // the code went to the clipboard
    // A table request (rooms_client.h RoomsTable): "seat", "stand", "queue",
    // "unqueue", "ready", "decline", "rules", "kick".
    std::string request;
    std::vector<std::string> args;
    int delay = -1;           // with "ready": the input delay chosen for this match
    bool myCharacter = false; // Y
    bool clearNotice = false; // the table notice read
};

void ResetRoomScreen(); // a new room: dialogs shut, nothing typed, nobody muted
// `delaySuggestion`: the your-turn card's starting delay (SFVR settings, or
// with Auto the suggestion for the ping to the opponent).
void DrawRoomScreen(const HubFonts& fonts, const RoomsView& view, bool takeInput, RoomScreenInput* input);
// Ready to start a card at this delay (the overlay works it out per opponent).
void SetCardDelay(int frames);
// A character's name for the seat cards ("Chun-Li"), from the overlay's tables.
void SetCharaNames(std::vector<std::pair<std::string, std::string>> codeToName);

} // namespace sfv::mod
