#pragma once
// A one-line status shown in the game window's title bar:
// "StreetFighterV [sfv 1] <text>". Visible without an overlay, and lets test
// scripts tell instances apart.

#include <string>

namespace sfv::mod {

void StatusSetInstance(int instance);
void StatusSet(const std::string& text);
// Writes the title again (the window may not have existed, or the game reset it).
void StatusRefresh();
// The current text and instance number (for the overlay).
std::string StatusText();
int StatusInstance();

} // namespace sfv::mod
