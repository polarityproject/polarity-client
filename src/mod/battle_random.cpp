// Random state in driven ticks, seeded from the match and the tick.

#include "mod/battle_internal.h"

#include "mod/crash_report.h"

#include <array>
#include <iterator>
#include <set>
#include <utility>
#include <intrin.h>

namespace sfv::mod {

using namespace battle;

namespace {

constexpr game::CodeSite kRoseSoulPick{
    "Rose soul level pick", 0x3AD7E0, {0x40, 0x55, 0x53, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00}};
constexpr game::CodeSite kRandomStreamDraw{
    "random stream draw", 0x32B2B0, {0x69, 0x41, 0x08, 0x0D, 0x66, 0x19, 0x00, 0x05, 0x5F, 0xF3, 0x6E, 0x3C, 0x89, 0x41, 0x08, 0xC1}};
constexpr uint32_t kGlobalRandomState = 0x3966540;
constexpr uint8_t kPickPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                       0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48};
constexpr game::CodeSite kGlobalPicks[] = {
    {"global random range pick", 0x391C30, {0x69, 0x05, 0x06, 0x49, 0x5D, 0x03, 0x0D, 0x66, 0x19, 0x00, 0x2B, 0xCA, 0x44, 0x8B, 0xC2, 0x33}},
    {"global random weighted pick 1", 0x3954F0, {}},
    {"global random weighted pick 2", 0x3956A0, {}},
    {"global random weighted pick 3", 0x395860, {}},
    {"global random pick of 21", 0x395AD0, {0x41, 0xC7, 0x40, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x69, 0x0D, 0x5E, 0x0A, 0x5D, 0x03, 0x0D, 0x66}},
    {"global random weighted pick 4", 0x395B20, {}},
    {"global random weighted pick 5", 0x395D00, {}},
    {"global random pick of 4", 0x395EB0, {0x69, 0x05, 0x86, 0x06, 0x5D, 0x03, 0x0D, 0x66, 0x19, 0x00, 0x05, 0x5F, 0xF3, 0x6E, 0x3C, 0x89}},
};
constexpr uint32_t kBattleTicksOff = 0x1B8;
constexpr uint32_t kRoseSoulLevelOff = 0x444;
constexpr uint32_t kStreamStateOff = 0x8;
constexpr uint32_t kPickSeedMultiplier = 0x9E3779B1u;
constexpr uint32_t kFighterTickDraw = 0x37D642;
constexpr uint64_t kRandSalt = 0x52414E44u;
constexpr uint32_t kSharedCount = 0xFFFF;

using PickFn = uint64_t (*)(uint8_t* entity, void* a2, void* a3, void* a4);
using DrawFn = uint32_t (*)(uint8_t* stream);
PickFn g_origPick = nullptr;
DrawFn g_origDraw = nullptr;

thread_local bool t_picking = false;
thread_local uint32_t t_pickSeed = 0;

uint64_t PickDetour(uint8_t* entity, void* a2, void* a3, void* a4) {
    HookGuard guard;
    const uint8_t* frame = g_battleFrame;
    const uint32_t ticks = frame ? *reinterpret_cast<const uint32_t*>(frame + kBattleTicksOff) : 0;
    t_pickSeed = ticks * kPickSeedMultiplier ^ *reinterpret_cast<const uint32_t*>(entity + kRoseSoulLevelOff);
    t_picking = true;
    const uint64_t result = g_origPick(entity, a2, a3, a4);
    t_picking = false;
    return result;
}

std::atomic<int> g_drawLog{0};
std::atomic<uint32_t> g_drawLogFrom{1}, g_drawLogTo{0};

bool InDrawLogTicks() {
    const uint32_t t = FlowDrivenTicks();
    return t >= g_drawLogFrom && t <= g_drawLogTo;
}

std::atomic<uint64_t> g_drawSeed{0};
constexpr uint32_t kStreamInOwnerOff = 0xC40;
constexpr uint32_t kOwnerVtable = 0x2C521F8;
constexpr uint32_t kOwnerSlotOff = 0xF8;

uint32_t Mix(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return static_cast<uint32_t>(x ^ (x >> 31));
}

constexpr int kTickCountsKept = 64;

struct TickCount {
    uintptr_t key;
    uint32_t count;
};

TickCount g_tickCounts[kTickCountsKept];
int g_tickCountUsed = 0;

uint32_t NextInTick(uintptr_t key) {
    for (int i = 0; i < g_tickCountUsed; i++) {
        if (g_tickCounts[i].key == key) return g_tickCounts[i].count++;
    }
    if (g_tickCountUsed == static_cast<int>(std::size(g_tickCounts))) return kSharedCount;
    g_tickCounts[g_tickCountUsed++] = {key, 1};
    return 0;
}

int64_t OwnerSlot(const uint8_t* stream) {
    CrashGuardEnter();
    __try {
        const uint8_t* owner = stream - kStreamInOwnerOff;
        int64_t slot = -1;
        if (*reinterpret_cast<const uintptr_t*>(owner) - g_imageBase == kOwnerVtable) slot = *reinterpret_cast<const uint32_t*>(owner + kOwnerSlotOff);
        CrashGuardLeave();
        return slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashGuardLeave();
        return -1;
    }
}

uint32_t TickDrawState(const uint8_t* stream, uintptr_t site) {
    const int64_t slot = OwnerSlot(stream);
    const uint64_t who = slot >= 0 ? static_cast<uint64_t>(slot)
                                   : 1ull << 63 | static_cast<uint64_t>(site - g_imageBase) << 24 | NextInTick(site);
    const uint64_t key = static_cast<uint64_t>(Mix(who)) << 32 | NextInTick(reinterpret_cast<uintptr_t>(stream));
    return Mix(g_drawSeed ^ (static_cast<uint64_t>(FlowDrivenTicks()) << 32) ^ Mix(key));
}

uint32_t* g_globalRandom = nullptr;

using GlobalPickFn = uint64_t (*)(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
GlobalPickFn g_origGlobalPicks[std::size(kGlobalPicks)] = {};

void SeedGlobalPick(uintptr_t site) {
    const uint32_t pick = NextInTick(site);
    const uint64_t key = static_cast<uint64_t>(site - g_imageBase) << 16 | pick;
    *g_globalRandom = Mix(g_drawSeed ^ (static_cast<uint64_t>(FlowDrivenTicks()) << 32) ^ Mix(key));
    const bool window = InDrawLogTicks();
    if (window || g_drawLog > 0) {
        if (!window) g_drawLog--;
        Log("draw: tick %u, global pick %u at rva 0x%llX, state %08X", FlowDrivenTicks(), pick,
            static_cast<unsigned long long>(site - g_imageBase), *g_globalRandom);
    }
}

template <int I>
uint64_t GlobalPickDetour(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    HookGuard guard;
    if (g_inDriver && g_globalRandom) SeedGlobalPick(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return g_origGlobalPicks[I](a1, a2, a3, a4);
}

template <size_t... I>
std::array<void*, sizeof...(I)> GlobalPickDetours(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&GlobalPickDetour<I>)...};
}

uint32_t DrawDetour(uint8_t* stream) {
    HookGuard guard;
    if (t_picking) *reinterpret_cast<uint32_t*>(stream + kStreamStateOff) = t_pickSeed;
    else if (g_inDriver) *reinterpret_cast<uint32_t*>(stream + kStreamStateOff) = TickDrawState(stream, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const bool window = g_inDriver && InDrawLogTicks();
    if ((g_drawLog > 0 && g_inDriver) || window) {
        static std::set<const void*> perTickSeen;
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
        if (window || caller != kFighterTickDraw || perTickSeen.insert(stream).second) {
            if (!window) g_drawLog--;
            std::string region = "not in the snapshot";
            for (const StateRegion& r : g_game->Regions()) {
                if (stream + kStreamStateOff >= r.ptr && stream + kStreamStateOff < r.ptr + r.size) {
                    region = r.name + "+0x" + std::to_string(stream + kStreamStateOff - r.ptr);
                }
            }
            Log("draw: tick %u, stream %p (state %s) %08X, from rva 0x%llX", FlowDrivenTicks(), static_cast<void*>(stream),
                region.c_str(), *reinterpret_cast<const uint32_t*>(stream + kStreamStateOff), static_cast<unsigned long long>(caller));
        }
    }
    return g_origDraw(stream);
}

using SrandFn = void(__cdecl*)(unsigned int);
SrandFn g_srand = nullptr;

} // namespace

bool battle::InstallRandomHooks(const GameImage& image) {
    bool ok = true;
    if (!Hook(image, kRoseSoulPick, reinterpret_cast<void*>(&PickDetour), reinterpret_cast<void**>(&g_origPick)) ||
        !Hook(image, kRandomStreamDraw, reinterpret_cast<void*>(&DrawDetour), reinterpret_cast<void**>(&g_origDraw))) {
        Log("battle driver: Rose's soul level and the fighters' random draws stay random per game");
        ok = false;
    }
    const auto detours = GlobalPickDetours(std::make_index_sequence<std::size(kGlobalPicks)>());
    bool picksHooked = true;
    for (size_t i = 0; i < std::size(kGlobalPicks); i++) {
        game::CodeSite site = kGlobalPicks[i];
        if (site.bytes[0] == 0) std::memcpy(site.bytes, kPickPrologue, sizeof(site.bytes));
        picksHooked = Hook(image, site, detours[i], reinterpret_cast<void**>(&g_origGlobalPicks[i])) && picksHooked;
    }
    if (picksHooked) g_globalRandom = image.At<uint32_t*>(kGlobalRandomState);
    else Log("battle driver: move variant picks stay random per game");
    if (HMODULE crt = GetModuleHandleW(L"msvcr120.dll")) g_srand = reinterpret_cast<SrandFn>(GetProcAddress(crt, "srand"));
    if (!g_srand) Log("battle driver: the game's srand not found; its rand() stays per game");
    return ok && picksHooked && g_srand;
}

void BattleSetMatchDrawSeed(uint64_t seed) {
    g_drawSeed = seed;
}

void BattleSeedTickRandom() {
    g_tickCountUsed = 0;
    if (g_srand) g_srand(Mix(g_drawSeed ^ (static_cast<uint64_t>(FlowDrivenTicks()) << 32 | kRandSalt)));
}

void BattleLogRandomDrawsInTicks(uint32_t from, uint32_t to) {
    g_drawLogFrom = from;
    g_drawLogTo = to;
    Log("draw: logging every draw in driven ticks %u..%u", from, to);
}

void BattleLogRandomDraws(int count) {
    g_drawLog = count;
    Log("draw: logging the next %d draws in driven ticks", count);
}

} // namespace sfv::mod
