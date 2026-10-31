#pragma once
// The character select flow drawn the way the game draws its own (stage
// select, the character grid, each side's loadout, ready), with the game's
// textures: for now a preview with mock players and the real character and
// stage data, opened from the panel's Debug tab.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ImFont;

namespace sfv::mod {

struct SelectChara {
    std::string code;  // "RYU"; "ELV" for Eleven
    std::string name;  // "RYU"
    std::vector<std::string> costumes;
    // Per costume: each color's two swatch colors (0xAARRGGBB), its colors then
    // its EX colors; empty when the game has no preview.
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
    std::string you; // the local player's name
};

struct SelectFonts {
    ImFont* title = nullptr; // CHARACTER SELECT, STAGE SELECT
    ImFont* name = nullptr;  // the big character names
    ImFont* text = nullptr;  // rows, labels
};

// `phase`: 0 stage select, 1 the grid, 2 the loadout, 3 ready.
void OpenSelectPreview(SelectData data, int phase = 0);
bool SelectPreviewOpen();
// Render thread, inside an ImGui frame: the whole screen, on the background
// draw list. Closes itself on Back from the stage select or its close button.
void DrawSelectPreview(const SelectFonts& fonts);

} // namespace sfv::mod
