#include "mod/textures.h"

#include "mod/flow.h"
#include "mod/image.h"
#include "mod/log.h"
#include "mod/ue4.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cctype>

namespace sfv::mod {

namespace {

struct Range {
    uintptr_t lo = 0, hi = 0;
    bool Has(uintptr_t p) const { return p >= lo && p < hi; }
};

Range ModuleRange(HMODULE m)
{
    if (!m) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    return {reinterpret_cast<uintptr_t>(m), reinterpret_cast<uintptr_t>(m) + nt->OptionalHeader.SizeOfImage};
}

uintptr_t VtableOf(const void* p)
{
    if (!p || !IsReadable(p, 8)) return 0;
    return *static_cast<const uintptr_t*>(p);
}

const void* PointerAt(const void* base, size_t offset)
{
    const auto* at = static_cast<const uint8_t*>(base) + offset;
    return IsReadable(at, 8) ? *reinterpret_cast<const void* const*>(at) : nullptr;
}

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

void LogTextures(const std::string& filter, int limit)
{
    if (!ue4::EnsureObjects(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) return;
    const std::string needle = Lower(filter);
    int shown = 0, total = 0;
    ue4::ForEachObject([&](const void* object) {
        if (ue4::ObjectName(ue4::ClassOf(object)) != "Texture2D") return;
        total++;
        const std::string name = ue4::ObjectName(object);
        const std::string outer = ue4::ObjectName(ue4::OuterOf(object));
        if (!needle.empty() && Lower(name).find(needle) == std::string::npos && Lower(outer).find(needle) == std::string::npos) return;
        if (shown++ >= limit) return;
        Log("texture %s (%p) in %s", name.c_str(), object, outer.c_str());
    });
    Log("textures: %d of %d loaded Texture2D objects match '%s'", std::min(shown, limit), total, filter.c_str());
}

const void* FindOrLoadTexture(const std::string& nameOrPath)
{
    if (nameOrPath.find('/') != std::string::npos) return LoadAsset("Texture2D", nameOrPath);
    return ue4::FindObject(nameOrPath, "Texture2D");
}

TextureView ViewOfTexture(const void* texture)
{
    TextureView out;
    if (!texture) return out;
    // The texture's render resource and its RHI texture are native objects
    // (vtables in the game image, the D3D11 RHI is linked in); the RHI
    // texture holds the D3D11 objects (vtables in d3d11.dll): the one that is
    // a shader resource view is what ImGui draws.
    const Range game = ModuleRange(GetModuleHandleW(nullptr));
    const Range d3d = ModuleRange(GetModuleHandleW(L"d3d11.dll"));
    const size_t size = std::clamp<size_t>(ue4::ObjectSize(texture), 0x100, 0x400);
    for (size_t a = 0; a + 8 <= size; a += 8) {
        const void* resource = PointerAt(texture, a);
        if (!game.Has(VtableOf(resource))) continue;
        for (size_t b = 0; b + 8 <= 0x100; b += 8) {
            const void* rhi = PointerAt(resource, b);
            if (!game.Has(VtableOf(rhi))) continue;
            for (size_t c = 0; c + 8 <= 0x100; c += 8) {
                void* d = const_cast<void*>(PointerAt(rhi, c));
                if (!d3d.Has(VtableOf(d))) continue;
                ID3D11ShaderResourceView* srv = nullptr;
                if (FAILED(static_cast<IUnknown*>(d)->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void**>(&srv))) ||
                    !srv) {
                    continue;
                }
                ID3D11Resource* r = nullptr;
                srv->GetResource(&r);
                ID3D11Texture2D* t2d = nullptr;
                if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2d))) && t2d) {
                    D3D11_TEXTURE2D_DESC desc{};
                    t2d->GetDesc(&desc);
                    out.width = desc.Width;
                    out.height = desc.Height;
                    t2d->Release();
                }
                if (r) r->Release();
                out.srv = srv;
                Log("texture %s: view %p, %ux%u (texture +0x%zx -> resource +0x%zx -> RHI +0x%zx)",
                    ue4::ObjectName(texture).c_str(), static_cast<void*>(srv), out.width, out.height, a, b, c);
                return out;
            }
        }
    }
    Log("texture %s: no shader resource view found", ue4::ObjectName(texture).c_str());
    return out;
}

} // namespace sfv::mod
