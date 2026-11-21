#pragma once
// The local player's input and the game's window: virtual XInput pads for
// tests, the pads and keyboard as the game maps them, the pause filter of a
// netplay match and the window procedure (overlay input, always active).
// docs/local-input.md has how the game reads its input.

#include "core/game.h"

#include <windows.h>
#include <xinput.h>

#include <cstdint>
#include <string>

namespace sfv::mod {

// A virtual pad's state: XINPUT_GAMEPAD_* buttons, the triggers and the left stick.
struct PadState {
    WORD buttons = 0;
    BYTE lt = 0, rt = 0;
    SHORT lx = 0, ly = 0;
};

// `virtualPads` pads (from user 0) are connected from the start: the game
// only looks for controllers when it starts or a device arrives. With
// alwaysActive the game believes its window is in front, so it keeps reading
// pads while in the background (two instances on one desktop).
bool InputInstall(int virtualPads, bool alwaysActive);
// Every 50 ms from the command thread: finds the game's window once it
// exists, and logs the pads' polls once a minute.
void InputTick();
// Buttons joined by '+': up down left right start back lb rb a b x y lt rt,
// sup sdown sleft sright (the left stick), none.
bool InputParseButtons(const std::string& spec, PadState* out);
// Presses a virtual pad (connecting it) until the next call; a state of
// nothing releases it.
void InputSetVirtual(int user, const PadState& state);
void InputConnectVirtual(int user, bool connected);
// The game's window (found if not known yet), or null.
HWND InputGameWindow();
// The real foreground window, unaffected by alwaysActive.
HWND InputRealForegroundWindow();
// Puts the game's window procedure back (before unloading).
void InputRestoreWindowProc();
// While set, Start/Back on every pad and Enter/Escape on the keyboard are
// swallowed: a pause on one side of a netplay match would desync it.
void InputBlockPause(bool block);
// The game's own pad input (UE4, menus and the win screens) sees `buttons`
// (XINPUT_GAMEPAD_*) on the first pad for `ms`, even while a match isolates
// the pads.
void InputPressUi(uint16_t buttons, int ms);
// Netplay: the game's input manager is cleared after each update (keyboard
// and pads), so local input reaches the simulation only as injected inputs.
void InputIsolatePads(bool isolate);
// The game's pad keys as its input manager last read them, both pads
// together (EKWKeyConfigPadKeyId bits, docs/local-input.md), from any pad
// the game reads.
uint32_t InputGamePadKeys();
// The local player's input as the game maps it (keyboard and pads, with the
// player's button config), with Start; the pad's default layout when the
// input manager is not hooked.
PackedInput InputMappedLocal(int unused);
// Scripted pad state, set from the battle tick: while enabled it replaces the
// pad steps for that user.
void InputScriptedPad(int user, bool enabled, uint16_t buttons = 0, uint8_t rightTrigger = 0);
// Number of XInputGetState calls the game has made for `user`.
uint64_t InputPadPolls(int user);
// Testing (mappedlog): logs the mapped input whenever it changes, for the
// next `updates` input updates.
void InputLogMapped(int updates);
// Pad 0's live Button Settings table (16 entries, docs/local-input.md), or null.
uint32_t* InputKeyTable();

} // namespace sfv::mod
