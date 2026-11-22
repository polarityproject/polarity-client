#pragma once
// What the SfvGame files share: the region kinds the snapshot adds and the region watch.

#include "mod/sfv_game.h"

#include "core/state.h"
#include "mod/battle_driver.h"
#include "mod/camera_rollback.h"
#include "mod/frame_meter.h"
#include "mod/sim_version.h"
#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/ue4.h"
#include "mod/watch.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace sfv::mod {

void GameCheckChangeWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase);

} // namespace sfv::mod

namespace sfv::mod::snapshot {

namespace mgr = game::manager;

template <typename T>
T Read(const uint8_t* base, uint32_t offset) {
    return *reinterpret_cast<const T*>(base + offset);
}

constexpr uint32_t kBattleCameraVtable = 0x2C5F520;
constexpr size_t kBattleCameraRegionSize = 112, kBattleCameraRegionAt = 0x1C0, kBattleCameraSize = 0x400, kBattleCameraPartAt = 0xE8;
constexpr size_t kFighterSimRegionSize = 300, kFighterSimRegionAt = 0x764, kFighterSimSize = 0x1000;
constexpr uintptr_t kImageSpan = 0x8000000;

constexpr uintptr_t kSoundRestoreCallback = 0x4B6230;

} // namespace sfv::mod::snapshot
