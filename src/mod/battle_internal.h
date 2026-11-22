#pragma once
// What the battle driver's files share: the game, its image, the tick being
// run, and each file's hooks.

#include "mod/battle_driver.h"
#include "mod/flow.h"
#include "mod/image.h"
#include "mod/lifecycle.h"
#include "mod/log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sfv::mod::battle {

inline SfvGame* g_game = nullptr;
inline uintptr_t g_imageBase = 0;
// The game thread is in the frame driver's OnFrame: a driven tick.
inline bool g_inDriver = false;
// The driven tick being run is a resimulation (BattleSetReplayingTick).
inline bool g_replayingTick = false;
// The battle frame of the latest frame.
inline std::atomic<uint8_t*> g_battleFrame{nullptr};

// A loaded move file object (KWBAC, KWBCH, KWBCM): its bytes and their count.
constexpr uint32_t kMoveFileDataOff = 0x28, kMoveFileSizeOff = 0x30;

// Hooks the site after checking its signature; logs a mismatch or a failure.
bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original);

// Each file's hooks; a false return means that part stays off (and is logged).
bool InstallRandomHooks(const GameImage& image);
void InstallPhaseHooks(const GameImage& image);
void InstallSoundHooks(const GameImage& image);
void InstallTraceHooks(const GameImage& image);

// Counts a driven tick for the phase timing; logs it every 600 ticks.
void CountPhaseTick();

} // namespace sfv::mod::battle
