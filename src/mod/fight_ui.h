#pragma once
// What the mod draws over a fight and its results screen in the 1d look (the
// Polarity design system: "Modals", "Notices"): the leave modal (Back + Start in
// a match), the vote after a game (a glass pane docked on each side), and the
// notices: toasts that come down from the top right out of a fight, small
// notes at the bottom centre in one (a desync too). The overlay gives each
// what it shows; they hand back what the player did. Render thread, inside an
// ImGui frame.

#include "mod/hub_screen.h" // HubFonts

#include <cstdint>
#include <string>
#include <vector>

namespace sfv::mod {

// ----- the leave modal ------------------------------------------------------------
void FightUiResetLeaveModal(); // it opens on Stay
// `set`: a table's set (else a Direct Match). *leave: Leave chosen; *stay:
// Stay, B or Back + Start again.
void FightUiDrawLeaveModal(const HubFonts& f, bool set, bool takeInput, bool* leave, bool* stay);

// ----- the vote after a game ----------------------------------------------------
struct VoteSide {
    bool shown = false;
    bool mine = false; // this player's: the buttons (while `choosing`) and the delay
    int slot = 0;      // 0 P1, 1 P2
    std::string name, country;
    int wins = 0;
    int firstTo = 0;                // a set's; 0 a Direct Match (wins this session)
    std::string status;             // "Wins the set", "Choosing...", "Rematch"
    int statusKind = 0;             // 0 soft, 1 ok, 2 muted
    std::vector<std::string> lines; // what follows, under the status
    bool choosing = false;          // the buttons are up
    int chosen = -1;                // 0 Rematch, 1 Character select: already chosen
    int delay = 0;                  // frames (this player's, chosen here; the other's, shown)
    std::string suggestion;         // under the delay: "Suggested delay 1 (ping 38 ms)"
    std::string hint;               // the footer: the vote's timer, or the other's delay
};

struct VoteInput {
    int choose = -1;   // 0 Rematch, 1 Character select, 2 Leave
    int delayStep = 0; // -1 or 1
};

void FightUiResetVote(); // a new vote: the focus on Rematch
// `armed`: presses count (a button held from the match does nothing).
void FightUiDrawVote(const HubFonts& f, const VoteSide (&sides)[2], bool armed, bool takeInput, VoteInput* in);

// ----- notices ----------------------------------------------------------------------
struct Notice {
    std::string kicker; // in a fight: before the text, in its colour ("DESYNC AT FRAME 3412")
    std::string text;
    uint32_t color = 0xFFFFFFFF; // ImU32: the diamond (and the kicker)
    float age = 0;               // seconds shown
    float left = 99;             // seconds to go
};

// Out of a fight: glass toasts from the top right, the newest on top,
// sliding down as they come.
// `overScreen`: over one of the mod's screens (glass over its stage), else
// over the game.
void FightUiDrawToasts(const HubFonts& f, const std::vector<Notice>& toasts, bool overScreen);
// In a fight: small notes at the bottom centre.
void FightUiDrawNotes(const HubFonts& f, const std::vector<Notice>& notes);
// During a session outside a fight (connecting, loading, the lobby): one
// line at the bottom right, what is going on.
void FightUiDrawStatusNote(const HubFonts& f, const std::string& text, uint32_t color);
// At the game's menus (Play Offline): a note at the top centre, how to get
// back to the hub.
void FightUiDrawMenuNote(const HubFonts& f);

} // namespace sfv::mod
