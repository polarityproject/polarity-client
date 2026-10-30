#include "mod/battle_driver.h"

#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/stack.h"

#include <MinHook.h>

#include <array>
#include <atomic>
#include <iterator>
#include <utility>
#include <set>
#include <string>
#include <intrin.h>
#include <cstdio>
#include <cstring>

namespace sfv::mod {

namespace {

using RecordFrameFn = uint64_t (*)(uint8_t* inputUnit, void* a2, void* a3, void* a4);

SfvGame* g_game = nullptr;
uintptr_t g_imageBase = 0;
SfvGame::BattleFrameFn g_origBattleFrame = nullptr;
RecordFrameFn g_origRecordFrame = nullptr;
std::atomic<IFrameDriver*> g_driver{nullptr};
bool g_inDriver = false;
// The game thread while it may be in a driver's OnFrame (set before it reads
// g_driver), else 0: DetachFrameDriver waits for it.
std::atomic<DWORD> g_frameThread{0};

// Rose's soul level (her entity's +0x444, a 2..5 she changes during the
// intro and later) is picked by rva 0x3AD7E0 from a random stream that is not
// in the snapshot and differs between processes, so the two games picked
// different levels and her effects followed (netplay-testing.md, finding 19).
// While the picker runs, every draw first reseeds that stream from state that
// is in the snapshot: the battle's tick count and her current level. Both
// games, and a resimulation, then draw the same.
constexpr game::CodeSite kRoseSoulPick{
    "Rose soul level pick", 0x3AD7E0,
    {0x40, 0x55, 0x53, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00}};
// The random stream's draw: state = state * 0x19660D + 0x3C6EF35F at +0x8.
constexpr game::CodeSite kRandomStreamDraw{
    "random stream draw", 0x32B2B0,
    {0x69, 0x41, 0x08, 0x0D, 0x66, 0x19, 0x00, 0x05, 0x5F, 0xF3, 0x6E, 0x3C, 0x89, 0x41, 0x08, 0xC1}};
// A second random state, one global word (rva 0x3966540) drawn by inlined
// copies of the same step in eight functions: the range pick at rva 0x391C30
// and seven weighted picks next to it, reached through a table of selector
// classes (one keeps `& 3`). Battle scripts pick move variants with them
// (R. Mika vs Akira, 0x368/0x36A/0x36C/0x36E at the fighter's +0x3E30).
// Nothing seeds the word, so it holds whatever the process drew before:
// peers, and two replays of one match, picked different variants. Seeding it
// once per tick was not enough: a resimulated tick picked differently, as
// other draws earlier in the tick (effects, which rollbacks lose) moved it.
// In driven ticks every pick first sets the word from the match seed, the
// tick, its call site and its count there in the tick.
constexpr uint32_t kGlobalRandomState = 0x3966540;
constexpr uint8_t kPickPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                       0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48};
constexpr game::CodeSite kGlobalPicks[] = {
    {"global random range pick", 0x391C30,
     {0x69, 0x05, 0x06, 0x49, 0x5D, 0x03, 0x0D, 0x66, 0x19, 0x00, 0x2B, 0xCA, 0x44, 0x8B, 0xC2, 0x33}},
    {"global random weighted pick 1", 0x3954F0, {}},
    {"global random weighted pick 2", 0x3956A0, {}},
    {"global random weighted pick 3", 0x395860, {}},
    {"global random pick of 21", 0x395AD0,
     {0x41, 0xC7, 0x40, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x69, 0x0D, 0x5E, 0x0A, 0x5D, 0x03, 0x0D, 0x66}},
    {"global random weighted pick 4", 0x395B20, {}},
    {"global random weighted pick 5", 0x395D00, {}},
    {"global random pick of 4", 0x395EB0,
     {0x69, 0x05, 0x86, 0x06, 0x5D, 0x03, 0x0D, 0x66, 0x19, 0x00, 0x05, 0x5F, 0xF3, 0x6E, 0x3C, 0x89}},
};
constexpr uint32_t kBattleTicks = 0x1B8; // int, in the battle frame's state (battleframe::kStateBegin..)
constexpr uint32_t kRoseSoulLevel = 0x444;
constexpr uint32_t kStreamState = 0x8;

using PickFn = uint64_t (*)(uint8_t* entity, void* a2, void* a3, void* a4);
using DrawFn = uint32_t (*)(uint8_t* stream);
PickFn g_origPick = nullptr;
DrawFn g_origDraw = nullptr;
std::atomic<uint8_t*> g_battleFrame{nullptr};
thread_local bool t_picking = false;
thread_local uint32_t t_pickSeed = 0;

uint64_t PickDetour(uint8_t* entity, void* a2, void* a3, void* a4)
{
    HookGuard guard;
    const uint8_t* frame = g_battleFrame;
    const uint32_t ticks = frame ? *reinterpret_cast<const uint32_t*>(frame + kBattleTicks) : 0;
    t_pickSeed = ticks * 0x9E3779B1u ^ *reinterpret_cast<const uint32_t*>(entity + kRoseSoulLevel);
    t_picking = true;
    const uint64_t result = g_origPick(entity, a2, a3, a4);
    t_picking = false;
    return result;
}

std::atomic<int> g_drawLog{0}; // research: draws left to log (`drawlog <count>`)
std::atomic<uint32_t> g_drawLogFrom{1}, g_drawLogTo{0}; // research: driven ticks whose draws are all logged

bool InDrawLogTicks()
{
    const uint32_t t = DrivenTicks();
    return t >= g_drawLogFrom && t <= g_drawLogTo;
}

// Every fighter draws one number per tick from its own stream (rva 0x37D642
// stores it at the fighter object's +0xC50), and moves with a random outcome
// (Dan's) use it. The streams are seeded per process, so a replay diverged at
// the first such move and a peer that rolled back drew again while
// resimulating and diverged from the other (netplay-testing.md, finding 28).
// In driven ticks every draw first sets its stream from the match seed, the
// tick, the drawing object's slot and the draw's count on that stream in the
// tick: the same on both machines and in any resimulation. Not the draw's
// place in the tick: the objects that draw (fighters and helpers, four with
// R. Mika vs Akira) update in an order that changed between a tick and its
// resimulation, and they swapped numbers. Not the stream's last number
// either: it is noise-masked, so the start state copy leaves it per process.
std::atomic<uint64_t> g_drawSeed{0};
constexpr uint32_t kStreamInOwner = 0xC40;
constexpr uint32_t kOwnerVtable = 0x2C521F8; // rva; the fighters' and helpers' class
constexpr uint32_t kOwnerSlot = 0xF8;        // 0..3: P1, P2, then the helpers

uint32_t Mix(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; // splitmix64's mix
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return static_cast<uint32_t>(x ^ (x >> 31));
}

// Draws or picks made in the current driven tick, per stream or call site
// (SeedTickRandom resets). The keys are addresses: they only tell the
// counters apart, and never reach a seed.
struct TickCount {
    uintptr_t key;
    uint32_t count;
};
TickCount g_tickCounts[64];
int g_tickCountUsed = 0;

uint32_t NextInTick(uintptr_t key)
{
    for (int i = 0; i < g_tickCountUsed; i++) {
        if (g_tickCounts[i].key == key) return g_tickCounts[i].count++;
    }
    if (g_tickCountUsed == static_cast<int>(std::size(g_tickCounts))) return 0xFFFF; // full: shares a count
    g_tickCounts[g_tickCountUsed++] = {key, 1};
    return 0;
}

// The slot of the object that holds the stream, or -1 when the stream is not
// a fighter's or helper's.
int64_t OwnerSlot(const uint8_t* stream)
{
    __try {
        const uint8_t* owner = stream - kStreamInOwner;
        if (*reinterpret_cast<const uintptr_t*>(owner) - g_imageBase != kOwnerVtable) return -1;
        return *reinterpret_cast<const uint32_t*>(owner + kOwnerSlot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

uint32_t TickDrawState(const uint8_t* stream)
{
    // Another stream (none seen so far) falls back to the order of such draws.
    const int64_t slot = OwnerSlot(stream);
    const uint64_t owner = slot >= 0 ? static_cast<uint64_t>(slot) : 0x10000 + NextInTick(0);
    const uint64_t key = owner << 32 | NextInTick(reinterpret_cast<uintptr_t>(stream));
    return Mix(g_drawSeed ^ (static_cast<uint64_t>(DrivenTicks()) << 32) ^ Mix(key));
}

uint32_t* g_globalRandom = nullptr; // kGlobalRandomState, once the picks are hooked

// The integer arguments the picks take (rcx, rdx, r8), passed through.
using GlobalPickFn = uint64_t (*)(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
GlobalPickFn g_origGlobalPicks[std::size(kGlobalPicks)] = {};

void SeedGlobalPick(uintptr_t site)
{
    // The site as an rva: the same in every process.
    const uint32_t pick = NextInTick(site);
    const uint64_t key = static_cast<uint64_t>(site - g_imageBase) << 16 | pick;
    *g_globalRandom = Mix(g_drawSeed ^ (static_cast<uint64_t>(DrivenTicks()) << 32) ^ Mix(key));
    const bool window = InDrawLogTicks();
    if (window || g_drawLog > 0) {
        if (!window) g_drawLog--;
        Log("draw: tick %u, global pick %u at rva 0x%llX, state %08X", DrivenTicks(), pick,
            static_cast<unsigned long long>(site - g_imageBase), *g_globalRandom);
    }
}

template <int I>
uint64_t GlobalPickDetour(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    HookGuard guard;
    if (g_inDriver && g_globalRandom) SeedGlobalPick(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return g_origGlobalPicks[I](a1, a2, a3, a4);
}

template <size_t... I>
std::array<void*, sizeof...(I)> GlobalPickDetours(std::index_sequence<I...>)
{
    return {reinterpret_cast<void*>(&GlobalPickDetour<I>)...};
}

uint32_t DrawDetour(uint8_t* stream)
{
    HookGuard guard;
    if (t_picking) *reinterpret_cast<uint32_t*>(stream + kStreamState) = t_pickSeed;
    else if (g_inDriver) *reinterpret_cast<uint32_t*>(stream + kStreamState) = TickDrawState(stream);
    const bool window = g_inDriver && InDrawLogTicks();
    if ((g_drawLog > 0 && g_inDriver) || window) {
        // The per-tick draw of every fighter (rva 0x37D642) only once per stream.
        static std::set<const void*> perTickSeen;
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
        if (window || caller != 0x37D642 || perTickSeen.insert(stream).second) {
            if (!window) g_drawLog--;
            std::string region = "not in the snapshot";
            for (const StateRegion& r : g_game->Regions()) {
                if (stream + kStreamState >= r.ptr && stream + kStreamState < r.ptr + r.size) {
                    region = r.name + "+0x" + std::to_string(stream + kStreamState - r.ptr);
                }
            }
            Log("draw: tick %u, stream %p (state %s) %08X, from rva 0x%llX", DrivenTicks(), static_cast<void*>(stream),
                region.c_str(), *reinterpret_cast<const uint32_t*>(stream + kStreamState), static_cast<unsigned long long>(caller));
        }
    }
    return g_origDraw(stream);
}

void BattleFrameDetour(uint8_t* battleFrame)
{
    HookGuard guard;
    g_battleFrame = battleFrame;
    g_game->OnBattleFrame(battleFrame);
    // The driver's own ticks come back through here via Advance: run them
    // (their time is the driver's, for the frame-drop log).
    if (g_inDriver) {
        g_origBattleFrame(battleFrame);
        return;
    }
    g_frameThread = GetCurrentThreadId();
    bool handled = false;
    if (IFrameDriver* driver = g_driver) {
        g_inDriver = true;
        {
            hitch::Scope timed(hitch::Part::NetFrame);
            handled = driver->OnFrame(*g_game);
        }
        g_inDriver = false;
    }
    g_frameThread = 0;
    if (handled) return;
    hitch::Scope timed(hitch::Part::BattleFrame);
    g_origBattleFrame(battleFrame);
}

int g_strayRecords = 0;

uint64_t RecordFrameDetour(uint8_t* inputUnit, void* a2, void* a3, void* a4)
{
    HookGuard guard;
    // With a driver in charge, inputs are recorded only by the ticks it runs.
    // A call from anywhere else would record a local, unsynchronised input
    // and advance the InputUnit's frame counter on one machine only.
    if (g_game->exclusiveTicks && g_driver && !g_inDriver) {
        if (g_strayRecords++ < 3) LogStack("InputUnit::RecordFrame outside a driven tick (skipped)", g_imageBase);
        return 0;
    }
    g_game->OnInputUnit(inputUnit);
    return g_origRecordFrame(inputUnit, a2, a3, a4);
}

bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original)
{
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("battle driver: signature mismatch at %s", site.name);
        return false;
    }
    const MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("battle driver: hook %s failed: %s", site.name, MH_StatusToString(st));
        return false;
    }
    return true;
}

} // namespace

void SetMatchDrawSeed(uint64_t seed)
{
    g_drawSeed = seed;
}

void SeedTickRandom()
{
    g_tickCountUsed = 0;
}

void LogGameInputs(int ticks)
{
    if (g_game) g_game->inputLog = ticks;
    Log("inputlog: the game's per-pad values for the next %d ticks, on change", ticks);
}

void LogRandomDrawsInTicks(uint32_t from, uint32_t to)
{
    g_drawLogFrom = from;
    g_drawLogTo = to;
    Log("draw: logging every draw in driven ticks %u..%u", from, to);
}

void LogRandomDraws(int count)
{
    g_drawLog = count;
    Log("draw: logging the next %d draws in driven ticks", count);
}

bool InstallBattleDriver(const GameImage& image, SfvGame* game)
{
    g_game = game;
    SetRegionLookup([game](const void* p) -> std::string {
        for (const StateRegion& r : game->Regions()) {
            if (p >= r.ptr && p < r.ptr + r.size) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s+0x%llx", r.name.c_str(),
                              static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - r.ptr));
                return buf;
            }
        }
        return "not in the snapshot";
    });
    const bool ok = Hook(image, game::kBattleFrame, reinterpret_cast<void*>(&BattleFrameDetour),
                         reinterpret_cast<void**>(&g_origBattleFrame)) &&
                    Hook(image, game::kInputUnitRecord, reinterpret_cast<void*>(&RecordFrameDetour),
                         reinterpret_cast<void**>(&g_origRecordFrame));
    if (!Hook(image, kRoseSoulPick, reinterpret_cast<void*>(&PickDetour), reinterpret_cast<void**>(&g_origPick)) ||
        !Hook(image, kRandomStreamDraw, reinterpret_cast<void*>(&DrawDetour), reinterpret_cast<void**>(&g_origDraw))) {
        Log("battle driver: Rose's soul level and the fighters' random draws stay random per game");
    }
    g_imageBase = image.base;
    const auto detours = GlobalPickDetours(std::make_index_sequence<std::size(kGlobalPicks)>());
    bool picksHooked = true;
    for (size_t i = 0; i < std::size(kGlobalPicks); i++) {
        game::CodeSite site = kGlobalPicks[i];
        if (site.bytes[0] == 0) std::memcpy(site.bytes, kPickPrologue, sizeof(site.bytes));
        picksHooked = Hook(image, site, detours[i], reinterpret_cast<void**>(&g_origGlobalPicks[i])) && picksHooked;
    }
    if (picksHooked) g_globalRandom = image.At<uint32_t*>(kGlobalRandomState);
    else Log("battle driver: move variant picks stay random per game");
    // SfvGame::Advance runs a tick through the original, not through the hook.
    game->SetBattleFrameFunction(g_origBattleFrame);
    Log("battle driver %s", ok ? "installed" : "NOT installed");
    return ok;
}

