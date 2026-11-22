// The game's textures: their shader resource views for the overlay, and savetex's DDS files (docs/research.md).

#include "mod/textures.h"

#include "core/file_open.h"
#include "mod/files.h"
#include "mod/flow.h"
#include "mod/image.h"
#include "mod/log.h"
#include "mod/ue4.h"
#include "proxy/real_xinput.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace sfv::mod {

namespace {

constexpr size_t kMinTextureScan = 0x100, kMaxTextureScan = 0x400, kRhiScan = 0x100;

struct Range {
    uintptr_t lo = 0, hi = 0;

    bool Has(uintptr_t p) const { return p >= lo && p < hi; }
};

Range ModuleRange(HMODULE m) {
    if (!m) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    return {reinterpret_cast<uintptr_t>(m), reinterpret_cast<uintptr_t>(m) + nt->OptionalHeader.SizeOfImage};
}

uintptr_t VtableOf(const void* p) {
    if (!p || !IsReadable(p, 8)) return 0;
    return *static_cast<const uintptr_t*>(p);
}

const void* PointerAt(const void* base, size_t offset) {
    const auto* at = static_cast<const uint8_t*>(base) + offset;
    return IsReadable(at, 8) ? *reinterpret_cast<const void* const*>(at) : nullptr;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::atomic<uintptr_t> g_srvVtable{0};

} // namespace

void TexturesNoteViewKind(void* d3dDevice) {
    auto* device = static_cast<ID3D11Device*>(d3dDevice);
    if (!device || g_srvVtable) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 1;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) && texture &&
        SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &srv)) && srv) {
        g_srvVtable = VtableOf(srv);
        Log("textures: shader resource views have vtable %p%s", reinterpret_cast<void*>(g_srvVtable.load()),
            proxy::RunningOnWine() ? " (Wine: only those are taken)" : "");
    } else {
        Log("textures: cannot make a shader resource view to recognize the game's");
    }
    if (srv) srv->Release();
    if (texture) texture->Release();
}

int64_t TextureClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void TexturesLog(const std::string& filter, int limit) {
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

const void* TexturesFindOrLoad(const std::string& nameOrPath) {
    if (nameOrPath.find('/') != std::string::npos) return FlowLoadAsset("Texture2D", nameOrPath);
    return ue4::FindObject(nameOrPath, "Texture2D");
}

TextureView TexturesViewOf(const void* texture, bool quiet) {
    TextureView out;
    if (!texture) return out;
    const Range game = ModuleRange(GetModuleHandleW(nullptr));
    const Range d3d = ModuleRange(GetModuleHandleW(L"d3d11.dll"));
    const uintptr_t srvVtable = g_srvVtable;
    const size_t size = std::clamp<size_t>(ue4::ObjectSize(texture), kMinTextureScan, kMaxTextureScan);
    auto scan = [&](auto takes) {
        for (size_t a = 0; a + 8 <= size; a += 8) {
            const void* resource = PointerAt(texture, a);
            if (!game.Has(VtableOf(resource))) continue;
            for (size_t b = 0; b + 8 <= kRhiScan; b += 8) {
                const void* rhi = PointerAt(resource, b);
                if (!game.Has(VtableOf(rhi))) continue;
                for (size_t c = 0; c + 8 <= kRhiScan; c += 8) {
                    void* d = const_cast<void*>(PointerAt(rhi, c));
                    if (!takes(VtableOf(d))) continue;
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
                    if (!quiet)
                        Log("texture %s: view %p, %ux%u (texture +0x%zx -> resource +0x%zx -> RHI +0x%zx)",
                            ue4::ObjectName(texture).c_str(), static_cast<void*>(srv), out.width, out.height, a, b, c);
                    return true;
                }
            }
        }
        return false;
    };
    if (srvVtable && scan([&](uintptr_t vtable) { return vtable == srvVtable; })) return out;
    if (!proxy::RunningOnWine() && scan([&](uintptr_t vtable) { return d3d.Has(vtable); })) return out;
    Log("texture %s: no shader resource view found", ue4::ObjectName(texture).c_str());
    return out;
}

namespace {

struct PendingSave {
    ID3D11ShaderResourceView* srv;
    std::string name;
};

std::mutex g_saveMutex;
std::vector<PendingSave> g_saves;

struct DdsPixelFormat {
    uint32_t size, flags, fourCc, rgbBits, rMask, gMask, bMask, aMask;
};

struct DdsHeader {
    uint32_t size, flags, height, width, linearSize, depth, mipCount, reserved[11];
    DdsPixelFormat format;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};

static_assert(sizeof(DdsHeader) == 124);

struct DdsHeaderDx10 {
    uint32_t format, dimension, miscFlags, arraySize, miscFlags2;
};

constexpr uint32_t kDdsCaps = 0x1, kDdsHeight = 0x2, kDdsWidth = 0x4, kDdsPixelFormat = 0x1000, kDdsLinearSize = 0x80000;
constexpr uint32_t kPixelAlpha = 0x1, kPixelFourCc = 0x4, kPixelRgb = 0x40;
constexpr uint32_t kCapsTexture = 0x1000;
constexpr uint32_t kDimensionTexture2d = 3;
constexpr uint32_t kRgbaBits = 32;

uint32_t BlockBytes(DXGI_FORMAT f, bool* compressed) {
    *compressed = true;
    switch (f) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            *compressed = false;
            return 4;
        default:
            return 0;
    }
}

constexpr uint32_t FourCcOf(const char* s) {
    return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24;
}

uint32_t FourCc(DXGI_FORMAT f) {
    const auto cc = FourCcOf;
    switch (f) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB: return cc("DXT1");
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB: return cc("DXT3");
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB: return cc("DXT5");
        default: return 0;
    }
}

