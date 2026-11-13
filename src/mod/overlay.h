#pragma once
// In-game overlay: Dear ImGui drawn into the game's own swap chain.
//
// IDXGISwapChain::Present and ResizeBuffers are hooked (their addresses come
// from a throwaway device and swap chain made at install). On the game's
// Present the overlay draws into the back buffer, then the frame goes on as
// usual (also through the Steam overlay, which hooks Present too).
//
// Input: window messages reach ImGui through the mod's window procedure
// (control.cpp), queued and fed to ImGui on the render thread (ImGui is not
// thread-safe). While one of the mod's screens takes input the game gets no
// keyboard or mouse messages and sees neutral pads.
//
// The Polarity hub fades in at launch in place of the game's main menu; Back +
// Start (or F1) brings it back from the game's menus. In a match the same
// opens the leave modal (`overlay show|hide|toggle` in the command file);
// watching, the watch panel.
//
// During a session: a status strip, the match HUD (both players, ping and
// jitter, rollbacks, the other's frame rate and link), notices (toasts at the
// top right; in a fight small notes at the bottom centre, a desync's too),
// and after a match the vote on what follows (Rematch, Character select,
// Leave), which takes the pads (fight_ui.h).

#include <windows.h>
#include <xinput.h>

#include <cstdint>
#include <string>

namespace sfv::mod {

// panelPath: where the panel saves the player's choices (config.h). The
// Present hooks go in from a thread once the game's window is up.
bool InstallOverlay(const std::wstring& panelPath);
// Hot reload, with the code hooks: puts the swap chain's table entry for
// Present back (the game calls through it).
void UnhookPresentTable();
// After the hooks are disabled and no detour runs (hot reload): releases ImGui
// and its D3D objects.
void ShutdownOverlay();

void SetOverlayVisible(bool visible);
// Tests (`overlay benchresult`): Polarity settings open on the last benchmark result.
void ShowLastBenchmarkResult();
// The next frame as shown (the overlay drawn) to a PNG at `path` (relative to
// the game's Win64 folder unless absolute), `scale` 1 full size, 2 half, ...
// (the `shot` command; the test agents' screenshots, also where no outside
// tool can capture the game, as in Steam's Game Mode on a Steam Deck).
void RequestScreenshot(const std::string& path, int scale);
// Research (`shotburst <dir> <frames> [scale]`): the next frames as shown, kept in memory, written after.
void RequestShotBurst(const std::string& dir, int frames, int scale);
void ToggleOverlay();
// The Debug tab's previews of the match windows (`overlay preview ...`):
// words hud, strip, desync, p1, p2 switch a window on; relay or p2p the link
// they show; vote or select pick
// what P1's and P2's windows show, you1/you2 whose side is this player's,
// toasts adds sample toasts, fade plays the player bars' fade at a match's
// end, off turns every preview off.
void PreviewOverlay(const std::string& words);
// Opens the select screen preview (as the Debug tab's button) at a phase:
// 0 stage select, 1 the grid, 2 the loadout, 3 ready; `spectator`: as one
// watching.
void RequestSelectPreview(int phase, bool spectator = false);
// Testing (`pick`): the panel's choices are read again from the netplay
// options (else a session's lobby sends the panel's saved pick over it).
void ReloadOverlayChoices();
// Vsync: off presents every frame at once (sync interval 0) instead of
// waiting for the display's refresh, whatever the game asked for.
void SetVsync(bool on);
bool VsyncOn();
// For the log: the game's own sync interval, the swap chain, and the time
// spent in Present per frame since the last call.
std::string VsyncInfo();
bool OverlayVisible();
// The overlay takes the keyboard, mouse and pads (the game sees neutral pads):
// the panel is open, a session's select screen is up (until the battle scene
// replaces the main menu), or the post-match vote waits for this player.
bool OverlayTakesInput();
// Research: a texture view drawn in a small window (null: none), with a
// label; the overlay holds the view's reference until it is replaced.
void SetOverlayTexture(void* srv, uint32_t width, uint32_t height, const std::string& label);
// The mod's version, shown in the panel's title.
void SetOverlayBuild(const std::string& version);
// This start moved an SFVR install's files to Polarity's names (migrate.h):
// a toast says so, once, at the first menu.
void SetMigratedNotice();
// Frames the game presented per second, recently.
float RenderFps();

// From the game window's procedure: true when the overlay took the message
// (the game must not see it).
bool OverlayWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp);
// From the pad poll, with a pad's real state: Back + Start toggles; while the
// panel is open the pads also navigate it (D-pad or left stick, A, B).
void OverlayPad(unsigned user, const XINPUT_GAMEPAD& pad);

} // namespace sfv::mod
