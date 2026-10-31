#pragma once
// The character select flow drawn the way the game draws its own (stage
// select, the character grid, each side's loadout, ready), with the game's
// textures. A session's lobby shows it live (DrawSelectLive: the overlay
// gives the session's state and applies what the player does); the Debug
// tab opens a preview of it with mock players.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ImFont;

namespace sfv::mod {

struct SelectChara {
    std::string code;  // "RYU"; "ELV" for Eleven
    std::string name;  // "RYU"
    // The rest is for the preview's mock players (a session's rows come from
    // the overlay): costume names, per costume each color's swatches, skills.
    std::vector<std::string> costumes;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> swatches;
    std::vector<std::string> vskills, vtriggers, vcomments;
};

struct SelectStage {
    std::string code; // "TAI"
    std::string name; // "Ring of Destiny"
};

struct SelectData {
    std::vector<SelectChara> charas;
    std::vector<SelectStage> stages;
    std::string you; // the local player's name (the preview)
    // The preview: watching instead of playing (both players mock ones).
    bool spectator = false;
};

struct SelectFonts {
    ImFont* title = nullptr; // small headings
    ImFont* name = nullptr;  // the big character names, the screen's headers
    ImFont* text = nullptr;  // rows, labels
};

// The loadout's rows, top to bottom.
constexpr int kSelectRows = 7; // Costume, Color, Variant, V-Skill, V-Trigger, V-Comment, Input delay

struct SelectRow {
    std::string value;
    uint32_t swatch1 = 0, swatch2 = 0; // the color's two swatches (0xAARRGGBB), 0: none
    bool fixed = false;                // not changeable (Eleven's: random)
    std::string hint;                  // the strip's text while this row is chosen
};

// One player as the screen shows it.
struct SelectSide {
    bool present = false;
    std::string player; // their name
    int link = 0;       // 0 unknown, 1 wired, 2 Wi-Fi
    int chara = -1;     // into SelectData::charas; -1 no pick yet
    SelectRow rows[kSelectRows];
    bool ready = false;
    // Where the other player is on their own screen (lobby::Seat::screen):
    // 0 not known (drawn from the pick alone), 1 the grid (with the character
    // under their cursor, -1 none, or the random tile), 2 their loadout (on
    // `row`). This player's own comes from this screen.
    int screen = 0;
    int hover = -1;
    bool hoverRandom = false;
    int row = -1;
};

// A session's lobby, as of this frame.
struct SelectLive {
    int you = -1; // the seat this game plays; -1 watching
    bool host = false;
    SelectSide sides[2];
    int stage = 0;            // the host's stage: into SelectData::stages; == their count: random
    bool stageLocked = false; // the host confirmed it: on to the characters
    bool bothReady = false;   // the match is locked in (nobody can say not ready)
    bool loading = false;     // both ready, the games loading the characters
    unsigned pingMs = 0;      // the link's round trip (a spectator's: to the host)
    bool relayed = false;
    int spectators = 0;
};

// What the local player did this frame.
struct SelectInput {
    // Picked this character (A on its tile; the cursor alone only shows a
    // character here). -2: no change.
    int chara = -2;
    bool randomPick = false; // picked the random tile
    int row = -1, delta = 0; // changed a loadout row
    int stage = -1;          // the host's stage cursor (into the stages; their count: random)
    bool lockStage = false, unlockStage = false;
    bool ready = false;
    bool unready = false; // B while ready, until both are
    bool menu = false;    // Start: the panel (Leave, settings)
    bool back = false;    // B with nothing to go back to
    // Where this player is on the screen, every frame (for the others'
    // screens): as SelectSide::screen, hover, hoverRandom and row.
    int screen = 0;
    int hover = -1;
    bool hoverRandom = false;
    int screenRow = -1;
};

// A session's select screen: its lists, once per session.
void SetSelectData(SelectData data);
// Starts the screen over (a new lobby): the grid cursor on the player's pick.
void ResetSelectScreen();
// Render thread, in an ImGui frame: the screen at `alpha` (0..1, fades),
// on the background draw list; with `takeInput` it reads the pads, keys and
// mouse and fills `input`.
void DrawSelectLive(const SelectFonts& fonts, const SelectLive& live, float alpha, bool takeInput, SelectInput* input);

// The Debug tab's preview with mock players. `phase`: 0 stage select, 1 the
// grid, 2 the loadout, 3 ready.
void OpenSelectPreview(SelectData data, int phase = 0);
bool SelectPreviewOpen();
void DrawSelectPreview(const SelectFonts& fonts);

} // namespace sfv::mod
