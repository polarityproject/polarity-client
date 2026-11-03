#pragma once
// The player's Fighter Profile favorites (the game's own save, read offline
// too): KWUserProfileDetails of the main user, +0x68 FavoriteBattleSettings
// (docs/custom-rooms-plan.md "Spike results"). The favorite character and
// stage, and per character the costume, color, V-Skill, V-Trigger and
// V-Comment last saved. My character starts from these (M4); the title's
// warm up loads their pictures first (M1).

#include <cstdint>
#include <string>
#include <vector>

namespace sfv::mod {

struct FavoriteChara {
    std::string code; // "RYU"
    int costume = 0, color = 0, comment = 0, vtrigger = 0, vskill = 0;
};

struct ProfileFavorites {
    std::string chara; // favorite character's code ("Z37"), empty if none
    std::string stage; // favorite stage's code ("S25"), empty if none
    std::vector<FavoriteChara> charas;
};

// Game thread (it walks the object list once, a few ms). False when the
// main profile is not loaded.
bool ReadProfileFavorites(ProfileFavorites* out);

} // namespace sfv::mod
