#include "mod/battle_driver.h"

#include "mod/battle_setup.h"
#include "mod/crash_report.h"
#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/stack.h"

#include <MinHook.h>

#include <array>
#include <atomic>
#include <iterator>
#include <map>
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
bool g_replayingTick = false; // SetReplayingTick: the tick being run is a resimulation
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
    CrashGuardEnter(); // a fault here is expected, not a crash
    __try {
        const uint8_t* owner = stream - kStreamInOwner;
        int64_t slot = -1;
        if (*reinterpret_cast<const uintptr_t*>(owner) - g_imageBase == kOwnerVtable) slot = *reinterpret_cast<const uint32_t*>(owner + kOwnerSlot);
        CrashGuardLeave();
        return slot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CrashGuardLeave();
        return -1;
    }
}

uint32_t TickDrawState(const uint8_t* stream, uintptr_t site)
{
    // Another object's stream (the battle camera's shake, a Rose mirror
    // match) by the draw's call site (an rva) and its count there in the
    // tick: by the order of all such draws, effects that a rollback loses
    // moved the camera's numbers.
    const int64_t slot = OwnerSlot(stream);
    const uint64_t who = slot >= 0 ? static_cast<uint64_t>(slot)
                                   : 1ull << 63 | static_cast<uint64_t>(site - g_imageBase) << 24 | NextInTick(site);
    const uint64_t key = static_cast<uint64_t>(Mix(who)) << 32 | NextInTick(reinterpret_cast<uintptr_t>(stream));
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
    else if (g_inDriver) *reinterpret_cast<uint32_t*>(stream + kStreamState) = TickDrawState(stream, reinterpret_cast<uintptr_t>(_ReturnAddress()));
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

// A fighter's damage gauge (gauge object +0x170 now, +0x174 its maximum,
// +0x178 its level, up to 5; 16.16) grows by an amount per hit taken: at the
// hit (rva 0x36D478) about 1000 / max vitality (+0xC4) * damage (the caller's
// r14) / the battle's round count. That count is the battle data's
// battledata::kSpecRounds (r13 + 0xE8 -> getter 0x2F85B0 -> word +8), outside
// the snapshot; two testers' games had kept 1 from their own settings (20 on
// one machine, 59 on the other, 2026-10-31), so match setups write it now.
// Every add is still logged. A stub saves the caller's r13 and r14 (still the
// caller's on entry) before the detour runs.
constexpr game::CodeSite kGaugeAdd{
    "damage gauge add", 0x346A20,
    {0x44, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC}};
constexpr uintptr_t kGaugeAddFromHit = 0x36D47D; // the return address of the hit's call
constexpr int kGaugeLogsPerBattle = 400;
using GaugeAddFn = int* (*)(uint8_t* gauge, int* out, int amount, int flags);
GaugeAddFn g_origGaugeAdd = nullptr;
uint64_t g_gaugeR13 = 0, g_gaugeR14 = 0; // the caller's, from the stub (game thread only)
int g_gaugeLogs = 0, g_gaugeBattle = -1;

int* GaugeAddDetour(uint8_t* gauge, int* out, int amount, int flags)
{
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
    const int32_t damage = static_cast<int32_t>(g_gaugeR14);
    int32_t before[3] = {}, vitality[2] = {};
    const bool readable = IsReadable(gauge + 0xC0, 0xBC);
    if (readable) {
        std::memcpy(before, gauge + 0x170, sizeof(before));
        std::memcpy(vitality, gauge + 0xC0, sizeof(vitality));
    }
    int* result = g_origGaugeAdd(gauge, out, amount, flags);
    if (BattleCreationCount() != g_gaugeBattle) {
        g_gaugeBattle = BattleCreationCount();
        g_gaugeLogs = 0;
    }
    if (readable && g_gaugeLogs++ < kGaugeLogsPerBattle) {
        int32_t after[3];
        std::memcpy(after, gauge + 0x170, sizeof(after));
        const double unit = 65536.0;
        char cause[96];
        if (from == kGaugeAddFromHit) {
            // The move's divisor, from the rest: 1000 / max vitality * damage / amount.
            const double divisor = amount && vitality[1] ? 1000.0 / (vitality[1] / unit) * (damage / unit) / (amount / unit) : 0;
            std::snprintf(cause, sizeof(cause), "for damage %.2f (divisor %.2f)", damage / unit, divisor);
        }
        else {
            std::snprintf(cause, sizeof(cause), "from rva 0x%llX", static_cast<unsigned long long>(from));
        }
        Log("gauge: tick %u%s, vitality %.0f/%.0f, add %.3f (flags %d) %s, now %.2f/%.0f level %d (was %.2f level %d)",
            DrivenTicks(), g_replayingTick ? " replayed" : "", vitality[0] / unit, vitality[1] / unit, amount / unit, flags,
            cause, after[0] / unit, after[1] / unit, after[2], before[0] / unit, before[2]);
    }
    return result;
}

// mov rax, &g_gaugeR14; mov [rax], r14; mov rax, &g_gaugeR13; mov [rax], r13;
// mov rax, GaugeAddDetour; jmp rax (rax is free on a function's entry).
void* MakeGaugeStub()
{
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) return nullptr;
    uint8_t* p = code;
    const auto movRax = [&p](const void* value) {
        *p++ = 0x48;
        *p++ = 0xB8;
        const uint64_t v = reinterpret_cast<uint64_t>(value);
        std::memcpy(p, &v, 8);
        p += 8;
    };
    movRax(&g_gaugeR14);
    *p++ = 0x4C, *p++ = 0x89, *p++ = 0x30; // mov [rax], r14
    movRax(&g_gaugeR13);
    *p++ = 0x4C, *p++ = 0x89, *p++ = 0x28; // mov [rax], r13
    movRax(reinterpret_cast<const void*>(&GaugeAddDetour));
    *p++ = 0xFF, *p++ = 0xE0; // jmp rax
    FlushInstructionCache(GetCurrentProcess(), code, p - code);
    return code;
}

