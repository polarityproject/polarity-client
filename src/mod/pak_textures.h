#pragma once
// The game's UI textures read straight from its paks on threads of our own
// (docs/custom-rooms-plan.md, M1), so screens never wait on the game thread's
// loader. The paks' entries are encrypted; the engine's own pak platform
// file (FPakPlatformFile) decrypts, so no key is in the mod. The package is
// parsed here (core/texture_package.h), the mip fitted to the size it is
// drawn at, and the D3D11 texture made with the game's device: on the worker
// when the device is free threaded (UE4's is), else on the render thread
// (PakTexturesCreate, from the overlay's Present).
//
// The game-thread path (textures.h, TexturesFindOrLoad) stays as the
// fallback: a texture this cannot read says so once (*failed).

#include "core/texture_package.h"
#include "mod/textures.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sfv::mod {

// The overlay's device (ID3D11Device*), when it sets up; a different one
// drops what was made with the last.
void PakTexturesSetDevice(void* device);

// The texture at a game path ("/Game/Chara/RYU/Hud/PartCharacter"), fitted
// to `maxSide` pixels on its longer side (0: as stored). Null while it
// loads; `urgent` (it is being drawn) moves it to the queue's front. Null
// for good, with *failed set, when the paks cannot give it: the caller
// uses the game's loader. Any thread. `look`: blurred, a shadow, ... (made
// on the workers, once; BC textures decoded for it).
const TextureView* PakTexturesGet(const std::string& path, uint32_t maxSide, bool urgent, bool* failed, const TexLook& look = {});

// A tile of grey noise, side x side (made on a worker, once); null until then.
const TextureView* PakTexturesNoise(uint32_t side);

// Asks for these ahead of time, behind everything asked for by drawing
// (the warm up at the title, M1).
void PakTexturesWarm(const std::vector<std::pair<std::string, uint32_t>>& textures);

// Render thread: textures a single-threaded device could not make elsewhere.
void PakTexturesCreate();

// Research and tests (`paktex`): on or off (off: every PakTexturesGet fails, so
// screens use the game's loader), and a line of figures.
void PakTexturesSetEnabled(bool on);
// Research: the package at a game path as the paks hold it (decrypted),
// written to `file`; logs what the parser makes of it.
void PakTexturesDumpPackage(const std::string& path, const std::string& file);
std::string PakTexturesStatus();

} // namespace sfv::mod
