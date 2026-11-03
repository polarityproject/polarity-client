#pragma once
// The game's UI textures read straight from its paks on threads of our own
// (docs/custom-rooms-plan.md, M1), so screens never wait on the game thread's
// loader. The paks' entries are encrypted; the engine's own pak platform
// file (FPakPlatformFile) decrypts, so no key is in the mod. The package is
// parsed here (core/texture_package.h), the mip fitted to the size it is
// drawn at, and the D3D11 texture made with the game's device: on the worker
// when the device is free threaded (UE4's is), else on the render thread
// (CreatePakTextures, from the overlay's Present).
//
// The game-thread path (textures.h, FindOrLoadTexture) stays as the
// fallback: a texture this cannot read says so once (*failed).

#include "mod/textures.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sfv::mod {

// The overlay's device (ID3D11Device*), when it sets up; a different one
// drops what was made with the last.
void SetPakTextureDevice(void* device);

// The texture at a game path ("/Game/Chara/RYU/Hud/PartCharacter"), fitted
// to `maxSide` pixels on its longer side (0: as stored). Null while it
// loads; `urgent` (it is being drawn) moves it to the queue's front. Null
// for good, with *failed set, when the paks cannot give it: the caller
// uses the game's loader. Any thread.
const TextureView* PakTexture(const std::string& path, uint32_t maxSide, bool urgent, bool* failed);

// Asks for these ahead of time, behind everything asked for by drawing
// (the warm up at the title, M1).
void WarmPakTextures(const std::vector<std::pair<std::string, uint32_t>>& textures);

// Render thread: textures a single-threaded device could not make elsewhere.
void CreatePakTextures();

// Research and tests (`paktex`): on or off (off: every PakTexture fails, so
// screens use the game's loader), and a line of figures.
void SetPakTexturesEnabled(bool on);
// Research: the package at a game path as the paks hold it (decrypted),
// written to `file`; logs what the parser makes of it.
void DumpPakPackage(const std::string& path, const std::string& file);
std::string PakTextureStatus();

} // namespace sfv::mod
