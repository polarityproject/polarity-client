// SfvGame's research on the battle camera's start state, and the start probe.

#include "mod/sfv_game_internal.h"

namespace sfv::mod {

using namespace snapshot;

namespace {

std::atomic<bool> g_watchCamera{false};

} // namespace

void GameWatchCameraAtFreeze() { g_watchCamera = true; }

namespace {

constexpr uint32_t kBattleCameraCtor = 0x3C72A0;
constexpr size_t kCameraFlagsOff = 0x94, kCameraE4Off = 0xE4;
constexpr size_t kCameraSetupFirst = kBattleCameraRegionAt + 0x28, kCameraSetupEnd = kBattleCameraRegionAt + 0x60;
constexpr uint32_t kCameraIdleFlags = 0x100, kCameraSetUpBit = 0x80000000u;
using CameraCtorFn = void* (*)(void*, void*, void*, void*);
CameraCtorFn g_origCameraCtor = nullptr;
std::atomic<bool> g_watchNewCamera{false};

void* CameraCtorDetour(void* self, void* a, void* b, void* c) {
    void* r = g_origCameraCtor(self, a, b, c);
    if (g_watchNewCamera.exchange(false)) {
        auto* cam = static_cast<uint8_t*>(self);
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        Log("watch: battle camera constructed at %p (thread %lu); watching +0x90, +0xE0, +0x1D0, +0x1F0", self,
            GetCurrentThreadId());
        std::thread arm([cam, base] {
            ThreadGuard running;
            WatchArm(0, cam + 0x90, 8, base);
            WatchArm(1, cam + 0xE0, 8, base);
            WatchArm(2, cam + 0x1D0, 8, base);
            WatchArm(3, cam + 0x1F0, 8, base);
        });
        arm.join();
    }
    return r;
}

} // namespace

void GameWatchNextCamera() {
    if (!g_origCameraCtor) {
        void* target = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + kBattleCameraCtor;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&CameraCtorDetour), reinterpret_cast<void**>(&g_origCameraCtor)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            Log("watch: could not hook the battle camera's constructor");
            g_origCameraCtor = nullptr;
            return;
        }
    }
    g_watchNewCamera = true;
}

const uint8_t* SfvGame::FindBattleCamera() const {
    for (const StateRegion& r : regions_) {
        if (r.size == kBattleCameraRegionSize && IsReadable(r.ptr - (kBattleCameraRegionAt - kBattleCameraPartAt), 8) &&
            *reinterpret_cast<const uintptr_t*>(r.ptr - (kBattleCameraRegionAt - kBattleCameraPartAt)) == image_.base + kBattleCameraVtable) {
            const uint8_t* camera = r.ptr - kBattleCameraRegionAt;
            if (IsReadable(camera, kBattleCameraSize)) return camera;
        }
    }
    return nullptr;
}

std::string SfvGame::CameraSetupText() const {
    const uint8_t* cam = FindBattleCamera();
    if (!cam) return "no battle camera";
    uint32_t flags, set = 0;
    float e4, x;
    std::memcpy(&flags, cam + kCameraFlagsOff, 4);
    std::memcpy(&e4, cam + kCameraE4Off, 4);
    std::memcpy(&x, cam + kBattleCameraRegionAt, 4);
    for (size_t at = kCameraSetupFirst; at < kCameraSetupEnd; at += 4) {
        uint32_t v;
        std::memcpy(&v, cam + at, 4);
        set += v != 0;
    }
    char text[160];
    std::snprintf(text, sizeof(text), "battle camera: +0x94 %08x, +0xE4 %.3f, %u of 14 setup words set, x %.4f", flags, e4, set, x);
    return text;
}

bool SfvGame::CameraNotStarted() const {
    const uint8_t* cam = FindBattleCamera();
    if (!cam) return false;
    uint32_t flags;
    std::memcpy(&flags, cam + kCameraFlagsOff, 4);
    for (size_t at = kCameraSetupFirst; at < kCameraSetupEnd; at += 4) {
        uint32_t v;
        std::memcpy(&v, cam + at, 4);
        if (v) return false;
    }
    return flags == kCameraIdleFlags;
}

