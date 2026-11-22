#pragma once
// What the battle driver's files share: the game, its image, the tick being run, and each file's hooks (docs/battle-driver.md).

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
inline bool g_inDriver = false;
inline bool g_replayingTick = false;
inline std::atomic<uint8_t*> g_battleFrame{nullptr};

constexpr uint32_t kMoveFileDataOff = 0x28, kMoveFileSizeOff = 0x30;

bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original);

bool InstallRandomHooks(const GameImage& image);
void InstallPhaseHooks(const GameImage& image);
void InstallSoundHooks(const GameImage& image);
void InstallTraceHooks(const GameImage& image);

void CountPhaseTick();

} // namespace sfv::mod::battle
