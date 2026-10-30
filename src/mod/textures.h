#pragma once
// The game's own textures for the overlay (research for the controller UI:
// character portraits, stage thumbnails). A UTexture2D's render data lives in
// the game's D3D11 device, the same one the overlay draws with, so ImGui can
// draw its shader resource view directly.

#include <cstdint>
#include <string>

namespace sfv::mod {

// Logs the loaded Texture2D objects whose name or outer contains `filter`
// (case-insensitive), at most `limit`, with their size where known.
void LogTextures(const std::string& filter, int limit = 80);

// A texture by object name ("T_UI_Foo") among the loaded ones, else by path
// ("/Game/UI/Foo/T_UI_Foo.T_UI_Foo", loaded on the game thread). Null when
// neither works. Game thread only for a path.
const void* FindOrLoadTexture(const std::string& nameOrPath);

struct TextureView {
    void* srv = nullptr; // ID3D11ShaderResourceView*, one reference held
    uint32_t width = 0, height = 0;
};
// The texture's shader resource view, found by walking its render resource
// (FTextureResource -> RHI texture -> D3D11 objects); logs the chain.
TextureView ViewOfTexture(const void* texture);

} // namespace sfv::mod
