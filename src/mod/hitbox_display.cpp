#include "mod/hitbox_display.h"

#include "mod/battle_actors.h"
#include "mod/config.h"
#include "mod/flow.h"
#include "mod/image.h"
#include "mod/log.h"
#include "mod/replay_driver.h"
#include "mod/sfv_game.h"
#include "mod/ue4.h"

#include <imgui.h>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace sfv::mod {

namespace {

using namespace actor; // battle_actors.h

// The player controller's PlayerCameraManager (+0x390), whose CameraCache.POV (+0x370) is the view
// drawn: location, rotation (pitch, yaw, roll), FOV (horizontal), ...,
// aspect ratio (+0x28).
constexpr size_t kCameraManager = 0x390, kPov = 0x370;
constexpr ULONGLONG kStaleMs = 250;  // no collection this long: the battle is gone
constexpr ULONGLONG kPausedMs = 150; // no battle tick this long out of a replay: the game is paused

uintptr_t g_base = 0;
std::atomic<int> g_training{-1}, g_replays{-1}; // -1: not read yet
std::atomic<bool> g_forced{false};
std::atomic<int> g_log{0};

std::mutex g_mutex;
std::vector<WorldBox> g_boxes;
std::atomic<ULONGLONG> g_collectedAt{0};
std::atomic<const uint8_t*> g_camera{nullptr};
std::atomic<uintptr_t> g_cameraVtable{0};
uint64_t g_cameraLayout = ~0ull;

template <class T>
T At(const uint8_t* p, size_t offset) {
    T v;
    std::memcpy(&v, p + offset, sizeof(T));
    return v;
}

bool ClassNamed(const void* object, const char* part) {
    return object && IsReadable(object, 0x30) && ue4::ObjectName(ue4::ClassOf(object)).find(part) != std::string::npos;
}

// The drawn camera's manager: the PlayerCameraManager (+0x390) of the player
// controller that has a local player (+0x360). The battle camera (the
// game's region of 528 bytes from its +0x100; its sub-object with vtable
// 0x2C4F4A0 at +0xE8) points at both battle player controllers (+0x198,
// +0x1C0); the object walk (as replay_driver's CameraText, ~0.5 s) only when
// that path does not hold.
const uint8_t* ManagerOf(const uint8_t* controller) {
    if (!ClassNamed(controller, "PlayerController") || !IsReadable(controller + kCameraManager, 8) || !At<const void*>(controller, 0x360)) {
        return nullptr;
    }
    const auto* manager = At<const uint8_t*>(controller, kCameraManager);
    return ClassNamed(manager, "CameraManager") ? manager : nullptr;
}

const uint8_t* FindCameraManager(SfvGame& game) {
    if (!ue4::EnsureObjects(g_base)) return nullptr;
    for (const StateRegion& r : game.Regions()) {
        if (r.size != 528 || !IsReadable(r.ptr - 0x100, 0x200)) continue;
        if (At<uintptr_t>(r.ptr - 0x18, 0) != g_base + 0x2C4F4A0) continue;
        for (size_t at : {size_t{0x198}, size_t{0x1C0}}) {
            if (const uint8_t* m = ManagerOf(At<const uint8_t*>(r.ptr - 0x100, at))) return m;
        }
    }
    const uint8_t* found = nullptr;
    ue4::ForEachObject([&](const void* object) {
        if (!found) found = ManagerOf(static_cast<const uint8_t*>(object));
    });
    Log("hitboxes: camera found by the object walk");
    return found;
}

bool Wanted() {
    if (g_forced) return true;
    const ReplayView v = ReplayViewNow();
    if (v.active) return !v.benchmark && HitboxesInReplays();
    return HitboxesInTraining() && CurrentScene() == "TrainingBattleState";
}

void Collect(SfvGame& game, std::vector<WorldBox>& boxes) {
    const bool logging = g_log > 0;
    std::string text;
    const uintptr_t entryVtable = g_base + kEntryVtable;
    for (const StateRegion& r : game.Regions()) {
        if (r.size != kRegionSize || !IsReadable(r.ptr - kPosition, kPosition)) continue;
        const uintptr_t vt = At<uintptr_t>(r.ptr - kVtableAt, 0) - g_base;
        const bool fighter = vt == kFighterVtable;
        if (!fighter && vt != kEffectVtable) continue;
        const uint32_t side = At<uint32_t>(r.ptr, kSide);
        if (side > 1) continue; // an effect slot never used
        if (!fighter && !At<uint32_t>(r.ptr, kEffectLiveA) && !At<uint32_t>(r.ptr, kEffectLiveB)) continue;
        const auto* pos = At<const uint8_t*>(r.ptr - kPosition, 0);
        const size_t xy = fighter ? kFighterXY : kEffectXY;
        if (!pos || !IsReadable(pos + xy, 8)) continue;
        const float px = At<float>(pos, xy), py = At<float>(pos, xy + 4);
        if (logging) {
            char b[64];
            std::snprintf(b, sizeof(b), " | %s %.2f,%.2f:", fighter ? "F" : "E", px, py);
            text += b;
        }
        auto list = [&](size_t from, size_t to, bool hit) {
            for (size_t at = from; at + kEntrySize <= to; at += kEntrySize) {
                const uint8_t* e = r.ptr + at;
                if (At<uintptr_t>(e, kEntryVtableAt) != entryVtable) continue;
                const uint32_t id = At<uint32_t>(e, kEntryId);
                if (id == 0xFFFFFFFFu) continue;
                const float bx = At<float>(e, 0), by = At<float>(e, 4), bw = At<float>(e, 12), bh = At<float>(e, 16);
                if (!(bw > 0 && bh > 0)) continue;
                WorldBox b{side ? px - bx - bw : px + bx, py + by, bw, bh, WorldBox::Hurt};
                if (hit) {
                    const uint8_t type = e[kEntryType];
                    if (type > 5) continue;
                    b.kind = type == 2 ? WorldBox::Throw : type == 4 ? WorldBox::Proximity
                                                                     : WorldBox::Hit;
                    boxes.push_back(b);
                } else {
                    const uint32_t type = At<uint32_t>(e, kEntryType);
                    if (type & 3) boxes.push_back(b);
                    if (type & 4) {
                        b.kind = WorldBox::Throwable;
                        boxes.push_back(b);
                    }
                }
                if (logging) {
                    char t[40];
                    std::snprintf(t, sizeof(t), " %s%u/%u", hit ? "H" : "u", id >> 16, (id >> 8) & 0xFF);
                    text += t;
                }
            }
        };
        list(kHitList, kHitListEnd, true);
        list(kHurtList, kHurtListEnd, false);
    }
    if (logging) Log("boxlog %d:%s", --g_log, text.c_str());
}

int ReadChoice(std::atomic<int>& cache, const wchar_t* key, int fallback) {
    int v = cache;
    if (v < 0) cache = v = ReadMainIni(L"hitboxes", key, fallback) ? 1 : 0;
    return v;
}

} // namespace

