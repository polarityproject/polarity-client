#pragma once
// Steam entitlements: the profile gets the content the player's Steam account
// owns, as CFN used to grant it.
//
// The game keeps what a profile owns in its save (UserProfileDataSave
// EntitlementsData: EntitlementVersion and AuthorizedEntitlementIds, the
// codes of DA_Spec's tables) and gates characters, costumes, colors and
// stages on it, offline too. CFN filled that list; without it the game only
// puts the free content in (EntitlementSpec rows with DefaultValue). As the
// save loads, right after the game decodes it, the mod adds the codes of the
// Steam apps the account owns (data/steam_grants.txt, built in) and the
// Dojo stage, which only joining a clan on CFN unlocked. Nothing else: what
// CFN granted beyond that comes from the list the save already holds. The game then loads, uses and saves them like codes CFN sent. A profile with no list (a fresh install) gets the free
// content and the list version too: otherwise the game would replace the list
// with the free content alone. A fresh profile also gets the Steam name as
// its fighter name (CFN gave it the fighter ID).
//
// On with [mod] steam_entitlements=1 (the default). The list the profile
// loaded is kept either way: the mod's own screens offer what it owns
// (EntitlementsOwns), as the game's menus do.

#include <cstdint>
#include <string>

namespace sfv::mod {

void EntitlementsInstall(uintptr_t imageBase, const std::wstring& iniPath);

// Whether the profile owns the content behind an entitlement code the way the
// game's own menus decide it: the code is in the list the profile loaded
// (after the Steam codes went in; the game keeps the free content in it too).
// An empty code is always owned. Until a profile has loaded, or when its list could not be
// read, everything counts as owned (the mod's screens then stay open). Any
// thread.
bool EntitlementsOwns(const std::string& code);
// True once a profile's list was read (EntitlementsOwns then answers for it).
bool EntitlementsKnown();

} // namespace sfv::mod
