#pragma once
// The room screen (custom-rooms-plan.md M3/M4; mock-up board "Room"): the
// tables on the left (four in view), the players and the room chat on the
// right, the code with Copy code beside the title.
//
// Tables (M4): A on a table sits you (left and right pick P1 or P2 when both
// seats are open), A again stands you up; on a busy table A joins its queue
// (or leaves it). When both seats are taken the your-turn card comes up for
// the two players: Ready (A), the input delay for this match (left and
// right, preset from SFVR settings), Leave queue (B); 8 s to answer. Then the
// connecting modal (mock-up board 6) until the match loads: the match room,
// the link with its ping, the characters loading, the match starting; B
// cancels until the match loads. Start (owner) opens the table rules; Y opens
// My character.
//
// Players (M6): RB moves the cursor to the player list; A mutes or unmutes a
// player's chat (this session), X (owner) removes them from the room.
//
// B asks before leaving the room. While the keeper is gone the room is
// paused: the tables dim, chat carries on. Chat: Enter (or Back + Y on a pad)
// starts typing, Enter sends, Escape stops.

#include "mod/hub_screen.h"
#include "mod/rooms_client.h"

#include <map>
#include <string>
#include <utility>
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
    bool cancelHandoff = false; // B on the connecting modal: out of this set before it began
    int watch = -2;             // X on a table: watch it (M5); -1 stop watching; -2 nothing
    int vskill = 0, vtrigger = 0; // the card: a V-Skill or V-Trigger chosen (from 1); 0 unchanged
};

// What this player plays at the tables, for the your-turn card's V-Skill and
// V-Trigger rows: the character, its V-Skill and V-Trigger (from 1); not
// `editable` for Eleven (random each time).
struct CardPick {
    std::string chara;
    int vskill = 1, vtrigger = 1;
    bool editable = false;
};
void SetCardPick(const CardPick& pick);
// Each character's V-Skill and V-Trigger names (the game's), by code.
void SetSkillNames(std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> names);

// This player's table session as the overlay sees it (NetView), for the
// connecting modal.
struct TableHandoff {
    bool session = false;   // the ticket's session exists
    bool roomOpen = false;  // P1: its match room is open; P2: joining it
    bool linked = false;    // both players are in the session
    unsigned pingMs = 0;    // the session's heartbeat round trip
    int path = 0;           // 0 not known, 1 peer to peer, 2 relayed
    bool preloaded[2] = {}; // P1's game, P2's game: both characters loaded
    bool jumping = false;   // the match is loading
    int played = 0;         // games of the set played so far
};
void SetTableHandoff(const TableHandoff& handoff);

void ResetRoomScreen(); // a new room: dialogs shut, nothing typed, nobody muted
// `delaySuggestion`: the your-turn card's starting delay (SFVR settings, or
// with Auto the suggestion for the ping to the opponent).
void DrawRoomScreen(const HubFonts& fonts, const RoomsView& view, bool takeInput, RoomScreenInput* input);
// Ready to start a card at this delay (the overlay works it out per opponent).
void SetCardDelay(int frames);
// A character's name for the seat cards ("Chun-Li"), from the overlay's tables.
void SetCharaNames(std::vector<std::pair<std::string, std::string>> codeToName);

} // namespace sfv::mod
