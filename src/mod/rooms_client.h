#pragma once
// The hub's presence (custom-rooms-plan.md M3; docs/online-lobby.md): one
// Weyvelength connection, beside any session's, that sits in a lobby channel
// the keeper hosts (hidden from other players) and moves into a custom room
// and back. Through the lobby it asks the keeper for new rooms and reads the
// public room list; in a room it has the members, the owner and the chat.
//
// It trusts a presence room only while its host is the keeper, proven by the
// keeper's signed member data (keeper_protocol.h); a room whose host is not
// the keeper is paused. A member who is host while the proven keeper is in the
// room hands host back to it (after a keeper restart).
//
// Runs on a thread of its own; View() is a copy for the screens.

#include "core/keeper_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sfv::mod {

struct RoomMember {
    uint32_t id = 0;
    std::string name;  // as shown: "Player N" when the name filter masks it (keeper_protocol.h)
    std::string chara; // member data `ch`
    bool owner = false;
    int pingMs = -1;   // the round trip to them over a P2P link; -1 not measured (or you)
    bool relayed = false; // that link goes through TURN
    int link = 0;      // their connection: 0 unknown, 1 wired, 2 Wi-Fi (member data `net`)
    bool masked = false;
    int watching = -1; // the table they watch (member data `w`, 1-based), -1 none
    std::string country; // ISO 3166 alpha-2, the rooms server's lookup (member data `@cc`); "" unknown or hidden
};

// A table as the room shows it (keeper_protocol.h TableState, queue, rules).
struct TableView {
    keeper::TableState s;
    int64_t callSeenMs = 0; // when this call (s.call) first showed here, RoomsClockMs
    std::vector<uint32_t> queue;
    std::string rules; // FormatRules: the next set's
};

struct ChatLine {
    uint32_t from = 0;  // 0: the room's own line (joins, leaves, owner changes)
    std::string name;     // as the word filter shows it ("Player 1234" for a masked name)
    std::string text;     // as typed
    std::string realName; // the player's own name
    std::string censored; // the text with the word filter's words starred (public rooms; else as typed)
};

struct RoomsView {
    enum class State {
        Off,
        Connecting,  // to the rooms server
        Finding,     // the desk (the keeper's lobby channel): its listing asked for
        NoLobby,     // no desk listed for this build (the keeper is not running): asked again shortly
        Browsing,    // the desk listed; in no room (the hub), or on the way into the desk
        Lobby,       // in the desk (the rooms screen open): requests go to the keeper
        Creating,    // asked the keeper for a room
        Joining,     // into a room (or back to the lobby)
        Room,        // in a custom room
        Failed,      // no server
    };
    State state = State::Off;
    std::string server;
    uint32_t self = 0;
    bool keeper = false; // the current presence room's host is the proven keeper
    std::string lobby;   // the lobby channel's code
    std::vector<keeper::ListedRoom> publicRooms; // from the server's room listing, signed by the keeper (rooms screen open)
    keeper::DeskSummary summary; // the desk's: public rooms and their players (polled whenever connected)
    // The focused public room's detail (RoomsPeek), when the keeper has
    // answered for it; `peek.code` empty otherwise.
    keeper::RoomDetail peek;
    // In a custom room.
    std::string room;
    keeper::RoomTitle title; // its title (keeper_protocol.h); none when the data holds no valid one
    bool isPublic = true;
    bool wordFilter = true; // the player's Word filter setting: masked names and starred chat (public rooms)
    int tables = 0;
    std::vector<std::string> tableRules; // t0.. (FormatRules), one per table
    std::vector<TableView> tableViews;   // s0.., q0.. with the rules, one per table
    uint32_t owner = 0;
    std::vector<RoomMember> members; // the keeper left out; oldest first
    std::vector<ChatLine> chat;      // the last kChatKept lines
    // No lobby for this build, but one for another: the server runs another
    // SFVR version ("14" the server's lobby protocol; empty otherwise).
    std::string otherBuild;
    // A table's match that did not start or finish (the opponent left during
    // the handoff, the match room never opened): what the room says about it.
    std::string tableNotice;
    // The last thing that went wrong, for the screen ("No room on that code",
    // "Room full", "Removed from the room", the keeper's refusal).
    std::string notice;
    // The table this player watches (RoomsWatch), -1 none.
    int watching = -1;
};

