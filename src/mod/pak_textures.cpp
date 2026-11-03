#include "mod/pak_textures.h"

#include "core/file_open.h"
#include "core/texture_package.h"
#include "mod/image.h"
#include "mod/log.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace sfv::mod {

namespace {

using Clock = std::chrono::steady_clock;

// The engine's platform file chain (spike, 2026-11-02): FPlatformFileManager::Get(),
// then FindPlatformFile(manager, L"PakFile"). Its vtable, and the slots used:
// GetName, FileSize(path), OpenRead(path, allowWrite); the handle's Read(dst,
// bytes) and its scalar deleting destructor.
constexpr uint32_t kPlatformFileManagerGet = 0x1131AD0;
constexpr uint32_t kFindPlatformFile = 0x1133090;
constexpr uint32_t kPakFileVtable = 0x33C49D8;
constexpr int kSlotGetName = 8, kSlotFileSize = 10, kSlotOpenRead = 19;
constexpr int kHandleSlotDestroy = 0, kHandleSlotRead = 4;
constexpr int kWorkers = 2;
constexpr int64_t kLargestFile = 64ll * 1024 * 1024;

enum class State { Queued, Loading, Upload, Ready, Failed };

struct Entry {
    std::string path;
    uint32_t maxSide = 0;
    State state = State::Queued;
    TextureView view;
    TexImage image; // read, waiting for the render thread (single-threaded device)
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::map<std::string, Entry> g_entries; // by Key()
std::deque<std::string> g_queue;        // keys, in the order to load
ID3D11Device* g_device = nullptr;
bool g_freeThreaded = false;
bool g_started = false;
std::atomic<bool> g_enabled{true};
void* g_pakFile = nullptr;
bool g_pakLooked = false;
// Figures: since start, and the batch since the queue last ran dry.
int g_loaded = 0, g_failed = 0;
size_t g_bytes = 0; // GPU bytes of the textures made
int g_batch = 0;
double g_batchMs = 0, g_slowestMs = 0;
Clock::time_point g_batchStart;
// Frames presented while a batch loads: how many, the longest, how many over 20 ms.
int g_batchFrames = 0, g_batchLongFrames = 0;
double g_batchWorstFrameMs = 0;
Clock::time_point g_lastPresent;

std::string Key(const std::string& path, uint32_t maxSide) { return path + "@" + std::to_string(maxSide); }

// The pak platform file, checked; null when this build's differs.
void* PakFile()
{
    if (g_pakLooked) return g_pakFile;
    g_pakLooked = true;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* manager = reinterpret_cast<void* (*)()>(base + kPlatformFileManagerGet)();
    void* file = manager ? reinterpret_cast<void* (*)(void*, const wchar_t*)>(base + kFindPlatformFile)(manager, L"PakFile") : nullptr;
    void** vt = file && IsReadable(file, 8) ? *static_cast<void***>(file) : nullptr;
    if (!vt || reinterpret_cast<uintptr_t>(vt) != base + kPakFileVtable) {
        Log("pak textures: no pak platform file (%p, vtable rva %llx): the game's loader draws", file,
            vt ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt) - base) : 0ull);
        return nullptr;
    }
    const auto name = reinterpret_cast<const wchar_t* (*)(void*)>(vt[kSlotGetName])(file);
    if (!name || !IsReadable(name, 16) || std::wstring(name) != L"PakFile") {
        Log("pak textures: the platform file is not the pak file: the game's loader draws");
        return nullptr;
    }
    g_pakFile = file;
    return file;
}

// "/Game/Chara/RYU/Hud/PartCharacter" -> the package's name in the paks.
std::wstring PakPath(const std::string& path)
{
    std::string rest = path.compare(0, 6, "/Game/") == 0 ? path.substr(6) : path;
    std::wstring out = L"../../../StreetFighterV/Content/";
    out.append(rest.begin(), rest.end());
    return out + L".uasset";
}

