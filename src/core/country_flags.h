#pragma once
// The game's own country flags (/Game/CommonAsset/CountryFlags/<code>, named
// by ISO 3166-1 alpha-3) for the country the rooms server gives a player
// (member data "@cc", alpha-2 from its address lookup).

#include <string_view>

namespace sfv {

// The flag texture's name for an alpha-2 code ("DE" -> "DEU"); empty when the
// game has no flag for it (Hong Kong, Puerto Rico, Kosovo, ...: drawn as the
// two letters instead) or the code is not one.
std::string_view FlagAsset(std::string_view iso2);
// How many countries the game has flags for (the table's size; tests).
int FlagCount();

} // namespace sfv
