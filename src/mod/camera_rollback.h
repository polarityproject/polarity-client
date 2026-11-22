#pragma once
// Camera view changes across rollbacks (docs/rollback-presentation.md).
//
// Throws with a camera and Critical Arts move the view to a camera actor
// and back with UE4's view-target blends (APlayerCameraManager::SetViewTarget,
// rva 0x1BC1E90), which live outside the snapshot. After a load the game's
// own callback (rva 0x330DB0) cancelled any blend in progress and put the
// view back on the fighter with no blend: a hard cut; and the resimulated
// ticks asked for a blend again from its start. Instead the callback is left
// out, the requests the battle's ticks make are journaled by tick, and on a
// load those after the loaded tick are set aside: a resimulated tick that
// makes one again is let off while its effect is still on screen, and a blend
// that has to start again starts as far on as it would be by now. The game's
// camera task itself asks again for the view the loaded state has (a throw
// that did not happen goes back to the fight camera).

#include "mod/image.h"

#include <cstdint>

namespace sfv::mod {

bool CameraInstall(const GameImage& image);

// On by default; `camrollback 0|1` (0: the game's own reset on loads).
void CameraSetRollback(bool on);
bool CameraRollbackOn();
// Research: logs every request and what was done with it (`camlog 1|0`).
void CameraSetLog(bool on);

// The game's post-restore callback that resets the views (skipped while on).
constexpr uintptr_t kCameraResetCallback = 0x330DB0;

// Around a battle tick the driver runs (requests inside one are journaled).
void CameraTickBegin();
void CameraTickEnd();
// After a load and the game's callbacks: the state is that of `tick`.
void CameraLoaded(uint32_t tick);
// After the frame's ticks, before it is drawn.
void CameraFrameEnd();
// A new battle: the journal is dropped.
void CameraJournalReset();

} // namespace sfv::mod