bool ReadPackage(void* file, const std::string& path, std::vector<uint8_t>* out, std::string* error)
{
    void** vt = *static_cast<void***>(file);
    const std::wstring name = PakPath(path);
    const int64_t size = reinterpret_cast<int64_t (*)(void*, const wchar_t*)>(vt[kSlotFileSize])(file, name.c_str());
    if (size <= 0 || size > kLargestFile) {
        *error = size <= 0 ? "not in the paks" : "too large";
        return false;
    }
    void* handle = reinterpret_cast<void* (*)(void*, const wchar_t*, bool)>(vt[kSlotOpenRead])(file, name.c_str(), false);
    if (!handle) {
        *error = "cannot open it";
        return false;
    }
    out->resize(static_cast<size_t>(size));
    void** hv = *static_cast<void***>(handle);
    const bool ok = reinterpret_cast<bool (*)(void*, uint8_t*, int64_t)>(hv[kHandleSlotRead])(handle, out->data(), size);
    reinterpret_cast<void (*)(void*, unsigned)>(hv[kHandleSlotDestroy])(handle, 1);
    if (!ok) *error = "the read failed";
    return ok;
}

DXGI_FORMAT DxgiFormat(TexFormat f)
{
    switch (f) {
    case TexFormat::Bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TexFormat::Bc1: return DXGI_FORMAT_BC1_UNORM;
    case TexFormat::Bc3: return DXGI_FORMAT_BC3_UNORM;
    case TexFormat::Bc5: return DXGI_FORMAT_BC5_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The texture and its view, on `device`.
bool Upload(ID3D11Device* device, const TexImage& image, TextureView* view, std::string* error)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DxgiFormat(image.format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const bool blocks = image.format != TexFormat::Bgra8;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = image.bytes.data();
    init.SysMemPitch = blocks ? static_cast<UINT>(TexBytes(image.format, image.width, 4)) : image.width * 4;
    ID3D11Texture2D* texture = nullptr;
    if (desc.Format == DXGI_FORMAT_UNKNOWN || FAILED(device->CreateTexture2D(&desc, &init, &texture)) || !texture) {
        *error = "the device made no texture";
        return false;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = device->CreateShaderResourceView(texture, nullptr, &srv);
    texture->Release();
    if (FAILED(hr) || !srv) {
        *error = "the device made no view";
        return false;
    }
    view->srv = srv;
    view->width = image.width;
    view->height = image.height;
    view->readyMs = TextureClockMs();
    return true;
}

// Under g_mutex: an entry is done (ready or failed).
void Finish(Entry& e, bool ok, double ms, const std::string& error)
{
    e.state = ok ? State::Ready : State::Failed;
    if (ok) {
        g_loaded++;
        g_bytes += TexBytes(e.image.format == TexFormat::Unknown ? TexFormat::Bgra8 : e.image.format, e.view.width, e.view.height);
    }
    else {
        g_failed++;
        Log("pak textures: %s: %s (the game's loader draws it)", e.path.c_str(), error.c_str());
    }
    e.image = {};
    g_batch++;
    g_batchMs += ms;
    g_slowestMs = std::max(g_slowestMs, ms);
}

void LogBatchIfDone()
{
    // Under g_mutex.
    if (!g_queue.empty() || g_batch == 0) return;
    bool busy = false;
    for (const auto& [key, e] : g_entries) busy |= e.state == State::Loading || e.state == State::Upload;
    if (busy) return;
    const double wall = std::chrono::duration<double, std::milli>(Clock::now() - g_batchStart).count();
    Log("pak textures: %d done in %.0f ms (%.0f ms of work, slowest %.1f ms); %d loaded, %d failed, %.1f MB in all; "
        "meanwhile %d frames, the longest %.1f ms, %d over 20 ms",
        g_batch, wall, g_batchMs, g_slowestMs, g_loaded, g_failed, g_bytes / 1048576.0, g_batchFrames, g_batchWorstFrameMs,
        g_batchLongFrames);
    g_batch = 0;
    g_batchMs = g_slowestMs = 0;
    g_batchFrames = g_batchLongFrames = 0;
    g_batchWorstFrameMs = 0;
}

void Work()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_lock lock(g_mutex);
    for (;;) {
        g_wake.wait(lock, [] { return !g_queue.empty(); });
        const std::string key = std::move(g_queue.front());
        g_queue.pop_front();
        Entry& e = g_entries[key];
        if (e.state != State::Queued) continue;
        e.state = State::Loading;
        const std::string path = e.path;
        const uint32_t maxSide = e.maxSide;
        ID3D11Device* device = g_device;
        if (device) device->AddRef();
        const bool freeThreaded = g_freeThreaded;
        lock.unlock();

        const auto start = Clock::now();
        std::string error;
        std::vector<uint8_t> bytes;
        TexPackage package;
        TexImage image;
        TextureView view;
        void* file = nullptr;
        {
            std::lock_guard look(g_mutex);
            file = PakFile();
        }
        bool ok = file && ReadPackage(file, path, &bytes, &error);
        if (!file) error = "no pak file";
        if (ok) ok = ParseTexturePackage(bytes.data(), bytes.size(), &package, &error);
        const TexMip* mip = ok ? PickMip(package, maxSide) : nullptr;
        if (ok) ok = mip && MipImage(bytes.data(), bytes.size(), package, *mip, maxSide, &image, &error);
        const bool now = ok && device && freeThreaded;
        if (now) ok = Upload(device, image, &view, &error);
        if (device) device->Release();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

        lock.lock();
        Entry& done = g_entries[key];
        if (ok && !now) {
            done.image = std::move(image);
            done.state = State::Upload;
            continue;
        }
        done.view = view;
        done.image.format = image.format;
        Finish(done, ok, ms, error);
        LogBatchIfDone();
    }
}

void StartLocked()
{
    if (g_started) return;
    g_started = true;
    for (int i = 0; i < kWorkers; i++) std::thread(Work).detach();
}

} // namespace

void SetPakTextureDevice(void* device)
{
    auto* d = static_cast<ID3D11Device*>(device);
    std::lock_guard lock(g_mutex);
    if (d == g_device) return;
    // What the last device made is of no use to the new one: made again on
    // request (a view still drawn this frame stays valid: it holds its texture).
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (it->second.state == State::Loading) {
            ++it;
            continue;
        }
        if (it->second.view.srv) static_cast<IUnknown*>(it->second.view.srv)->Release();
        it = g_entries.erase(it);
    }
    g_queue.clear();
    g_bytes = 0;
    if (g_device) g_device->Release();
    g_device = d;
    if (d) {
        d->AddRef();
        g_freeThreaded = !(d->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED);
        Log("pak textures: device %p (%s)", device, g_freeThreaded ? "free threaded: textures made on the workers" : "single threaded: textures made at Present");
    }
}