void SfvGame::WatchCameraIfAsked() {
    if (!g_watchCamera.exchange(false)) return;
    const uint8_t* cam = FindBattleCamera();
    if (!cam) {
        Log("watch: no battle camera to watch");
        return;
    }
    const uintptr_t base = image_.base;
    Log("watch: battle camera at %p (%s); watching +0x90 and +0xE0", static_cast<const void*>(cam), CameraSetupText().c_str());
    std::thread([cam, base] {
        ThreadGuard running;
        WatchArm(0, cam + 0x90, 8, base);
        WatchArm(1, cam + 0xE0, 8, base);
    }).detach();
}

bool SfvGame::CameraSetUp() const {
    const uint8_t* cam = FindBattleCamera();
    if (!cam) return false;
    uint32_t flags;
    std::memcpy(&flags, cam + kCameraFlagsOff, 4);
    return (flags & kCameraSetUpBit) != 0;
}

void SfvGame::ArmStartProbe() {
    {
        std::lock_guard lock(probeMutex_);
        probe_.clear();
        probeLogged_ = false;
    }
    CaptureStartProbe("at the freeze");
    probeTicks_ = 2;
}

void SfvGame::CaptureStartProbe(const char* when) {
    ProbeCapture capture{when, {}};
    for (const StateRegion& r : regions_) {
        if (r.size == kBattleCameraRegionSize && IsReadable(r.ptr - (kBattleCameraRegionAt - kBattleCameraPartAt), 8) &&
            *reinterpret_cast<const uintptr_t*>(r.ptr - (kBattleCameraRegionAt - kBattleCameraPartAt)) == image_.base + kBattleCameraVtable) {
            const uint8_t* camera = r.ptr - kBattleCameraRegionAt;
            if (IsReadable(camera, kBattleCameraSize)) capture.objects.push_back({"camera (" + r.name + ")", {camera, camera + kBattleCameraSize}});
        }
        if (r.size == kFighterSimRegionSize && r.name.rfind("s300.", 0) == 0) {
            const uint8_t* sim = r.ptr - kFighterSimRegionAt;
            if (IsReadable(sim, kFighterSimSize)) capture.objects.push_back({"fighter sim (" + r.name + ")", {sim, sim + kFighterSimSize}});
        }
    }
    std::lock_guard lock(probeMutex_);
    if (probe_.size() < 6) probe_.push_back(std::move(capture));
}

void SfvGame::LogStartProbe(const char* why) {
    std::lock_guard lock(probeMutex_);
    if (probeLogged_ || probe_.empty()) return;
    probeLogged_ = true;
    Log("start probe (%s; objects after %d neutral ticks): %zu captures, words that are not zero or an address",
        why, objectsAt_, probe_.size());
    for (const ProbeCapture& c : probe_) {
        for (const auto& [name, bytes] : c.objects) {
            std::string line;
            int words = 0;
            for (size_t at = 0; at + 8 <= bytes.size(); at += 8) {
                uint64_t q;
                std::memcpy(&q, &bytes[at], 8);
                if (q == 0 || GameLooksLikeAddress(q)) continue;
                for (size_t half = 0; half < 8; half += 4) {
                    uint32_t v;
                    std::memcpy(&v, &bytes[at + half], 4);
                    if (!v) continue;
                    char b[24];
                    std::snprintf(b, sizeof(b), " %zx=%x", at + half, v);
                    line += b;
                    if (++words % 48 == 0) {
                        Log("start probe %s, %s:%s", c.when.c_str(), name.c_str(), line.c_str());
                        line.clear();
                    }
                }
            }
            if (!line.empty() || words == 0) Log("start probe %s, %s:%s", c.when.c_str(), name.c_str(), line.c_str());
        }
    }
}

} // namespace sfv::mod
