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
    std::string code; // "RYU"; "ELV" for Eleven
    std::string name; // "RYU"
    // Not owned by this profile (entitlements.h): shown dimmed, not picked,
    // as the game's own character select has it.
    bool locked = false;
    // The rest is for the preview's mock players (a session's rows come from
    // the overlay): costume names, per costume each color's swatches, skills.
    std::vector<std::string> costumes;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> swatches;
    std::vector<std::string> vskills, vtriggers, vcomments;
};

struct SelectStage {
    std::string code;    // "TAI"
    std::string name;    // "Ring of Destiny"
    bool locked = false; // not owned: dimmed, the cursor steps over it
    // Its other settings, each a stage of its own (The Grid: The Grid
    // Alternative, S25), as the game's stage select offers them.
    std::vector<SelectStage> settings;
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
    ImFont* bold = nullptr;  // Barlow Bold: the modal's buttons and kicker
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

// My character's side card (CURRENT LOADOUT): the pick in effect until a new
// one is saved, where the player is, and its background loading.
struct SelectCard {
    std::string player;    // "POLARITYDEV"
    std::string chara;     // its code (the grid icon); empty: none
    std::string charaName; // "RYU"
    std::string place;     // "Table 2 \xC2\xB7 2nd" (gold, right)

    struct Row {
        std::string label, value;
        uint32_t swatch1 = 0, swatch2 = 0;
    };

    std::vector<Row> rows;
    std::string status;      // under the rows
    std::string statusRight; // "loading", "ready"
    float progress = -1;     // the bar (0..1); -1: none
    bool done = false;       // green: loaded
};

// A session's lobby, as of this frame.
struct SelectLive {
    int you = -1; // the seat this game plays; -1 watching
    bool host = false;
    SelectSide sides[2];
    int stage = 0;            // the host's stage: into SelectData::stages; == their count: random
    int setting = 0;          // its setting: 0 the stage itself, k its settings[k - 1]
    bool stageLocked = false; // the host confirmed it: on to the characters
    bool set = false;         // a table's set: leaving gives it to the other player
    bool bothReady = false;   // the match is locked in (nobody can say not ready)
    bool loading = false;     // both ready, the games loading the characters
    unsigned pingMs = 0;      // the link's round trip (a spectator's: to the host)
    bool relayed = false;
    int spectators = 0;
    // My character (custom rooms, mock-up board 5): one player, no match.
    // MY CHARACTER over the grid; on the grid X opens the stage select (the
    // preferred stage, stageLocked false), Y picks at random, B keeps the
    // current pick ("Keep RYU": keepLabel); ready is "saved" (SAVED, A back
    // to the room, B changes again).
    bool myCharacter = false;
    std::string keepLabel;
    SelectCard card;  // in place of the other player
    std::string note; // the line at the bottom left (the loadout: the row's hint)
    // My character's Random stages: the stage codes (each stage and setting
    // its own) random leaves out (core/random_stage.h). X on the stage
    // select's random tile opens the list.
    std::vector<std::string> randomOff;
};

// What the local player did this frame.
struct SelectInput {
    // Picked this character (A on its tile; the cursor alone only shows a
    // character here). -2: no change.
    int chara = -2;
    bool randomPick = false; // picked the random tile
    int row = -1, delta = 0; // changed a loadout row
    int stage = -1;          // the host's stage cursor (into the stages; their count: random)
    int setting = -1;        // the host's setting for it (as SelectLive::setting)
    bool lockStage = false, unlockStage = false;
    bool ready = false;
    bool unready = false; // B while ready, until both are
    // My character: X on the grid (the stage select), B there (back to the
    // grid), A once saved (back to the room), B on the grid (keep the
    // current pick and close).
    bool openStage = false, closeStage = false, done = false, cancel = false;
    // My character's Random stages list changed: the new one (saved at once).
    bool randomOffChanged = false;
    std::vector<std::string> randomOff;
    // Leave, confirmed: B with nothing to go back to asks first (the screen's
    // own dialog; the panel does not open over this screen).
    bool leave = false;
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
// Asks for every texture the screen draws (with the lists SetSelectData
// gave), the stage select's first: called while hosting or joining, so
// nothing pops in when the screen opens. They load in the background, a few
// ms' worth a frame.
void WarmSelectTextures();
// The title's warm up (M1): the pad glyphs, the player's favorite character
// and stage (Fighter Profile, `chara`, `stage`; either may be empty), then
// every grid tile, stage icon and stage background, read from the paks
// behind anything drawn meanwhile. Any thread.
void WarmBootTextures(const SelectData& data, const std::string& chara, const std::string& stage);
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