const TextureView* PakTexture(const std::string& path, uint32_t maxSide, bool urgent, bool* failed)
{
    *failed = false;
    if (!g_enabled) {
        *failed = true;
        return nullptr;
    }
    const std::string key = Key(path, maxSide);
    std::lock_guard lock(g_mutex);
    if (!g_device) return nullptr;
    auto it = g_entries.find(key);
    if (it == g_entries.end()) {
        Entry& e = g_entries[key];
        e.path = path;
        e.maxSide = maxSide;
        if (g_queue.empty()) g_batchStart = Clock::now();
        if (urgent) g_queue.push_front(key);
        else g_queue.push_back(key);
        StartLocked();
        g_wake.notify_one();
        return nullptr;
    }
    Entry& e = it->second;
    if (e.state == State::Ready) return &e.view;
    if (e.state == State::Failed) {
        *failed = true;
        return nullptr;
    }
    if (urgent && e.state == State::Queued && !g_queue.empty() && g_queue.front() != key) {
        const auto queued = std::find(g_queue.begin(), g_queue.end(), key);
        if (queued != g_queue.end()) {
            g_queue.erase(queued);
            g_queue.push_front(key);
        }
    }
    return nullptr;
}

void WarmPakTextures(const std::vector<std::pair<std::string, uint32_t>>& textures)
{
    bool failed = false;
    for (const auto& [path, maxSide] : textures) PakTexture(path, maxSide, false, &failed);
}

