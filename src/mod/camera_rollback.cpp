// The camera journal: view-target requests replayed after a load (docs/rollback-presentation.md).

#include "mod/camera_rollback.h"

#include "mod/battle_driver.h"
#include "mod/flow.h"
#include "mod/log.h"
#include "mod/ue4.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

constexpr game::CodeSite kSetViewTarget{
    "camera manager SetViewTarget", 0x1BC1E90, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C}};
constexpr size_t kViewTarget = 0x9E8, kPendingTarget = 0xD30, kBlendTimeToGo = 0x1078, kManagerSize = 0x1090;
constexpr float kTickSeconds = 1.0f / 60.0f;
constexpr size_t kActorOwner = 0x300;
constexpr size_t kDescribedBytes = 0x30;
constexpr int kRequestsLogged = 2000;
constexpr size_t kJournalLimit = 512, kJournalKept = 256;
constexpr int kRollbackReachTicks = 600;

struct Transition {
    float blendTime;
    uint8_t function;
    uint8_t pad[3];
    float exponent;
    uint32_t bits;
};

static_assert(sizeof(Transition) == 16);

bool SameTransition(const Transition& a, const Transition& b) {
    return a.blendTime == b.blendTime && a.function == b.function && a.exponent == b.exponent && (a.bits & 1) == (b.bits & 1);
}

struct Request {
    uint8_t* manager;
    uint32_t tick;
    void* target;
    Transition transition;
    bool inTick;
};

using SetViewTargetFn = void (*)(uint8_t* manager, void* target, const Transition* transition);
SetViewTargetFn g_origSetViewTarget = nullptr;

std::atomic<bool> g_on{true};
std::atomic<bool> g_log{false};
bool g_inTick = false;
std::vector<Request> g_journal;
std::vector<Request> g_aside;

struct LateStart {
    uint8_t* manager;
    void* target;
    uint32_t tick;
};

std::vector<LateStart> g_late;
int g_logs = 0;

template <typename T>
T& At(uint8_t* p, size_t off) { return *reinterpret_cast<T*>(p + off); }

void* TargetOf(uint8_t* manager, void* target) { return target ? target : At<void*>(manager, kActorOwner); }

bool InEffect(const Request& r) {
    void* target = TargetOf(r.manager, r.target);
    void* view = At<void*>(r.manager, kViewTarget);
    void* pending = At<void*>(r.manager, kPendingTarget);
    return pending == target || (view == target && !pending);
}

std::string Name(const void* object) {
    return object && IsReadable(object, kDescribedBytes) ? ue4::Describe(object) : "none";
}

void LogRequest(const char* what, const Request& r) {
    if (!g_log || g_logs++ > kRequestsLogged) return;
    Log("camera: %s: tick %u%s%s phase %d, %s -> %s, blend %.3f (function %u), now view %s pending %s, to go %.3f", what, r.tick,
        r.inTick ? "" : " (outside a tick)", BattleInReplayedTick() ? " replayed" : "", FlowSchedulerPhaseNow(), Name(r.manager).c_str(),
        Name(r.target).c_str(), r.transition.blendTime, r.transition.function, Name(At<void*>(r.manager, kViewTarget)).c_str(),
        Name(At<void*>(r.manager, kPendingTarget)).c_str(), At<float>(r.manager, kBlendTimeToGo));
}

void SetViewTargetDetour(uint8_t* manager, void* target, const Transition* transition) {
    if (!g_on || !transition || !BattleCurrentFrameDriver() || !IsReadable(manager, kManagerSize)) {
        g_origSetViewTarget(manager, target, transition);
        return;
    }
    const Request r{manager, FlowDrivenTicks(), target, *transition, g_inTick};
    if (r.inTick) {
        for (size_t i = 0; i < g_aside.size(); i++) {
            const Request& a = g_aside[i];
            if (a.manager != manager || a.tick != r.tick || a.target != target || !SameTransition(a.transition, *transition)) continue;
            g_aside.erase(g_aside.begin() + static_cast<std::ptrdiff_t>(i));
            if (InEffect(r)) {
                LogRequest("made again, let off", r);
                g_journal.push_back(r);
                return;
            }
            LogRequest("made again, no longer in effect", r);
            break;
        }
    }
    g_journal.push_back(r);
    LogRequest("new", r);
    g_origSetViewTarget(manager, target, transition);
    if (r.inTick && BattleInReplayedTick() && transition->blendTime > 0) g_late.push_back({manager, TargetOf(manager, target), r.tick});
}

} // namespace

bool CameraInstall(const GameImage& image) {
    void* target = image.At<void*>(kSetViewTarget.rva);
    if (std::memcmp(target, kSetViewTarget.bytes, sizeof(kSetViewTarget.bytes)) != 0) {
        Log("camera rollback: signature mismatch at %s: camera views reset on loads as the game does", kSetViewTarget.name);
        g_on = false;
        return false;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&SetViewTargetDetour), reinterpret_cast<void**>(&g_origSetViewTarget)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("camera rollback: hook failed: camera views reset on loads as the game does");
        g_on = false;
        return false;
    }
    return true;
}

void CameraSetRollback(bool on) {
    g_on = on && g_origSetViewTarget;
    Log("camrollback: %s", g_on ? "camera views follow rollbacks" : "the game's own reset of camera views on loads");
}

bool CameraRollbackOn() { return g_on; }

void CameraSetLog(bool on) {
    g_log = on;
    g_logs = 0;
    Log("camlog: %s", on ? "every view request logged" : "off");
}

void CameraTickBegin() { g_inTick = true; }

void CameraTickEnd() { g_inTick = false; }

void CameraLoaded(uint32_t tick) {
    if (!g_on) return;
    for (size_t i = 0; i < g_journal.size();) {
        if (g_journal[i].inTick && g_journal[i].tick > tick) {
            g_aside.push_back(g_journal[i]);
            g_journal.erase(g_journal.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    for (size_t i = 0; i < g_late.size();) {
        if (g_late[i].tick > tick) g_late.erase(g_late.begin() + static_cast<std::ptrdiff_t>(i));
        else i++;
    }
}

void CameraFrameEnd() {
    if (!g_on) return;
    const uint32_t now = FlowDrivenTicks();
    for (const Request& gone : g_aside) LogRequest("not made again", gone);
    g_aside.clear();
    for (const LateStart& l : g_late) {
        if (!IsReadable(l.manager, kManagerSize) || At<void*>(l.manager, kPendingTarget) != l.target || now <= l.tick) continue;
        float& left = At<float>(l.manager, kBlendTimeToGo);
        left = std::max(1e-4f, left - static_cast<float>(now - l.tick) * kTickSeconds);
    }
    g_late.clear();
    if (g_journal.size() > kJournalLimit) {
        std::vector<Request> kept;
        for (const Request& r : g_journal) {
            if (!r.inTick || r.tick + kRollbackReachTicks >= now) kept.push_back(r);
        }
        if (kept.size() > kJournalLimit) kept.erase(kept.begin(), kept.end() - kJournalKept);
        g_journal.swap(kept);
    }
}

void CameraJournalReset() {
    g_journal.clear();
    g_aside.clear();
    g_late.clear();
}

} // namespace sfv::mod