bool HitboxesInTraining() { return ReadChoice(g_training, L"training", 1) != 0; }

bool HitboxesInReplays() { return ReadChoice(g_replays, L"replays", 0) != 0; }

void SetHitboxesInTraining(bool on) {
    g_training = on ? 1 : 0;
    SaveMainIni(L"hitboxes", L"training", on ? 1 : 0);
}

void SetHitboxesInReplays(bool on) {
    g_replays = on ? 1 : 0;
    SaveMainIni(L"hitboxes", L"replays", on ? 1 : 0);
}

void SetHitboxesForced(bool on) {
    g_forced = on;
    Log("hitboxes: %s in any battle", on ? "forced on" : "back to the choices");
}

void CollectHitboxes(SfvGame& game) {
    if (!g_base) g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!Wanted()) return;
    // Training's pause menu stops the ticks (a paused replay runs none either,
    // but keeps its picture): no boxes over the game's menu.
    if (!ReplayViewNow().active && GetTickCount64() - game.LastTickMs() > kPausedMs) return;
    // The camera manager once per battle (a new region layout).
    if (game.LayoutVersion() != g_cameraLayout) {
        g_cameraLayout = game.LayoutVersion();
        const uint8_t* manager = FindCameraManager(game);
        g_cameraVtable = manager ? At<uintptr_t>(manager, 0) : 0;
        g_camera = manager;
        Log("hitboxes: %s, camera %s", CurrentScene().c_str(), manager ? ue4::Describe(manager).c_str() : "not found");
    }
    std::vector<WorldBox> boxes;
    Collect(game, boxes);
    {
        std::lock_guard lock(g_mutex);
        g_boxes = std::move(boxes);
    }
    g_collectedAt = GetTickCount64();
}

bool HitboxesVisible() {
    return g_camera.load() && GetTickCount64() - g_collectedAt.load() < kStaleMs;
}