// Research (`bonelog <count> [<caller rva>]`): a fighter's bone world
// position (rva 0x3919D0), from its skeletal mesh (fighter +0xA90) through rva
// 0xE58F10, which battle code asks for (Vega's knocked-off claw and mask
// launch from his hand or head, rva 0x3A4B74). The mesh is posed once per
// rendered frame, outside the snapshot: a resimulated tick reads the newest
// frame's pose. Logs the next `count` calls in driven ticks (from one caller,
// by the rva its call returns to, when given).
constexpr game::CodeSite kBonePosition{
    "fighter bone position", 0x3919D0,
    {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0x05, 0x9F, 0x08, 0x63}};
using BonePositionFn = void (*)(uint8_t* fighter, const void* bone, float* out, int flag);
BonePositionFn g_origBonePosition = nullptr;
std::atomic<int> g_boneLog{0};
std::atomic<uintptr_t> g_boneLogFrom{0};

void BonePositionDetour(uint8_t* fighter, const void* bone, float* out, int flag)
{
    g_origBonePosition(fighter, bone, out, flag);
    if (g_boneLog <= 0 || !g_inDriver) return;
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
    if (g_boneLogFrom && from != g_boneLogFrom) return;
    g_boneLog--;
    uint32_t bits[3];
    std::memcpy(bits, out, sizeof(bits));
    Log("bone: tick %u%s, from rva 0x%llX, fighter %p bone %p: %.6f %.6f %.6f (%08X %08X %08X)", DrivenTicks(),
        g_replayingTick ? " replayed" : "", static_cast<unsigned long long>(from), static_cast<void*>(fighter), bone,
        out[0], out[1], out[2], bits[0], bits[1], bits[2]);
}

// Research (`soundlog <count>`): the CriWare plugin's "play a cue" request
// (rva 0xEE7F10: a player, the cue sheet, the cue id and one more int). The
// game thread queues it (a type-0 record pushed by rva 0xEE8BE0) and the
// plugin's sound thread plays it later (rva 0xEE8830 -> 0xEE8F40), so this is
// where a resimulated tick asks for a sound again. Logs the next `count`
// requests: the driven tick and whether it was a resimulation, the thread,
// and the caller; the first ones outside driven ticks with their stacks.
constexpr game::CodeSite kCuePlay{
    "CriWare cue play request", 0xEE7F10,
    {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8B}};
using CuePlayFn = uint64_t (*)(void* player, int sheet, int cue, int a, int b);
CuePlayFn g_origCuePlay = nullptr;
std::atomic<int> g_soundLog{0};
std::atomic<int> g_soundStacks{0};

// The plugin's stop request for one sound (rva 0xEE8710: a player, the
// handle its play request returned, then 1 to stop without release time):
// queues a type-2 command; its handler (rva 0xEE9080) finds the handle's
// playback in the player's table and stops just that one (stable in the
// 2026-11-08 sound tests, docs/sound-dedupe-stop.patch).
constexpr game::CodeSite kCueStop{
    "CriWare playback stop request", 0xEE8710,
    {0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0x39, 0x00, 0x48, 0x8B, 0xF9, 0x0F, 0x84, 0xEA}};