void SetFrameDriver(IFrameDriver* driver) { g_driver = driver; }

void DetachFrameDriver()
{
    g_driver = nullptr;
    // A frame that read the driver before runs to its end (on the game
    // thread itself there is nothing to wait for).
    const DWORD self = GetCurrentThreadId();
    for (int waited = 0; waited < 5000; waited++) {
        const DWORD busy = g_frameThread;
        if (busy == 0 || busy == self) return;
        Sleep(1);
    }
    Log("battle driver: the game thread is still in a frame driver after 5 s");
}

IFrameDriver* CurrentFrameDriver() { return g_driver; }

std::string RegionOwner(const void* region)
{
    // The nearest word at or before the region that points at a vtable in
    // the game's .rdata (whose first entry points into .text).
    const uintptr_t base = g_imageBase ? g_imageBase : reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr uintptr_t kTextBegin = 0x1000, kTextEnd = 0x2C28000, kRdataEnd = 0x3966000;
    const auto* p = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(region) & ~uintptr_t{7});
    for (size_t back = 0; back <= 0x4000; back += 8) {
        const uint8_t* at = p - back;
        if (!IsReadable(at, 8)) break;
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(at);
        if (v < base + kTextEnd || v >= base + kRdataEnd || !IsReadable(reinterpret_cast<const void*>(v), 8)) continue;
        const uintptr_t first = *reinterpret_cast<const uintptr_t*>(v);
        if (first < base + kTextBegin || first >= base + kTextEnd) continue;
        char b[64];
        std::snprintf(b, sizeof(b), "vtable rva 0x%llx at -0x%zx", static_cast<unsigned long long>(v - base),
                      static_cast<size_t>(static_cast<const uint8_t*>(region) - at));
        return b;
    }
    return "unknown";
}

void LogRegionsOfSize(size_t size)
{
    if (!g_game) return;
    for (const StateRegion& r : g_game->Regions()) {
        if (r.size != size) continue;
        std::string words;
        for (size_t w = 0; w + 8 <= r.size && w < 0x100; w += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t*>(r.ptr + w);
            if (!v) continue;
            char b[40];
            std::snprintf(b, sizeof(b), " +0x%zx=%llx", w, static_cast<unsigned long long>(v));
            words += b;
        }
        Log("regions: %s at %p, owner %s:%s", r.name.c_str(), static_cast<void*>(r.ptr), RegionOwner(r.ptr).c_str(),
            words.c_str());
    }
}

} // namespace sfv::mod