std::vector<WorldBox> HitboxSnapshot() {
    std::lock_guard lock(g_mutex);
    return g_boxes;
}

void LogHitboxes(int frames) { g_log = frames; }

void DrawHitboxes(ImDrawList* d, float width, float height) {
    if (!HitboxesVisible()) return;
    // The camera as drawn this frame (the manager still the one found: its
    // vtable checked, the object gone with the battle).
    const uint8_t* manager = g_camera;
    if (!manager || !IsReadable(manager, kPov + 0x30) || At<uintptr_t>(manager, 0) != g_cameraVtable) return;
    float pov[7];
    std::memcpy(pov, manager + kPov, sizeof(pov));
    // The game draws a view of the camera's aspect (16:9) with bars around
    // it in a window of another shape (tested at 4:3 and 21:9).
    float aspect = At<float>(manager, kPov + 0x28);
    if (!(aspect > 1.2f && aspect < 2.5f)) aspect = 16.0f / 9.0f;
    const std::vector<WorldBox> boxes = HitboxSnapshot();
    if (boxes.empty() || !(pov[6] > 1 && pov[6] < 170)) return;
    float vx = 0, vy = 0, vw = width, vh = height;
    if (width / height > aspect) {
        vw = height * aspect;
        vx = (width - vw) / 2;
    } else {
        vh = width / aspect;
        vy = (height - vh) / 2;
    }
    constexpr float kRad = 3.14159265f / 180.0f;
    const float cp = std::cos(pov[3] * kRad), sp = std::sin(pov[3] * kRad);
    const float cy = std::cos(pov[4] * kRad), sy = std::sin(pov[4] * kRad);
    const float f[3] = {cp * cy, cp * sy, sp}, r[3] = {-sy, cy, 0}, u[3] = {-sp * cy, -sp * sy, cp};
    const float scale = vw * 0.5f / std::tan(pov[6] * 0.5f * kRad); // horizontal FOV over the view's width
    bool ok = true;
    auto project = [&](float bx, float by) {
        const float p[3] = {bx * 100.0f - pov[0], -pov[1], by * 100.0f - pov[2]};
        const float depth = p[0] * f[0] + p[1] * f[1] + p[2] * f[2];
        if (depth < 1.0f) {
            ok = false;
            return ImVec2();
        }
        return ImVec2(vx + vw * 0.5f + (p[0] * r[0] + p[1] * r[1] + p[2] * r[2]) / depth * scale,
                      vy + vh * 0.5f - (p[0] * u[0] + p[1] * u[1] + p[2] * u[2]) / depth * scale);
    };
    const float line = std::max(1.0f, height / 540.0f); // 2 px at 1080p
    d->PushClipRect(ImVec2(vx, vy), ImVec2(vx + vw, vy + vh), false);
    // Back to front: throwable and hurt under the attacking boxes.
    for (WorldBox::Kind kind : {WorldBox::Throwable, WorldBox::Hurt, WorldBox::Proximity, WorldBox::Throw, WorldBox::Hit}) {
        for (const WorldBox& b : boxes) {
            if (b.kind != kind) continue;
            ok = true;
            const ImVec2 p0 = project(b.x, b.y + b.h), p1 = project(b.x + b.w, b.y);
            if (!ok) continue;
            switch (kind) {
                case WorldBox::Hurt: d->AddRect(p0, p1, IM_COL32(40, 230, 70, 255), 0, 0, line); break;
                case WorldBox::Throwable:
                    d->AddRectFilled(p0, p1, IM_COL32(250, 220, 40, 70));
                    d->AddRect(p0, p1, IM_COL32(250, 220, 40, 210), 0, 0, line * 0.5f);
                    break;
                case WorldBox::Proximity:
                    d->AddRectFilled(p0, p1, IM_COL32(150, 150, 150, 60));
                    d->AddRect(p0, p1, IM_COL32(175, 175, 175, 170), 0, 0, line * 0.5f);
                    break;
                case WorldBox::Throw:
                    d->AddRectFilled(p0, p1, IM_COL32(40, 90, 250, 110));
                    d->AddRect(p0, p1, IM_COL32(70, 120, 255, 255), 0, 0, line);
                    break;
                case WorldBox::Hit:
                    d->AddRectFilled(p0, p1, IM_COL32(240, 40, 40, 110));
                    d->AddRect(p0, p1, IM_COL32(255, 60, 60, 255), 0, 0, line);
                    break;
            }
        }
    }
    d->PopClipRect();
}

} // namespace sfv::mod