// Watching a table (M5): its set's match room, as a spectator.
struct WatchTicket {
    int table = 0;
    std::string code;  // the match room
    std::string rules; // FormatRules
};
const char* RoomsStateName(RoomsView::State state);
// The Word filter setting (SFVR settings, F1 Settings): in public rooms,
// names the keeper masks show as "Player 1234" and chat has the keeper's
// words starred. Off: names and chat as they are. Any thread.
void SetRoomsWordFilter(bool on);
bool RoomsWordFilter();

// Opens the hub's connection (`server` "host[:port]", `alsoPort` as
// WeyveService::Start). `keeperKey`: an extra trusted keeper key, 128 hex
// digits ([rooms] keeper_key; local tests), beside the built-in one.
void RoomsOpen(const std::string& server, const std::string& name, uint16_t alsoPort, const std::string& keeperKey);
void RoomsClose();
bool RoomsOpened();
// Ask the keeper for a room (from the lobby); the answer moves us into it.
bool RoomsCreate(const keeper::RoomSettings& settings);
// Into a room by its code (from the lobby or another room).
bool RoomsJoin(const std::string& code);
// Out of the room, back to the lobby.
void RoomsLeave();
// False (and nothing sent) past the chat limits.
bool RoomsChat(const std::string& text);
void RoomsClearNotice(); // also the table notice
// The public room whose detail to keep asking for while it is focused ("" none).
void RoomsPeek(const std::string& code);
// A table request to the keeper (keeper_protocol.h: seat, stand, queue,
// unqueue, ready, decline, rules, kick).
void RoomsTable(const std::string& kind, const std::vector<std::string>& args);
// The keeper's ticket for a table's set, once (the overlay starts the session).
bool RoomsTakeTicket(keeper::TableTicket* out);
// The session for a table's set has been started: the client reports its
// match room's code (P1), the score after each game and the result.
void RoomsTableSession(int table, bool host, const std::string& rules);
// The table moved on while its set's session had not started (the opponent
// left during the handoff, the keeper called the table again): true once,
// and the overlay ends that session.
bool RoomsTakeAbort();
// The table set this player plays, once `winnerSeat` has won it: what follows
// for the player in `seat` ("Stays on Table 2 \xC2\xB7 plays Yuzu next",
// "Back in the queue \xC2\xB7 2nd", "Back to the room"), worked out as the
// keeper will (ApplySetResult); "" outside a table set.
std::string RoomsAfterSet(int seat, int winnerSeat);
// Cancel on the connecting modal: off the table before its match began (the
// keeper calls the table again), and this player's session ends.
void RoomsCancelHandoff();
// Watch a table (M5; -1 stops): each set it plays, its match room is a watch
// ticket (RoomsTakeWatch) once, so watching follows the table from set to
// set; it never queues. Others see "Watching Table N". It stops on leaving
// the room, and when this player is called to a table (RoomsTakeAbort ends
// the watching session then).
void RoomsWatch(int table);
bool RoomsTakeWatch(WatchTicket* out);
// Whether member `id` of room `room` (a match room) is the keeper, by the
// proof in its member data (keeper_protocol.h): a table set's host lets only
// the keeper watch (its relay serves every viewer).
bool RoomsIsKeeper(const std::string& room, uint32_t id, const std::string& proof);
// The clock TableView::callSeenMs is on (milliseconds).
int64_t RoomsClockMs();
// A member's place, for the player list: "Table 2 · playing", "Idle".
std::string RoomsPlace(const RoomsView& v, uint32_t id);
// The character others see beside this player (member data `ch`: the
// Fighter Profile favorite until My character, M4).
void RoomsSetCharacter(const std::string& code);
RoomsView RoomsSnapshot();
std::string DescribeRooms(const RoomsView& v);

} // namespace sfv::mod
