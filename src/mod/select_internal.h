#pragma once
// What the select screen's files share: the screen's state, the grid and the parts.

#include "mod/select_screen.h"

#include "mod/ui_kit.h"

#include <imgui.h>

#include <mutex>
#include <string>
#include <vector>

namespace sfv::mod::selectui {

inline std::mutex g_dataMutex;
// The screen being drawn: the live one's lists (SetSelectData) or the preview's.
inline const SelectData* g_draw = nullptr;
inline bool g_myCharacter = false;

// What the player is doing on the screen; a session only knows their pick
// and whether they are ready.
struct Ui {
    bool loadout = false;
    int row = 0;
    int cursorRow = 0, cursorX = 0;
    bool onRandom = false;
    int lastChara = kNoChara; // the pick the cursor last followed
    std::string lastStage;    // the stage behind the screen (the last real one while random is chosen)
    bool wasLocked = false;
    bool leaving = false;
    bool leaveChosen = false;
    bool cameFromStage = false; // My character: back from its stage select
    bool randomList = false;
    int listCursor = 0;
    int variantsOf = -1; // the tile whose settings list is up
    int variantCursor = 0;
    std::string listNote;
    double listNoteUntil = 0;

    static constexpr int kNoChara = -2;
};

inline Ui g_ui;

// A grid slot: its character (-1 on the random tile), corners in design
// coordinates (top left, top right, bottom right, bottom left) and lean.
struct Slot {
    int chara = -1;
    bool random = false;
    bool right = false;
    ImVec2 q[4];
};

inline std::vector<Slot> g_rows[2];
inline const SelectData* g_gridFor = nullptr;

constexpr float kArtSlope = 12.0f / 62.0f;
constexpr float kC = 800, kY0 = 606, kTH = 112, kY1 = 726, kSlant = kTH * kArtSlope, kTopW = 52, kPitch = 60, kHalf = 58;
constexpr float kK = kSlant / kTH;
constexpr ImVec2 kArtLeft[4] = {{6.45f, 2}, {68.45f, 2}, {95.55f, 142}, {33.55f, 142}};
constexpr ImVec2 kArtRight[4] = {{47.55f, 2}, {109.55f, 2}, {82.45f, 142}, {20.45f, 142}};
constexpr float kIconW = 116, kIconH = 144;

constexpr const char* kLoadingText = "Both ready. Loading...";

int CharaAt(const SelectData& data, const std::string& code);
void BuildGrid(const SelectData& data);

const SelectChara* Chara(int i);
// Stage `stage` in setting `setting` (0: the stage itself); null for random.
const SelectStage* Shown(int stage, int setting);

void DrawBackdrop(ImDrawList* d, int stage, int setting, ImU32 tint);
// `highlight`: the row lit up (this player's, or the row the other is on), -1 none.
void DrawLoadout(ImDrawList* d, const SelectFonts& f, bool right, const SelectSide& side, int highlight, bool interactive, SelectInput* in);
void DrawTile(ImDrawList* d, const Slot& t, bool p1, bool p2, const SelectFonts& f);
void DrawPlayerLine(ImDrawList* d, const SelectFonts& f, const SelectLive& live, int i);
void DrawRender(ImDrawList* d, int chara, bool right);
void DrawCard(ImDrawList* d, const SelectFonts& f, const SelectCard& c);
void StatusPill(ImDrawList* d, const SelectFonts& f, const char* text);
void OpenLeave();
void DrawLeaveDialog(ImDrawList* d, const SelectFonts& f, const SelectLive& live, bool takeInput, SelectInput* in);

void DrawRandomList(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in);
void DrawStageSelect(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in);

// The whole screen for a lobby's state; what the player does goes to `in`.
void Render(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in);

} // namespace sfv::mod::selectui
