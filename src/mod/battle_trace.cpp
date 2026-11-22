// Research logs and traces of the battle: gauges, bones, actors, cinematics, cameras.

#include "mod/battle_internal.h"

#include "mod/battle_actors.h"
#include "mod/battle_setup.h"
#include "mod/files.h"
#include "mod/ue4.h"

#include <intrin.h>

namespace sfv::mod {

using namespace battle;

namespace {

constexpr game::CodeSite kGaugeAdd{
    "damage gauge add", 0x346A20, {0x44, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC}};
constexpr uintptr_t kGaugeAddFromHit = 0x36D47D;
constexpr int kGaugeLogsPerBattle = 400;
constexpr size_t kGaugeVitalityOff = 0xC0, kGaugeValueOff = 0x170, kGaugeReadBytes = 0xBC;
constexpr double kFixedOne = 65536.0;
constexpr size_t kControllerPlayerOff = 0x360, kControllerCameraOff = 0x390;
constexpr size_t kCameraViewOff = 0x370, kCameraViewBytes = 0x1C;
constexpr int kCineFramesAfter = 30;
constexpr float kCameraCut = 60;

constexpr uint8_t kMovRaxImm64[] = {0x48, 0xB8};
constexpr uint8_t kStoreR14AtRax[] = {0x4C, 0x89, 0x30};
constexpr uint8_t kStoreR13AtRax[] = {0x4C, 0x89, 0x28};
constexpr uint8_t kJmpRax[] = {0xFF, 0xE0};
constexpr size_t kStubBytes = 64;
using GaugeAddFn = int* (*)(uint8_t* gauge, int* out, int amount, int flags);
GaugeAddFn g_origGaugeAdd = nullptr;
uint64_t g_gaugeR13 = 0, g_gaugeR14 = 0;
int g_gaugeLogs = 0, g_gaugeBattle = -1;

int* GaugeAddDetour(uint8_t* gauge, int* out, int amount, int flags) {
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
    const int32_t damage = static_cast<int32_t>(g_gaugeR14);
    int32_t before[3] = {}, vitality[2] = {};
    const bool readable = IsReadable(gauge + kGaugeVitalityOff, kGaugeReadBytes);
    if (readable) {
        std::memcpy(before, gauge + kGaugeValueOff, sizeof(before));
        std::memcpy(vitality, gauge + kGaugeVitalityOff, sizeof(vitality));
    }
    int* result = g_origGaugeAdd(gauge, out, amount, flags);
    if (BattleCreationCount() != g_gaugeBattle) {
        g_gaugeBattle = BattleCreationCount();
        g_gaugeLogs = 0;
    }
    if (readable && g_gaugeLogs++ < kGaugeLogsPerBattle) {
        int32_t after[3];
        std::memcpy(after, gauge + kGaugeValueOff, sizeof(after));
        const double unit = kFixedOne;
        char cause[96];
        if (from == kGaugeAddFromHit) {
            const double divisor = amount && vitality[1] ? 1000.0 / (vitality[1] / unit) * (damage / unit) / (amount / unit) : 0;
            std::snprintf(cause, sizeof(cause), "for damage %.2f (divisor %.2f)", damage / unit, divisor);
        } else {
            std::snprintf(cause, sizeof(cause), "from rva 0x%llX", static_cast<unsigned long long>(from));
        }
        Log("gauge: tick %u%s, vitality %.0f/%.0f, add %.3f (flags %d) %s, now %.2f/%.0f level %d (was %.2f level %d)",
            FlowDrivenTicks(), g_replayingTick ? " replayed" : "", vitality[0] / unit, vitality[1] / unit, amount / unit, flags,
            cause, after[0] / unit, after[1] / unit, after[2], before[0] / unit, before[2]);
    }
    return result;
}

void* MakeGaugeStub() {
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, kStubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return nullptr;
    uint8_t* p = code;
    const auto put = [&p](const void* bytes, size_t n) {
        std::memcpy(p, bytes, n);
        p += n;
    };
    const auto movRax = [&put](const void* value) {
        const uint64_t v = reinterpret_cast<uint64_t>(value);
        put(kMovRaxImm64, sizeof(kMovRaxImm64));
        put(&v, sizeof(v));
    };
    movRax(&g_gaugeR14);
    put(kStoreR14AtRax, sizeof(kStoreR14AtRax));
    movRax(&g_gaugeR13);
    put(kStoreR13AtRax, sizeof(kStoreR13AtRax));
    movRax(reinterpret_cast<const void*>(&GaugeAddDetour));
    put(kJmpRax, sizeof(kJmpRax));
    FlushInstructionCache(GetCurrentProcess(), code, p - code);
    return code;
}

constexpr game::CodeSite kBonePosition{
    "fighter bone position", 0x3919D0, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0x05, 0x9F, 0x08, 0x63}};
using BonePositionFn = void (*)(uint8_t* fighter, const void* bone, float* out, int flag);
BonePositionFn g_origBonePosition = nullptr;
std::atomic<int> g_boneLog{0};
std::atomic<uintptr_t> g_boneLogFrom{0};

void BonePositionDetour(uint8_t* fighter, const void* bone, float* out, int flag) {
    g_origBonePosition(fighter, bone, out, flag);
    if (g_boneLog <= 0 || !g_inDriver) return;
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
    if (g_boneLogFrom && from != g_boneLogFrom) return;
    g_boneLog--;
    uint32_t bits[3];
    std::memcpy(bits, out, sizeof(bits));
    Log("bone: tick %u%s, from rva 0x%llX, fighter %p bone %p: %.6f %.6f %.6f (%08X %08X %08X)", FlowDrivenTicks(),
        g_replayingTick ? " replayed" : "", static_cast<unsigned long long>(from), static_cast<void*>(fighter), bone,
        out[0], out[1], out[2], bits[0], bits[1], bits[2]);
}

} // namespace

