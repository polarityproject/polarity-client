#pragma once
// Testing: a side that sends picks and stages no game has, as a hostile or
// broken peer would, so the other side's check (core/pick_check.h) can be
// tried on real games (the mod's `badpick` command).

#include "core/lobby.h"

#include <optional>
#include <string>

namespace sfv {

enum class BadPickField { Character,
                          Costume,
                          Color,
                          VSkill,
                          VTrigger,
                          Comment,
                          Variant,
                          Stage };

struct BadPick {
    BadPickField field = BadPickField::Character;
    std::optional<lobby::Type> only; // just this message type; every type when empty
};

// "character", "costume", "color", "vskill", "vtrigger", "comment",
// "variant" or "stage", and optionally "state" (PlayerState),
// "session" (SessionState), "welcome", "loaded" or "matches".
std::optional<BadPick> BadPickParse(const std::string& field, const std::string& message);
// Writes the bad value into every pick (or the stage) `m` carries.
void BadPickApply(const BadPick& bad, lobby::Message& m);

} // namespace sfv
