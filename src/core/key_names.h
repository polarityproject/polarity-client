#pragma once
// The game's keyboard assignment (Input2.ini [AssignKeyboard]): sixteen slots,
// KeyboardKeys_0..15, each an Unreal key name ("W", "Enter", "Comma"). Each
// slot drives one pad key of the game's input manager (measured in game,
// 2026-11-07: W S D A G H B N K J M with the default keys): up, down, right,
// left, then A, B, X, Y, LB, RB, LT, RT, L3, R3, Start, Back. The mod's
// screens read the keyboard the same way, so a key does there what it does
// in the game's own menus.

#include <cstdint>
#include <string>

namespace sfv {

constexpr int kKeyboardSlots = 16;

// The game's pad-key bit a slot drives (the input manager's raw bits: up 0x1,
// down 0x2, left 0x4, right 0x8, X 0x10, Y 0x20, A 0x40, B 0x80, LB 0x100,
// RB 0x200, LT 0x400, RT 0x800, L3 0x1000, R3 0x2000, Start 0x4000, Back
// 0x8000); 0 for no such slot.
uint32_t PadKeyOfSlot(int slot);

// A Windows virtual-key code for an Unreal key name, 0 when not known.
// Left and right Shift, Control and Alt are the one key each (the window's
// messages give VK_SHIFT, VK_CONTROL, VK_MENU).
int VirtualKeyOf(const std::string& name);

} // namespace sfv