using CueStopFn = void (*)(void* player, int handle, bool withoutRelease);
CueStopFn g_cueStop = nullptr;
// The sound capture (BeginSoundCapture): the sounds asked for meanwhile.
std::mutex g_captureMutex;
std::atomic<bool> g_capturing{false}; // read unlocked: battle job threads ask for sounds too
std::vector<std::pair<void*, int>> g_captured; // player, handle
thread_local bool t_captureExempt = false;
constexpr int kAmbienceSheet = 58;
// A viewed replay's battle (RecordBattleSounds): every sound it asked for,
// the newest kBattleSoundsKept, for stopping all of it when the battle is
// started again (its loops would play on: a jump back's leftover sound).
constexpr size_t kBattleSoundsKept = 16384;
std::atomic<bool> g_recordBattle{false};
std::vector<std::pair<void*, int>> g_battleSounds; // player, handle (under g_captureMutex)

uint64_t CuePlayDetour(void* player, int sheet, int cue, int a, int b)
{
    // Sheet 58 is the stage's ambience (a loop started at the battle's start
    // and again at each round's set-up): a landing keeps it, as it is meant
    // to go on (stopped, the paused bed went from rms 0.0085 to 0.0018).
    const bool capture = g_capturing && !t_captureExempt && sheet != kAmbienceSheet;
    const bool keepForBattle = g_recordBattle && !t_captureExempt;
    if (g_soundLog > 0) {
        g_soundLog--;
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
        Log("sound: cue %d (sheet %d, %d %d) %s tick %u%s%s, thread %lu, from rva 0x%llX", cue, sheet, a, b,
            g_inDriver ? "in driven" : "outside driven", DrivenTicks(), g_inDriver && g_replayingTick ? " replayed" : "",
            capture ? " (captured)" : "", GetCurrentThreadId(), static_cast<unsigned long long>(from));
        if (g_soundStacks > 0) {
            g_soundStacks--;
            LogStack("sound: cue play", g_imageBase);
        }
    }
    const uint64_t handle = g_origCuePlay(player, sheet, cue, a, b);
    if (const int id = static_cast<int>(handle); id != -1 && (capture || keepForBattle)) { // -1: nothing started
        std::lock_guard lock(g_captureMutex);
        if (capture && g_capturing && g_captured.size() < 4096) g_captured.push_back({player, id});
        if (keepForBattle && g_recordBattle) {
            if (g_battleSounds.size() >= kBattleSoundsKept) g_battleSounds.erase(g_battleSounds.begin(), g_battleSounds.begin() + kBattleSoundsKept / 2);
            g_battleSounds.push_back({player, id});
        }
    }
    return handle;
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

// The engine's C runtime rand() (MSVCR120.dll, state per thread) is called
// in battle ticks (rva 0xEB2D64, 0xEE7F6E, 0x1E288CA, 0x1E48A27); its state
// is outside the snapshot, so a resimulated tick would draw from wherever it
// had got to. Each driven tick starts the game thread's state from the match
// seed and the tick.
using SrandFn = void(__cdecl*)(unsigned int);
SrandFn g_srand = nullptr;

// Research (`phasecost 1`): the time each part of BattleFrame::Update takes
// in driven ticks, normal and replayed apart: the update's start (rva
// 0x4DC2D0), scheduler phases 1, 2, 4 and 8 (rva 0x4CD890) and the tick
// proper (rva 0x4D2850). Logged every 600 driven ticks.
constexpr game::CodeSite kUpdateStart{"battle update start", 0x4DC2D0,
    {0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xF1, 0x48, 0x8B, 0x49, 0x18, 0x48}};
constexpr game::CodeSite kSchedulerPhase{"scheduler phase", 0x4CD890,
    {0x4C, 0x8B, 0xDC, 0x53, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05, 0xDE, 0x49}};
constexpr game::CodeSite kTickProper{"battle tick", 0x4D2850,
    {0x83, 0xB9, 0x90, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC1, 0x74, 0x0C, 0x83, 0xB9, 0x94, 0x01}};
using UpdateStartFn = uint64_t (*)(void*);
using PhaseFn = uint64_t (*)(void* scheduler, int phase);
using TickProperFn = uint64_t (*)(void* battleFrame);
UpdateStartFn g_origUpdateStart = nullptr;
PhaseFn g_origPhase = nullptr;
TickProperFn g_origTickProper = nullptr;
std::atomic<bool> g_phaseTiming{false};
// Experiment (`resimskip 1`): replayed ticks leave out scheduler phases 4 and
// 8 (presentation: animation, effects, camera), as the game's own update
// does for all but its last tick when it runs several; the newest tick of
// the frame runs them.
std::atomic<bool> g_skipResimPresentation{false};
enum PhasePart { kPartStart, kPart1, kPart2, kPart4, kPart8, kPartTick, kPartCount };
const char* const kPartNames[kPartCount] = {"start", "phase 1", "phase 2", "phase 4", "phase 8", "tick"};
uint64_t g_partTicks[2][kPartCount] = {}; // [replayed][part], performance counter ticks
uint64_t g_partCalls[2] = {};             // driven ticks counted, [replayed]
uint64_t g_profiledTicks = 0;             // driven ticks since timing started (task profile)

template <typename F> uint64_t Timed(PhasePart part, F call)
{
    if (!g_phaseTiming || !g_inDriver) return call();
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const uint64_t r = call();
    QueryPerformanceCounter(&b);
    g_partTicks[g_replayingTick ? 1 : 0][part] += b.QuadPart - a.QuadPart;
    return r;
}

uint64_t UpdateStartDetour(void* frame) { return Timed(kPartStart, [&] { return g_origUpdateStart(frame); }); }
uint64_t PhaseDetour(void* scheduler, int phase)
{
    const PhasePart part = phase == 1 ? kPart1 : phase == 2 ? kPart2 : phase == 4 ? kPart4 : kPart8;
    if (phase != 1 && phase != 2 && phase != 4 && phase != 8) return g_origPhase(scheduler, phase);
    if (g_skipResimPresentation && g_inDriver && g_replayingTick && (phase == 4 || phase == 8)) return 0;
    if (g_inDriver) SetSchedulerPhase(phase, g_replayingTick);
    const uint64_t r = Timed(part, [&] { return g_origPhase(scheduler, phase); });
    SetSchedulerPhase(0);
    return r;
}
uint64_t TickProperDetour(void* frame) { return Timed(kPartTick, [&] { return g_origTickProper(frame); }); }

void CountPhaseTick()
{
    if (!g_phaseTiming) return;
    g_partCalls[g_replayingTick ? 1 : 0]++;
    g_profiledTicks++;
    if (g_partCalls[0] + g_partCalls[1] < 600) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (int rep = 0; rep < 2; rep++) {
        if (!g_partCalls[rep]) continue;
        std::string text;
        uint64_t sum = 0;
        for (int p = 0; p < kPartCount; p++) {
            sum += g_partTicks[rep][p];
            char b[48];
            std::snprintf(b, sizeof(b), " %s %.0f", kPartNames[p], g_partTicks[rep][p] * 1e6 / f.QuadPart / g_partCalls[rep]);
            text += b;
        }
        Log("phasecost: %s ticks (%llu), us per tick:%s; sum %.0f", rep ? "replayed" : "normal",
            static_cast<unsigned long long>(g_partCalls[rep]), text.c_str(), sum * 1e6 / f.QuadPart / g_partCalls[rep]);
    }
    std::memset(g_partTicks, 0, sizeof(g_partTicks));
    std::memset(g_partCalls, 0, sizeof(g_partCalls));
}

void SeedTickRandom()
{
    g_tickCountUsed = 0;
    if (g_srand) g_srand(Mix(g_drawSeed ^ (static_cast<uint64_t>(DrivenTicks()) << 32 | 0x52414E44u)));
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

void LogStartProbe()
{
    if (g_game) g_game->LogStartProbe("asked");
}

void LogBonePositions(int count, uintptr_t from)
{
    g_boneLog = count;
    g_boneLogFrom = from;
    Log("bone: logging the next %d bone positions asked for in driven ticks (from rva 0x%llX)", count,
        static_cast<unsigned long long>(from));
}

void BeginSoundCapture()
{
    std::lock_guard lock(g_captureMutex);
    g_captured.clear();
    g_capturing = g_cueStop != nullptr;
}

int StopCapturedSounds()
{
    std::vector<std::pair<void*, int>> captured;
    {
        std::lock_guard lock(g_captureMutex);
        g_capturing = false;
        captured.swap(g_captured);
    }
    // A handle whose sound has ended finds nothing in its player's table.
    for (const auto& [player, handle] : captured) g_cueStop(player, handle, true);
    return static_cast<int>(captured.size());
}

void SetSoundCaptureExempt(bool exempt) { t_captureExempt = exempt; }

void RecordBattleSounds(bool on)
{
    std::lock_guard lock(g_captureMutex);
    g_recordBattle = on;
    g_battleSounds.clear();
}

int StopBattleSounds()
{
    std::vector<std::pair<void*, int>> sounds, captured;
    {
        std::lock_guard lock(g_captureMutex);
        sounds.swap(g_battleSounds);
        // The capture's are the battle's too: stopped now, while their
        // players exist, not after the battle is gone.
        captured.swap(g_captured);
    }
    if (g_cueStop) {
        for (const auto& [player, handle] : sounds) g_cueStop(player, handle, true);
        for (const auto& [player, handle] : captured) g_cueStop(player, handle, true);
    }
    return static_cast<int>(sounds.size());
}

size_t SoundCaptureMark()
{
    std::lock_guard lock(g_captureMutex);
    return g_captured.size();
}

void KeepCapturedFrom(size_t mark)
{
    std::lock_guard lock(g_captureMutex);
    if (mark < g_captured.size()) g_captured.resize(mark);
}

void LogSoundPlays(int count)
{
    g_soundLog = count;
    g_soundStacks = 6;
    Log("sound: logging the next %d cue plays (the first 6 with stacks)", count);
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
    if (HMODULE crt = GetModuleHandleW(L"msvcr120.dll")) g_srand = reinterpret_cast<SrandFn>(GetProcAddress(crt, "srand"));
    if (!picksHooked) Log("battle driver: move variant picks stay random per game");
    if (!g_srand) Log("battle driver: the game's srand not found; its rand() stays per game");
    if (!Hook(image, kUpdateStart, reinterpret_cast<void*>(&UpdateStartDetour), reinterpret_cast<void**>(&g_origUpdateStart)) ||
        !Hook(image, kSchedulerPhase, reinterpret_cast<void*>(&PhaseDetour), reinterpret_cast<void**>(&g_origPhase)) ||
        !Hook(image, kTickProper, reinterpret_cast<void*>(&TickProperDetour), reinterpret_cast<void**>(&g_origTickProper))) {
        Log("battle driver: phasecost unavailable");
    }
    if (!Hook(image, kBonePosition, reinterpret_cast<void*>(&BonePositionDetour), reinterpret_cast<void**>(&g_origBonePosition))) {
        Log("battle driver: bonelog unavailable");
    }
    if (!Hook(image, kCuePlay, reinterpret_cast<void*>(&CuePlayDetour), reinterpret_cast<void**>(&g_origCuePlay))) {
        Log("battle driver: soundlog unavailable");
    }
    else if (void* stop = image.At<void*>(kCueStop.rva); std::memcmp(stop, kCueStop.bytes, sizeof(kCueStop.bytes)) == 0) {
        g_cueStop = reinterpret_cast<CueStopFn>(stop);
    }
    else {
        Log("battle driver: signature mismatch at %s: sounds from muted runs may be heard after", kCueStop.name);
    }
    void* gaugeStub = MakeGaugeStub();
    if (!gaugeStub || !Hook(image, kGaugeAdd, gaugeStub, reinterpret_cast<void**>(&g_origGaugeAdd))) {
        Log("battle driver: damage gauge adds not logged");
    }
    // SfvGame::Advance runs a tick through the original, not through the hook.
    game->SetBattleFrameFunction(g_origBattleFrame);
    Log("battle driver %s", ok ? "installed" : "NOT installed");
    return ok;
}

void SetFrameDriver(IFrameDriver* driver) { g_driver = driver; }

void SetReplayingTick(bool replaying)
{
    g_replayingTick = replaying;
    CountPhaseTick();
}

void SetResimPresentationSkip(bool on)
{
    g_skipResimPresentation = on;
    Log("resimskip: replayed ticks %s scheduler phases 4 and 8", on ? "leave out" : "run");
}

void SetPhaseTiming(bool on)
{
    g_phaseTiming = on;
    SetTaskProfiling(on, g_profiledTicks);
    g_profiledTicks = 0;
    Log("phasecost: %s", on ? "timing the battle update's parts" : "off");
}

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
    if (size == 0) {
        // Every region by its owner (vtable and offset): which sizes, how many.
        std::map<std::string, std::map<size_t, int>> byOwner;
        for (const StateRegion& r : g_game->Regions()) byOwner[RegionOwner(r.ptr)][r.size]++;
        for (const auto& [owner, sizes] : byOwner) {
            std::string text;
            for (const auto& [s, n] : sizes) text += " " + std::to_string(s) + (n > 1 ? "x" + std::to_string(n) : "");
            Log("regions: owner %s:%s", owner.c_str(), text.c_str());
        }
        return;
    }
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