void battle::InstallTraceHooks(const GameImage& image) {
    if (!Hook(image, kBonePosition, reinterpret_cast<void*>(&BonePositionDetour), reinterpret_cast<void**>(&g_origBonePosition))) {
        Log("battle driver: bonelog unavailable");
    }
    void* gaugeStub = MakeGaugeStub();
    if (!gaugeStub || !Hook(image, kGaugeAdd, gaugeStub, reinterpret_cast<void**>(&g_origGaugeAdd))) {
        Log("battle driver: damage gauge adds not logged");
    }
}

void BattleLogGameInputs(int ticks) {
    if (g_game) g_game->inputLog = ticks;
    Log("inputlog: the game's per-pad values for the next %d ticks, on change", ticks);
}

void BattleLogStartProbe() {
    if (g_game) g_game->LogStartProbe("asked");
}

void BattleLogBonePositions(int count, uintptr_t from) {
    g_boneLog = count;
    g_boneLogFrom = from;
    Log("bone: logging the next %d bone positions asked for in driven ticks (from rva 0x%llX)", count,
        static_cast<unsigned long long>(from));
}

void BattleDumpMoveFile(const std::string& name) {
    void* object = ue4::EnsureObjects(g_imageBase) ? ue4::FindObject(name, "") : nullptr;
    if (!object || !IsReadable(static_cast<uint8_t*>(object) + kMoveFileDataOff, 0x10)) return Log("dumpbac: no %s", name.c_str());
    const auto* data = *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(object) + kMoveFileDataOff);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(object) + kMoveFileSizeOff);
    if (!data || !size || !IsReadable(data, size)) return Log("dumpbac: %s has no readable data", name.c_str());
    const std::wstring dir = ResearchDir();
    const std::wstring path = dir + L"\\" + std::wstring(name.begin(), name.end()) + L".bin";
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
        Log("dumpbac: %s, %u bytes, to Polarity\\research", name.c_str(), size);
    }
}

namespace {

struct MoveFileRange {
    std::string name;
    uintptr_t lo = 0, hi = 0;
};

std::vector<MoveFileRange> g_moveFiles;
std::atomic<int> g_actorLog{0};
const uint8_t* g_cameraManager = nullptr;

void RefreshActorObjects() {
    g_moveFiles.clear();
    if (ue4::EnsureObjects(g_imageBase)) {
        ue4::ForEachObject([](const void* object) {
            const std::string d = ue4::Describe(object);
            if (d.rfind("KWBAC ", 0) != 0 || !IsReadable(static_cast<const uint8_t*>(object) + kMoveFileDataOff, 0x10)) return;
            const uintptr_t lo = *reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(object) + kMoveFileDataOff);
            const uint32_t size = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(object) + kMoveFileSizeOff);
            if (lo && size) g_moveFiles.push_back({d.substr(6), lo, lo + size});
        });
    }
    g_cameraManager = nullptr;
    ue4::ForEachObject([](const void* object) {
        if (g_cameraManager || ue4::ObjectName(ue4::ClassOf(object)).find("PlayerController") == std::string::npos) return;
        const auto* p = static_cast<const uint8_t*>(object);
        if (!IsReadable(p + kControllerCameraOff, 8) || !*reinterpret_cast<const void* const*>(p + kControllerPlayerOff)) return;
        const auto* manager = *reinterpret_cast<const uint8_t* const*>(p + kControllerCameraOff);
        if (manager && ue4::ObjectName(ue4::ClassOf(manager)).find("CameraManager") != std::string::npos) g_cameraManager = manager;
    });
}

} // namespace

