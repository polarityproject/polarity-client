#pragma once
// The match ticket (docs/online-lobby.md, "Mod"; custom-rooms-plan.md M2):
// the one input to the session driver, whatever brought the player to the
// match. Direct Match makes one from a typed code; a table (M4) gets one
// from the keeper. The session host is always P1 (rotation seats P1's host),
// so the ticket says who hosts, which seat is whose, the rules, and what
// follows a match.

#include "core/game.h"
#include "core/rules.h"

#include <cstdint>
#include <string>

namespace sfv {

enum class AfterMatch : uint8_t {
    Vote, // Direct Match: both choose rematch, character select or leave
    Set,  // a table: after each game a vote as Vote's (rematch: the set's next
          // game; character select: the select screen, then the next game;
          // 15 s without a choice counts as rematch), until one player wins
          // the set (rules.firstTo) or leaves it (a forfeit); then the
          // session ends, back to the room
};

struct MatchTicket {
    bool host = false;      // creates the match room and plays P1
    std::string roomCode;   // the match room (joiners and spectators)
    bool spectate = false;  // joins to watch
    // The P2 seat's token. The host's: what a joiner must present (0: the
    // first player to join takes the seat, as Direct Match always did); a
    // joiner's: what it presents. Whoever joins without it watches.
    uint64_t seatToken = 0;
    Rules rules;
    AfterMatch after = AfterMatch::Vote;
};

// The ticket Direct Match makes: the rules it always had (best of 3, 99
// seconds) and a vote after each match.
MatchTicket DirectMatchHost();
MatchTicket DirectMatchJoin(const std::string& code, bool spectate);

// The rules' part of a match setup: rounds and timer (the host writes them
// into every setup it sends).
void ApplyRulesToSetup(const Rules& rules, MatchSetup* setup);

// With AfterMatch::Set: the set is over once a player has rules.firstTo wins
// (setup.wins counts the session's games); -1 while it goes on, else the
// winner's seat.
int SetWinner(const Rules& rules, const uint16_t (&wins)[kNumPlayers]);

std::string DescribeTicket(const MatchTicket& ticket);

} // namespace sfv