void CreatePakTextures()
{
    std::vector<std::pair<std::string, TexImage>> todo;
    ID3D11Device* device = nullptr;
    {
        std::lock_guard lock(g_mutex);
        // Frame times while textures load (the batch's log line).
        const auto now = Clock::now();
        if (!g_queue.empty() || g_batch > 0) {
            const double frameMs = std::chrono::duration<double, std::milli>(now - g_lastPresent).count();
            if (g_lastPresent != Clock::time_point{} && frameMs < 1000) {
                g_batchFrames++;
                g_batchWorstFrameMs = std::max(g_batchWorstFrameMs, frameMs);
                g_batchLongFrames += frameMs > 20;
            }
        }
        g_lastPresent = now;
        for (auto& [key, e] : g_entries) {
            if (e.state == State::Upload) todo.emplace_back(key, std::move(e.image));
        }
        if (todo.empty()) return;
        device = g_device;
        if (device) device->AddRef();
    }
    // A few a frame: each costs the copy into the device.
    const auto start = Clock::now();
    for (auto& [key, image] : todo) {
        TextureView view;
        std::string error;
        const bool ok = device && Upload(device, image, &view, &error);
        std::lock_guard lock(g_mutex);
        auto it = g_entries.find(key);
        if (it == g_entries.end()) {
            if (view.srv) static_cast<IUnknown*>(view.srv)->Release();
            continue;
        }
        it->second.view = view;
        it->second.image.format = image.format;
        Finish(it->second, ok, 0, error);
        if (Clock::now() - start > std::chrono::milliseconds(2)) break;
    }
    if (device) device->Release();
    std::lock_guard lock(g_mutex);
    // Those not reached wait for the next frame (their images moved back).
    for (auto& [key, image] : todo) {
        auto it = g_entries.find(key);
        if (it != g_entries.end() && it->second.state == State::Upload && !image.bytes.empty()) it->second.image = std::move(image);
    }
    LogBatchIfDone();
}

void DumpPakPackage(const std::string& path, const std::string& file)
{
    void* pak = nullptr;
    {
        std::lock_guard lock(g_mutex);
        pak = PakFile();
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!pak || !ReadPackage(pak, path, &bytes, &error)) {
        Log("paktex dump %s: %s", path.c_str(), pak ? error.c_str() : "no pak file");
        return;
    }
    if (FILE* f = OpenFile(file, "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    TexPackage package;
    if (!ParseTexturePackage(bytes.data(), bytes.size(), &package, &error)) {
        Log("paktex dump %s: %zu bytes -> %s; not parsed: %s", path.c_str(), bytes.size(), file.c_str(), error.c_str());
        return;
    }
    Log("paktex dump %s: %zu bytes -> %s; %s %ux%u, %zu mips with data", path.c_str(), bytes.size(), file.c_str(),
        package.formatName.c_str(), package.width, package.height, package.mips.size());
    for (const TexMip& m : package.mips) Log("  mip %ux%u at %zu, %zu bytes", m.width, m.height, m.offset, m.size);
}

void SetPakTexturesEnabled(bool on)
{
    g_enabled = on;
    Log("pak textures: %s", on ? "on" : "off (the game's loader draws)");
}

std::string PakTextureStatus()
{
    std::lock_guard lock(g_mutex);
    int queued = 0, ready = 0, failed = 0;
    for (const auto& [key, e] : g_entries) {
        queued += e.state == State::Queued || e.state == State::Loading || e.state == State::Upload;
        ready += e.state == State::Ready;
        failed += e.state == State::Failed;
    }
    char line[200];
    std::snprintf(line, sizeof(line), "pak textures %s: %d ready (%.1f MB), %d loading, %d failed; device %s", g_enabled ? "on" : "off",
                  ready, g_bytes / 1048576.0, queued, failed, !g_device ? "none" : g_freeThreaded ? "free threaded" : "single threaded");
    return line;
}

} // namespace sfv::mod