void BattleLogActors(int ticks) {
    RefreshActorObjects();
    g_actorLog = ticks;
    Log("actorlog: %zu move files, camera %s, the next %d ticks", g_moveFiles.size(),
        g_cameraManager ? ue4::Describe(g_cameraManager).c_str() : "none", ticks);
}

void BattleActorLogTick() {
    if (g_actorLog <= 0 || !g_game) return;
    const int left = --g_actorLog;
    std::string line;
    for (const StateRegion& r : g_game->Regions()) {
        if (r.size != actor::kRegionSize || !IsReadable(r.ptr - actor::kPosition, 0x48)) continue;
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(r.ptr - actor::kVtableAt) - g_imageBase;
        if (vt != actor::kFighterVtable && vt != actor::kEffectVtable) continue;
        const auto* runner = *reinterpret_cast<const uint8_t* const*>(r.ptr + actor::kRunner);
        if (!runner || !IsReadable(runner, 0x60)) continue;
        const uintptr_t script = *reinterpret_cast<const uintptr_t*>(runner + actor::kRunnerScript);
        const MoveFileRange* file = nullptr;
        for (const MoveFileRange& m : g_moveFiles) {
            if (script >= m.lo && script < m.hi) file = &m;
        }
        if (!file) continue;
        const auto* pos = *reinterpret_cast<const uint8_t* const*>(r.ptr - actor::kPosition);
        const bool hasPos = pos && IsReadable(pos + actor::kFighterXY, 8);
        char b[200];
        std::snprintf(b, sizeof(b), " | %s %s %s+%llX #%u f%.2f side %u pos %s %.3f %.3f", vt == actor::kFighterVtable ? "F" : "E", r.name.c_str(),
                      file->name.c_str(), static_cast<unsigned long long>(script - file->lo),
                      *reinterpret_cast<const uint32_t*>(runner + actor::kRunnerIndex), *reinterpret_cast<const int32_t*>(runner + actor::kRunnerFrame) / kFixedOne,
                      *reinterpret_cast<const uint32_t*>(r.ptr + actor::kSide),
                      hasPos ? "ok" : "none", hasPos ? *reinterpret_cast<const float*>(pos + actor::kFighterXY) : 0.0f,
                      hasPos ? *reinterpret_cast<const float*>(pos + actor::kFighterXY + 4) : 0.0f);
        line += b;
    }
    if (g_cameraManager && left % 30 == 0 && IsReadable(g_cameraManager + kCameraViewOff, kCameraViewBytes)) {
        const auto* v = reinterpret_cast<const float*>(g_cameraManager + kCameraViewOff);
        char b[160];
        std::snprintf(b, sizeof(b), " | cam %.1f %.1f %.1f r %.2f %.2f %.2f fov %.2f", v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
        line += b;
    }
    Log("actorlog %d:%s", left, line.c_str());
}

namespace {

std::atomic<int> g_cineLog{0};

struct CineSeen {
    uint32_t script = ~0u, cine = ~0u;
};

CineSeen g_cineSeen[4];
int g_cineAfter = 0;
float g_cineCam[7] = {};

std::string ScriptName(uintptr_t script) {
    for (const MoveFileRange& m : g_moveFiles) {
        if (script < m.lo || script >= m.hi || m.hi - m.lo < 24) continue;
        const auto* b = reinterpret_cast<const uint8_t*>(m.lo);
        const size_t size = m.hi - m.lo;
        auto u16 = [&](size_t o) { return o + 2 <= size ? *reinterpret_cast<const uint16_t*>(b + o) : uint16_t{0}; };
        auto u32 = [&](size_t o) { return o + 4 <= size ? *reinterpret_cast<const uint32_t*>(b + o) : 0u; };
        if (std::memcmp(b, "#BAC", 4) != 0) return m.name + "?";
        const uint16_t tables = u16(12);
        const uint32_t list = u32(16);
        for (uint16_t t = 0; t < tables; t++) {
            const uint32_t at = u32(list + 4 * t);
            if (!at) continue;
            const uint16_t count = u16(at + 2);
            const uint32_t moves = u32(at + 4), names = u32(at + 8);
            for (uint16_t i = 0; i < count; i++) {
                const uint32_t s = u32(at + moves + 4 * i);
                if (!s || m.lo + at + s != script) continue;
                const uint32_t n = u32(at + names + 4 * i);
                std::string name;
                for (size_t o = at + n; n && o < size && b[o] && name.size() < 48; o++) name += static_cast<char>(b[o]);
                return m.name + ":" + name;
            }
        }
        return m.name + "+" + std::to_string(script - m.lo);
    }
    return "?";
}

} // namespace

void BattleLogCinematics(int mode) {
    for (CineSeen& s : g_cineSeen) s = {};
    g_cineAfter = 0;
    g_cineLog = mode;
    Log("cinelog: %s", mode >= 2 ? "every tick's fighters, every frame's camera"
                       : mode    ? "the fighters' cinematics and scripts per tick, the camera per frame while one plays"
                                 : "off");
}

void BattleCineLogTick() {
    if (!g_cineLog || !g_game) return;

    struct Fighter {
        const char* name;
        uint32_t side, script, cine;
        float frame, speed, x, y;
        uintptr_t scriptPtr;
    };

    Fighter f[4] = {};
    int found = 0;
    for (const StateRegion& r : g_game->Regions()) {
        if (found == 4) break;
        if (r.size != actor::kRegionSize || !IsReadable(r.ptr - actor::kPosition, 0x48)) continue;
        if (*reinterpret_cast<const uintptr_t*>(r.ptr - actor::kVtableAt) - g_imageBase != actor::kFighterVtable) continue;
        const auto* runner = *reinterpret_cast<const uint8_t* const*>(r.ptr + actor::kRunner);
        if (!runner || !IsReadable(runner, 0x68)) continue;
        Fighter& g = f[found++];
        g.name = r.name.c_str();
        g.side = *reinterpret_cast<const uint32_t*>(r.ptr + actor::kSide);
        g.scriptPtr = *reinterpret_cast<const uintptr_t*>(runner + actor::kRunnerScript);
        g.script = *reinterpret_cast<const uint32_t*>(runner + actor::kRunnerIndex);
        g.frame = *reinterpret_cast<const int32_t*>(runner + actor::kRunnerFrame) / static_cast<float>(kFixedOne);
        g.speed = *reinterpret_cast<const int32_t*>(runner + actor::kRunnerSpeed) / static_cast<float>(kFixedOne);
        g.cine = *reinterpret_cast<const uint32_t*>(r.ptr + actor::kCinematic);
        const auto* pos = *reinterpret_cast<const uint8_t* const*>(r.ptr - actor::kPosition);
        if (pos && IsReadable(pos + actor::kFighterXY, 8)) {
            g.x = *reinterpret_cast<const float*>(pos + actor::kFighterXY);
            g.y = *reinterpret_cast<const float*>(pos + actor::kFighterXY + 4);
        }
    }
    if (found < 2) return;
    static ULONGLONG lookedUp = 0;
    const bool known = ScriptName(f[0].scriptPtr) != "?" && ScriptName(f[1].scriptPtr) != "?";
    const ULONGLONG since = GetTickCount64() - lookedUp;
    if ((g_moveFiles.empty() && since > 2000) || ((!known || !g_cameraManager) && since > 60000)) {
        lookedUp = GetTickCount64();
        RefreshActorObjects();
        Log("cinelog: %zu move files, camera %s", g_moveFiles.size(), g_cameraManager ? ue4::Describe(g_cameraManager).c_str() : "none");
    }
    bool changed = false, cine = false;
    for (int i = 0; i < found; i++) {
        changed |= f[i].script != g_cineSeen[i].script || f[i].cine != g_cineSeen[i].cine;
        cine |= f[i].cine != 0;
    }
    if (!changed && !cine && g_cineLog < 2) return;
    std::string line;
    for (int i = 0; i < found; i++) {
        char b[220];
        std::snprintf(b, sizeof(b), " | %s s%u %s #%u f%.2f cine %u speed %.2f pos %.3f %.3f", f[i].name, f[i].side,
                      ScriptName(f[i].scriptPtr).c_str(), f[i].script, f[i].frame, f[i].cine, f[i].speed, f[i].x, f[i].y);
        line += b;
        g_cineSeen[i].script = f[i].script;
        g_cineSeen[i].cine = f[i].cine;
    }
    Log("cinelog tick %u%s%s", FlowDrivenTicks(), g_inDriver && g_replayingTick ? " replayed" : "", line.c_str());
}

void BattleCineLogFrame() {
    if (!g_cineLog || !g_cameraManager || !IsReadable(g_cameraManager + kCameraViewOff, kCameraViewBytes)) return;
    bool cine = g_cineLog >= 2;
    for (const CineSeen& s : g_cineSeen) cine |= s.cine == 1;
    if (cine) g_cineAfter = kCineFramesAfter;
    else if (g_cineAfter > 0) g_cineAfter--;
    else return;
    const auto* v = reinterpret_cast<const float*>(g_cameraManager + kCameraViewOff);
    float jump = 0;
    for (int i = 0; i < 3; i++) jump = std::max(jump, std::fabs(v[i] - g_cineCam[i]));
    std::memcpy(g_cineCam, v, sizeof(g_cineCam));
    Log("cinelog frame: tick %u cam %.1f %.1f %.1f r %.2f %.2f %.2f fov %.2f%s", FlowDrivenTicks(), v[0], v[1], v[2], v[3], v[4], v[5], v[6],
        jump > kCameraCut ? " CUT" : "");
}

namespace {

struct TracedObject {
    const uint8_t* p;
    size_t size;
    std::string name;
};

constexpr uint32_t kTraceTick = 0, kTraceReplayedTick = 1, kTraceFrame = 2;

std::vector<TracedObject> g_camObjects;
FILE* g_camFile = nullptr;
uint32_t g_camFrom = 0, g_camTo = 0;

} // namespace

void BattleTraceCameraObjects(uint32_t from, uint32_t to, const std::string& text) {
    if (g_camFile) std::fclose(g_camFile), g_camFile = nullptr;
    g_camObjects.clear();
    if (!ue4::EnsureObjects(g_imageBase)) return Log("camtrace: no object table");
    ue4::ForEachObject([&](const void* object) {
        const std::string cls = ue4::ObjectName(ue4::ClassOf(object));
        if (cls.find(text) == std::string::npos || ue4::ObjectName(object).rfind("Default__", 0) == 0) return;
        size_t size = ue4::ObjectSize(object);
        if (size == 0 || size > 0x4000) size = 0x1000;
        if (!IsReadable(object, size)) return;
        g_camObjects.push_back({static_cast<const uint8_t*>(object), size, ue4::Describe(object)});
    });
    const std::wstring dir = ResearchDir();
    g_camFile = _wfopen((dir + L"\\camtrace.bin").c_str(), L"wb");
    if (!g_camFile) return;
    const uint32_t n = static_cast<uint32_t>(g_camObjects.size());
    std::fwrite(&n, 4, 1, g_camFile);
    for (const TracedObject& o : g_camObjects) {
        const uint16_t len = static_cast<uint16_t>(o.name.size());
        const uint64_t at = reinterpret_cast<uintptr_t>(o.p);
        const uint32_t size = static_cast<uint32_t>(o.size);
        std::fwrite(&len, 2, 1, g_camFile);
        std::fwrite(o.name.data(), 1, len, g_camFile);
        std::fwrite(&at, 8, 1, g_camFile);
        std::fwrite(&size, 4, 1, g_camFile);
        Log("camtrace: %s at %p, %zu bytes", o.name.c_str(), static_cast<const void*>(o.p), o.size);
    }
    g_camFrom = from;
    g_camTo = to;
    Log("camtrace: %u objects, ticks %u..%u", n, from, to);
}

void BattleCamTraceRecord(TraceAt at) {
    if (!g_camFile) return;
    const uint32_t kind = at == TraceAt::Frame ? kTraceFrame : g_inDriver && g_replayingTick ? kTraceReplayedTick
                                                                                             : kTraceTick;
    const uint32_t tick = FlowDrivenTicks();
    if (tick > g_camTo + 2) {
        std::fclose(g_camFile);
        g_camFile = nullptr;
        Log("camtrace: done");
        return;
    }
    if (tick < g_camFrom) return;
    const uint32_t head[2] = {tick, kind};
    std::fwrite(head, 4, 2, g_camFile);
    for (const TracedObject& o : g_camObjects) {
        if (IsReadable(o.p, o.size)) std::fwrite(o.p, 1, o.size, g_camFile);
        else {
            const std::vector<uint8_t> zero(o.size);
            std::fwrite(zero.data(), 1, o.size, g_camFile);
        }
    }
}

} // namespace sfv::mod
