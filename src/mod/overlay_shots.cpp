#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool ReadBack(IDXGISwapChain* swapChain, int scale, int* outW, int* outH, std::vector<uint8_t>* outRgb) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
        Log("overlay: screenshot: no back buffer");
        return false;
    }
    ID3D11Device* device = nullptr;
    back->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    const DXGI_FORMAT f = desc.Format;
    const bool bgr = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM ||
                     f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool rgb10 = f == DXGI_FORMAT_R10G10B10A2_UNORM;
    ID3D11Texture2D* resolved = nullptr;
    ID3D11Texture2D* staging = nullptr;
    auto release = [&] {
        if (staging) staging->Release();
        if (resolved) resolved->Release();
        context->Release();
        device->Release();
        back->Release();
    };
    if (!bgr && !rgba && !rgb10) {
        Log("overlay: screenshot: back buffer format %d not handled", static_cast<int>(f));
        release();
        return false;
    }
    ID3D11Texture2D* source = back;
    if (desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc = {1, 0};
        rd.BindFlags = 0;
        rd.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&rd, nullptr, &resolved))) {
            Log("overlay: screenshot: cannot resolve the back buffer");
            release();
            return false;
        }
        context->ResolveSubresource(resolved, 0, back, 0, f);
        source = resolved;
    }
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) {
        Log("overlay: screenshot: cannot make a staging texture");
        release();
        return false;
    }
    context->CopyResource(staging, source);
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        Log("overlay: screenshot: cannot read the back buffer");
        release();
        return false;
    }
    const int w = static_cast<int>(desc.Width) / scale, h = static_cast<int>(desc.Height) / scale;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * scale * mapped.RowPitch;
        uint8_t* out = rgb.data() + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; x++) {
            const uint8_t* p = row + static_cast<size_t>(x) * scale * 4;
            if (rgb10) {
                uint32_t v;
                std::memcpy(&v, p, 4);
                out[x * 3 + 0] = static_cast<uint8_t>((v >> 2) & 0xFF);
                out[x * 3 + 1] = static_cast<uint8_t>((v >> 12) & 0xFF);
                out[x * 3 + 2] = static_cast<uint8_t>((v >> 22) & 0xFF);
                continue;
            }
            out[x * 3 + 0] = bgr ? p[2] : p[0];
            out[x * 3 + 1] = p[1];
            out[x * 3 + 2] = bgr ? p[0] : p[2];
        }
    }
    context->Unmap(staging, 0);
    release();
    *outW = w;
    *outH = h;
    *outRgb = std::move(rgb);
    return true;
}

bool WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    const std::vector<uint8_t> png = EncodePng(w, h, rgb.data());
    std::wstring full = Widen(path);
    const bool absolute = full.size() > 1 && (full[1] == L':' || full[0] == L'\\' || full[0] == L'/');
    if (!absolute) {
        std::wstring win64 = FilesDataDir();
        const size_t cut = win64.find_last_of(L"\\/");
        if (cut != std::wstring::npos) win64.resize(cut);
        full = win64 + L"\\" + full;
    }
    for (auto& c : full) {
        if (c == L'/') c = L'\\';
    }
    const std::wstring tmp = full + L".tmp";
    FILE* file = _wfopen(tmp.c_str(), L"wb");
    if (!file) {
        Log("overlay: screenshot: cannot write %s", path.c_str());
        return false;
    }
    const bool written = fwrite(png.data(), 1, png.size(), file) == png.size();
    fclose(file);
    if (!written || !MoveFileExW(tmp.c_str(), full.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log("overlay: screenshot: cannot write %s", path.c_str());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void TakeBurstFrame(IDXGISwapChain* swapChain) {
    std::string dir;
    {
        std::lock_guard lock(g_shotMutex);
        if (g_burstLeft <= 0) return;
        dir = g_burstDir;
    }
    BurstFrame f{FlowDrivenTicks(), 0, 0, {}};
    if (ReadBack(swapChain, g_burstScale, &f.w, &f.h, &f.rgb)) g_burstFrames.push_back(std::move(f));
    std::lock_guard lock(g_shotMutex);
    if (--g_burstLeft > 0) return;
    std::vector<BurstFrame> frames;
    frames.swap(g_burstFrames);
    std::thread([dir, frames = std::move(frames)] {
        ThreadGuard running;
        std::wstring folder = Widen(dir);
        if (!(folder.size() > 1 && folder[1] == L':')) {
            std::wstring win64 = FilesDataDir();
            const size_t cut = win64.find_last_of(L"\\/");
            if (cut != std::wstring::npos) win64.resize(cut);
            folder = win64 + L"\\" + folder;
        }
        CreateDirectoryW(folder.c_str(), nullptr);
        int i = 0;
        for (const BurstFrame& b : frames) {
            char name[64];
            std::snprintf(name, sizeof(name), "\\%03d_t%u.png", i++, b.tick);
            WritePng(dir + name, b.w, b.h, b.rgb);
        }
        Log("overlay: burst of %zu frames written to %s", frames.size(), dir.c_str());
    }).detach();
}

void TakeScreenshot(IDXGISwapChain* swapChain) {
    TakeBurstFrame(swapChain);
    std::string path;
    int scale = 1;
    {
        std::lock_guard lock(g_shotMutex);
        if (g_shotPath.empty()) return;
        path.swap(g_shotPath);
        scale = g_shotScale;
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!ReadBack(swapChain, scale, &w, &h, &rgb)) return;
    if (WritePng(path, w, h, rgb)) Log("overlay: screenshot %s (%dx%d)", path.c_str(), w, h);
}

} // namespace sfv::mod::overlay
