#pragma once
// SFVR settings (docs/online-lobby.md "The SFVR hub and settings"; mock-up
// board "Settings"): Start in the hub. One scrolling list with a header per
// category (Updates first, so a new version is seen at once; then Online,
// Game, Reports), a side panel for the focused row, and the stripe's hint.
// The F1 panel's Settings and Debug options a player needs, on a pad; each
// change saves at once.
//
// The screen only draws rows and reports what was done to them; the overlay
// builds the rows from the live state and applies the changes.

#include "mod/hub_screen.h"

#include <string>
#include <vector>

namespace sfv::mod {

struct SettingsRow {
    std::string id;       // what the overlay calls it ("check", "name", ...)
    std::string section;  // "UPDATES"; a new header where it changes
    std::string label;    // "Check at start"
    std::string value;    // "On"
    bool arrows = false;  // left and right change it
    bool text = false;    // A types into it (keyboard), Enter keeps, Escape drops
    std::string editValue; // what typing starts from (`value` may say "(Steam name)")
    bool enabled = true;
    bool highlight = false; // the value in gold (an action: "Check", "Open")
    std::string action;   // the A prompt's word ("Check", "Install"); empty: none
    std::string hint;     // the stripe while focused
    // The side panel while focused.
    std::string kicker, title;
    std::vector<std::string> lines; // the first in gold when `goldFirst`
    bool goldFirst = false;
};

struct SettingsInput {
    std::string row;   // the row acted on ("" none)
    int delta = 0;     // left / right
    bool confirm = false;
    bool typed = false; // `text` holds the row's new text
    std::string text;
    bool back = false;
};

void OpenSettingsScreen(const std::string& focusRow = "");
void CloseSettingsScreen();
bool SettingsScreenOpen();
void DrawSettingsScreen(const HubFonts& fonts, const std::string& version, const std::vector<SettingsRow>& rows, bool takeInput,
                        SettingsInput* input);

} // namespace sfv::mod
