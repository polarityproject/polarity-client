#pragma once
// The mod's start, run on its own thread from DllMain.

#include <windows.h>

namespace sfv::mod {

DWORD WINAPI ModMain(LPVOID self);

} // namespace sfv::mod