void SaveOne(ID3D11ShaderResourceView* srv, const std::string& name) {
    ID3D11Resource* resource = nullptr;
    srv->GetResource(&resource);
    ID3D11Texture2D* texture = nullptr;
    if (!resource || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
        if (resource) resource->Release();
        Log("savetex %s: not a 2D texture", name.c_str());
        return;
    }
    resource->Release();
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    bool compressed = false;
    const uint32_t unit = BlockBytes(desc.Format, &compressed);
    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC copyDesc = desc;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.Usage = D3D11_USAGE_STAGING;
    copyDesc.BindFlags = 0;
    copyDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    copyDesc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!unit) {
        Log("savetex %s: format %d not written", name.c_str(), int(desc.Format));
    } else if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &staging))) {
        Log("savetex %s: no staging texture", name.c_str());
    } else {
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, nullptr);
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            Log("savetex %s: cannot map the copy", name.c_str());
        } else {
            const uint32_t rows = compressed ? (desc.Height + 3) / 4 : desc.Height;
            const uint32_t rowBytes = compressed ? (desc.Width + 3) / 4 * unit : desc.Width * unit;
            DdsHeader h = {};
            h.size = sizeof(DdsHeader);
            h.flags = kDdsCaps | kDdsHeight | kDdsWidth | kDdsPixelFormat | kDdsLinearSize;
            h.height = desc.Height;
            h.width = desc.Width;
            h.linearSize = rowBytes * rows;
            h.format.size = sizeof(DdsPixelFormat);
            const uint32_t fourCc = FourCc(desc.Format);
            const bool dx10 = compressed && !fourCc;
            if (!compressed) {
                const bool bgr = desc.Format >= DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
                h.format.flags = kPixelRgb | kPixelAlpha;
                h.format.rgbBits = kRgbaBits;
                h.format.rMask = bgr ? 0x00FF0000 : 0x000000FF;
                h.format.gMask = 0x0000FF00;
                h.format.bMask = bgr ? 0x000000FF : 0x00FF0000;
                h.format.aMask = 0xFF000000;
            } else {
                h.format.flags = kPixelFourCc;
                h.format.fourCc = fourCc ? fourCc : FourCcOf("DX10");
            }
            h.caps = kCapsTexture;
            const DdsHeaderDx10 dx10Header = {uint32_t(desc.Format), kDimensionTexture2d, 0, 1, 0};
            CreateDirectoryW((FilesDataDir() + L"\\textures").c_str(), nullptr);
            const std::string path = FilesUtf8Path(FilesDataDir()) + "\\textures\\" + name + ".dds";
            if (FILE* f = OpenFile(path, "wb")) {
                std::fwrite("DDS ", 1, 4, f);
                std::fwrite(&h, sizeof(h), 1, f);
                if (dx10) std::fwrite(&dx10Header, sizeof(dx10Header), 1, f);
                for (uint32_t y = 0; y < rows; y++) std::fwrite(static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, 1, rowBytes, f);
                std::fclose(f);
                Log("savetex %s: %ux%u format %d -> %s", name.c_str(), desc.Width, desc.Height, int(desc.Format), path.c_str());
            } else {
                Log("savetex %s: cannot write %s", name.c_str(), path.c_str());
            }
            context->Unmap(staging, 0);
        }
        staging->Release();
    }
    context->Release();
    device->Release();
    texture->Release();
}

} // namespace

void TexturesSaveLater(void* srv, const std::string& name) {
    std::lock_guard lock(g_saveMutex);
    g_saves.push_back({static_cast<ID3D11ShaderResourceView*>(srv), name});
}

void TexturesSaveQueued() {
    std::vector<PendingSave> saves;
    {
        std::lock_guard lock(g_saveMutex);
        if (g_saves.empty()) return;
        saves.swap(g_saves);
    }
    for (PendingSave& s : saves) {
        SaveOne(s.srv, s.name);
        s.srv->Release();
    }
}

} // namespace sfv::mod
