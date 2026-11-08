#pragma once
// A player's name as the game's text shows it. The mod's fonts (Barlow,
// Archivo Black, Noto Sans CJK) have no emoji, and without FreeType ImGui
// cannot draw color ones, so a Steam name's emoji showed as "?" (user,
// 2026-11-08: allow them or strip them). They are stripped: emoji and
// pictographs, dingbats and other symbols, their joiners, skin tones and
// variation selectors, and the rest of plane 1 (historic scripts used as
// decoration, e.g. U+1065A). Mathematical alphanumerics (bold or script
// letters, a common Steam style) become plain letters and digits. Spaces left
// over collapse; a name with nothing left reads "Player".
#include <string>

namespace sfv::core {

std::string DisplayName(const std::string& name);

} // namespace sfv::core
