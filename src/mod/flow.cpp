#include "mod/flow.h"

#include "core/random_stage.h"

#include "mod/benchmark.h"
#include "mod/battle_setup.h"
#include "mod/boot.h"
#include "mod/control.h"
#include "mod/crash_report.h"
#include "mod/entitlements.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/stack.h"
#include "mod/ue4.h"
#include "mod/variant_table.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace sfv::mod {

uint8_t* BattleContext(uint8_t* task);

namespace {

constexpr game::CodeSite kChangeSceneState{
    "SceneManager::ChangeSceneState", 0x584B70,
    {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x99, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89}};
constexpr game::CodeSite kLaunchNextGameScene{
    "LaunchNextGameScene", 0x56C100,
    {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x33, 0xC0, 0x48, 0x8B, 0xF9, 0x48}};

constexpr game::CodeSite kSetupStep{
    "BattleSetupLoader::Step", 0x714BD0,
    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}};
constexpr game::CodeSite kSetupStep0Call{
    "BattleSetupLoader::Step0Call", 0x69ADA0,
    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48}};

constexpr uint32_t kEngineGlobal = 0x3D878C8; // engine; +0x1E0 KiwiGameSingleton
constexpr uint32_t kSingleton = 0x1E0;
// The Versus battle loader (C++ object): singleton +0xB8 -> +0x10. Its
// per-frame state machine (rva 0x724970) runs phases 1..8: 2 loads the stage
// (step 0), 6 sets up the players (step 2, CheckPlayers), 8 launches the
// battle. Phase 0 is idle; character select starts it at 1.
constexpr uint32_t kSingletonBattleFlow = 0xB8;
constexpr uint32_t kBattleFlowLoader = 0x10;
constexpr uint32_t kLoaderPhase = 0x8;
constexpr int kLoaderStartDelay = 30; // frames
constexpr uint32_t kLoaderStepDone = 0x710E40; // bool (loader, step): step done and its loading finished
constexpr int kStageWaitFrames = 60 * 30;
// From outside Versus: frames to wait for the battle setup state to create
// the battle loader and load character select.
constexpr int kSetupWaitFrames = 60 * 30;
constexpr int kSetupSettleFrames = 30;
// The battle flow (singleton +0xB8): a registry of step objects (TArray of
// shared pointers at +0x0, registered by rva 0x71A790 from every mode's
// setup), then the battle loader (+0x10, shared pointer). EnsureLoader(flow,
// type) keeps the loader if it has that type, else creates one bound to the
// registered step of that type; Versus setup calls it with 0xE.
constexpr uint32_t kEnsureLoader = 0x4AA510;
constexpr int kVersusLoaderType = 0xE;
// Frames after a scene change before a jump switches the scene again.
constexpr int kSceneSettleFrames = 60;
// Frames to wait for a battle loader to start after the switch.
constexpr int kLoaderWaitFrames = 60 * 20;
constexpr uint32_t kSingletonStageTable = 0x68; // KWStageEntryDataAsset; entries at +0x28 (TArray)
constexpr size_t kStageEntrySize = 0x50;         // five FStrings: code, then level/asset paths

// The battle's task call thunk: closure { fn, int32 this-adjust, object }.
constexpr game::CodeSite kTaskThunk{
    "BattleTask::CallThunk", 0x4C1D10,
    {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xC1, 0x48, 0x63, 0x49, 0x08, 0x48, 0x03, 0x48, 0x10, 0xFF}};

// A battle node's event handler (the round counters' node): sees the round
// flow events (code at event+0xC; 1 starts the fight).
constexpr game::CodeSite kRoundEvent{
    "BattleNode::OnEvent", 0x4818C0,
    {0x40, 0x55, 0x53, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC}};

// The scene manager's actor tick (this, delta seconds): once per frame in
// every scene, also while loading. Runs the game-thread queue.
constexpr game::CodeSite kSceneTick{
    "GameScene::Tick", 0x5C9A90,
    {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}};

// Battle flow reset (flow): drops the loader and registers the step objects
// again. Leaving the main menu calls it (its exit, a few frames after the
// switch), so a loader made before that is gone.
// The character cache (getter rva 0x68CEA0): slots that keep a character's
// assets loaded, least recently used first (in use at +0x1D0, free at
// +0x1E0). Request(cache, code, costume, color, x, y) moves a matching slot
// (code, costume, color, y) to the end, or takes a free one or the oldest,
// and starts loading that character's asset paths (built by rva 0x68D2B0)
// there. Battle creation (rva 0x6A45E0) requests every player's character.
constexpr game::CodeSite kCharaCacheRequest{
    "CharaCache::Request", 0x6A4880,
    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}};
constexpr uint32_t kCharaCache = 0x68CEA0;
constexpr uint32_t kCacheSlotsInUse = 0x1D0; // TArray<Slot*>
constexpr uint32_t kCacheSlotsFree = 0x1E0;  // TArray<Slot*>
// Slot: +0x0 its load queue (+0x8: 0 when idle), +0x8 code (FString),
// +0x18 costume, +0x1C character version, +0x20 y.
// The game singleton (rva 0x521690) +0x60 holds the character versions;
// Resolve(versions, code, &version) keeps a version that character has and
// makes anything else (negative: on purpose) its latest.
constexpr uint32_t kKiwiSingleton = 0x521690;
constexpr uint32_t kSingletonVersions = 0x60;
constexpr uint32_t kResolveCharaVersion = 0x56B360;
constexpr uint32_t kCharaVersionInfo = 0x57F030;
// The asset manager (0x220 bytes, global at rva 0x3A91FA8, -1 when shut
// down): +0x108 its load queue. The queue holds a set of asset paths
// (+0x10; +0xC0 while a load runs) and a state (+0x8, 0 when idle).
// AddChara(queue, code, costume, version, x, y) adds a character's paths;
// Start(queue, type) loads the set (battle setup step 2 passes 3).
constexpr uint32_t kAssetManager = 0x3A91FA8;
constexpr uint32_t kAssetManagerQueue = 0x108;
constexpr uint32_t kQueueState = 0x8;
constexpr uint32_t kQueueAddChara = 0x6996B0;
constexpr uint32_t kQueueStart = 0x69C500;
constexpr int kQueueLoadBattle = 3;

constexpr game::CodeSite kFlowReset{
    "BattleFlow::Reset", 0x71A790,
    {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55}};

using ChangeSceneStateFn = void (*)(void* self, const void* stateClass, void* param);
using LaunchFn = uint64_t (*)(void* self, void* a2, void* a3);
ChangeSceneStateFn g_origChange = nullptr;
using FlowResetFn = void (*)(void* flow);
FlowResetFn g_origFlowReset = nullptr;
// A jump from outside Versus switched to the battle: the next flow reset
// (the old scene's exit) must leave a Versus loader behind.
std::atomic<bool> g_loaderAfterReset{false};
using SceneTickFn = void (*)(void* self, float deltaSeconds);
SceneTickFn g_origSceneTick = nullptr;
std::atomic<uint64_t> g_sceneTicks{0};
std::atomic<uint64_t> g_pumps{0};
std::atomic<uint64_t> g_lastPumpUs{0};
using StepFn = uint64_t (*)(void* self, int step, uint8_t force);
StepFn g_origStep = nullptr;
using Step0Fn = uint64_t (*)(void* self, void* data, uint8_t a, uint8_t b);
Step0Fn g_origStep0 = nullptr;
LaunchFn g_origLaunch = nullptr;
uintptr_t g_base = 0;
using TaskThunkFn = uint64_t (*)(void* closure, void* a2, void* a3, void* a4);
TaskThunkFn g_origThunk = nullptr;
using EventFn = uint64_t (*)(void* node, void* event, void* a3, void* a4);
EventFn g_origEvent = nullptr;
// The flow events' type (vtable rva); the same handler also gets the timer's
// (0x2C70E48) and others whose codes overlap.
constexpr uintptr_t kFlowEventType = 0x2C5F8F8;
// Flow event codes: 1 battle start, 3 and 2 the intro, 4 and 5 the round
// call, 6 "FIGHT!", 7 the round's end (KO or time), 8 and 9 after it, 12 the
// match's end. Other codes leave the phase alone.
constexpr int32_t kFightStarts = 6;
constexpr int32_t kRoundCall = 5;
constexpr int32_t kRoundEnd = 7; // after the KO's stop (or time over)
std::atomic<uint32_t> g_roundEnds{0};
// Between the round call (5) and "FIGHT!" (6). The presentation runs in
// lockstep (never rolled back), so a plain flag follows it.
std::atomic<bool> g_roundCall{false};
// The phase machine's index of the round call's phase (learned at its flow
// event; -1 until then): RoundCallRunning reads the machine, which loads put
// back, so a rollback across the call leaves it right.
std::atomic<int> g_roundCallPhase{-1};
// The match is over (after its last KO): the win screens that follow take
// their input from the pads, not from the battle. 13: the results screen.
constexpr int32_t kResults = 13;
bool MatchOver(int32_t code) { return code == 11 || code == 12; }

bool EndsFight(int32_t code)
{
    return (code >= 1 && code <= 5) || (code >= 7 && code <= 12);
}
struct RoundFlow {
    int32_t inFight;
    int32_t lastCode;
    uint32_t startPressed; // presentation: who pressed Start (bit per player)
    int32_t startTicks;    // presentation: ticks Start still goes to the game
    // The round director as of this frame (RunRoundDirector), once seen.
    int32_t directorSeen;
    int32_t director[14];       // +0xF8..+0x130: state, then the round's stop record
    int32_t directorTimers[16]; // +0x110C4..+0x11104: four timers its check advances
    int32_t directorCounts[5];  // +0x11128..+0x1113C: counts the round's end adds to
    int32_t stopTicks;          // ticks the director has been stopped for a KO (or time over)
    uint32_t ticks;             // driven ticks run: keys the round's end copies (BeginDrivenTick)
    int32_t unused[2];
};
static_assert(sizeof(RoundFlow) == kRoundFlowStateSize);
alignas(16) RoundFlow g_roundFlow{};
// The round director: a battle task object whose state (+0xF8) says whether
// the battle is stopped (KO and other stops). 0x485240, run outside the
// battle tick, moves it to 2 and sets the battle frame's stop bit (+0x190 bit
// 0x20, through 0x4D5DE0); 0x487CD0, run by the tick, clears the bit and moves
// it to 1, or to 3 after a stop that ends the round (+0x108 = 7). The director
// is not in the snapshot: a rollback that brought back the stop bit after the
// director had moved on froze the battle for good.
constexpr uintptr_t kRoundDirectorTask = 0x487CD0;
constexpr uintptr_t kRoundDirectorVtable = 0x2C70E30; // at +0xE8, then a pointer to the director
constexpr uint32_t kDirectorState = 0xF8;
constexpr int32_t kDirectorStopped = 2;
constexpr int32_t kDirectorRoundOver = 3;
constexpr uint32_t kStopFlags = 0x190; // battle frame: two flag words
constexpr uint32_t kStopBit = 0x20;
std::atomic<uint8_t*> g_roundDirector{nullptr};
// The battle's phase machine (a task in the tick, rva 0x3DD890): phases as
// shared pointers in a TArray at +0xF8 (16 bytes each), the current phase
// at +0x120 / +0x128 (object, reference block), its index at +0x130, the
// requested one at +0x134; a switch posts the flow event (code: the new
// phase's id). Research (`phases`): the round's end switches it, and it is
// not in the snapshot.
constexpr uintptr_t kPhaseMachineTask = 0x3DD890;
std::atomic<uint8_t*> g_phaseMachine{nullptr};
// The director's check (a battle task run once per rendered frame, outside
// the tick). Only in state 1 with no stop recorded (+0x108 = 0): it advances
// four timers (rva 0x4869F0), then looks at the fighters for a KO (or time
// over), records the stop (+0xFC..+0x130; +0x108 its kind) and moves to 2
// with the stop bit set. In state 2 it only sets the bit again if it is off.
// Every call copies +0x11128.. out to the battle.
constexpr game::CodeSite kRoundDirectorCheck{
    "RoundDirector::Check", 0x485240,
    {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}};
constexpr uint32_t kDirectorRecord = 0xF8;
constexpr uint32_t kDirectorTimers = 0x110C4;
constexpr uint32_t kDirectorCounts = 0x11128;
constexpr uint32_t kDirectorStopKind = 0x108; // 0: a plain stop, ended by the next tick; else a KO or time over
// The round's end (director state 3) switches the battle's phase machine
// (rva 0x3DD890: phase objects, flow events) to the round end, and that is
// not in the snapshot: a load from after it to before the KO left the game in
// its round end with the flow state in the fight, and the games went apart
// (85e27684). The game ends the round a tick or two after the KO; netplay
// holds it until the KO is older than any rollback can reach (the prediction
// window, 8), so only confirmed inputs ever lead to it.
constexpr int32_t kRoundOverAfterTicks = 10;
using DirectorCheckFn = void (*)(uint8_t* director);
DirectorCheckFn g_origDirectorCheck = nullptr;
// The director's handler for one class of the fighters' events (this =
// director +0x48). It tallies three kinds at +0x11130..+0x11138 (by bits 7, 6
// and 5 of the event's +0x3C; the battle copies them out each tick and adds
// them to the profile's fight stats at the round's end) and sets bits at
// +0x110C0, with plain read-modify-writes. The battle runs both fighters' jobs
// that post them on its
// worker threads as well as the game thread (job loop rva 0x4D8F2E), so a
// trade could lose one side's count: 8 on one PC and 9 on the other, and the
// same recording counted differently from one replay to the next (af30d806,
// mirror match with identical inputs). One handler at a time: the counts are
// the same everywhere.
constexpr game::CodeSite kRoundHitTally{
    "RoundDirector::OnHitEvent", 0x482010,
    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55}};
using HitTallyFn = uint64_t (*)(uint8_t*, void*, void*, void*);
HitTallyFn g_origHitTally = nullptr;
std::recursive_mutex g_hitTallyMutex;
std::atomic<bool> g_directorPerTick{false};
thread_local bool t_directorFromTick = false;
// SetHoldResults: the results screen is not to open / it was kept from opening.
std::atomic<bool> g_holdResults{false};
std::atomic<bool> g_resultsHeld{false};

// The director as last seen by its task, if it still looks like one (the
// pointer outlives its battle until the next director task runs).
uint8_t* LiveRoundDirector()
{
    uint8_t* d = g_roundDirector;
    if (!d || !IsReadable(d + 0xE8, 0x18)) return nullptr;
    if (*reinterpret_cast<const uintptr_t*>(d + 0xE8) != g_base + kRoundDirectorVtable) return nullptr;
    if (*reinterpret_cast<uint8_t* const*>(d + 0xF0) != d) return nullptr;
    return d;
}
std::atomic<int> g_taskCensus{0}; // calls still to record
std::mutex g_censusMutex;
std::vector<std::pair<uintptr_t, uintptr_t>> g_census; // (fn, object)
std::mutex g_mutex;
std::vector<std::string> g_history;
std::atomic<void*> g_manager{nullptr}; // the GameScene seen switching states
// The scene state class last switched to (the game's switches and ours).
std::mutex g_sceneMutex;
std::string g_lastScene;

std::atomic<uint64_t> g_sceneChangedAt{0}; // queue run (about a frame) of the last scene change
std::atomic<uint64_t> g_battleSwitches{0};  // jumps switched to the battle scene (BattleSceneSwitches)

void RememberScene(const std::string& stateClassName)
{
    // A held results screen belongs to the battle being left.
    g_holdResults = false;
    g_resultsHeld = false;
    std::lock_guard lock(g_sceneMutex);
    if (stateClassName == "TrainingBattleState" && g_lastScene != stateClassName) NoteSessionActivity("training");
    g_lastScene = stateClassName;
    g_sceneChangedAt = g_pumps.load();
}

std::string LastScene()
{
    std::lock_guard lock(g_sceneMutex);
    return g_lastScene;
}
std::mutex g_stageMutex;
std::string g_pendingStage;
// The stage code the last jump's stage request carried (the pending stage,
// or the one already prepared): battle setup writes it into the battle data
// (JumpStageCode).
std::string g_jumpStage;
std::mutex g_taskMutex;
std::deque<std::function<void()>> g_tasks;

void Remember(const std::string& line)
{
    std::lock_guard lock(g_mutex);
    g_history.push_back(line);
    if (g_history.size() > 64) g_history.erase(g_history.begin());
}

void TraceSceneSteps(const std::string& name);

void FlowResetDetour(void* flow)
{
    HookGuard guard;
    g_origFlowReset(flow);
    if (g_loaderAfterReset.exchange(false)) {
        reinterpret_cast<void (*)(void*, int)>(g_base + kEnsureLoader)(flow, kVersusLoaderType);
        Log("flow: the old scene reset the battle flow; Versus battle loader made again");
    }
}

using CharaCacheRequestFn = uint64_t (*)(void* cache, const void* code, int costume, int color, const void* x, int y);
CharaCacheRequestFn g_origCacheRequest = nullptr;
uint64_t CharaCacheRequestDetour(void* cache, const void* code, int costume, int color, const void* x, int y);
void PreloadTick();
extern std::atomic<bool> g_preloading;

void SceneTickDetour(void* self, float deltaSeconds)
{
    HookGuard guard;
    g_origSceneTick(self, deltaSeconds);
    g_sceneTicks++;
    NoteGameTick();
    PumpGameThread();
    if (g_sceneTicks % 30 == 0) PreloadTick();
}

void ChangeDetour(void* self, const void* stateClass, void* param)
{
    HookGuard guard;
    const std::string line = "ChangeSceneState " + ue4::ObjectName(stateClass) + " on " + ue4::Describe(self) +
                             " param " + ue4::Describe(param);
    Log("flow: %s (manager %p class %p param %p)", line.c_str(), self, stateClass, param);
    g_manager = self;
    RememberScene(ue4::ObjectName(stateClass));
    Remember(line);
    LogStack("ChangeSceneState", g_base);
    g_origChange(self, stateClass, param);
    TraceSceneSteps(ue4::ObjectName(stateClass));
    if (ue4::ObjectName(stateClass) == "InitialState") GoOfflineFromTitle();
}

// Scene states run one step function at a time (+0x338, after +0x378
// frames): logs the steps of the state just entered, until the next change.
std::atomic<int> g_sceneGeneration{0};

void TraceStep(int generation, const std::string& name, uint8_t* state, uintptr_t lastStep, int frame)
{
    if (generation != g_sceneGeneration || Stopping() || frame > 60 * 120) return;
    if (!state) {
        if (!ue4::EnsureObjects(g_base)) return;
        ue4::ForEachObject([&](const void* object) {
            if (ue4::ObjectName(ue4::ClassOf(object)) == name) state = static_cast<uint8_t*>(const_cast<void*>(object));
        });
        if (!state) return;
    }
    if (!IsReadable(state + 0x338, 0x48)) return;
    const uintptr_t step = *reinterpret_cast<const uintptr_t*>(state + 0x338);
    if (step != lastStep) {
        Log("flow: %s step rva %llx (delay %d) at frame %d", name.c_str(), static_cast<unsigned long long>(step - g_base),
            *reinterpret_cast<const int32_t*>(state + 0x378), frame);
    }
    RunOnGameThreadAfter(1, [=] { TraceStep(generation, name, state, step, frame + 1); });
}

void TraceSceneSteps(const std::string& name)
{
    const int generation = ++g_sceneGeneration;
    RunOnGameThreadAfter(1, [=] { TraceStep(generation, name, nullptr, 0, 0); });
}

uint64_t LaunchDetour(void* self, void* a2, void* a3)
{
    HookGuard guard;
    const std::string line = "LaunchNextGameScene on " + ue4::Describe(self) + " (" + ue4::Describe(a2) + ")";
    Log("flow: %s (this %p a2 %p a3 %p)", line.c_str(), self, a2, a3);
    Remember(line);
    return g_origLaunch(self, a2, a3);
}

uint64_t StepDetour(void* self, int step, uint8_t force)
{
    HookGuard guard;
    Log("flow: setup step %d (force %d) on %s (%p)", step, force, ue4::Describe(self).c_str(), self);
    LogStack("setup step", g_base);
    return g_origStep(self, step, force);
}

uint64_t Step0Detour(void* self, void* data, uint8_t a, uint8_t b)
{
    HookGuard guard;
    std::string text;
    uint64_t words[6] = {};
    if (data && IsReadable(data, sizeof(words))) {
        std::memcpy(words, data, sizeof(words));
        const auto* w = reinterpret_cast<const wchar_t*>(words[0]);
        for (int i = 0; i < 120 && w && IsReadable(w + i, 2) && w[i]; i++) text += w[i] < 128 ? char(w[i]) : '?';
    }
    Log("flow: step0 call data %p [%llx %llx %llx %llx] text '%s' a %d b %d", data, (unsigned long long)words[0],
        (unsigned long long)words[1], (unsigned long long)words[2], (unsigned long long)words[3], text.c_str(), a, b);
    // A stage request carries the stage code (a 3-letter FString); map loads
    // carry a path. Swap in the pending stage, in place.
    std::string stage;
    {
        std::lock_guard lock(g_stageMutex);
        stage = g_pendingStage;
    }
    auto* chars = reinterpret_cast<wchar_t*>(words[0]);
    if (!stage.empty() && text.size() == 3 && stage.size() == 3 && static_cast<int32_t>(words[1]) >= 4) {
        for (int i = 0; i < 3; i++) chars[i] = static_cast<wchar_t>(stage[i]);
        Log("flow: stage request %s -> %s", text.c_str(), stage.c_str());
    }
    return g_origStep0(self, data, a, b);
}

uint64_t EventDetour(void* node, void* event, void* a3, void* a4)
{
    if (event && IsReadable(static_cast<uint8_t*>(event), 0x10)) {
        const int code = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(event) + 0xC);
        const uintptr_t vtable = *static_cast<const uintptr_t*>(event);
        if (vtable - g_base == kFlowEventType && g_holdResults && (code == kResults || code == kResults + 1)) {
            if (!g_resultsHeld.exchange(true)) Log("flow: results screen held (flow event %d dropped)", code);
            return 0;
        }
        if (vtable - g_base == kFlowEventType) {
            g_roundCall = code == kRoundCall;
            if (code == kRoundCall) {
                if (const uint8_t* m = g_phaseMachine; m && IsReadable(m + 0x130, 4)) g_roundCallPhase = *reinterpret_cast<const int32_t*>(m + 0x130);
            }
            if (code == kRoundEnd) g_roundEnds++;
            if (code == kFightStarts || EndsFight(code) || code == kResults) {
                g_roundFlow.lastCode = code;
                g_roundFlow.inFight = code == kFightStarts;
            }
            Log("flow: round flow event %d", code);
        }
    }
    return g_origEvent(node, event, a3, a4);
}

// Rolling back across a round's end (netplay-testing.md, findings 24 and 27).
// The round's end changes two things the snapshot does not hold:
// - the phase machine switches from the fight to the round's end: its fields
//   and its phase objects (each with a timer whose bound argument is a
//   16-byte heap object; leaving a phase frees it). A copy of the machine and
//   all its phases is kept after every driven tick, keyed by a tick count in
//   the round flow region (so in the snapshot), and a load puts back the copy of
//   the tick it loaded: timers rebuilt with a fresh argument through the
//   timer's own clone function, the current phase swapped with its shared
//   pointer's count kept right (the phase vector holds every phase, so a
//   swap never frees one);
// - the director's round over (state 2 to 3) adds to the round count and
//   writes the round's record into a history: the ranges it writes are kept
//   as they were before (once per round), and a load to before it writes
//   them back.
// So the round's end no longer has to wait until no rollback can reach it.
constexpr uintptr_t kPhaseMachineVtable = 0x2C5F838;
constexpr uint32_t kMachineFields = 0x120; // current object, reference block, index, requested, parameter, flag
constexpr size_t kMachineFieldsSize = 0x20;
constexpr uint32_t kPhaseTimerDestroy = 0x50; // the timer block at +0x48: +0x08 destroy (argument)
constexpr uint32_t kPhaseTimerClone = 0x58;   // +0x10 clone (argument) -> a new argument
constexpr uint32_t kPhaseTimerArgument = 0x60; // +0x18 the bound argument: a 16-byte closure on the heap
constexpr uint32_t kPhaseTimerCall = 0x68;    // +0x20 call (argument)
constexpr size_t kMaxPhases = 24;
constexpr size_t kPhaseRing = 64; // ticks kept (more than any rollback reaches)
struct PhaseCopy {
    uint8_t bytes[0xC0];
    uint8_t argument[16];
    bool hasArgument;
};
// The game's battle input recorder (global at rva 0x39DA0D0, 0x79580 bytes,
// outside the snapshot): its last message (+0x8), per-round records (+0x18)
// and 4 slots of 0xD0 from +0xE0 (a state: 1 or 2 while recording, 0 after
// the round's end; the round; positions). A battle task counts frames while
// a slot records (the 16-byte region at +0xF8 of its object, rva 0x48DF30),
// and the round markers above the health bars follow the round records: a
// load across a round's end or call left the recorder as it was after it,
// and the replayed frames counted differently (stress replay, 2026-10-28).
constexpr uint32_t kRecorderGlobal = 0x39DA0D0;
constexpr size_t kRecorderHead = 0x420;
struct MachineCopy {
    uint32_t tick;
    bool valid;
    uint8_t fields[kMachineFieldsSize];
    int count;
    PhaseCopy phases[kMaxPhases];
    bool hasRecorder;
    uint8_t recorder[kRecorderHead];
    bool hasKoWait;
    uint8_t koWait[0x48];
    void* koWaitHeld; // the wait's reference block this copy holds a reference to (HoldRef), or null
};
std::vector<MachineCopy> g_phaseRing(kPhaseRing);
// The phase machine and the round director as they were at the freeze (tick
// 0): the ring has copies only after a driven tick ran the director, and the
// snapshot's round flow has no director copy before then. A rollback that
// loaded the frozen state left the machine in phase 1 (the first tick's
// switch had run): the replayed first tick did not switch again and so did
// not fire its flow event (the camera's reset among them), and the session
// parted from frame 0 on (554f088b, 7cc312ad; 0.4.0, later games of a set).
struct FreezeCopy {
    bool valid;
    bool hasDirector;
    int32_t director[14];
    int32_t directorTimers[16];
    int32_t directorCounts[5];
    MachineCopy machine;
};
FreezeCopy g_freeze{};
// The round's end waits (the round-end phase's timer, rva 0x40BC00, on rva
// 0x2F2E90) until an object's mode (+0x104) is 6 or 7, then for its one-tick
// latch (+0x100); +0x108 is set with the mode. The object (0x140 bytes,
// vtable rva 0x2C602F8, made by rva 0x2F48C0 -> 0x3C7AE0) is not in the
// snapshot: the round's end events reset it (rva 0x3D66E0) and its own code
// sets the mode (rva 0x3CCA20), and a load left it as the later timeline had
// it, so a resimulated round end went on on another tick (stress replay of
// 2026-11-08_05-14-51 ..._91ba0287: from frame 2813). Its 16 bytes at +0x100
// go into the per-tick copies and come back with a load.
// Not only those: the KO's flow event arms it (rva 0x3D4550: +0xF8, +0xFC,
// the latch and the mode, a weak object reference at +0x118, the first
// fighter's position at +0x120), and a stage transition KO reads them as
// it plays: a load after the arming left them as the later timeline had
// them, and a stress replay parted from its recording 2 ticks into the
// match's last KO (LAR vs NCL 364c0a17, frame 9274, 28 regions). Its whole
// tail, +0xF8 to its end (+0x140), is kept.
constexpr uint32_t kKoWaitCtor = 0x3C7AE0;
constexpr uintptr_t kKoWaitVtable = 0x2C602F8;
constexpr uint32_t kKoWaitState = 0xF8;
constexpr size_t kKoWaitStateSize = 0x48;
constexpr size_t kKoWaitLatch = 0x100 - kKoWaitState, kKoWaitMode = 0x104 - kKoWaitState;
// The tail holds a shared pointer: +0x110 an object, +0x118 its reference
// block (uses at +8, weak references at +0xC). The KO's arming empties it,
// the stage transition KO holds its object through it, and the round's end
// (rva 0x3D66E0, the mode back to -1) lets go of it. Copied back as bytes
// by a load from the next round to the transition, it pointed at the block
// again without a reference of its own; the replayed round's end let go of
// it a second time and freed the object while the battle's unit (rva
// 0x3C72A0) still listed it as a child, and destroying the battle at the
// next game of a set read the freed child (crash at StreetFighterV.exe+
// 0x4D77DC: 4f5db3fe, 7fde3ab3, 3b02cd4b; 0.4.0). Each copy now holds a
// reference of its own through the game's assign, and a load puts the
// pointer back through it, the counts kept right.
constexpr uint32_t kKoWaitObject = 0x110, kKoWaitRef = 0x118;
constexpr uint32_t kSharedAssign = 0x2F02D0; // (slot, &block): the new block counted up, the old one down (freed at zero)
constexpr uint8_t kSharedAssignBytes[16] = {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
                                            0xEC, 0x20, 0x48, 0x8B, 0x32, 0x48, 0x8B, 0xF9};
using SharedAssignFn = void (*)(void** slot, void* const* block);
SharedAssignFn g_sharedAssign = nullptr;
std::atomic<DWORD> g_gameThread{0}; // the thread that runs the driven ticks
int g_waitRefBacks = 0, g_waitRefEmptied = 0, g_waitRefLeft = 0; // loads that put the pointer back, per battle
uint8_t* g_koWait = nullptr; // the newest such object
std::atomic<bool> g_koWaitRollback{true};
int g_koWaitLogs = 0;
std::atomic<bool> g_roundEndRollback{false}; // on: no hold; the copies and the journal below instead
int g_phaseRestoreSkips = 0;                 // pointer words not put back (logged a few times)

// The round over's writes outside the snapshot (battle context offsets).
constexpr uint32_t kRoundCount = 0x1904;     // +0x1904 the round's time, +0x1908 rounds recorded (at most 7)
constexpr size_t kRoundCountSize = 0x0C;
constexpr uint32_t kRoundHistory = 0x1910;   // 7 records of 0x10FC8
constexpr size_t kRoundRecord = 0x10FC8;
constexpr uint32_t kLastRecord = 0x787AC;    // the latest round's record
// +0x78788..+0x78797 the results the round over writes (+0x78798, the
// director's timers, is a snapshot region of its own: an undo must not
// write it back; +0x787B0.. are flags it only reads).
constexpr uint32_t kRoundResult = 0x78788;
constexpr size_t kRoundResultSize = 0x10;
struct RoundOverUndo {
    uint32_t tick;
    uint8_t* context;
    uint8_t* slot;
    std::vector<uint8_t> count, slotBytes, lastRecord, result;
};
std::vector<RoundOverUndo> g_roundOverJournal;
RoundOverUndo g_roundOverPending; // filled before a director tick that may end the round

// Ticks counted by the mod in the round flow region: in the snapshot, the
// same on both machines. (The battle frame's +0x1B8 counts the ticks of one
// update, not of the battle.)
uint32_t TickNow()
{
    return g_roundFlow.ticks;
}

std::atomic<bool> g_testEmptyStage{false};

uint8_t* LivePhaseMachine()
{
    uint8_t* m = g_phaseMachine;
    if (!m || !IsReadable(m, 0x140)) return nullptr;
    return *reinterpret_cast<const uintptr_t*>(m) == g_base + kPhaseMachineVtable ? m : nullptr;
}

// The phase objects: 0xC0 bytes for the classes in the allocator's 192-byte
// bin (the round's end, ids 11 and 16), 0xA0 for the rest (160-byte bin).
size_t PhaseSize(const uint8_t* phase)
{
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(phase) - g_base;
    return vtable == 0x2C6A668 || vtable == 0x2C6A878 || vtable == 0x2C6AA18 ? 0xC0 : 0xA0;
}

template <typename T> T Field(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const T*>(p + off); }

uint8_t* LiveKoWait()
{
    uint8_t* w = g_koWaitRollback ? g_koWait : nullptr;
    if (!w || !IsReadable(w, kKoWaitState + kKoWaitStateSize)) return nullptr;
    return *reinterpret_cast<const uintptr_t*>(w) == g_base + kKoWaitVtable ? w : nullptr;
}

// `*slot` to `block` through the game's assign: a reference taken on the new
// block, the old one's let go of. Without the assign nothing is held.
void HoldRef(void** slot, void* block)
{
    if (*slot == block) return;
    if (g_sharedAssign) g_sharedAssign(slot, &block);
    else *slot = nullptr;
}

// A copy's reference let go of: at once on the game thread, else posted to
// it (letting go of the last one destroys the object).
void ReleaseHeld(MachineCopy& c)
{
    void* block = c.koWaitHeld;
    if (!block) return;
    c.koWaitHeld = nullptr;
    const DWORD game = g_gameThread;
    if (!game || GetCurrentThreadId() == game) {
        HoldRef(&block, nullptr);
        return;
    }
    RunOnGameThread([block]() mutable { HoldRef(&block, nullptr); });
}

// The ring's copies dropped, with their references.
void ForgetCopies()
{
    for (MachineCopy& c : g_phaseRing) {
        c.valid = false;
        ReleaseHeld(c);
    }
}

uint8_t* LiveRecorder()
{
    auto* recorder = *reinterpret_cast<uint8_t* const*>(g_base + kRecorderGlobal);
    return recorder && IsReadable(recorder, kRecorderHead) ? recorder : nullptr;
}

bool TimerSet(const uint8_t* phase)
{
    return Field<uintptr_t>(phase, kPhaseTimerDestroy) && Field<uintptr_t>(phase, kPhaseTimerCall) &&
           Field<uintptr_t>(phase, kPhaseTimerArgument);
}

// The phase vector: (object, reference block) pairs.
int PhaseVector(const uint8_t* m, uint8_t* const** begin)
{
    auto* b = Field<uint8_t* const*>(m, 0xF8);
    auto* e = Field<uint8_t* const*>(m, 0x100);
    const int count = b && e > b ? static_cast<int>((e - b) / 2) : 0;
    if (count <= 0 || count > static_cast<int>(kMaxPhases) || !IsReadable(b, count * 16)) return 0;
    *begin = b;
    return count;
}

// The phase machine, its phases, the recorder's head and the round end
// wait, as they are now, into `c` (valid only when all were read).
bool CaptureMachine(MachineCopy& c, uint32_t tick)
{
    c.valid = false;
    const uint8_t* m = LivePhaseMachine();
    uint8_t* const* phases = nullptr;
    const int count = m ? PhaseVector(m, &phases) : 0;
    if (!count) return false;
    c.tick = tick;
    std::memcpy(c.fields, m + kMachineFields, kMachineFieldsSize);
    c.count = count;
    for (int i = 0; i < count; i++) {
        const uint8_t* phase = phases[i * 2];
        PhaseCopy& p = c.phases[i];
        if (!phase || !IsReadable(phase, PhaseSize(phase))) return false;
        std::memcpy(p.bytes, phase, PhaseSize(phase));
        const auto* argument = Field<const uint8_t*>(phase, kPhaseTimerArgument);
        p.hasArgument = TimerSet(phase) && IsReadable(argument, 16);
        if (p.hasArgument) std::memcpy(p.argument, argument, 16);
    }
    const uint8_t* recorder = LiveRecorder();
    c.hasRecorder = recorder != nullptr;
    if (recorder) std::memcpy(c.recorder, recorder, kRecorderHead);
    const uint8_t* wait = LiveKoWait();
    c.hasKoWait = wait != nullptr;
    if (wait) std::memcpy(c.koWait, wait + kKoWaitState, kKoWaitStateSize);
    // The block its shared pointer has kept alive while this copy is (one
    // with a count of uses left).
    auto* block = wait ? Field<uint8_t*>(wait, kKoWaitRef) : nullptr;
    if (block && (!IsReadable(block, 16) || Field<int32_t>(block, 8) <= 0)) block = nullptr;
    HoldRef(&c.koWaitHeld, block);
    c.valid = true;
    return true;
}

void CapturePhases()
{
    const uint32_t tick = TickNow();
    if (!CaptureMachine(g_phaseRing[tick % kPhaseRing], tick)) return;
    const uint8_t* wait = LiveKoWait();
    // The mode goes to 6 or 7 when a KO plays the stage transition (it needs
    // the fighters' stage data, battle_setup.h kStageName): logged, a few per
    // battle, to tell in a session's log whether one played.
    static const uint8_t* modeOf = nullptr;
    static int32_t lastMode = -1;
    static int modeLogs = 0;
    if (wait != modeOf) {
        modeOf = wait;
        lastMode = -1;
        modeLogs = 0;
    }
    if (wait) {
        const int32_t mode = Field<int32_t>(wait, kKoWaitState + kKoWaitMode);
        if (mode != lastMode && (mode == 6 || mode == 7) && modeLogs++ < 8) {
            Log("flow: stage transition at tick %u (round end wait mode %d)", tick, mode);
        }
        lastMode = mode;
    }
}

// A word that points into memory (aligned, mapped): timer rates (0x10000)
// and packed small fields are not.
bool LooksLikePointer(uint64_t v)
{
    return v >= 0x10000 && v < 0x800000000000ull && (v & 7) == 0 && IsReadable(reinterpret_cast<const void*>(v), 8);
}

// Puts a phase object back as it was in the copy.
void RestorePhase(uint8_t* phase, const PhaseCopy& saved)
{
    const size_t size = PhaseSize(phase);
    const auto* liveArgument = Field<const uint8_t*>(phase, kPhaseTimerArgument);
    const bool liveTimer = TimerSet(phase) && IsReadable(liveArgument, 16);
    const bool sameTimer = liveTimer && saved.hasArgument &&
                           Field<uintptr_t>(phase, kPhaseTimerArgument) == Field<uintptr_t>(saved.bytes, kPhaseTimerArgument) &&
                           std::memcmp(liveArgument, saved.argument, 16) == 0;
    if (std::memcmp(phase, saved.bytes, size) == 0 && (sameTimer || (!liveTimer && !saved.hasArgument))) return;
    // A timer that is not the saved one goes, as the phase's leave would.
    if (liveTimer && !sameTimer) {
        using DestroyFn = void (*)(void*);
        Field<DestroyFn>(phase, kPhaseTimerDestroy)(const_cast<uint8_t*>(liveArgument));
    }
    for (uint32_t off = 8; off + 8 <= size; off += 8) {
        if (off == kPhaseTimerArgument) continue;
        const uint64_t want = Field<uint64_t>(saved.bytes, off);
        const uint64_t have = Field<uint64_t>(phase, off);
        if (want == have) continue;
        // Timer functions are the game's own code; other pointers stay as
        // they are (none were seen to change within a battle).
        const bool timerFunction = off == kPhaseTimerDestroy || off == kPhaseTimerClone || off == kPhaseTimerCall;
        // A pointer that changed is left alone; a word that is a pointer only
        // on one side is data (the parameter at +0x8 is an int beside
        // leftover bytes that can look like an address).
        if (!timerFunction && LooksLikePointer(want) && LooksLikePointer(have)) {
            if (g_phaseRestoreSkips++ < 5) {
                Log("flow: phase %p +0x%x not put back (%016llx, now %016llx)", static_cast<void*>(phase), off,
                    static_cast<unsigned long long>(want), static_cast<unsigned long long>(have));
            }
            continue;
        }
        *reinterpret_cast<uint64_t*>(phase + off) = want;
    }
    void* argument = Field<void*>(saved.bytes, kPhaseTimerArgument);
    if (saved.hasArgument && !sameTimer) {
        using CloneFn = void* (*)(const void*);
        argument = Field<CloneFn>(saved.bytes, kPhaseTimerClone)(saved.argument);
    }
    if (!sameTimer) *reinterpret_cast<void**>(phase + kPhaseTimerArgument) = argument;
}

// Puts the phase machine, its phases, the round end wait and the recorder's
// head back as `c` has them.
void RestoreMachine(const MachineCopy& c)
{
    uint8_t* m = LivePhaseMachine();
    uint8_t* const* phases = nullptr;
    const int count = m ? PhaseVector(m, &phases) : 0;
    if (!count || !c.valid || c.count != count) return;
    const int liveIndex = Field<int32_t>(m, 0x130);
    const int savedIndex = Field<int32_t>(c.fields, 0x130 - kMachineFields);
    for (int i = 0; i < count; i++) RestorePhase(phases[i * 2], c.phases[i]);
    if (savedIndex != liveIndex && savedIndex >= 0 && savedIndex < count) {
        // The current phase as a shared pointer copy: the new one's count up
        // first, the old one's down (never to zero: the vector holds it).
        uint8_t* object = phases[savedIndex * 2];
        uint8_t* block = phases[savedIndex * 2 + 1];
        auto* oldBlock = Field<uint8_t*>(m, 0x128);
        if (block) InterlockedIncrement(reinterpret_cast<volatile LONG*>(block + 8));
        *reinterpret_cast<uint8_t**>(m + 0x120) = object;
        *reinterpret_cast<uint8_t**>(m + 0x128) = block;
        if (oldBlock) InterlockedDecrement(reinterpret_cast<volatile LONG*>(oldBlock + 8));
        Log("flow: phase machine back to phase %d (was %d) by a load", savedIndex, liveIndex);
    }
    std::memcpy(m + 0x130, c.fields + (0x130 - kMachineFields), kMachineFieldsSize - (0x130 - kMachineFields));
    if (uint8_t* wait = c.hasKoWait ? LiveKoWait() : nullptr) {
        if (std::memcmp(wait + kKoWaitState, c.koWait, kKoWaitStateSize) != 0) {
            if (g_koWaitLogs++ < 8) {
                Log("flow: round end wait back to mode %d latch %d (was mode %d latch %d) by a load to tick %u",
                    Field<int32_t>(c.koWait, kKoWaitMode), Field<int32_t>(c.koWait, kKoWaitLatch),
                    Field<int32_t>(wait, kKoWaitState + kKoWaitMode), Field<int32_t>(wait, kKoWaitState + kKoWaitLatch), c.tick);
            }
            // The words as bytes, but the shared pointer (+0x110/+0x118).
            uint8_t live[16];
            std::memcpy(live, wait + kKoWaitObject, sizeof(live));
            std::memcpy(wait + kKoWaitState, c.koWait, kKoWaitStateSize);
            std::memcpy(wait + kKoWaitObject, live, sizeof(live));
            void* const want = Field<void*>(c.koWait, kKoWaitRef - kKoWaitState);
            void* const have = Field<void*>(wait, kKoWaitRef);
            // Through the game's assign, and only to a block this copy keeps
            // alive (or none); a copy without its reference (a Battle Log
            // keyframe) leaves the live one.
            if (want != have && g_sharedAssign && (!want || want == c.koWaitHeld)) {
                HoldRef(reinterpret_cast<void**>(wait + kKoWaitRef), want);
                *reinterpret_cast<void**>(wait + kKoWaitObject) = Field<void*>(c.koWait, kKoWaitObject - kKoWaitState);
                (want ? g_waitRefBacks : g_waitRefEmptied)++;
                static int s_backs = 0;
                if (s_backs++ < 6) {
                    Log("flow: round end wait's object back to %p (block %p, %d uses; was block %p) by a load to tick %u",
                        Field<void*>(c.koWait, kKoWaitObject - kKoWaitState), want,
                        want ? Field<int32_t>(static_cast<const uint8_t*>(want), 8) : 0, have, c.tick);
                }
            }
            else if (want != have) {
                g_waitRefLeft++;
                static int s_kept = 0;
                if (s_kept++ < 3) Log("flow: round end wait's object left as it is by a load (no reference kept for it)");
            }
        }
    }
    // The recorder's head, word by word: data back, pointers (both sides)
    // left alone, as for the phases.
    if (uint8_t* recorder = c.hasRecorder ? LiveRecorder() : nullptr) {
        for (size_t off = 8; off + 8 <= kRecorderHead; off += 8) {
            uint64_t want, have;
            std::memcpy(&want, c.recorder + off, 8);
            std::memcpy(&have, recorder + off, 8);
            if (want == have || (LooksLikePointer(want) && LooksLikePointer(have))) continue;
            std::memcpy(recorder + off, &want, 8);
        }
    }
}

void RestorePhases(uint32_t tick)
{
    const MachineCopy& c = g_phaseRing[tick % kPhaseRing];
    if (c.valid && c.tick == tick) RestoreMachine(c);
}

// Before a director tick that may end the round: what the round over writes.
void PrepareRoundOverUndo(uint8_t* director)
{
    RoundOverUndo& u = g_roundOverPending;
    u.context = BattleContext(director);
    if (!u.context || !IsReadable(u.context + kRoundHistory, kLastRecord + kRoundRecord - kRoundHistory) ||
        !IsReadable(u.context + kRoundResult, kRoundResultSize)) {
        static int s_unreadable = 0;
        if (s_unreadable++ < 3) Log("flow: round over cannot be kept (battle context %p)", static_cast<void*>(u.context));
        u.context = nullptr;
        return;
    }
    const uint32_t rounds = Field<uint32_t>(u.context, kRoundCount + 4);
    u.slot = u.context + kRoundHistory + std::min<uint32_t>(rounds, 6) * kRoundRecord;
    u.tick = TickNow();
    u.count.assign(u.context + kRoundCount, u.context + kRoundCount + kRoundCountSize);
    u.slotBytes.assign(u.slot, u.slot + kRoundRecord);
    u.lastRecord.assign(u.context + kLastRecord, u.context + kLastRecord + kRoundRecord);
    u.result.assign(u.context + kRoundResult, u.context + kRoundResult + kRoundResultSize);
}

void UndoRoundOvers(uint32_t tick)
{
    while (!g_roundOverJournal.empty() && g_roundOverJournal.back().tick > tick) {
        const RoundOverUndo& u = g_roundOverJournal.back();
        std::memcpy(u.context + kRoundCount, u.count.data(), u.count.size());
        std::memcpy(u.slot, u.slotBytes.data(), u.slotBytes.size());
        std::memcpy(u.context + kLastRecord, u.lastRecord.data(), u.lastRecord.size());
        std::memcpy(u.context + kRoundResult, u.result.data(), u.result.size());
        Log("flow: round over of tick %u taken back by a load to tick %u", u.tick, tick);
        g_roundOverJournal.pop_back();
    }
    // Older ones no rollback reaches any more.
    while (!g_roundOverJournal.empty() && g_roundOverJournal.front().tick + kPhaseRing < tick) {
        g_roundOverJournal.erase(g_roundOverJournal.begin());
    }
}

// What the round over leaves in the battle context, as a keyframe keeps it:
// kRoundCount to the results, then the latest round's record (kLastRecord,
// the round undo's lastRecord), whose +0x787B8 the fight phase's tick
// callback (rva 0x40EC60) reads to start the round's end: without it a
// keyframe from before a KO, loaded after it, ended the round on its first
// tick (the Battle Log's restarts after a round's end; LAR vs NCL frame 6840).
// The battle context of the live battle, or null when it cannot be read.
uint8_t* RoundContextNow()
{
    uint8_t* d = LiveRoundDirector();
    uint8_t* context = d ? BattleContext(d) : nullptr;
    if (!context || !IsReadable(context + kRoundCount, kRoundResult + kRoundResultSize - kRoundCount) ||
        !IsReadable(context + kLastRecord, kRoundRecord)) {
        return nullptr;
    }
    return context;
}
constexpr size_t kRoundHistoryPart = kRoundResult + kRoundResultSize - kRoundCount;
constexpr size_t kRoundHistoryBytes = kRoundHistoryPart + kRoundRecord;
constexpr size_t kRoundChunk = 4096; // keyframes share unchanged chunks of it
// Both parts, one after the other, into `out` (kRoundHistoryBytes); and back.
void CopyRoundHistory(const uint8_t* context, uint8_t* out)
{
    std::memcpy(out, context + kRoundCount, kRoundHistoryPart);
    std::memcpy(out + kRoundHistoryPart, context + kLastRecord, kRoundRecord);
}
void PutRoundHistory(uint8_t* context, const uint8_t* in)
{
    std::memcpy(context + kRoundCount, in, kRoundHistoryPart);
    std::memcpy(context + kLastRecord, in + kRoundHistoryPart, kRoundRecord);
}

constexpr uintptr_t kVirtualTask = 0x3EB3F4; // a task that calls its object's virtual +0x38
// The battle's sound manager has the game's own rollback handling for sounds
// (built for Capcom's netcode): its post-restore callback (rva 0x4B6230,
// sfv_game.cpp) moves the sounds played after the restored frame into a list
// of candidates and sets replay mode (+0x5C8); in replay mode a sound request
// (rva 0x4B83D0) is queued with its frame (InputUnit +0x2180) instead of
// played; this phase-4 task (rva 0x4AFEF0) then ends replay mode and settles
// the queue against the candidates: a sound played before is not played
// again, a new one plays, a candidate nobody asked for again is stopped (the
// mispredicted whiff). Capcom's update ran all resimulated ticks and then
// phase 4 once; ours run phase 4 on every tick, so the task ended replay
// mode after the first resimulated tick and the others played their sounds a
// second time (doubled sounds at low delay, 2026-11-08). Resimulated ticks
// leave it out: the newest tick settles them all. `soundrollback 0|1`.
constexpr uintptr_t kSoundReconcileTask = 0x4AFEF0;
std::atomic<bool> g_soundRollback{true};

// Research (`phasecost`): each task's time by the scheduler phase it ran in
// (battle_driver.cpp tells the phase), game thread only.
std::atomic<int> g_schedPhase{0};
std::atomic<bool> g_schedReplaying{false};
std::atomic<bool> g_taskProfiling{false};
// Experiment (`resimtasks <rva>... | off`): tasks replayed ticks leave out
// in phases 4 and 8 (game thread reads; set from the command thread).
std::mutex g_resimSkipMutex;
// Phase-4 tasks replayed ticks leave out (lean replayed ticks, sfv_game.h
// SetLeanResim; also in whole updates when `resimtasks` sets them). By rva,
// or 0x80000000 | the class vtable rva for the generic task 0x3EB3F4. Each
// left out of every replayed tick in full-match stress 8 replays without a
// difference from a straight run (2026-10-30, 2026-11-08), and it saves
// more there than the newest tick spends making it up: per driven tick
// 53.6 -> 30.0 us (class 0x2C63BB8), 34.4 -> 23.2 (0x2C63E00), 17.3 -> 12.4
// (0x2C64100), 5.3 -> 1.2 (0x3E3490), 4.3 -> 3.1 (0x4D6B40), 21.7 -> 4.0
// (0x37D1B0, the fighter's visual update, rva 0x3897C0). Not here: the
// fighters' 0x37D170 (their box ring, s13092 +0x6E8..), 0x37D1C0 (s724 +0x2C8
// differs in replayed frames, and it costs more once after many ticks than
// once per tick: 83.7 against 35.6 us), 0x37CF60, and the phase machine's
// 0x3DD1B0 (s652).
const std::vector<uintptr_t> kLeanResimTasks = {0x80000000u | 0x2C63BB8, 0x80000000u | 0x2C63E00, 0x80000000u | 0x2C64100,
                                                0x3E3490, 0x4D6B40, 0x37D1B0};
std::vector<uintptr_t> g_resimSkip = kLeanResimTasks;
std::atomic<bool> g_resimSkipAny{true};
struct TaskCost {
    uint64_t ticks = 0, calls = 0;
    uintptr_t object = 0;
};
std::map<uint64_t, TaskCost> g_taskCosts; // phase << 32 | task function rva (or 0x80000000 | class vtable rva)

// The thunk passes its other arguments (rdx, r8, r9, xmm) on to the task:
// take and forward them untouched, and record only after the call.
uint64_t TaskThunkDetour(void* closure, void* a2, void* a3, void* a4)
{
    {
        const auto* c = static_cast<const uint8_t*>(closure);
        if (*reinterpret_cast<const uintptr_t*>(c) == g_base + kPhaseMachineTask) {
            g_phaseMachine = reinterpret_cast<uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + 0x10) +
                                                        *reinterpret_cast<const int32_t*>(c + 8));
        }
        if (*reinterpret_cast<const uintptr_t*>(c) == g_base + kRoundDirectorTask) {
            auto* d = reinterpret_cast<uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + 0x10) +
                                                 *reinterpret_cast<const int32_t*>(c + 8));
            g_roundDirector = d;
            // The battle's phase machine (rva 0x3DD890, a task in the tick)
            // posts the flow events as it switches phases. In a KO's stop the task does nothing until the round is to end.
            const bool mayEnd = g_directorPerTick && *reinterpret_cast<const int32_t*>(d + kDirectorState) == kDirectorStopped &&
                                *reinterpret_cast<const int32_t*>(d + kDirectorStopKind) != 0;
            // Without the copies and the journal: skipping it holds the
            // round's end (kRoundOverAfterTicks).
            if (mayEnd && !g_roundEndRollback && g_roundFlow.stopTicks < kRoundOverAfterTicks) return 0;
            if (mayEnd && g_roundEndRollback) {
                PrepareRoundOverUndo(d);
                const uint64_t result = g_origThunk(closure, a2, a3, a4);
                if (g_roundOverPending.context && *reinterpret_cast<const int32_t*>(d + kDirectorState) == kDirectorRoundOver) {
                    Log("flow: round over at tick %u kept for a rollback (round %u)", g_roundOverPending.tick,
                        Field<uint32_t>(g_roundOverPending.count.data(), 4));
                    g_roundOverJournal.push_back(std::move(g_roundOverPending));
                }
                g_roundOverPending.context = nullptr;
                return result;
            }
        }
    }
    if (g_soundRollback && g_schedReplaying && *static_cast<const uintptr_t*>(closure) == g_base + kSoundReconcileTask) {
        return 0;
    }
    if (g_resimSkipAny && g_schedReplaying && (g_schedPhase == 4 || g_schedPhase == 8)) {
        const auto* c = static_cast<const uint8_t*>(closure);
        uintptr_t key = *reinterpret_cast<const uintptr_t*>(c) - g_base;
        if (key == kVirtualTask) {
            // By the class of the object the generic task runs on.
            const auto* object = reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + 0x10) +
                                                                  *reinterpret_cast<const int32_t*>(c + 8));
            key = 0x80000000u | (*reinterpret_cast<const uintptr_t*>(object) - g_base);
        }
        std::lock_guard lock(g_resimSkipMutex);
        if (std::find(g_resimSkip.begin(), g_resimSkip.end(), key) != g_resimSkip.end()) return 0;
    }
    uint64_t result;
    if (g_taskProfiling && g_schedPhase) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        result = g_origThunk(closure, a2, a3, a4);
        QueryPerformanceCounter(&b);
        const auto* c = static_cast<const uint8_t*>(closure);
        const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(c);
        uint64_t key = static_cast<uint64_t>(g_schedPhase.load()) << 32 | (fn - g_base);
        if (fn - g_base == kVirtualTask) {
            // The generic task calls the object's +0x38: by the object's class.
            const auto* object = reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + 0x10) +
                                                                  *reinterpret_cast<const int32_t*>(c + 8));
            key = static_cast<uint64_t>(g_schedPhase.load()) << 32 | 0x80000000u |
                  (*reinterpret_cast<const uintptr_t*>(object) - g_base);
        }
        TaskCost& cost = g_taskCosts[key];
        cost.ticks += b.QuadPart - a.QuadPart;
        cost.calls++;
        if (!cost.object) cost.object = *reinterpret_cast<const uintptr_t*>(c + 0x10) + *reinterpret_cast<const int32_t*>(c + 8);
    }
    else {
        result = g_origThunk(closure, a2, a3, a4);
    }
    if (g_taskCensus > 0) {
        g_taskCensus--;
        const auto* c = static_cast<const uint8_t*>(closure);
        const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(c);
        const uintptr_t object = *reinterpret_cast<const uintptr_t*>(c + 0x10) + *reinterpret_cast<const int32_t*>(c + 8);
        std::lock_guard lock(g_censusMutex);
        g_census.emplace_back(fn, object);
    }
    return result;
}

int g_otherDirectors = 0;

uint64_t HitTallyDetour(uint8_t* self, void* event, void* a3, void* a4)
{
    HookGuard guard;
    std::unique_lock lock(g_hitTallyMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // Another thread is in it: without the lock, a count could be lost here.
        static std::atomic<int> waits{0};
        const int n = ++waits;
        if (n == 1 || n == 10 || n == 100 || n == 1000) Log("flow: hit tallies: %d calls waited for another thread's", n);
        lock.lock();
    }
    return g_origHitTally(self, event, a3, a4);
}

void DirectorCheckDetour(uint8_t* director)
{
    HookGuard guard;
    // While a session runs the ticks, the check runs after each of them
    // (RunRoundDirector), not per rendered frame.
    if (g_directorPerTick && !t_directorFromTick) {
        if (director != g_roundDirector && g_otherDirectors++ < 3) {
            Log("flow: director check skipped for %p, not the round director (%p)", static_cast<void*>(director),
                static_cast<void*>(g_roundDirector.load()));
        }
        return;
    }
    g_origDirectorCheck(director);
}

bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original)
{
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("flow: signature mismatch at %s", site.name);
        return false;
    }
    return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

constexpr game::CodeSite kKoWaitCtorSite{"round end wait object", kKoWaitCtor,
    {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x42, 0x45, 0xF2, 0xFF, 0xC7, 0x83}};
using KoWaitCtorFn = void* (*)(uint8_t*);
KoWaitCtorFn g_origKoWaitCtor = nullptr;
void* KoWaitCtorDetour(uint8_t* self)
{
    void* r = g_origKoWaitCtor(self);
    g_koWait = self;
    return r;
}

} // namespace

void SetKoWaitRollback(bool on)
{
    g_koWaitRollback = on;
    Log("flow: the round end wait's mode %s", on ? "is put back by loads" : "stays as it is on loads (as before 2026-11-08)");
}

bool InstallFlowProbes(const GameImage& image)
{
    g_base = image.base;
    // Walking the image for the name and object tables takes seconds.
    std::thread([image] {
        ThreadGuard running;
        ue4::Init(image);
    }).detach();
    const bool ok = Hook(image, kChangeSceneState, reinterpret_cast<void*>(&ChangeDetour), reinterpret_cast<void**>(&g_origChange)) &&
                    Hook(image, kLaunchNextGameScene, reinterpret_cast<void*>(&LaunchDetour), reinterpret_cast<void**>(&g_origLaunch));
    Hook(image, kSetupStep, reinterpret_cast<void*>(&StepDetour), reinterpret_cast<void**>(&g_origStep));
    Hook(image, kTaskThunk, reinterpret_cast<void*>(&TaskThunkDetour), reinterpret_cast<void**>(&g_origThunk));
    Hook(image, kRoundEvent, reinterpret_cast<void*>(&EventDetour), reinterpret_cast<void**>(&g_origEvent));
    Hook(image, kSceneTick, reinterpret_cast<void*>(&SceneTickDetour), reinterpret_cast<void**>(&g_origSceneTick));
    Hook(image, kFlowReset, reinterpret_cast<void*>(&FlowResetDetour), reinterpret_cast<void**>(&g_origFlowReset));
    Hook(image, kSetupStep0Call, reinterpret_cast<void*>(&Step0Detour), reinterpret_cast<void**>(&g_origStep0));
    Hook(image, kCharaCacheRequest, reinterpret_cast<void*>(&CharaCacheRequestDetour), reinterpret_cast<void**>(&g_origCacheRequest));
    if (!Hook(image, kRoundDirectorCheck, reinterpret_cast<void*>(&DirectorCheckDetour),
              reinterpret_cast<void**>(&g_origDirectorCheck))) {
        g_origDirectorCheck = nullptr;
        Log("flow: the round director's check stays per rendered frame (a KO in a rollback is found late)");
    }
    if (!Hook(image, kKoWaitCtorSite, reinterpret_cast<void*>(&KoWaitCtorDetour), reinterpret_cast<void**>(&g_origKoWaitCtor))) {
        Log("flow: the round end wait's mode is not put back by loads");
    }
    if (std::memcmp(reinterpret_cast<const void*>(g_base + kSharedAssign), kSharedAssignBytes, sizeof(kSharedAssignBytes)) == 0) {
        g_sharedAssign = reinterpret_cast<SharedAssignFn>(g_base + kSharedAssign);
    }
    else {
        Log("flow: the game's shared pointer assign is not at rva 0x%X: the round end wait's object is not put back by loads", kSharedAssign);
    }
    if (!Hook(image, kRoundHitTally, reinterpret_cast<void*>(&HitTallyDetour), reinterpret_cast<void**>(&g_origHitTally))) {
        Log("flow: the round's hit tallies stay unguarded (a trade can count differently on each side)");
    }
    Log("flow probes %s", ok ? "installed" : "NOT installed");
    return ok;
}

namespace {

// A UE4 FString (TArray<wchar_t>: data, num, max) as ASCII.
std::string FStringText(const uint8_t* fstring);

// A UE4 FString over our own characters, for the game to read (never to
// keep or free).
struct FStringArg {
    explicit FStringArg(const std::wstring& text) : chars(text)
    {
        data = chars.c_str();
        num = max = static_cast<int32_t>(chars.size() + 1);
    }
    std::wstring chars;
    const wchar_t* data;
    int32_t num;
    int32_t max;
    const void* Get() const { return &data; }
};

// The x and y battle creation last passed to the cache (x: an FString,
// y: an int, both from the battle setup); a preload passes the same.
std::mutex g_cacheArgsMutex;
std::wstring g_cacheX;
int g_cacheY = 0;
bool g_cacheArgsSeen = false;

std::wstring WideOf(const std::string& text) { return std::wstring(text.begin(), text.end()); }

uint64_t CharaCacheRequestDetour(void* cache, const void* code, int costume, int color, const void* x, int y)
{
    HookGuard guard;
    const std::string xText = FStringText(static_cast<const uint8_t*>(x));
    {
        std::lock_guard lock(g_cacheArgsMutex);
        g_cacheX = WideOf(xText);
        g_cacheY = y;
        g_cacheArgsSeen = true;
    }
    const auto* c = static_cast<const uint8_t*>(cache);
    Log("flow: character cache request %s costume %d version %d x '%s' y %d (slots %d in use, %d free)",
        FStringText(static_cast<const uint8_t*>(code)).c_str(), costume, color, xText.c_str(), y,
        *reinterpret_cast<const int32_t*>(c + kCacheSlotsInUse + 8), *reinterpret_cast<const int32_t*>(c + kCacheSlotsFree + 8));
    return g_origCacheRequest(cache, code, costume, color, x, y);
}

std::string FStringText(const uint8_t* fstring)
{
    const wchar_t* data = nullptr;
    int32_t num = 0;
    if (!IsReadable(fstring, 16)) return "";
    std::memcpy(&data, fstring, 8);
    std::memcpy(&num, fstring + 8, 4);
    std::wstring wide;
    for (int i = 0; data && i < num && i < 200 && IsReadable(data + i, 2) && data[i]; i++) wide += data[i];
    // As UTF-8: the game's text is in its language (anything not ASCII used
    // to become '?', so a Chinese game's names were all question marks).
    if (wide.empty()) return "";
    std::string out(wide.size() * 3, '\0');
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                                        static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(static_cast<size_t>(std::max(len, 0)));
    return out;
}

} // namespace

void WaitForStageThenSwitch(void* manager, void* stateClass, uint8_t* loader, int frames, uint64_t sceneAt, int generation);
void StartLoaderWhenReady(int frames, uint8_t* seen, int seenAt);
void JumpOnGameThread(int setupFrames);

void RunOnGameThreadAfter(int frames, std::function<void()> task)
{
    if (frames <= 0) {
        RunOnGameThread(std::move(task));
        return;
    }
    RunOnGameThread([frames, task = std::move(task)]() mutable { RunOnGameThreadAfter(frames - 1, std::move(task)); });
}

void RunOnGameThread(std::function<void()> task)
{
    std::lock_guard lock(g_taskMutex);
    g_tasks.push_back(std::move(task));
}

std::string PumpStats()
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "scene ticks %llu, queue runs %llu", static_cast<unsigned long long>(g_sceneTicks.load()),
                  static_cast<unsigned long long>(g_pumps.load()));
    return buf;
}

void PumpGameThread()
{
    // Called from the scene tick and from the pad poll; one run per frame
    // (both come within a few ms of each other).
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const uint64_t us = static_cast<uint64_t>(now.QuadPart * 1000000 / freq.QuadPart);
    if (us - g_lastPumpUs < 10000) return;
    g_lastPumpUs = us;
    g_pumps++;
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard lock(g_taskMutex);
        if (g_tasks.empty()) return;
        tasks.swap(g_tasks);
    }
    for (auto& t : tasks) t();
}

bool RoundCallRunning()
{
    const int phase = g_roundCallPhase;
    if (const uint8_t* m = phase >= 0 ? LivePhaseMachine() : nullptr) return Field<int32_t>(m, 0x130) == phase;
    return g_roundCall;
}
uint32_t RoundEndEvents() { return g_roundEnds; }

bool RoundInFight()
{
    if (!g_roundFlow.inFight) return false;
    // From the KO on (before the round's end event), as the director sees it.
    const uint8_t* d = LiveRoundDirector();
    const int32_t state = d ? *reinterpret_cast<const int32_t*>(d + kDirectorState) : 1;
    return state != kDirectorStopped && state != kDirectorRoundOver;
}

void MatchStopToDirector(uint8_t* battleFrame)
{
    const uint8_t* d = LiveRoundDirector();
    if (!d || !battleFrame) return;
    const bool stopped = *reinterpret_cast<const int32_t*>(d + kDirectorState) == kDirectorStopped;
    auto* flags = reinterpret_cast<uint32_t*>(battleFrame + kStopFlags);
    const bool set = (flags[0] & kStopBit) != 0;
    if (set == stopped) return;
    // As 0x4D5DE0 does: on sets the first word, off clears both.
    if (stopped) {
        flags[0] |= kStopBit;
    }
    else {
        flags[0] &= ~kStopBit;
        flags[1] &= ~kStopBit;
    }
    Log("flow: stop bit %s after a load, as the round director has it", stopped ? "set" : "cleared");
}

void SetRoundDirectorPerTick(bool on, bool roundEndRollback)
{
    if (g_waitRefBacks || g_waitRefEmptied || g_waitRefLeft) {
        Log("flow: round end wait's object put back by loads: %d times to a kept object, %d to none, %d left as it was",
            g_waitRefBacks, g_waitRefEmptied, g_waitRefLeft);
    }
    g_waitRefBacks = g_waitRefEmptied = g_waitRefLeft = 0;
    g_directorPerTick = on && g_origDirectorCheck;
    g_roundEndRollback = g_directorPerTick && roundEndRollback;
    ForgetCopies();
    g_roundOverJournal.clear();
    // From 0 on both machines as the session takes the ticks over (the
    // warm-up ticks before differ).
    g_roundFlow.ticks = 0;
    // The freeze: what a load back to it puts back.
    g_freeze.valid = false;
    ReleaseHeld(g_freeze.machine);
    if (!g_directorPerTick) return;
    g_freeze.hasDirector = false;
    if (const uint8_t* d = LiveRoundDirector()) {
        std::memcpy(g_freeze.director, d + kDirectorRecord, sizeof(g_freeze.director));
        std::memcpy(g_freeze.directorTimers, d + kDirectorTimers, sizeof(g_freeze.directorTimers));
        std::memcpy(g_freeze.directorCounts, d + kDirectorCounts, sizeof(g_freeze.directorCounts));
        g_freeze.hasDirector = true;
    }
    CaptureMachine(g_freeze.machine, 0);
    g_freeze.valid = g_freeze.hasDirector || g_freeze.machine.valid;
    const uint8_t* m = LivePhaseMachine();
    Log("flow: freeze copy: director %s, phase machine %s (phase %d, requested %d)", g_freeze.hasDirector ? "kept" : "missing",
        g_freeze.machine.valid ? "kept" : "missing", m ? Field<int32_t>(m, 0x130) : -1, m ? Field<int32_t>(m, 0x134) : -1);
}

namespace {
// A load to the freeze: the director and the machine back as they were then.
void RestoreFreeze()
{
    if (!g_freeze.valid || TickNow() != 0) return;
    if (uint8_t* d = g_freeze.hasDirector ? LiveRoundDirector() : nullptr) {
        const int32_t before = *reinterpret_cast<const int32_t*>(d + kDirectorState);
        std::memcpy(d + kDirectorRecord, g_freeze.director, sizeof(g_freeze.director));
        std::memcpy(d + kDirectorTimers, g_freeze.directorTimers, sizeof(g_freeze.directorTimers));
        std::memcpy(d + kDirectorCounts, g_freeze.directorCounts, sizeof(g_freeze.directorCounts));
        if (before != g_freeze.director[0]) Log("flow: round director back to state %d (was %d) by a load to the freeze", g_freeze.director[0], before);
    }
    if (g_roundEndRollback) {
        UndoRoundOvers(0);
        if (g_freeze.machine.valid) RestoreMachine(g_freeze.machine);
    }
    static int s_logs = 0;
    if (s_logs++ < 3) Log("flow: a load to the freeze put the director and the phase machine back");
}
} // namespace

void BeginDrivenTick()
{
    if (g_directorPerTick) g_roundFlow.ticks++;
    g_gameThread = GetCurrentThreadId();
}

uint32_t DrivenTicks()
{
    return g_roundFlow.ticks;
}

void RunRoundDirector()
{
    uint8_t* d = LiveRoundDirector();
    if (!d || !g_directorPerTick) return;
    t_directorFromTick = true;
    g_origDirectorCheck(d);
    t_directorFromTick = false;
    RoundFlow& f = g_roundFlow;
    std::memcpy(f.director, d + kDirectorRecord, sizeof(f.director));
    std::memcpy(f.directorTimers, d + kDirectorTimers, sizeof(f.directorTimers));
    std::memcpy(f.directorCounts, d + kDirectorCounts, sizeof(f.directorCounts));
    f.directorSeen = 1;
    if (g_roundEndRollback) CapturePhases();
    const bool koStop = f.director[0] == kDirectorStopped && *reinterpret_cast<const int32_t*>(d + kDirectorStopKind) != 0;
    f.stopTicks = koStop ? f.stopTicks + 1 : 0;
}

void RoundDirectorFromSnapshot()
{
    const RoundFlow& f = g_roundFlow;
    uint8_t* d = LiveRoundDirector();
    if (!d) return;
    if (!f.directorSeen) return RestoreFreeze(); // a load to the freeze
    const int32_t before = *reinterpret_cast<const int32_t*>(d + kDirectorState);
    std::memcpy(d + kDirectorRecord, f.director, sizeof(f.director));
    std::memcpy(d + kDirectorTimers, f.directorTimers, sizeof(f.directorTimers));
    std::memcpy(d + kDirectorCounts, f.directorCounts, sizeof(f.directorCounts));
    if (before != f.director[0]) Log("flow: round director back to state %d (was %d) by a load", f.director[0], before);
    if (g_roundEndRollback) {
        const uint32_t tick = TickNow();
        if (before != f.director[0]) {
            const MachineCopy& c = g_phaseRing[tick % kPhaseRing];
            const uint8_t* m = LivePhaseMachine();
            Log("flow:   load to tick %u: %zu round overs kept (last tick %u), phase copy %s (tick %u, phase %d), live phase %d",
                tick, g_roundOverJournal.size(), g_roundOverJournal.empty() ? 0u : g_roundOverJournal.back().tick,
                c.valid ? "valid" : "missing", c.tick, Field<int32_t>(c.fields, 0x10), m ? Field<int32_t>(m, 0x130) : -1);
        }
        UndoRoundOvers(tick);
        RestorePhases(tick);
    }
}

namespace {
using Chunks = std::vector<std::shared_ptr<const std::vector<uint8_t>>>;
// `size` bytes from `src` in kRoundChunk pieces, each shared with `before`'s
// piece while the same.
void SaveChunks(const uint8_t* src, size_t size, const Chunks* before, Chunks* out)
{
    const size_t chunks = (size + kRoundChunk - 1) / kRoundChunk;
    const bool samePlan = before && before->size() == chunks;
    out->assign(chunks, nullptr);
    for (size_t i = 0; i < chunks; i++) {
        const size_t at = i * kRoundChunk, n = std::min(kRoundChunk, size - at);
        const auto& old = samePlan ? (*before)[i] : nullptr;
        if (old && old->size() == n && std::memcmp(old->data(), src + at, n) == 0) (*out)[i] = old;
        else (*out)[i] = std::make_shared<const std::vector<uint8_t>>(src + at, src + at + n);
    }
}
bool PutChunks(const Chunks& chunks, uint8_t* dst, size_t size)
{
    size_t at = 0;
    for (const auto& c : chunks) {
        if (!c || at + c->size() > size) return false;
        at += c->size();
    }
    if (at != size) return false;
    at = 0;
    for (const auto& c : chunks) {
        std::memcpy(dst + at, c->data(), c->size());
        at += c->size();
    }
    return true;
}
} // namespace

bool SaveOutsideState(OutsideState* out, const OutsideState* previous)
{
    out->machine.assign(sizeof(MachineCopy), 0);
    auto* c = reinterpret_cast<MachineCopy*>(out->machine.data());
    if (!CaptureMachine(*c, TickNow())) {
        out->machine.clear();
        return false;
    }
    // A keyframe keeps no reference (it has no place to let go of it): a load
    // from it leaves the round end wait's object as it is.
    ReleaseHeld(*c);
    // The round director's fields as they are (the snapshot has its copy only
    // once a driven tick ran it: not at frame 0).
    out->director.clear();
    if (const uint8_t* d = LiveRoundDirector()) {
        const RoundFlow& rf = g_roundFlow;
        out->director.resize(sizeof(rf.director) + sizeof(rf.directorTimers) + sizeof(rf.directorCounts));
        uint8_t* p = out->director.data();
        std::memcpy(p, d + kDirectorRecord, sizeof(rf.director));
        std::memcpy(p + sizeof(rf.director), d + kDirectorTimers, sizeof(rf.directorTimers));
        std::memcpy(p + sizeof(rf.director) + sizeof(rf.directorTimers), d + kDirectorCounts, sizeof(rf.directorCounts));
    }
    const uint8_t* context = RoundContextNow();
    out->rounds.clear();
    if (!context) return true;
    // The round history and the latest round's record (part of it changes
    // every tick) in chunks, each shared with the previous keyframe's while
    // the same: a keyframe holds only the chunks that moved.
    thread_local std::vector<uint8_t> now;
    now.resize(kRoundHistoryBytes);
    CopyRoundHistory(context, now.data());
    SaveChunks(now.data(), kRoundHistoryBytes, previous ? &previous->rounds : nullptr, &out->rounds);
    return true;
}

namespace {
// Research (`ufunc`, `functrace`): a blueprint-callable function's native
// code, found from its UFunction; one traced at a time.
using ExecFn = void (*)(void* context, void* stack, void* result);
ExecFn g_tracedOrig = nullptr;
std::string g_tracedName;
std::atomic<int> g_traceCalls{0};

void TraceDetour(void* context, void* stack, void* result)
{
    if (g_traceCalls++ < 200) {
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
        Log("functrace: %s on %s (tick %u), from rva 0x%llX", g_tracedName.c_str(), ue4::Describe(context).c_str(), TickNow(),
            static_cast<unsigned long long>(from));
    }
    g_tracedOrig(context, stack, result);
}

// The code pointers in a UFunction (its native thunk among them).
std::vector<std::pair<size_t, uintptr_t>> CodePointers(const uint8_t* fn)
{
    std::vector<std::pair<size_t, uintptr_t>> out;
    for (size_t at = 0x30; at < 0x140; at += 8) {
        if (!IsReadable(fn + at, 8)) break;
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(fn + at);
        if (v > g_base + 0x1000 && v < g_base + 0x2800000) out.push_back({at, v - g_base});
    }
    return out;
}
} // namespace

void LogFunction(const std::string& name)
{
    if (!ue4::EnsureObjects(g_base)) return Log("ufunc: no object array");
    int found = 0;
    // "*Class": every function of that class.
    const bool ofClass = !name.empty() && name[0] == '*';
    ue4::ForEachObject([&](const void* object) {
        if (ue4::ObjectName(ue4::ClassOf(object)) != "Function") return;
        if (ofClass ? ue4::ObjectName(ue4::OuterOf(object)) != name.substr(1) : ue4::ObjectName(object) != name) return;
        found++;
        std::string pointers;
        for (const auto& [at, rva] : CodePointers(static_cast<const uint8_t*>(object))) {
            char b[48];
            std::snprintf(b, sizeof(b), " +0x%zX=0x%llX", at, static_cast<unsigned long long>(rva));
            pointers += b;
        }
        Log("ufunc: %s.%s at %p, code pointers:%s", ue4::ObjectName(ue4::OuterOf(object)).c_str(), ue4::ObjectName(object).c_str(), object,
            pointers.c_str());
    });
    if (!found) Log("ufunc: no function %s", name.c_str());
}

void LogObjectsNamed(const std::string& name, bool dump)
{
    if (!ue4::EnsureObjects(g_base)) return Log("uobj: no object array");
    int found = 0;
    ue4::ForEachObject([&](const void* object) {
        if (ue4::ObjectName(object) != name) return;
        found++;
        const void* outer = ue4::OuterOf(object);
        Log("uobj: %s at %p, class %s, outer %s (%s), size %zu", name.c_str(), object, ue4::ObjectName(ue4::ClassOf(object)).c_str(),
            ue4::ObjectName(outer).c_str(), outer ? ue4::ObjectName(ue4::ClassOf(outer)).c_str() : "-", ue4::ObjectSize(object));
        if (!dump) return;
        // Its words, and where a word points into readable memory: the first
        // bytes there as text (a "#BAC" file, a name).
        const auto* o = static_cast<const uint8_t*>(object);
        for (size_t at = 0; at < 0x100 && IsReadable(o + at, 8); at += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t*>(o + at);
            std::string text;
            if (v > 0x10000 && IsReadable(reinterpret_cast<const void*>(v), 16)) {
                const auto* p = reinterpret_cast<const uint8_t*>(v);
                for (int i = 0; i < 16; i++) text += p[i] >= 0x20 && p[i] < 0x7F ? static_cast<char>(p[i]) : '.';
            }
            Log("uobj:   +0x%02zX %016llX %s", at, static_cast<unsigned long long>(v), text.c_str());
        }
    });
    if (!found) Log("uobj: nothing named %s", name.c_str());
}

void TraceFunction(const std::string& name, const std::string& owner, uint32_t offset)
{
    if (g_tracedOrig) return Log("functrace: %s is traced already (one at a time)", g_tracedName.c_str());
    if (!ue4::EnsureObjects(g_base)) return Log("functrace: no object array");
    void* target = nullptr;
    ue4::ForEachObject([&](const void* object) {
        if (target || ue4::ObjectName(object) != name || ue4::ObjectName(ue4::ClassOf(object)) != "Function") return;
        if (!owner.empty() && ue4::ObjectName(ue4::OuterOf(object)) != owner) return;
        const auto* fn = static_cast<const uint8_t*>(object);
        if (IsReadable(fn + offset, 8)) target = *reinterpret_cast<void* const*>(fn + offset);
    });
    if (!target) return Log("functrace: %s%s%s not found", owner.c_str(), owner.empty() ? "" : ".", name.c_str());
    g_tracedName = owner.empty() ? name : owner + "." + name;
    g_traceCalls = 0;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&TraceDetour), reinterpret_cast<void**>(&g_tracedOrig)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        g_tracedOrig = nullptr;
        return Log("functrace: hook at rva 0x%llX failed", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - g_base));
    }
    Log("functrace: tracing %s (native rva 0x%llX)", g_tracedName.c_str(), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - g_base));
}

void LogRoundInfo()
{
    uint8_t* d = LiveRoundDirector();
    uint8_t* context = d ? BattleContext(d) : nullptr;
    if (!context || !IsReadable(context + 0x1820, kRoundResult + kRoundResultSize - 0x1820)) {
        Log("roundinfo: no battle context");
        return;
    }
    auto words = [](const uint8_t* p, size_t n) {
        std::string s;
        char b[16];
        for (size_t i = 0; i < n; i += 4) {
            std::snprintf(b, sizeof(b), "%s%x", i ? " " : "", *reinterpret_cast<const uint32_t*>(p + i));
            s += b;
        }
        return s;
    };
    Log("roundinfo: round state (ctx+0x1820): %s", words(context + 0x1820, 0xE4).c_str());
    Log("roundinfo: +0x1904: %s | result +0x78788: %s", words(context + kRoundCount, kRoundCountSize).c_str(),
        words(context + kRoundResult, kRoundResultSize).c_str());
    const uint32_t rounds = std::min<uint32_t>(*reinterpret_cast<const uint32_t*>(context + kRoundCount + 4), 7);
    for (uint32_t i = 0; i < rounds; i++) {
        const uint8_t* rec = context + kRoundHistory + i * kRoundRecord;
        Log("roundinfo: record %u: %s", i, words(rec, 0x80).c_str());
    }
    Log("roundinfo: last record: %s", words(context + kLastRecord, 0x80).c_str());
}

bool ReadRoundResults(std::vector<RoundResult>* rounds, uint8_t wins[2])
{
    // The round history (research `roundinfo`, 2026-11-09): +0x1908 the
    // rounds recorded; each round's record (0x10FC8 bytes from +0x1910) has
    // the side that won it at +4 (0 or 1; anything else: a draw).
    rounds->clear();
    wins[0] = wins[1] = 0;
    uint8_t* d = LiveRoundDirector();
    uint8_t* context = d ? BattleContext(d) : nullptr;
    if (!context || !IsReadable(context + kRoundCount, kRoundResult + kRoundResultSize - kRoundCount)) return false;
    const uint32_t count = std::min<uint32_t>(*reinterpret_cast<const uint32_t*>(context + kRoundCount + 4), 7);
    for (uint32_t i = 0; i < count; i++) {
        const int32_t side = *reinterpret_cast<const int32_t*>(context + kRoundHistory + i * kRoundRecord + 4);
        RoundResult r;
        r.winner = static_cast<int8_t>(side == 0 || side == 1 ? side : -1);
        if (r.winner >= 0) wins[r.winner]++;
        rounds->push_back(r);
    }
    return true;
}

void RestoreOutsideState(const OutsideState& s)
{
    const RoundFlow& rf = g_roundFlow;
    if (uint8_t* d = LiveRoundDirector(); d && s.director.size() == sizeof(rf.director) + sizeof(rf.directorTimers) + sizeof(rf.directorCounts)) {
        const uint8_t* p = s.director.data();
        std::memcpy(d + kDirectorRecord, p, sizeof(rf.director));
        std::memcpy(d + kDirectorTimers, p + sizeof(rf.director), sizeof(rf.directorTimers));
        std::memcpy(d + kDirectorCounts, p + sizeof(rf.director) + sizeof(rf.directorTimers), sizeof(rf.directorCounts));
    }
    if (s.machine.size() == sizeof(MachineCopy)) RestoreMachine(*reinterpret_cast<const MachineCopy*>(s.machine.data()));
    if (uint8_t* context = s.rounds.empty() ? nullptr : RoundContextNow()) {
        std::vector<uint8_t> all(kRoundHistoryBytes);
        if (PutChunks(s.rounds, all.data(), kRoundHistoryBytes)) PutRoundHistory(context, all.data());
    }
    // What the copies and the journal held belongs to the timeline left.
    ForgetCopies();
    g_roundOverJournal.clear();
}

uint32_t PresentationStart(uint32_t p1, uint32_t p2)
{
    constexpr uint32_t kGameStart = 0x4000; // the game's Start bit (core/game.h kStart)
    constexpr int kStartTicks = 10;         // as a short press
    constexpr int kUiStartMs = 150;
    constexpr uint16_t kUiButtons = 0x1000; // XINPUT_GAMEPAD_A: the win screens advance on confirm, not Start
    RoundFlow& f = g_roundFlow;
    if (RoundInFight()) {
        f.startPressed = 0;
        f.startTicks = 0;
        return 0;
    }
    // A press under way: its presses do not count again.
    if (f.startTicks > 0) {
        f.startTicks--;
        return kGameStart;
    }
    f.startPressed |= ((p1 & kGameStart) ? 1u : 0u) | ((p2 & kGameStart) ? 2u : 0u);
    if (f.startPressed != 3) return 0;
    f.startPressed = 0;
    f.startTicks = kStartTicks - 1;
    static int loggedFor = -1; // log once per flow event (a rollback may repeat it)
    if (loggedFor != f.lastCode) Log("flow: both players pressed Start during the presentation (after flow event %d): skipping", f.lastCode);
    loggedFor = f.lastCode;
    // The battle is over: each game skips its win screens on its own (they
    // no longer need to agree), at the same session tick.
    if (MatchOver(f.lastCode)) {
        Log("flow: both players pressed Start after the match: confirm on each game's pad input");
        PressUiButtons(kUiButtons, kUiStartMs);
    }
    return kGameStart;
}

bool MatchDecided() { return MatchOver(g_roundFlow.lastCode) || g_roundFlow.lastCode == kResults; }

void SetHoldResults(bool hold)
{
    g_holdResults = hold;
    if (!hold) g_resultsHeld = false;
}

bool ResultsHeld() { return g_resultsHeld; }

uint8_t* RoundFlowState()
{
    return reinterpret_cast<uint8_t*>(&g_roundFlow);
}

uint8_t* RoundDirector() { return LiveRoundDirector(); }

void ResetRoundFlow()
{
    g_roundFlow = {};
    g_roundCall = false;       // a battle left during its round call
    g_roundCallPhase = -1;
    g_roundDirector = nullptr; // the old battle's; its task points to the new one
}

namespace {

// The asset manager's load queue, or nullptr.
uint8_t* AssetQueue()
{
    auto* manager = *reinterpret_cast<uint8_t**>(g_base + kAssetManager);
    if (!manager || manager == reinterpret_cast<uint8_t*>(-1)) return nullptr;
    return *reinterpret_cast<uint8_t**>(manager + kAssetManagerQueue);
}

// The character's versions (oldest first) from the game's version data:
// singleton +0x60, per character (rva 0x57F030) +0x80, a list of 0x18-byte
// entries (+0x28, count +0x30) starting with the version.
std::vector<int> VersionsOf(const std::string& code)
{
    std::vector<int> out;
    uint8_t* singleton = reinterpret_cast<uint8_t* (*)()>(g_base + kKiwiSingleton)();
    void* versions = singleton ? *reinterpret_cast<void**>(singleton + kSingletonVersions) : nullptr;
    if (!versions) return out;
    const FStringArg codeArg(WideOf(code));
    auto* info = reinterpret_cast<uint8_t* (*)(void*, const void*)>(g_base + kCharaVersionInfo)(versions, codeArg.Get());
    const uint8_t* list = info ? *reinterpret_cast<uint8_t* const*>(info + 0x80) : nullptr;
    if (!list) return out;
    const uint8_t* entries = *reinterpret_cast<uint8_t* const*>(list + 0x28);
    const int count = *reinterpret_cast<const int32_t*>(list + 0x30);
    for (int i = 0; entries && i < count && i < 64; i++) out.push_back(*reinterpret_cast<const int32_t*>(entries + i * 0x18));
    return out;
}

// The version a battle resolves -1 to (rva 0x56B360): the latest.
int LatestVersion(const std::string& code)
{
    int version = -1;
    uint8_t* singleton = reinterpret_cast<uint8_t* (*)()>(g_base + kKiwiSingleton)();
    void* versions = singleton ? *reinterpret_cast<void**>(singleton + kSingletonVersions) : nullptr;
    const FStringArg codeArg(WideOf(code));
    if (versions) reinterpret_cast<bool (*)(void*, const void*, int*)>(g_base + kResolveCharaVersion)(versions, codeArg.Get(), &version);
    return version;
}

// The final game's balance version of each playable character: the last
// entry of its version list on an up-to-date install (battle version 26).
// Sessions play these (the host writes them into the match setup) instead of
// whatever each game takes as its latest: two testers' games took Season 4
// versions (42, 43) as their latest, though their game files were the same
// as everyone's (RIP, 2026-11-03).
const std::map<std::string, int> kFinalVersions = {
    {"RYU", 60}, {"CNL", 61}, {"KEN", 61}, {"KRN", 60}, {"ZGF", 61}, {"DSM", 61}, {"NSH", 61}, {"BLR", 61},
    {"JRI", 61}, {"BRD", 60}, {"RMK", 60}, {"RSD", 60}, {"FAN", 60}, {"LAR", 60}, {"NCL", 61}, {"CMY", 60},
    {"GUL", 60}, {"IBK", 61}, {"BSN", 60}, {"URN", 60}, {"ALX", 61}, {"NDK", 60}, {"VEG", 60}, {"Z20", 61},
    {"Z21", 61}, {"Z22", 61}, {"Z23", 61}, {"Z24", 60}, {"Z25", 61}, {"Z26", 60}, {"Z27", 61}, {"Z28", 60},
    {"Z29", 61}, {"Z30", 61}, {"Z31", 60}, {"Z32", 60}, {"Z33", 60}, {"Z34", 60}, {"Z35", 61}, {"Z36", 62},
    {"Z37", 61}, {"Z38", 60}, {"Z39", 60}, {"Z40", 60}, {"Z41", 61}, {"Z42", 61},
};

// This game's version lists, read once on the game thread when its
// character table is there (they do not change while it runs), for checks
// from other threads.
std::mutex g_localVersionsMutex;
std::map<std::string, std::vector<int>> g_localVersions;
std::atomic<bool> g_localVersionsRead{false};
std::string g_localVersionsText = "not read yet";

void ReadLocalVersions()
{
    if (CharaCount() <= 0 || LatestVersion("RYU") < 0) return;
    std::map<std::string, std::vector<int>> lists;
    for (int i = 0; i < CharaCount(); i++) {
        const std::string code = CharaCode(i);
        if (!code.empty()) lists[code] = VersionsOf(code);
    }
    std::string older;
    int olderCount = 0;
    for (const auto& [code, final] : kFinalVersions) {
        const auto it = lists.find(code);
        const int newest = it == lists.end() || it->second.empty() ? -1 : it->second.back();
        if (newest == final) continue;
        olderCount++;
        older += (older.empty() ? "" : ", ") + code + " " + std::to_string(newest) + "/" + std::to_string(final);
    }
    const std::string text = olderCount == 0 ? "the final game's"
                                             : std::to_string(olderCount) + " not the final game's (newest/final): " + older;
    Log("flow: character versions: %s", text.c_str());
    {
        std::lock_guard lock(g_localVersionsMutex);
        g_localVersions = std::move(lists);
        g_localVersionsText = text;
    }
    g_localVersionsRead = true;
}

// The version a match plays: the setup's, else the final game's when this
// game has it, else this game's latest.
int MatchVersion(const std::string& code, int version)
{
    if (version >= 0) return version;
    const int final = FinalCharaVersion(code);
    return final >= 0 && HasCharaVersion(code, final) ? final : LatestVersion(code);
}

// Whether the character cache has a slot for exactly this character.
bool InCharaCache(const std::string& code, int costume, int version, int y)
{
    const auto* cache = reinterpret_cast<const uint8_t* (*)()>(g_base + kCharaCache)();
    if (!cache) return false;
    const auto slots = *reinterpret_cast<uint8_t* const* const*>(cache + kCacheSlotsInUse);
    const int inUse = *reinterpret_cast<const int32_t*>(cache + kCacheSlotsInUse + 8);
    for (int i = 0; i < inUse && slots; i++) {
        const uint8_t* slot = slots[i];
        if (FStringText(slot + 8) == code && *reinterpret_cast<const int32_t*>(slot + 0x18) == costume &&
            *reinterpret_cast<const int32_t*>(slot + 0x1C) == version && *reinterpret_cast<const int32_t*>(slot + 0x20) == y) {
            return true;
        }
    }
    return false;
}

std::atomic<bool> g_preloading{false};       // a preload holds the asset queue
std::atomic<uint64_t> g_jumpRequestedAt{0};  // queue run (≈ frame) of the last jump request
std::mutex g_preloadMutex;
std::function<std::vector<PreloadPick>()> g_preloadSource;
std::string g_preloadSeen;                   // the source's picks as last seen
uint64_t g_preloadSeenAt = 0;                // ... since this queue run
std::map<std::string, int> g_preloadTries;   // per character key, preloads started
std::atomic<bool> g_preloadsDone{false};     // every pick of the source is in the cache (or tried enough)
std::string g_preloadsDoneFor;               // ... the picks that was checked for (PreloadsDoneFor)
std::map<std::string, int> g_preloadStatus;  // per "code/costume" of the source: PreloadStatus's 2, 0 or -1
std::string g_preloadingKey;                 // the preload running now ("code/costume")
std::chrono::steady_clock::time_point g_preloadingSince;
constexpr int kPreloadStableFrames = 60;     // picks unchanged this long before preloading
constexpr int kPreloadMaxTries = 2;          // per key, so one that never stays cached is not loaded forever

// The cache's other arguments: what battle creation passed last (the stage
// code and -1 in Versus), or the same defaults before any battle.
void CacheArgs(std::wstring* x, int* y)
{
    std::lock_guard lock(g_cacheArgsMutex);
    *x = g_cacheArgsSeen ? g_cacheX : L"S25";
    *y = g_cacheArgsSeen ? g_cacheY : -1;
}

// On the game thread: loads one character through the asset queue as battle
// setup step 2 does (rva 0x69BC20), then, once the queue is idle, asks the
// character cache for it so it stays loaded (the cache only holds what is
// loaded). False if the queue is busy.
bool StartPreload(const std::string& code, int costume, int setupVersion, const char* why)
{
    uint8_t* queue = AssetQueue();
    if (!queue || g_preloading || *reinterpret_cast<int32_t*>(queue + kQueueState) != 0) return false;
    std::wstring x;
    int y = 0;
    CacheArgs(&x, &y);
    const int version = MatchVersion(code, setupVersion);
    if (InCharaCache(code, costume, version, y)) {
        Log("flow: preload %s costume %d (%s): already loaded", code.c_str(), costume, why);
        return true;
    }
    Log("flow: preloading %s costume %d version %d (%s)", code.c_str(), costume, version, why);
    g_preloading = true;
    {
        std::lock_guard lock(g_preloadMutex);
        g_preloadingKey = code + "/" + std::to_string(costume);
        g_preloadingSince = std::chrono::steady_clock::now();
    }
    {
        const FStringArg codeArg(WideOf(code));
        const FStringArg xArg(x);
        reinterpret_cast<void (*)(void*, const void*, int, int, const void*, int)>(g_base + kQueueAddChara)(
            queue, codeArg.Get(), costume, version, xArg.Get(), y);
    }
    reinterpret_cast<void (*)(void*, int)>(g_base + kQueueStart)(queue, kQueueLoadBattle);
    struct Waiter {
        static void Step(std::string code, int costume, int version, std::wstring x, int y, uint8_t* queue,
                         std::chrono::steady_clock::time_point started, int frames)
        {
            if (*reinterpret_cast<int32_t*>(queue + kQueueState) != 0 && frames < 60 * 60) {
                RunOnGameThreadAfter(1, [=] { Step(code, costume, version, x, y, queue, started, frames + 1); });
                return;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (void* cache = reinterpret_cast<void* (*)()>(g_base + kCharaCache)()) {
                const FStringArg codeArg(WideOf(code));
                const FStringArg xArg(x);
                CharaCacheRequestDetour(cache, codeArg.Get(), costume, version, xArg.Get(), y);
            }
            Log("flow: preloaded %s costume %d in %.1f s (%d frames)%s", code.c_str(), costume, seconds, frames,
                InCharaCache(code, costume, version, y) ? "" : "; NOT in the character cache");
            {
                std::lock_guard lock(g_preloadMutex);
                g_preloadingKey.clear();
            }
            g_preloading = false;
        }
    };
    Waiter::Step(code, costume, version, x, y, queue, std::chrono::steady_clock::now(), 0);
    return true;
}

// From the scene tick: preloads the source's picks at the main menu once
// they have not changed for a moment, one at a time, never while a jump is
// on its way.
void PreloadTick()
{
    // The source runs outside g_preloadMutex: it takes the netplay driver's
    // lock, and the driver's thread asks PreloadsDoneFor while holding that
    // one. Called under it, the two waited on each other (a host's game froze
    // a second into a match, 2026-11-01).
    std::function<std::vector<PreloadPick>()> source;
    {
        std::lock_guard lock(g_preloadMutex);
        source = g_preloadSource;
    }
    if (!g_localVersionsRead) ReadLocalVersions();
    std::vector<PreloadPick> picks;
    if (source) picks = source();
    std::string seen;
    for (const auto& p : picks) seen += p.code + "/" + std::to_string(p.costume) + " ";
    const uint64_t now = g_pumps;
    {
        std::wstring x;
        int y = 0;
        CacheArgs(&x, &y);
        bool done = !g_preloading;
        std::map<std::string, int> status;
        for (const auto& p : picks) {
            const std::string key = p.code + "/" + std::to_string(p.costume);
            const bool cached = !p.code.empty() && InCharaCache(p.code, p.costume, MatchVersion(p.code, p.version), y);
            const bool tried = g_preloadTries[key] >= kPreloadMaxTries;
            status[key] = cached ? 2 : tried ? -1 : 0;
            done = done && (p.code.empty() || cached || tried);
        }
        // Preloads run at the main menu only. In the lobby over a held results
        // screen a new pick cannot be preloaded: waiting for it would only hold
        // the match back (the jump loads it).
        if (LastScene() != "MainMenuState" && !g_preloading) done = true;
        std::lock_guard lock(g_preloadMutex);
        g_preloadsDone = done;
        g_preloadsDoneFor = seen;
        g_preloadStatus = std::move(status);
    }
    if (seen != g_preloadSeen) {
        g_preloadSeen = seen;
        g_preloadSeenAt = now;
        return;
    }
    // Why nothing starts, logged when it changes (diagnostics).
    static std::string lastWhy;
    auto wait = [&](const std::string& why) {
        if (why != lastWhy && !picks.empty()) Log("flow: preload of %swaits: %s", seen.c_str(), why.c_str());
        lastWhy = why;
    };
    if (picks.empty() || g_preloading || now - g_preloadSeenAt < kPreloadStableFrames) return;
    if (LastScene() != "MainMenuState") return wait("scene " + LastScene());
    if (now - g_sceneChangedAt < kSceneSettleFrames * 2) return wait("scene settling");
    if (g_jumpRequestedAt && now - g_jumpRequestedAt < kSceneSettleFrames * 10) return wait("a jump was requested");
    std::wstring x;
    int y = 0;
    CacheArgs(&x, &y);
    for (const auto& p : picks) {
        if (p.code.empty() || InCharaCache(p.code, p.costume, MatchVersion(p.code, p.version), y)) {
            wait("loaded");
            continue;
        }
        int& tries = g_preloadTries[p.code + "/" + std::to_string(p.costume)];
        if (tries >= kPreloadMaxTries) {
            wait("tried enough");
            continue;
        }
        if (StartPreload(p.code, p.costume, p.version, "pick")) {
            tries++;
            return;
        }
        static int loggedState = 0;
        const uint8_t* queue = AssetQueue();
        const int state = queue ? *reinterpret_cast<const int32_t*>(queue + kQueueState) : -1;
        if (state != loggedState) Log("flow: preload %s waits for the asset queue (state %d)", p.code.c_str(), state);
        loggedState = state;
        return;
    }
}

} // namespace

void SetPreloadSource(std::function<std::vector<PreloadPick>()> source)
{
    std::lock_guard lock(g_preloadMutex);
    g_preloadSource = std::move(source);
}

bool PreloadsDone() { return g_preloadsDone; }

bool PreloadsDoneFor(const std::string& picks)
{
    std::lock_guard lock(g_preloadMutex);
    return g_preloadsDone && g_preloadsDoneFor == picks;
}

int PreloadStatus(const std::string& code, int costume, double* seconds)
{
    const std::string key = code + "/" + std::to_string(costume);
    std::lock_guard lock(g_preloadMutex);
    *seconds = 0;
    if (g_preloadingKey == key) {
        *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_preloadingSince).count();
        return 1;
    }
    const auto it = g_preloadStatus.find(key);
    return it == g_preloadStatus.end() ? -1 : it->second;
}

void PreloadCharacter(const std::string& code, int costume)
{
    RunOnGameThread([code, costume] {
        if (!StartPreload(code, costume, -1, "command")) Log("flow: preload %s: the asset queue is busy", code.c_str());
    });
}

void PreloadCharacters(std::vector<PreloadPick> picks, std::function<void()> then)
{
    // One at a time (a preload holds the asset queue), each once the queue
    // is free, then `then`: a jump made only then cannot start between two.
    struct Chain {
        static void Step(std::vector<PreloadPick> picks, size_t next, std::function<void()> then, int frames)
        {
            if (next == picks.size() && !g_preloading) {
                if (then) then();
                return;
            }
            if (frames >= 60 * 60) {
                Log("flow: preloads gave up waiting for the asset queue");
                if (then) then();
                return;
            }
            // Right after the game starts its character table is not loaded
            // yet (no version known): a preload then never ends.
            if (next < picks.size() && LatestVersion(picks[next].code) >= 0 &&
                StartPreload(picks[next].code, picks[next].costume, picks[next].version, "replay")) {
                next++;
            }
            RunOnGameThreadAfter(1, [=] { Step(picks, next, then, frames + 1); });
        }
    };
    RunOnGameThread([picks = std::move(picks), then = std::move(then)] { Chain::Step(picks, 0, then, 0); });
}

void LogCharaVersions()
{
    RunOnGameThread([] {
        for (int i = 0; i < CharaCount(); i++) {
            const std::string code = CharaCode(i);
            if (code.empty()) continue;
            std::string list;
            for (int v : VersionsOf(code)) list += " " + std::to_string(v);
            Log("flow: versions %s:%s (latest %d)", code.c_str(), list.c_str(), LatestVersion(code));
        }
    });
}

int LatestCharaVersion(const std::string& code) { return LatestVersion(code); }

int FinalCharaVersion(const std::string& code)
{
    const auto it = kFinalVersions.find(code);
    return it == kFinalVersions.end() ? -1 : it->second;
}

bool HasCharaVersion(const std::string& code, int version)
{
    if (version < 0 || !g_localVersionsRead) return true;
    std::lock_guard lock(g_localVersionsMutex);
    const auto it = g_localVersions.find(code);
    if (it == g_localVersions.end()) return true;
    return std::find(it->second.begin(), it->second.end(), version) != it->second.end();
}

int NewestLocalVersion(const std::string& code)
{
    std::lock_guard lock(g_localVersionsMutex);
    const auto it = g_localVersions.find(code);
    return it == g_localVersions.end() || it->second.empty() ? -1 : it->second.back();
}

std::string CharaVersionsText()
{
    std::lock_guard lock(g_localVersionsMutex);
    return g_localVersionsText;
}

namespace {

// The player's voice settings (Options, Sound): KWOptionSettingsSave, the
// one in GameSystemSave (its class default has one too). +0x40
// VoiceLanguageMode (Japanese, English, Custom), +0xC0
// CharacterVoiceLanguage (TArray<uint8>, count +0xC8): the language each
// character speaks, by its unique id, what the menus' character select
// passes to the battle (GetCharaVoiceLanguageFromCharaCode, rva 0x78A450).
constexpr uint32_t kOptionVoiceMode = 0x40;
constexpr uint32_t kOptionCharaVoices = 0xC0;
std::atomic<int> g_voiceOverride{-1};

const uint8_t* OptionSettings()
{
    static const void* found = nullptr;
    if (found && ue4::ObjectName(found) == "OptionSettingsSave") return static_cast<const uint8_t*>(found);
    found = nullptr;
    if (!g_base || !ue4::EnsureObjects(g_base)) return nullptr;
    ue4::ForEachObject([](const void* o) {
        if (found || ue4::ObjectName(o) != "OptionSettingsSave") return;
        const void* outer = ue4::OuterOf(o);
        if (outer && ue4::ObjectName(outer) == "GameSystemSave" && ue4::ObjectName(ue4::ClassOf(o)) == "KWOptionSettingsSave") found = o;
    });
    return static_cast<const uint8_t*>(found);
}

} // namespace

int VoiceLanguageFor(const std::string& code)
{
    if (const int forced = g_voiceOverride; forced >= 0) return forced;
    const uint8_t* options = OptionSettings();
    if (!options || !IsReadable(options + kOptionVoiceMode, kOptionCharaVoices + 0x10 - kOptionVoiceMode)) return -1;
    const int mode = options[kOptionVoiceMode];
    uint8_t* singleton = reinterpret_cast<uint8_t* (*)()>(g_base + kKiwiSingleton)();
    void* versions = singleton ? *reinterpret_cast<void**>(singleton + kSingletonVersions) : nullptr;
    const FStringArg codeArg(WideOf(code));
    const uint8_t* info = versions ? reinterpret_cast<uint8_t* (*)(void*, const void*)>(g_base + kCharaVersionInfo)(versions, codeArg.Get()) : nullptr;
    const int id = info ? *reinterpret_cast<const int32_t*>(info + 0x14) : -1;
    const auto* langs = *reinterpret_cast<const uint8_t* const*>(options + kOptionCharaVoices);
    const int count = *reinterpret_cast<const int32_t*>(options + kOptionCharaVoices + 8);
    if (langs && id >= 0 && id < count && IsReadable(langs + id, 1)) return langs[id];
    return mode <= 1 ? mode : -1; // Custom without an entry: leave it
}

void SetVoiceLanguageOverride(int language) { g_voiceOverride = language; }

void LogCharaCache()
{
    RunOnGameThread([] {
        const auto* cache = reinterpret_cast<const uint8_t* (*)()>(g_base + kCharaCache)();
        if (!cache) return;
        const auto slots = *reinterpret_cast<uint8_t* const* const*>(cache + kCacheSlotsInUse);
        const int inUse = *reinterpret_cast<const int32_t*>(cache + kCacheSlotsInUse + 8);
        const int free = *reinterpret_cast<const int32_t*>(cache + kCacheSlotsFree + 8);
        Log("flow: character cache %p: %d slots in use (oldest first), %d free; battle data %p", static_cast<const void*>(cache), inUse, free, static_cast<void*>(BattleData()));
        for (int i = 0; i < inUse && slots; i++) {
            const uint8_t* slot = slots[i];
            const auto* queue = *reinterpret_cast<const uint8_t* const*>(slot);
            Log("  %s costume %d version %d y %d, queue state %d", FStringText(slot + 8).c_str(),
                *reinterpret_cast<const int32_t*>(slot + 0x18), *reinterpret_cast<const int32_t*>(slot + 0x1C),
                *reinterpret_cast<const int32_t*>(slot + 0x20), queue ? *reinterpret_cast<const int32_t*>(queue + 8) : -1);
        }
    });
}

void SetPendingStage(const std::string& code)
{
    std::lock_guard lock(g_stageMutex);
    g_pendingStage = code;
}

std::string PendingStage()
{
    std::lock_guard lock(g_stageMutex);
    return g_pendingStage;
}

std::string JumpStageCode()
{
    std::lock_guard lock(g_stageMutex);
    return g_jumpStage;
}

std::atomic<int> g_jumpGeneration{0};
std::atomic<int> g_jumpRetries{0};
constexpr int kJumpRetries = 5;

void ReturnToMainMenu(int delayFrames)
{
    const int generation = g_jumpGeneration;
    RunOnGameThreadAfter(delayFrames, [generation] {
        if (generation != g_jumpGeneration) return; // a new battle is on its way
        {
            std::lock_guard lock(g_sceneMutex);
            if (g_lastScene == "MainMenuState") return;
        }
        ChangeScene("MainMenuState");
    });
}

std::string CurrentScene() { return LastScene(); }

uint64_t BattleSceneSwitches() { return g_battleSwitches; }

void JumpToBattle()
{
    // Netplay and replays set what is played first. A bare jump (`battlenow`,
    // the panel's Battle now) plays Ryu against Ken in local Versus: the
    // defaults a jump builds are not Versus (infinite time, P2 a CPU), and
    // the battle HUD's branch for them reads an object a jump from the menu
    // does not have (null from rva 0x521EB0, read at rva 0x2FF130): the
    // game crashed.
    if (!HasPendingSetup()) {
        MatchSetup setup;
        setup.players[0].character = PackCode("RYU");
        setup.players[1].character = PackCode("KEN");
        setup.stage = kKeepStage;
        SetPendingSetup(setup);
        Log("flow: jump to battle without a match setup: Ryu against Ken in local Versus");
    }
    g_jumpGeneration++;
    g_jumpRequestedAt = g_pumps.load();
    g_jumpRetries = 0;
    // Names are needed to find the scene objects; right after loading, the
    // table may still be being located.
    if (!ue4::Ready()) {
        Log("flow: jump to battle waits for the name table");
        std::thread([] {
            ThreadGuard running;
            for (int i = 0; i < 300 && !ue4::Ready() && !Stopping(); i++) Sleep(100);
            if (ue4::Ready()) JumpToBattle();
            else Log("flow: no name table; cannot jump");
        }).detach();
        return;
    }
    RunOnGameThread([] { JumpOnGameThread(0); });
}

namespace {

// A class (or other long-lived object) by name, looked up once: a lookup
// walks every object (about 130k), far too slow to repeat every frame.
void* CachedObject(const std::string& name, const std::string& className)
{
    static std::mutex mutex;
    static std::map<std::string, void*> cache;
    std::lock_guard lock(mutex);
    const std::string key = className + "/" + name;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    void* object = ue4::EnsureObjects(g_base) ? ue4::FindObject(name, className) : nullptr;
    if (object) cache[key] = object;
    return object;
}

uint8_t* BattleFlow()
{
    if (!g_base) return nullptr;
    const auto engine = *reinterpret_cast<uint8_t* const*>(g_base + kEngineGlobal);
    uint8_t* singleton = engine ? *reinterpret_cast<uint8_t* const*>(engine + kSingleton) : nullptr;
    uint8_t* flow = singleton && IsReadable(singleton + kSingletonBattleFlow, 8)
                        ? *reinterpret_cast<uint8_t* const*>(singleton + kSingletonBattleFlow)
                        : nullptr;
    return flow && IsReadable(flow, 0x20) ? flow : nullptr;
}

uint8_t* BattleLoader()
{
    if (!g_base) return nullptr;
    const auto engine = *reinterpret_cast<uint8_t* const*>(g_base + kEngineGlobal);
    uint8_t* singleton = engine ? *reinterpret_cast<uint8_t* const*>(engine + kSingleton) : nullptr;
    uint8_t* flow = singleton && IsReadable(singleton + kSingletonBattleFlow, 8)
                        ? *reinterpret_cast<uint8_t* const*>(singleton + kSingletonBattleFlow)
                        : nullptr;
    uint8_t* loader = flow && IsReadable(flow, 0x18) ? *reinterpret_cast<uint8_t* const*>(flow + kBattleFlowLoader) : nullptr;
    return loader && IsReadable(loader, 0x40) ? loader : nullptr;
}

} // namespace

// The jump, on the game thread. `setupFrames` counts the frames since this
// jump switched to Versus battle setup (0: not switched).
void JumpOnGameThread(int setupFrames)
{
    {
        void* stateClass = CachedObject("VersusBattleState", "Class");
        void* manager = g_manager;
        if (!manager) g_manager = manager = ue4::FindObject("GameScene_0", "GameScene");
        if (!setupFrames) {
            Log("flow: jump to battle: manager %s (%p), state class %s (%p)", ue4::Describe(manager).c_str(), manager,
                ue4::Describe(stateClass).c_str(), stateClass);
        }
        if (!manager || !stateClass || !g_origChange) return;
        // A preload holds the asset queue the battle loads with: let it end
        // (its character is probably in this battle anyway).
        if (!setupFrames && g_preloading) {
            static uint64_t loggedAt = 0;
            if (g_pumps - loggedAt > 60) Log("flow: jump to battle: waiting for a preload to end");
            loggedAt = g_pumps;
            RunOnGameThreadAfter(1, [] { JumpOnGameThread(0); });
            return;
        }
        // A scene change still settling (for example back to the main menu
        // after a match): switching again now crashed the game.
        if (!setupFrames && g_pumps - g_sceneChangedAt < kSceneSettleFrames) {
            RunOnGameThreadAfter(1, [] { JumpOnGameThread(0); });
            return;
        }
        // Outside Versus (main menu, other modes) there is no battle loader:
        // the Versus battle setup state (character and stage select) creates
        // it, as the Versus menu does with its init message (the class
        // default: plain Versus). A loader left over from the last battle
        // does not count there.
        const std::string scene = LastScene();
        const bool inVersus = scene == "VersusBattleState" || scene == "VersusBattleSetupState";
        uint8_t* loader = setupFrames || inVersus ? BattleLoader() : nullptr;
        // The setup state creates the loader early, then loads character
        // select (not a loader step): give it a few frames after the loader
        // appears.
        static int loaderSeenAt = -1;
        if (!setupFrames) loaderSeenAt = -1;
        if (setupFrames && loader && loaderSeenAt < 0) loaderSeenAt = setupFrames;
        const bool setupReady = loader && (!setupFrames || setupFrames - loaderSeenAt >= kSetupSettleFrames);
        if (!setupFrames && !loader) {
            // Straight from here: the Versus loader, as the setup scene makes
            // it, without the setup scene (character select's map and UI).
            if (uint8_t* flow = BattleFlow()) {
                reinterpret_cast<void (*)(void*, int)>(g_base + kEnsureLoader)(flow, kVersusLoaderType);
                loader = BattleLoader();
                Log("flow: jump to battle: Versus battle loader made here (%p), no setup scene", static_cast<void*>(loader));
            }
        }
        if (!setupFrames && !loader) {
            void* setupClass = CachedObject("VersusBattleSetupState", "Class");
            void* message = CachedObject("Default__VersusMenuInitMessage", "VersusMenuInitMessage");
            Log("flow: jump to battle: no battle loader here; switching to %s first", ue4::Describe(setupClass).c_str());
            if (!setupClass || !message) return;
            RememberScene("VersusBattleSetupState");
            g_origChange(manager, setupClass, message);
            RunOnGameThreadAfter(1, [] { JumpOnGameThread(1); });
            return;
        }
        if (setupFrames && setupFrames % 60 == 0) {
            Log("flow: jump to battle: waiting for battle setup (frame %d, loader %p, ready %d)", setupFrames,
                static_cast<void*>(loader), setupReady);
        }
        if (setupFrames && !setupReady) {
            if (setupFrames >= kSetupWaitFrames) {
                Log("flow: jump to battle: no battle setup after %d frames", setupFrames);
                return;
            }
            RunOnGameThreadAfter(1, [setupFrames] { JumpOnGameThread(setupFrames + 1); });
            return;
        }
        if (setupFrames) Log("flow: jump to battle: battle setup ready after %d frames", setupFrames);
        if (!loader) {
            Log("flow: jump to battle: no battle loader");
            return;
        }
        // The stage: the loader's stage request (what step 0 passes on, rva
        // 0x714DE7) holds the stage code; the battle state loads it as it
        // starts, so it is set before switching.
        std::string stage;
        {
            std::lock_guard lock(g_stageMutex);
            stage = g_pendingStage;
        }
        // A request with no characters to write into (a freshly made
        // loader's can be empty: no stage was loaded before the switch, the
        // battle loaded it in its own setup and started differently from
        // frame 0, 43c9c9cb) borrows these for the request below, then gets
        // its own back (the game frees its own buffer, never this one).
        static wchar_t lent[4];
        uint8_t* lentTo = nullptr;
        uint8_t saved[16] = {};
        if (!stage.empty()) {
            auto* holder = *reinterpret_cast<uint8_t* const*>(loader + 0x10);
            using GetFn = uint8_t* (*)(void*);
            uint8_t* request = holder ? (*reinterpret_cast<GetFn* const*>(holder))[0x70 / 8](holder) : nullptr;
            if (request && g_testEmptyStage.exchange(false) && IsReadable(request + 0x18, 4)) {
                *reinterpret_cast<int32_t*>(request + 0x18) = 0;
                Log("flow: test: the new loader's stage request emptied");
            }
            const std::string before = request ? FStringText(request + 0x10) : "";
            if (request && before.size() == 3 && stage.size() == 3) {
                auto* chars = *reinterpret_cast<wchar_t* const*>(request + 0x10);
                for (int i = 0; i < 3; i++) chars[i] = static_cast<wchar_t>(stage[i]);
                Log("flow: stage %s -> %s", before.c_str(), stage.c_str());
            }
            else if (request && stage.size() == 3 && IsReadable(request + 0x10, 16)) {
                for (int i = 0; i < 3; i++) lent[i] = static_cast<wchar_t>(stage[i]);
                lent[3] = 0;
                std::memcpy(saved, request + 0x10, 16);
                const wchar_t* data = lent;
                const int32_t count = 4;
                std::memcpy(request + 0x10, &data, 8);
                std::memcpy(request + 0x18, &count, 4);
                std::memcpy(request + 0x1C, &count, 4);
                lentTo = request;
                Log("flow: stage request '%s' -> %s for this request", before.c_str(), stage.c_str());
            }
            else {
                Log("flow: cannot set the stage (request '%s')", before.c_str());
            }
        }
        {
            // What this jump loads: the pending stage, or the one prepared.
            std::string code = stage.size() == 3 ? stage : std::string{};
            if (code.empty() && loader) {
                auto* holder = *reinterpret_cast<uint8_t* const*>(loader + 0x10);
                using GetFn = uint8_t* (*)(void*);
                uint8_t* request = holder ? (*reinterpret_cast<GetFn* const*>(holder))[0x70 / 8](holder) : nullptr;
                const std::string prepared = request ? FStringText(request + 0x10) : "";
                if (prepared.size() == 3) code = prepared;
            }
            std::lock_guard lock(g_stageMutex);
            g_jumpStage = code;
        }
        // Request the stage now, as stage select does (also the one already
        // prepared: nothing else requests it), and switch once it is loaded:
        // the battle state uses the stage already prepared.
        if (g_origStep) g_origStep(loader, 0, 1);
        if (lentTo) std::memcpy(lentTo + 0x10, saved, 16);
        WaitForStageThenSwitch(manager, stateClass, loader, 0, g_sceneChangedAt.load(), g_jumpGeneration.load());
    }
}

void ChangeScene(const std::string& stateClassName, const std::string& paramName)
{
    RunOnGameThread([stateClassName, paramName] {
        void* stateClass = CachedObject(stateClassName, "Class");
        void* param = paramName.empty() ? nullptr : CachedObject(paramName, "");
        void* manager = g_manager.load();
        if (!manager) g_manager = manager = ue4::FindObject("GameScene_0", "GameScene");
        Log("flow: change scene to %s (%p) with %s on %p", stateClassName.c_str(), stateClass, ue4::Describe(param).c_str(),
            manager);
        if (!paramName.empty() && !param) return;
        if (!stateClass || !manager || !g_origChange) return;
        RememberScene(stateClassName);
        g_origChange(manager, stateClass, param);
        // (The detour is bypassed: the step trace moves on to this state, or
        // it kept reading the one just left, freed.)
        TraceSceneSteps(stateClassName);
    });
}

void WaitForStageThenSwitch(void* manager, void* stateClass, uint8_t* loader, int frames, uint64_t sceneAt, int generation)
{
    // A scene change while the stage loads (a session left and went back to
    // the menu, the loader kept) or a newer jump: this one is not wanted any
    // more. A 0.4.1 spectator stopped watching 0.3 s into a set's next jump;
    // the menu came, then this switched to the battle anyway with no setup
    // (a bare jump's defaults), whose HUD read null at rva 0x13958B0
    // (16a56551, 2026-11-12).
    if (g_sceneChangedAt != sceneAt || g_jumpGeneration != generation) {
        Log("flow: jump to battle cancelled (%s while its stage loaded)",
            g_jumpGeneration != generation ? "another jump came" : ("the scene changed to " + LastScene()).c_str());
        return;
    }
    // The scene may have moved on meanwhile (leave, back to the menu): the
    // loader is then gone.
    if (BattleLoader() != loader) {
        // Without a scene change since the jump, something reset the battle
        // flow (the main menu still setting itself up resets it too): jump
        // again. After one, the player moved on: let it be.
        if (g_sceneChangedAt < g_jumpRequestedAt && g_jumpRetries < kJumpRetries) {
            g_jumpRetries++;
            Log("flow: the battle loader went away without a scene change; jumping again (%d)", g_jumpRetries.load());
            RunOnGameThreadAfter(kSceneSettleFrames, [] { JumpOnGameThread(0); });
            return;
        }
        Log("flow: jump to battle cancelled (the battle loader went away)");
        return;
    }
    using DoneFn = bool (*)(void*, int);
    const bool loaded = reinterpret_cast<DoneFn>(g_base + kLoaderStepDone)(loader, 0);
    if (!loaded && frames < kStageWaitFrames) {
        RunOnGameThreadAfter(1, [=] { WaitForStageThenSwitch(manager, stateClass, loader, frames + 1, sceneAt, generation); });
        return;
    }
    Log("flow: stage %s after %d frames; switching to the battle", loaded ? "loaded" : "NOT loaded", frames);
    // From outside Versus the old scene's exit resets the battle flow a few
    // frames after this switch (FlowResetDetour makes the loader again).
    const std::string from = LastScene();
    g_loaderAfterReset = from != "VersusBattleState" && from != "VersusBattleSetupState";
    RememberScene("VersusBattleState");
    g_origChange(manager, stateClass, nullptr);
    g_battleSwitches++;
    StartLoaderWhenReady(0, nullptr, 0);
}

// Starts the battle loader once the battle state has run a few frames with it
// (started in the same frame, its stage request never completes). The loader
// may be replaced meanwhile (flow reset): whichever is current counts.
void StartLoaderWhenReady(int frames, uint8_t* seen, int seenAt)
{
    uint8_t* loader = BattleLoader();
    if (loader != seen) {
        seen = loader;
        seenAt = frames;
    }
    if (!loader || frames - seenAt < kLoaderStartDelay) {
        if (frames < kLoaderWaitFrames) {
            RunOnGameThreadAfter(1, [=] { StartLoaderWhenReady(frames + 1, seen, seenAt); });
        }
        else {
            g_loaderAfterReset = false;
            Log("flow: no battle loader to start after %d frames", frames);
        }
        return;
    }
    // Past the old scene's exit by now: a later reset is not ours.
    g_loaderAfterReset = false;
    auto* phase = reinterpret_cast<int32_t*>(loader + kLoaderPhase);
    Log("flow: battle loader %p was at phase %d; starting it", static_cast<void*>(loader), *phase);
    *phase = 1;
}


namespace {

const uint8_t* StageEntries(int* count)
{
    // Checked step by step: while the game starts, these point at memory that
    // is not set up yet (and before the flow probes are installed g_base is 0).
    if (!g_base) return nullptr;
    auto* engine = *reinterpret_cast<uint8_t* const*>(g_base + kEngineGlobal);
    if (!engine || !IsReadable(engine + kSingleton, 8)) return nullptr;
    uint8_t* singleton = *reinterpret_cast<uint8_t* const*>(engine + kSingleton);
    if (!singleton || !IsReadable(singleton + kSingletonStageTable, 8)) return nullptr;
    uint8_t* table = *reinterpret_cast<uint8_t* const*>(singleton + kSingletonStageTable);
    if (!table || !IsReadable(table + 0x28, 16)) return nullptr;
    *count = *reinterpret_cast<const int32_t*>(table + 0x30);
    return *reinterpret_cast<uint8_t* const*>(table + 0x28);
}

} // namespace

int StageCount()
{
    int count = 0;
    return StageEntries(&count) ? count : 0;
}

std::string StageCode(int index)
{
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    return FStringText(entries + index * kStageEntrySize);
}

bool IsPlayableStage(int index)
{
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries || index < 0 || index >= count) return false;
    const uint8_t* e = entries + index * kStageEntrySize;
    return FStringText(e).size() == 3 && !FStringText(e + 0x10).empty() && !FStringText(e + 0x20).empty();
}

namespace {

// Stage settings (the stage select's "Setting" for a stage): DA_StageArrange
// (KWStageArrangeDataAsset), ArrangeInfos at +0x28, entries of 0x18 bytes:
// BaseStageId (int) and ArrangeItems (TArray at +0x8, items of 0x30 bytes
// whose first int is the stage that setting plays, followed by its label,
// "Setting 1"). The Grid (TRN) has The Grid Alternative (S25), Shadaloo Base
// (SHA) Shadaloo Base at Night (SH4), and so on.
std::mutex g_arrangeMutex;
std::map<int, std::vector<int>> g_arrange; // base stage -> its settings' stages
bool g_arrangeRead = false;

const std::map<int, std::vector<int>>& StageArrange()
{
    std::lock_guard lock(g_arrangeMutex);
    if (g_arrangeRead || !g_base || !ue4::EnsureObjects(g_base)) return g_arrange;
    const auto* asset = static_cast<const uint8_t*>(ue4::FindObject("DA_StageArrange", "KWStageArrangeDataAsset"));
    if (!asset || !IsReadable(asset + 0x28, 16)) return g_arrange;
    g_arrangeRead = true;
    const auto* infos = *reinterpret_cast<const uint8_t* const*>(asset + 0x28);
    const int n = *reinterpret_cast<const int32_t*>(asset + 0x30);
    for (int i = 0; infos && i < n && IsReadable(infos + i * 0x18, 0x18); i++) {
        const uint8_t* info = infos + i * 0x18;
        const auto* items = *reinterpret_cast<const uint8_t* const*>(info + 8);
        const int m = *reinterpret_cast<const int32_t*>(info + 0x10);
        for (int k = 0; items && k < m && IsReadable(items + k * 0x30, 4); k++) {
            g_arrange[*reinterpret_cast<const int32_t*>(info)].push_back(*reinterpret_cast<const int32_t*>(items + k * 0x30));
        }
    }
    Log("flow: %zu stages have settings", g_arrange.size());
    return g_arrange;
}

} // namespace

std::vector<int> StageSettings(int base)
{
    const auto& arrange = StageArrange();
    const auto it = arrange.find(base);
    return it == arrange.end() ? std::vector<int>{} : it->second;
}

int StageSettingBase(int index)
{
    for (const auto& [base, settings] : StageArrange()) {
        if (std::find(settings.begin(), settings.end(), index) != settings.end()) return base;
    }
    return -1;
}

bool IsMenuStage(int index)
{
    if (!IsPlayableStage(index) || StageSettingBase(index) >= 0) return false;
    // A stage of its own: its own personal data (story variants such as IN1
    // or NY1 reuse their base stage's) and a name (RND is random select;
    // B00 has none; Ring of Arcade, S24, has one once StageNamesReady loaded it).
    int count = 0;
    const uint8_t* e = StageEntries(&count) + index * kStageEntrySize;
    const std::string code = FStringText(e);
    return code != "RND" && FStringText(e + 0x20).find("/" + code + "/") != std::string::npos && !StageName(index).empty();
}

bool IsSelectableStage(int index)
{
    if (IsMenuStage(index)) return true;
    const int base = StageSettingBase(index);
    return base >= 0 && IsMenuStage(base) && IsPlayableStage(index);
}

int RandomStage(uint64_t roll, bool ownedOnly, const std::vector<std::string>& off)
{
    std::vector<RandomStageChoice> stages;
    for (int i = 0; i < StageCount(); i++) {
        if (!IsSelectableStage(i) || (ownedOnly && !OwnsEntitlement(StageAvailableCode(i)))) continue;
        const int base = StageSettingBase(i);
        stages.push_back({i, StageCode(i), base >= 0 ? StageCode(base) : ""});
    }
    return PickRandomStage(stages, roll, off);
}

namespace {

// The character table: KWCharaEntryDataAsset, a field of the singleton (found
// by its class, once); CharaInfos at +0x28, entries of 0xE8 bytes
// (KWCharaEntryInfo: CharaCode FString +0x0, CostumeDatas TArray +0x50).
constexpr size_t kCharaEntrySize = 0xE8;
constexpr uint32_t kCharaCostumes = 0x50;
constexpr uint32_t kCharaPersonalData = 0x18;  // KWCharaPersonalDataAsset*
constexpr uint32_t kPersonalEnglishName = 0x50; // FText
std::atomic<int> g_charaTableOffset{-1};

const uint8_t* CharaEntries(int* count)
{
    if (!g_base) return nullptr;
    auto* engine = *reinterpret_cast<uint8_t* const*>(g_base + kEngineGlobal);
    uint8_t* singleton = engine && IsReadable(engine + kSingleton, 8) ? *reinterpret_cast<uint8_t* const*>(engine + kSingleton) : nullptr;
    if (!singleton || !IsReadable(singleton, 0x300)) return nullptr;
    if (g_charaTableOffset < 0) {
        for (int off = 0x28; off < 0x300; off += 8) {
            const void* p = *reinterpret_cast<void* const*>(singleton + off);
            if (p && IsReadable(p, 0x38) && ue4::ObjectName(ue4::ClassOf(p)) == "KWCharaEntryDataAsset") {
                g_charaTableOffset = off;
                break;
            }
        }
        if (g_charaTableOffset < 0) return nullptr;
    }
    uint8_t* table = *reinterpret_cast<uint8_t* const*>(singleton + g_charaTableOffset);
    if (!table || !IsReadable(table + 0x28, 16)) return nullptr;
    *count = *reinterpret_cast<const int32_t*>(table + 0x30);
    return *reinterpret_cast<uint8_t* const*>(table + 0x28);
}

} // namespace

int CharaCount()
{
    int count = 0;
    return CharaEntries(&count) ? count : 0;
}

std::string CharaCode(int index)
{
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    return FStringText(entries + index * kCharaEntrySize);
}

std::string CharaName(int index)
{
    // KWCharaEntryInfo +0x18 PersonalData (KWCharaPersonalDataAsset),
    // EnglishName (FText) at +0x50; an FText starts with a pointer to its
    // display string (FString).
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    const uint8_t* e = entries + index * kCharaEntrySize;
    const uint8_t* personal = IsReadable(e + kCharaPersonalData, 8) ? *reinterpret_cast<const uint8_t* const*>(e + kCharaPersonalData) : nullptr;
    if (!personal || !IsReadable(personal + kPersonalEnglishName, 8)) return "";
    const uint8_t* display = *reinterpret_cast<const uint8_t* const*>(personal + kPersonalEnglishName);
    return display ? FStringText(display) : "";
}

// Research (`costumes <code>`): per costume, its data asset path and the
// colours its colour preview asset (KWCharaCostumeColorPreviewDataAsset,
// PreviewColorList +0x28, PreviewSpecialColorList +0x38) lists; then the
// sponsored costume table and the general story costume table rows.
void LogCharaCostumes(const std::string& code)
{
    if (!ue4::EnsureObjects(g_base)) return;
    std::map<std::string, std::pair<int, int>> previews;
    std::vector<const uint8_t*> sponsored, general;
    ue4::ForEachObject([&](const void* object) {
        const std::string kind = ue4::ObjectName(ue4::ClassOf(object));
        const auto* o = static_cast<const uint8_t*>(object);
        if (kind == "KWCharaCostumeColorPreviewDataAsset" && IsReadable(o + 0x28, 0x20)) {
            previews[ue4::ObjectName(object)] = {*reinterpret_cast<const int32_t*>(o + 0x30), *reinterpret_cast<const int32_t*>(o + 0x40)};
        }
        if ((kind == "KWCostumeDataAsset" || kind == "KWBattleCharaCostumeDataAsset") && IsReadable(o + 0x98, 0x20)) {
            // CostumeMaterialPresetList +0x98, SponsordMaterialPresetList +0xA8 (TArrays).
            // CustomizeCluster +0x50 (KWColorCustomizeClusterDataAsset): GetMaxColorNum is the
            // largest colour list of its clusters (+0x28, 0x50 bytes each, count at +0x18).
            const auto* cluster = *reinterpret_cast<const uint8_t* const*>(o + 0x50);
            int maxColors = -1, maxSpecial = -1;
            if (cluster && IsReadable(cluster + 0x28, 16)) {
                const auto* items = *reinterpret_cast<const uint8_t* const*>(cluster + 0x28);
                for (int j = 0; items && j < *reinterpret_cast<const int32_t*>(cluster + 0x30); j++) {
                    maxColors = std::max(maxColors, *reinterpret_cast<const int32_t*>(items + j * 0x50 + 0x18));
                    maxSpecial = std::max(maxSpecial, *reinterpret_cast<const int32_t*>(items + j * 0x50 + 0x28));
                }
            }
            Log("costumes: loaded %s: material presets %d, sponsored %d, max colours %d, special %d", ue4::ObjectName(object).c_str(),
                *reinterpret_cast<const int32_t*>(o + 0xA0), *reinterpret_cast<const int32_t*>(o + 0xB0), maxColors, maxSpecial);
        }
        if (kind == "KWCharaSponsoredCostumeDataAsset") sponsored.push_back(o);
        if (kind == "KWGeneralCostumeAvailableCheckDataAsset") general.push_back(o);
    });
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    for (int i = 0; entries && i < count; i++) {
        if (CharaCode(i) != code) continue;
        const uint8_t* e = entries + i * kCharaEntrySize;
        const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + 0x50);
        const int num = *reinterpret_cast<const int32_t*>(e + 0x58);
        const auto* colors = *reinterpret_cast<const uint8_t* const*>(e + 0x70);
        const int colorNum = *reinterpret_cast<const int32_t*>(e + 0x78);
        if (!previews.empty()) Log("costumes: %zu colour previews, first %s", previews.size(), previews.begin()->first.c_str());
        Log("costumes %s (prog id %d, unique id %d): %d", code.c_str(), *reinterpret_cast<const int32_t*>(e + 0x10),
            *reinterpret_cast<const int32_t*>(e + 0x14), num);
        for (int k = 0; k < num; k++) {
            const std::string preview = colors && k < colorNum ? FStringText(colors + k * 16) : "";
            // "RYU01" names DA_RYU_CostumeColorPreview_01.
            auto it = preview.size() == 5 ? previews.find("DA_" + preview.substr(0, 3) + "_CostumeColorPreview_" + preview.substr(3))
                                          : previews.end();
            Log("costumes %s:   [%d] %s variants %d colors %s %d special %d", code.c_str(), k, FStringText(paths + k * 16).c_str(),
                CostumeVariants(i, k),
                it == previews.end() ? preview.c_str() : it->first.c_str(),
                it == previews.end() ? -1 : it->second.first, it == previews.end() ? -1 : it->second.second);
        }
        for (const uint8_t* o : sponsored) {
            const auto* rows = *reinterpret_cast<const uint8_t* const*>(o + 0x28);
            for (int r = 0; rows && r < *reinterpret_cast<const int32_t*>(o + 0x30); r++) {
                const uint8_t* row = rows + r * 0x18;
                if (*reinterpret_cast<const int32_t*>(row) != *reinterpret_cast<const int32_t*>(e + 0x10) &&
                    *reinterpret_cast<const int32_t*>(row) != *reinterpret_cast<const int32_t*>(e + 0x14)) continue;
                std::string similar;
                const auto* ids = *reinterpret_cast<const int32_t* const*>(row + 8);
                for (int j = 0; ids && j < *reinterpret_cast<const int32_t*>(row + 0x10); j++) similar += " " + std::to_string(ids[j]);
                Log("costumes %s: sponsored (%s) chara %d costume %d similar%s", code.c_str(), ue4::ObjectName(o).c_str(),
                    *reinterpret_cast<const int32_t*>(row), *reinterpret_cast<const int32_t*>(row + 4), similar.c_str());
            }
        }
        for (const uint8_t* o : general) {
            const auto* rows = *reinterpret_cast<const uint8_t* const*>(o + 0x28);
            for (int r = 0; rows && r < *reinterpret_cast<const int32_t*>(o + 0x30); r++) {
                const uint8_t* row = rows + r * 0x20;
                if (FStringText(row) != code) continue;
                std::string list;
                const auto* ids = *reinterpret_cast<const int32_t* const*>(row + 0x10);
                for (int j = 0; ids && j < *reinterpret_cast<const int32_t*>(row + 0x18); j++) list += " " + std::to_string(ids[j]);
                Log("costumes %s: general story available (%s):%s", code.c_str(), ue4::ObjectName(o).c_str(), list.c_str());
            }
        }
        return;
    }
    Log("costumes: no character %s", code.c_str());
}

int CharaCostumeCount(int index)
{
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return 0;
    const uint8_t* e = entries + index * kCharaEntrySize + kCharaCostumes;
    return IsReadable(e, 16) ? *reinterpret_cast<const int32_t*>(e + 8) : 0;
}

namespace {

// FPackageName::DoesPackageExist(const FString& LongPackageName, const FGuid*,
// FString* OutFilename) (rva 0x11EE840, as DoesExistCostumeDataAsset uses it):
// whether the game's paks hold the package, without loading it.
constexpr uint32_t kDoesPackageExist = 0x11EE840;
constexpr uint8_t kDoesPackageExistBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                              0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};

bool PackageExists(const std::string& path)
{
    using ExistsFn = bool (*)(const void* name, const void* guid, void* filename);
    static const bool ok = g_base && std::memcmp(reinterpret_cast<const void*>(g_base + kDoesPackageExist),
                                                  kDoesPackageExistBytes, sizeof(kDoesPackageExistBytes)) == 0;
    if (!ok) return false;
    const FStringArg name(std::wstring(path.begin(), path.end()));
    return reinterpret_cast<ExistsFn>(g_base + kDoesPackageExist)(name.Get(), nullptr, nullptr);
}

} // namespace

uint8_t CostumeVariants(int index, int costume)
{
    // What the menus give for buttons held at load. Most set the costume's
    // dress state 1, which plays one of two kinds of variant: a dress state
    // asset next to its battle data
    // (/Game/Chara/RYU/SkelMesh/07/DataAsset/DA_RYU_Costume_07_DressState_01;
    // its DressStateCostumeDatas, +0xD8, lists it once loaded), asked of the
    // paks, or its material preset's variant entry (entry 6, "01_着脱"), which
    // only a load shows, so those come from variant_table.h. Urien's thong
    // and Akira's helmet are unique params instead (battle_setup.cpp).
    static std::mutex mutex;
    static std::map<std::string, uint8_t> known;
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return 0;
    const uint8_t* e = entries + index * kCharaEntrySize;
    const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
    if (!paths || costume < 0 || costume >= *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8)) return 0;
    const std::string path = FStringText(paths + costume * 16);
    std::lock_guard lock(mutex);
    if (const auto it = known.find(path); it != known.end()) return it->second;
    auto listed = [&path](const auto& table) {
        return std::any_of(std::begin(table), std::end(table), [&path](const char* p) { return path == p; });
    };
    uint8_t variants = listed(variant_table::kPresetVariants) ? kVariantDress : 0;
    if (listed(variant_table::kUrienThong) || listed(variant_table::kAkiraHelmet)) variants |= kVariantSpecial;
    // Most are named after the costume's asset; a few after the character's
    // folder instead (Chun-Li's Onitsuka Tiger, DA_Z02_Costume_23, has
    // DA_CNL_Costume_23_DressState_01).
    std::vector<std::string> bases{path};
    const size_t slash = path.find('/'), name = path.rfind("/DA_");
    if (slash != std::string::npos && name != std::string::npos && path.size() > name + 8 && path[name + 7] == '_') {
        std::string other = path;
        other.replace(name + 4, 3, path.substr(0, slash));
        if (other != path) bases.push_back(other);
    }
    // (No costume has a second dress state: the paks hold only _01s.)
    for (const std::string& base : bases) {
        if (PackageExists("/Game/Chara/" + base + "_DressState_01")) variants |= kVariantDress;
    }
    known[path] = variants;
    return variants;
}

const char* SpecialVariantName(const std::string& code)
{
    if (code == "URN") return "Thong";
    if (code == "Z41") return "Helmet off";
    return nullptr;
}

std::string VariantLabel(const std::string& code, uint8_t variation)
{
    const char* special = SpecialVariantName(code);
    const bool dress = variation & kVariantDress, other = (variation & kVariantSpecial) && special;
    if (dress && other) return std::string("Variant + ") + special;
    if (other) return special;
    return dress ? "Variant" : "Normal";
}

namespace {

// Colour preview assets (KWCharaCostumeColorPreviewDataAsset, loaded with the
// game) by name, with their PreviewColorList's length (TArray at +0x28): the
// colours a costume offers. A costume names its preview in the character
// entry's CostumeColorPreviewDatas (+0x70): "RYU01" for
// DA_RYU_CostumeColorPreview_01.
std::mutex g_previewMutex;
std::map<std::string, int> g_previewColors;
std::map<std::string, const uint8_t*> g_previewAssets; // the same assets, by name
bool g_previewsRead = false;

int PreviewColors(const std::string& name)
{
    std::lock_guard lock(g_previewMutex);
    if (!g_previewsRead && ue4::EnsureObjects(g_base)) {
        g_previewsRead = true;
        ue4::ForEachObject([](const void* object) {
            const auto* o = static_cast<const uint8_t*>(object);
            if (ue4::ObjectName(ue4::ClassOf(object)) == "KWCharaCostumeColorPreviewDataAsset" && IsReadable(o + 0x28, 16)) {
                g_previewColors[ue4::ObjectName(object)] = *reinterpret_cast<const int32_t*>(o + 0x30);
                g_previewAssets[ue4::ObjectName(object)] = o;
            }
        });
        if (g_previewColors.empty()) Log("flow: no costume colour previews loaded");
    }
    const auto it = g_previewColors.find(name);
    return it == g_previewColors.end() ? -1 : it->second;
}

} // namespace

int CostumeColorCount(int index, int costume)
{
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return 0;
    const uint8_t* e = entries + index * kCharaEntrySize;
    const auto* keys = *reinterpret_cast<const uint8_t* const*>(e + 0x70);
    if (!keys || costume < 0 || costume >= *reinterpret_cast<const int32_t*>(e + 0x78)) return 0;
    const std::string key = FStringText(keys + costume * 16);
    if (key.size() != 5) return 0;
    const int colors = PreviewColors("DA_" + key.substr(0, 3) + "_CostumeColorPreview_" + key.substr(3));
    // The 30-37 costumes name a preview the game does not have; their colour
    // clusters (KWColorCustomizeClusterDataAsset::GetMaxColorNum) hold 2.
    return colors > 0 ? colors : 2;
}

CostumeSwatches CostumeSwatchColors(int index, int costume)
{
    // KWCharaCostumeColorPreviewDataAsset: PreviewColorList +0x28 and
    // PreviewSpecialColorList +0x38, TArrays of {FColor Color1, Color2}
    // (BGRA bytes, so 0xAARRGGBB read as one word).
    CostumeSwatches out;
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return out;
    const uint8_t* e = entries + index * kCharaEntrySize;
    const auto* keys = *reinterpret_cast<const uint8_t* const*>(e + 0x70);
    if (!keys || costume < 0 || costume >= *reinterpret_cast<const int32_t*>(e + 0x78)) return out;
    const std::string key = FStringText(keys + costume * 16);
    if (key.size() != 5) return out;
    PreviewColors(""); // reads the previews on the first call
    std::lock_guard lock(g_previewMutex);
    const auto it = g_previewAssets.find("DA_" + key.substr(0, 3) + "_CostumeColorPreview_" + key.substr(3));
    if (it == g_previewAssets.end()) return out;
    auto read = [](const uint8_t* array, std::vector<std::pair<uint32_t, uint32_t>>& to) {
        if (!IsReadable(array, 16)) return;
        const auto* data = *reinterpret_cast<const uint32_t* const*>(array);
        const int n = *reinterpret_cast<const int32_t*>(array + 8);
        if (!data || n <= 0 || n > 64 || !IsReadable(data, n * 8)) return;
        for (int k = 0; k < n; k++) to.emplace_back(data[k * 2] | 0xFF000000u, data[k * 2 + 1] | 0xFF000000u);
    };
    read(it->second + 0x28, out.colors);
    read(it->second + 0x38, out.ex);
    return out;
}

int CostumeInFolder(int index, const std::string& folder)
{
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return -1;
    const uint8_t* e = entries + index * kCharaEntrySize;
    const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
    const int n = *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8);
    const std::string part = "/SkelMesh/" + folder + "/";
    for (int k = 0; paths && k < n && k < 64; k++) {
        if (FStringText(paths + k * 16).find(part) != std::string::npos) return k;
    }
    return -1;
}

bool ElevenStandIn(uint32_t roll, std::string* code, int* costume)
{
    std::vector<std::pair<std::string, int>> fighters;
    for (int i = 0; i < CharaCount(); i++) {
        // Fighters only (random select and dummies have a handful of costumes).
        if (CharaCostumeCount(i) < 10 || CharaCode(i).rfind('X', 0) == 0) continue;
        const int k = CostumeInFolder(i, "0C");
        if (k >= 0) fighters.emplace_back(CharaCode(i), k);
    }
    if (fighters.empty()) return false;
    const auto& f = fighters[roll % fighters.size()];
    *code = f.first;
    *costume = f.second;
    return true;
}

int CharaIndex(const std::string& code)
{
    for (int i = 0; i < CharaCount(); i++) {
        if (CharaCode(i) == code) return i;
    }
    return -1;
}

namespace {

// An FText (as used here) starts with a pointer to its display string.
std::string FTextText(const uint8_t* text)
{
    if (!text || !IsReadable(text, 8)) return "";
    const uint8_t* display = *reinterpret_cast<const uint8_t* const*>(text);
    return display ? FStringText(display) : "";
}

} // namespace

// One property's value in a struct as text (research): ints, floats,
// bytes, bools, names, strings and texts; the type for anything else.
std::string PropertyText(const uint8_t* property, const uint8_t* data)
{
    const std::string type = ue4::ObjectName(ue4::ClassOf(property));
    const uint8_t* p = data + *reinterpret_cast<const int32_t*>(property + 0x4C);
    char buf[64];
    if (type == "IntProperty") return std::to_string(*reinterpret_cast<const int32_t*>(p));
    if (type == "ByteProperty") return std::to_string(*p);
    if (type == "BoolProperty") return *p ? "true" : "false";
    if (type == "FloatProperty") {
        std::snprintf(buf, sizeof(buf), "%g", *reinterpret_cast<const float*>(p));
        return buf;
    }
    if (type == "NameProperty") return ue4::NameText(*reinterpret_cast<const int32_t*>(p));
    if (type == "StrProperty") return "\"" + FStringText(p) + "\"";
    if (type == "TextProperty") return "'" + FTextText(p) + "'";
    if (type == "ArrayProperty") return "[" + std::to_string(*reinterpret_cast<const int32_t*>(p + 8)) + "]";
    return "(" + type + ")";
}

namespace {

// The offset of a row struct's property by name, or -1.
int PropertyOffset(const uint8_t* rowStruct, const char* name)
{
    const uint8_t* f = rowStruct ? *reinterpret_cast<const uint8_t* const*>(rowStruct + 0x38) : nullptr;
    for (int k = 0; f && k < 60 && IsReadable(f, 0x50); k++) {
        if (ue4::ObjectName(f) == name) return *reinterpret_cast<const int32_t*>(f + 0x4C);
        f = *reinterpret_cast<const uint8_t* const*>(f + 0x28);
    }
    return -1;
}

// The game's CharaSpec table (DA_Spec; rows of KWCharaSpec): a row per
// character (its prog id), costume (the index into its costume list) and
// color id (0.. the costume's colors, 100.. its EX colors); the costume's
// first row carries its name (CostumeText: "Battle Outfit 1", "Track Suit",
// a team's name for the team tracksuits).
std::mutex g_specMutex;
std::map<int, std::map<int, CostumeSpec>> g_specs;
bool g_specsRead = false;

void ReadCharaSpecs()
{
    if (g_specsRead || !g_base) return;
    if (!ue4::EnsureObjects(g_base)) {
        Log("flow: CharaSpec: no object list yet");
        return;
    }
    const auto* table = static_cast<const uint8_t*>(ue4::FindObject("CharaSpec", "DataTable"));
    if (!table || !IsReadable(table + 0x28, 0x28)) {
        Log("flow: CharaSpec: table %p not found or not readable", static_cast<const void*>(table));
        return;
    }
    g_specsRead = true;
    const auto* rowStruct = *reinterpret_cast<const uint8_t* const*>(table + 0x28);
    const int chara = PropertyOffset(rowStruct, "CharaID"), costume = PropertyOffset(rowStruct, "CostumeId");
    const int text = PropertyOffset(rowStruct, "CostumeText"), color = PropertyOffset(rowStruct, "ColorId");
    const int charaCode = PropertyOffset(rowStruct, "RefAvailableCode"), visibleCode = PropertyOffset(rowStruct, "RefVisibleCode");
    const int costumeCode = PropertyOffset(rowStruct, "RefCostumeAvailableCode"), colorCode = PropertyOffset(rowStruct, "RefColorAvailableCode");
    if (chara < 0 || costume < 0 || text < 0 || color < 0) {
        Log("flow: the CharaSpec table has other fields than expected");
        return;
    }
    const auto* elements = *reinterpret_cast<const uint8_t* const*>(table + 0x40);
    const int count = *reinterpret_cast<const int32_t*>(table + 0x48);
    for (int i = 0; elements && i < count; i++) {
        const auto* row = *reinterpret_cast<const uint8_t* const*>(elements + i * 0x18 + 8);
        if (!row || !IsReadable(row, color + 4)) continue;
        CostumeSpec& spec = g_specs[*reinterpret_cast<const int32_t*>(row + chara)][*reinterpret_cast<const int32_t*>(row + costume)];
        const std::string name = FTextText(row + text);
        if (spec.name.empty() && !name.empty()) spec.name = name;
        const int colorId = *reinterpret_cast<const int32_t*>(row + color);
        spec.colors.push_back(colorId);
        auto code = [row](int at) { return at >= 0 && IsReadable(row + at, 16) ? FStringText(row + at) : std::string(); };
        if (spec.charaCode.empty()) spec.charaCode = code(charaCode);
        if (spec.charaVisibleCode.empty()) spec.charaVisibleCode = code(visibleCode);
        if (spec.costumeCode.empty()) spec.costumeCode = code(costumeCode);
        spec.colorCodes[colorId] = code(colorCode);
    }
    for (auto& [c, costumes] : g_specs) {
        for (auto& [k, spec] : costumes) std::sort(spec.colors.begin(), spec.colors.end());
    }
    Log("flow: costume names and colors of %zu characters", g_specs.size());
}

// The game's Battle Lounge comments (FixedPhraseCommentSpec; rows of
// KWFixedPhraseCommentSpec: Id, CommentText with {common1}/{common2}), in the
// game's language; read once the table is loaded (at the main menu).
std::mutex g_commentMutex;
std::map<int, std::string> g_comments;
bool g_commentsRead = false;
uint64_t g_commentsTriedAt = 0;

void ReadLoungeComments()
{
    if (g_commentsRead || !g_base) return;
    const uint64_t now = GetTickCount64();
    if (g_commentsTriedAt && now - g_commentsTriedAt < 2000) return;
    g_commentsTriedAt = now;
    if (!ue4::EnsureObjects(g_base)) return;
    const auto* table = static_cast<const uint8_t*>(ue4::FindObject("FixedPhraseCommentSpec", "DataTable"));
    if (!table || !IsReadable(table + 0x28, 0x28)) return;
    g_commentsRead = true;
    const auto* rowStruct = *reinterpret_cast<const uint8_t* const*>(table + 0x28);
    const int id = PropertyOffset(rowStruct, "Id"), text = PropertyOffset(rowStruct, "CommentText");
    if (id < 0 || text < 0) {
        Log("flow: the FixedPhraseCommentSpec table has other fields than expected");
        return;
    }
    const auto* elements = *reinterpret_cast<const uint8_t* const*>(table + 0x40);
    const int count = *reinterpret_cast<const int32_t*>(table + 0x48);
    for (int i = 0; elements && i < count; i++) {
        const auto* row = *reinterpret_cast<const uint8_t* const*>(elements + i * 0x18 + 8);
        if (!row || !IsReadable(row, std::max(id, text) + 8)) continue;
        std::string s = FTextText(row + text);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        g_comments[*reinterpret_cast<const int32_t*>(row + id)] = s;
    }
    Log("flow: %zu Battle Lounge comments (room titles)", g_comments.size());
}

} // namespace

std::atomic<bool> g_commentsReading{false};

std::string LoungeCommentText(int id)
{
    // Asked for while drawing (the room titles): finding the table walks the
    // game's objects (a frame of 100 ms or more, again every 2 s until the
    // table is there), so that is done on a thread of its own; until then
    // the title is "".
    {
        std::unique_lock lock(g_commentMutex, std::try_to_lock);
        if (lock && g_commentsRead) {
            const auto it = g_comments.find(id);
            return it != g_comments.end() ? it->second : std::string();
        }
    }
    if (!g_commentsReading.exchange(true)) {
        std::thread([] {
            ThreadGuard running;
            {
                std::lock_guard lock(g_commentMutex);
                ReadLoungeComments();
            }
            g_commentsReading = false;
        }).detach();
    }
    return std::string();
}

void LogAllVariants()
{
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    int total = 0;
    for (int i = 0; entries && i < count; i++) {
        const uint8_t* e = entries + i * kCharaEntrySize;
        const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
        const int num = *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8);
        const auto specs = CharaCostumeSpecs(i);
        for (int k = 0; paths && k < num; k++) {
            const auto it = specs.find(k);
            const int n = CostumeVariants(i, k);
            total += n ? 1 : 0;
            Log("variants %s [%d] %s: %d (%s)", CharaCode(i).c_str(), k, it == specs.end() ? "(not offered)" : it->second.name.c_str(), n,
                FStringText(paths + k * 16).c_str());
        }
    }
    Log("variants: %d costumes with variants", total);
    // Loaded costumes list their dress states themselves
    // (DressStateCostumeDatas +0xD8): what the menus offer.
    if (!ue4::EnsureObjects(g_base)) return;
    ue4::ForEachObject([](const void* object) {
        const auto* o = static_cast<const uint8_t*>(object);
        if (ue4::ObjectName(ue4::ClassOf(object)) != "KWBattleCharaCostumeDataAsset" || !IsReadable(o + 0xD8, 16)) return;
        Log("variants: loaded %s lists %d", ue4::ObjectName(object).c_str(), *reinterpret_cast<const int32_t*>(o + 0xE0));
        // Research: its material presets (KWMaterialPresetDataAsset at +0x40;
        // PresetArray +0x28 of {FString Description, TArray Materials}).
        const auto* preset = *reinterpret_cast<const uint8_t* const*>(o + 0x40);
        if (!preset || !IsReadable(preset + 0x28, 16)) return;
        const auto* items = *reinterpret_cast<const uint8_t* const*>(preset + 0x28);
        const int n = *reinterpret_cast<const int32_t*>(preset + 0x30);
        for (int k = 0; items && k < n && k < 40; k++) {
            Log("variants:   preset %s [%d] '%s' materials %d", ue4::ObjectName(preset).c_str(), k, FStringText(items + k * 0x20).c_str(),
                *reinterpret_cast<const int32_t*>(items + k * 0x20 + 0x18));
        }
    });
}

namespace {

// StaticLoadObjectInternal(UClass*, UObject* outer, const TCHAR* name,
// const TCHAR* filename, uint32 flags, UPackageMap*, bool reconcile) (rva
// 0x1242910; StaticLoadClass at 0x12424B0 calls it): loads one asset.
constexpr uint32_t kStaticLoadObject = 0x1242910;
constexpr uint8_t kStaticLoadObjectBytes[] = {0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x49, 0x8D, 0xAB,
                                              0x18, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xD8};

std::string Utf8Of(const uint8_t* fstring)
{
    const auto* data = *reinterpret_cast<const wchar_t* const*>(fstring);
    const int n = *reinterpret_cast<const int32_t*>(fstring + 8);
    if (!data || n <= 1) return "";
    std::string out(static_cast<size_t>(n) * 3, '\0');
    const int len = WideCharToMultiByte(CP_UTF8, 0, data, n - 1, out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(std::max(len, 0));
    return out;
}

} // namespace

void* LoadAsset(const std::string& className, const std::string& path)
{
    if (!g_base || std::memcmp(reinterpret_cast<const void*>(g_base + kStaticLoadObject), kStaticLoadObjectBytes,
                               sizeof(kStaticLoadObjectBytes)) != 0) {
        Log("flow: StaticLoadObject not where expected");
        return nullptr;
    }
    // The class, found once: finding it walks every object (about 70 ms, as
    // long as the load itself takes for a small texture).
    static std::mutex classMutex;
    static std::map<std::string, const void*> classes;
    const void* cls = nullptr;
    {
        std::lock_guard lock(classMutex);
        const void*& known = classes[className];
        if (!known) known = ue4::FindObject(className, "Class");
        cls = known;
    }
    if (!cls) return nullptr;
    using LoadFn = void* (*)(const void* cls, void* outer, const wchar_t* name, const wchar_t* file, uint32_t flags, void* sandbox, bool reconcile);
    return reinterpret_cast<LoadFn>(g_base + kStaticLoadObject)(cls, nullptr, WideOf(path).c_str(), nullptr, 0, nullptr, true);
}

namespace {

void LogPresetsOf(int index)
{
    using LoadFn = void* (*)(const void* cls, void* outer, const wchar_t* name, const wchar_t* file, uint32_t flags, void* sandbox, bool reconcile);
    static const void* presetClass = ue4::FindObject("KWMaterialPresetDataAsset", "Class");
    int count = 0;
    const uint8_t* e = CharaEntries(&count) + index * kCharaEntrySize;
    const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
    const int num = *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8);
    const auto specs = CharaCostumeSpecs(index);
    for (int k = 0; paths && k < num; k++) {
        const std::string path = FStringText(paths + k * 16);
        const size_t slash = path.find('/'), name = path.rfind("/DA_"), at = path.find("_Costume_");
        if (slash == std::string::npos || name == std::string::npos || at == std::string::npos) continue;
        const auto it = specs.find(k);
        const std::string label = it == specs.end() ? "(not offered)" : it->second.name;
        for (const std::string& code : {path.substr(name + 4, 3), path.substr(0, slash)}) {
            const std::string asset = "DA_" + code + "_MaterialPreset_" + path.substr(at + 9);
            const std::string package = "/Game/Chara/" + path.substr(0, name + 1) + asset;
            if (!PackageExists(package)) continue;
            const std::wstring full = WideOf(package + "." + asset);
            const auto* preset = static_cast<const uint8_t*>(
                reinterpret_cast<LoadFn>(g_base + kStaticLoadObject)(presetClass, nullptr, full.c_str(), nullptr, 0, nullptr, true));
            if (!preset || !IsReadable(preset + 0x28, 16)) {
                Log("presets %s [%d] %s: %s did not load", CharaCode(index).c_str(), k, label.c_str(), asset.c_str());
                continue;
            }
            const auto* items = *reinterpret_cast<const uint8_t* const*>(preset + 0x28);
            const int n = *reinterpret_cast<const int32_t*>(preset + 0x30);
            std::string list;
            for (int j = 0; items && j < n && j < 40; j++) {
                list += " [" + Utf8Of(items + j * 0x20) + " " + std::to_string(*reinterpret_cast<const int32_t*>(items + j * 0x20 + 0x18)) + "]";
            }
            Log("presets %s [%d] %s (variants %d) %s:%s", CharaCode(index).c_str(), k, label.c_str(), CostumeVariants(index, k),
                asset.c_str(), list.c_str());
            if (code == path.substr(0, slash)) break;
        }
    }
}

void LogPresetsFrom(int index, std::string code)
{
    int count = 0;
    if (!CharaEntries(&count)) return;
    for (; index < count; index++) {
        if (code.empty() || CharaCode(index) == code) break;
    }
    if (index >= count) {
        Log("presets: done");
        return;
    }
    LogPresetsOf(index);
    RunOnGameThreadAfter(5, [index, code] { LogPresetsFrom(index + 1, code); });
}

} // namespace

void LogCostumePresets(const std::string& code)
{
    if (!g_base || std::memcmp(reinterpret_cast<const void*>(g_base + kStaticLoadObject), kStaticLoadObjectBytes,
                               sizeof(kStaticLoadObjectBytes)) != 0) {
        Log("presets: StaticLoadObject not where expected");
        return;
    }
    RunOnGameThread([code] { LogPresetsFrom(0, code); });
}

std::map<int, CostumeSpec> CharaCostumeSpecs(int index)
{
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return {};
    const int progId = *reinterpret_cast<const int32_t*>(entries + index * kCharaEntrySize + 0x10);
    std::lock_guard lock(g_specMutex);
    // The table's object was once missed while a costume preload changed the
    // object list (the first character then had no names): try again.
    for (int tries = 0; tries < 5 && !g_specsRead; tries++) {
        if (tries) Sleep(200);
        ReadCharaSpecs();
    }
    const auto it = g_specs.find(progId);
    return it == g_specs.end() ? std::map<int, CostumeSpec>{} : it->second;
}

void LogDataTable(const std::string& name, const std::string& filter, int limit)
{
    // UDataTable: RowStruct +0x28, RowMap (TMap<FName, uint8*>) from +0x30;
    // its elements (0x18 bytes: FName, row, hash links) at +0x40, count +0x48.
    if (!ue4::EnsureObjects(g_base)) return;
    const auto* table = static_cast<const uint8_t*>(ue4::FindObject(name, "DataTable"));
    if (!table || !IsReadable(table + 0x28, 0x28)) {
        Log("datatable %s: not found", name.c_str());
        return;
    }
    const auto* rowStruct = *reinterpret_cast<const uint8_t* const*>(table + 0x28);
    const auto* elements = *reinterpret_cast<const uint8_t* const*>(table + 0x40);
    const int count = *reinterpret_cast<const int32_t*>(table + 0x48);
    Log("datatable %s: %d rows of %s", name.c_str(), count, ue4::ObjectName(rowStruct).c_str());
    int shown = 0;
    for (int i = 0; elements && i < count && shown < limit; i++) {
        const uint8_t* e = elements + i * 0x18;
        const auto* row = *reinterpret_cast<const uint8_t* const*>(e + 8);
        if (!row || !IsReadable(row, 8)) continue;
        std::string line = ue4::NameText(*reinterpret_cast<const int32_t*>(e)) + ":";
        const uint8_t* f = rowStruct ? *reinterpret_cast<const uint8_t* const*>(rowStruct + 0x38) : nullptr;
        for (int k = 0; f && k < 60 && IsReadable(f, 0x50); k++) {
            line += " " + ue4::ObjectName(f) + "=" + PropertyText(f, row);
            f = *reinterpret_cast<const uint8_t* const*>(f + 0x28);
        }
        if (!filter.empty() && line.find(filter) == std::string::npos) continue;
        Log("  %s", line.c_str());
        shown++;
    }
}

std::string StageAvailableCode(int index)
{
    // StageSpec (DA_Spec; KWStageSpec rows): StageId is the stage's index.
    static std::mutex m;
    static std::map<int, std::string> codes;
    static bool read = false;
    std::lock_guard lock(m);
    if (!read && g_base && ue4::EnsureObjects(g_base)) {
        const auto* table = static_cast<const uint8_t*>(ue4::FindObject("StageSpec", "DataTable"));
        if (table && IsReadable(table + 0x28, 0x28)) {
            read = true;
            const auto* rowStruct = *reinterpret_cast<const uint8_t* const*>(table + 0x28);
            const int id = PropertyOffset(rowStruct, "StageId"), code = PropertyOffset(rowStruct, "RefAvailableCode");
            const auto* elements = *reinterpret_cast<const uint8_t* const*>(table + 0x40);
            const int count = *reinterpret_cast<const int32_t*>(table + 0x48);
            for (int i = 0; id >= 0 && code >= 0 && elements && i < count; i++) {
                const auto* row = *reinterpret_cast<const uint8_t* const*>(elements + i * 0x18 + 8);
                if (row && IsReadable(row, std::max(id + 4, code + 16))) codes[*reinterpret_cast<const int32_t*>(row + id)] = FStringText(row + code);
            }
            Log("flow: stage codes of %zu stages", codes.size());
        }
    }
    const auto it = codes.find(index);
    return it == codes.end() ? std::string() : it->second;
}

namespace {

// Names read from stage personal data the game had not loaded (Ring of
// Arcade's, S24: StageNamesReady), by the asset's name ("DA_S24_Personal").
// Kept as text: the asset loaded for it is not held, and may be collected.
std::mutex g_loadedNamesMutex;
std::map<std::string, std::string> g_loadedNames;
std::atomic<int> g_stageNamesState{0}; // 0 not asked, 1 loading, 2 done

} // namespace

bool StageNamesReady()
{
    int expected = 0;
    if (g_stageNamesState.compare_exchange_strong(expected, 1)) {
        RunOnGameThread([] {
            int count = 0;
            const uint8_t* entries = StageEntries(&count);
            // StageSpec (StageAvailableCode) lists the stages the game sells
            // and offers; story ones such as SH2 have a name but no row.
            if (!entries || StageAvailableCode(0).empty()) {
                g_stageNamesState = 0; // the tables are not there yet: asked again
                return;
            }
            std::string loaded;
            for (int i = 0; i < count; i++) {
                const uint8_t* e = entries + i * kStageEntrySize;
                const std::string code = FStringText(e), path = FStringText(e + 0x20);
                const size_t dot = path.rfind('.');
                if (code.size() != 3 || code == "RND" || dot == std::string::npos || path.find("/" + code + "/") == std::string::npos ||
                    !IsPlayableStage(i) || StageAvailableCode(i).empty() || !StageName(i).empty()) {
                    continue;
                }
                const auto* asset = static_cast<const uint8_t*>(LoadAsset("KWStagePersonalDataAsset", path));
                const std::string name = asset && IsReadable(asset + 0x28, 8) ? FTextText(asset + 0x28) : "";
                if (name.empty()) continue;
                std::lock_guard lock(g_loadedNamesMutex);
                g_loadedNames[path.substr(dot + 1)] = name;
                loaded += (loaded.empty() ? "" : ", ") + code + " " + name;
            }
            Log("flow: stage names loaded: %s", loaded.empty() ? "none needed" : loaded.c_str());
            g_stageNamesState = 2;
        });
    }
    return g_stageNamesState == 2;
}

std::string StageName(int index)
{
    // The stage entry's third string is its KWStagePersonalDataAsset
    // ("/Game/Stage/CHN/DA_CHN_Personal.DA_CHN_Personal"); Name (FText) at
    // +0x28. The assets are collected once (a walk of every object); those
    // the game had not loaded come from StageNamesReady.
    static std::mutex mutex;
    static std::map<std::string, const uint8_t*> personal;
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    const std::string path = FStringText(entries + index * kStageEntrySize + 0x20);
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::lock_guard lock(mutex);
    if (personal.empty() && ue4::EnsureObjects(g_base)) {
        ue4::ForEachObject([&](const void* object) {
            if (ue4::ObjectName(ue4::ClassOf(object)) == "KWStagePersonalDataAsset") {
                personal[ue4::ObjectName(object)] = static_cast<const uint8_t*>(object);
            }
        });
    }
    const auto it = personal.find(path.substr(dot + 1));
    if (it != personal.end()) return FTextText(it->second + 0x28);
    std::lock_guard loadedLock(g_loadedNamesMutex);
    const auto loaded = g_loadedNames.find(path.substr(dot + 1));
    return loaded == g_loadedNames.end() ? "" : loaded->second;
}

std::vector<std::string> SkillNames(const std::string& charaCode, bool vtrigger)
{
    // DA_VSkillNameAsset / DA_VTriggerNameAsset (KWVersionDataAsset):
    // VersionCharaInfo at +0x28, entries of 0x20 (charaName FString +0x0,
    // TextID TArray<FText> +0x10, FText 0x28 bytes).
    std::vector<std::string> names;
    const auto* asset = static_cast<const uint8_t*>(
        CachedObject(vtrigger ? "DA_VTriggerNameAsset" : "DA_VSkillNameAsset", "KWVersionDataAsset"));
    if (!asset || !IsReadable(asset + 0x28, 16)) return names;
    const uint8_t* entries = *reinterpret_cast<const uint8_t* const*>(asset + 0x28);
    const int count = *reinterpret_cast<const int32_t*>(asset + 0x30);
    for (int i = 0; entries && i < count && i < 256; i++) {
        const uint8_t* e = entries + i * 0x20;
        if (!IsReadable(e, 0x20) || FStringText(e) != charaCode) continue;
        const uint8_t* texts = *reinterpret_cast<const uint8_t* const*>(e + 0x10);
        const int n = *reinterpret_cast<const int32_t*>(e + 0x18);
        for (int k = 0; texts && k < n && k < 8; k++) names.push_back(FTextText(texts + k * 0x28));
        break;
    }
    return names;
}

std::vector<std::string> VCommentNames(const std::string& charaCode)
{
    // KWSubtitleDataAsset DA_<code>_Subtitle: FixedVCommentLocalizeIds at
    // +0x48, a TArray<FText> (0x28 bytes each). Only the character's own
    // battle data loads it, so load it here when the menus have not.
    const std::string name = "DA_" + charaCode + "_Subtitle";
    const auto* asset = static_cast<const uint8_t*>(CachedObject(name, "KWSubtitleDataAsset"));
    if (!asset) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        const std::string path = "/Game/Chara/" + charaCode + "/DataAsset/" + name + "." + name;
        RunOnGameThread([path, done] {
            LoadAsset("KWSubtitleDataAsset", path);
            *done = true;
        });
        for (int i = 0; i < 200 && !*done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        asset = static_cast<const uint8_t*>(CachedObject(name, "KWSubtitleDataAsset"));
    }
    std::vector<std::string> names;
    if (!asset || !IsReadable(asset + 0x48, 16)) return names;
    const uint8_t* texts = *reinterpret_cast<const uint8_t* const*>(asset + 0x48);
    const int n = *reinterpret_cast<const int32_t*>(asset + 0x50);
    for (int k = 0; texts && k < n && k < 32; k++) {
        // Some are written over two lines for the versus screen.
        std::string text = FTextText(texts + k * 0x28);
        std::replace(text.begin(), text.end(), '\n', ' ');
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        names.push_back(std::move(text));
    }
    return names;
}

int StageIndex(const std::string& code)
{
    for (int i = 0; i < StageCount(); i++) {
        if (StageCode(i) == code) return i;
    }
    return -1;
}

void LogStageTable()
{
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries) {
        Log("stages: the stage table is not loaded yet");
        return;
    }
    Log("stages: %d entries", count);
    for (int i = 0; i < count && i < 256; i++) {
        const uint8_t* e = entries + i * kStageEntrySize;
        Log("  stage %2d: %-5s %-28s %s | %s | %s | %s", i, FStringText(e).c_str(), StageName(i).c_str(), FStringText(e + 0x10).c_str(),
            FStringText(e + 0x20).c_str(), FStringText(e + 0x30).c_str(), FStringText(e + 0x40).c_str());
    }
}

void LogWorldContext()
{
    RunOnGameThread([] {
        auto* engine = *reinterpret_cast<uint8_t* const*>(g_base + kEngineGlobal);
        if (!engine) return;
        void* viewport = *reinterpret_cast<void* const*>(engine + 0x5E8);
        if (!viewport) return;
        using GetWorldFn = void* (*)(void*);
        void* world = (*reinterpret_cast<GetWorldFn* const*>(viewport))[0xE0 / 8](viewport);
        using ContextFn = uint8_t* (*)(void*, void*);
        uint8_t* ctx = reinterpret_cast<ContextFn>(g_base + 0x1CD9130)(engine, world);
        Log("worldctx: world %s (%p) context %p", ue4::Describe(world).c_str(), world, static_cast<void*>(ctx));
        if (!ctx) return;
        for (int off = 0x1C0; off < 0x220; off += 0x10) {
            uint64_t a = 0, b = 0;
            std::memcpy(&a, ctx + off, 8);
            std::memcpy(&b, ctx + off + 8, 8);
            Log("worldctx +0x%x: %016llx %016llx", off, (unsigned long long)a, (unsigned long long)b);
        }
    });
}

int SchedulerPhaseNow() { return g_schedPhase; }

void SetSchedulerPhase(int phase, bool replaying)
{
    g_schedPhase = phase;
    g_schedReplaying = replaying;
}

void SetSoundRollback(bool on)
{
    g_soundRollback = on;
    Log("sound: the game's own rollback of sounds %s", on ? "on (resimulated ticks' repeats are not played again)"
                                                          : "off (resimulated ticks play their sounds again)");
}

bool SoundRollbackOn() { return g_soundRollback; }

std::vector<uintptr_t> DefaultLeanResimTasks() { return kLeanResimTasks; }

void SetResimTaskSkip(std::vector<uintptr_t> rvas)
{
    std::lock_guard lock(g_resimSkipMutex);
    g_resimSkip = std::move(rvas);
    g_resimSkipAny = !g_resimSkip.empty();
    std::string text;
    for (uintptr_t r : g_resimSkip) text += " 0x" + [r] { char b[16]; std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(r)); return std::string(b); }();
    Log("resimtasks: replayed ticks leave out %zu tasks in phases 4 and 8:%s", g_resimSkip.size(), text.c_str());
}

void SetTaskProfiling(bool on, uint64_t ticks)
{
    if (on) {
        RunOnGameThread([] { g_taskCosts.clear(); });
        g_taskProfiling = true;
        return;
    }
    g_taskProfiling = false;
    RunOnGameThread([ticks] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        for (int phase : {1, 2, 4, 8}) {
            std::vector<std::pair<uint64_t, const TaskCost*>> rows;
            uint64_t total = 0;
            for (const auto& [key, cost] : g_taskCosts) {
                if (static_cast<int>(key >> 32) != phase) continue;
                rows.push_back({key, &cost});
                total += cost.ticks;
            }
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second->ticks > b.second->ticks; });
            Log("phasecost: phase %d: %zu tasks, %.0f us per driven tick", phase, rows.size(),
                ticks ? total * 1e6 / f.QuadPart / ticks : 0.0);
            for (size_t i = 0; i < rows.size() && i < 25; i++) {
                const TaskCost& c = *rows[i].second;
                const uint32_t low = static_cast<uint32_t>(rows[i].first);
                Log("phasecost:   %s 0x%x: %.1f us per tick, %llu calls, on %s", low & 0x80000000u ? "virtual task, class vtable" : "rva",
                    low & 0x7FFFFFFFu,
                    ticks ? c.ticks * 1e6 / f.QuadPart / ticks : 0.0, static_cast<unsigned long long>(c.calls),
                    ue4::Describe(reinterpret_cast<void*>(c.object)).c_str());
            }
        }
    });
}

void TaskCensus(int calls)
{
    {
        std::lock_guard lock(g_censusMutex);
        g_census.clear();
    }
    g_taskCensus = calls;
    std::thread([calls] {
        ThreadGuard running;
        for (int i = 0; i < 100 && g_taskCensus > 0 && !Stopping(); i++) Sleep(50);
        g_taskCensus = 0;
        std::vector<std::pair<uintptr_t, uintptr_t>> seen;
        {
            std::lock_guard lock(g_censusMutex);
            seen = g_census;
        }
        // Distinct tasks in first-call order, with how often each ran.
        std::vector<std::pair<std::pair<uintptr_t, uintptr_t>, int>> distinct;
        for (const auto& t : seen) {
            bool found = false;
            for (auto& d : distinct) {
                if (d.first == t) {
                    d.second++;
                    found = true;
                    break;
                }
            }
            if (!found) distinct.push_back({t, 1});
        }
        Log("tasks: %zu calls, %zu distinct", seen.size(), distinct.size());
        for (const auto& [t, n] : distinct) {
            const uintptr_t fn = t.first, object = t.second;
            const bool inImage = fn >= g_base && fn < g_base + 0x5000000;
            Log("  task fn %s%llx x%d on %p %s", inImage ? "rva " : "", static_cast<unsigned long long>(inImage ? fn - g_base : fn), n,
                reinterpret_cast<void*>(object), ue4::Describe(reinterpret_cast<void*>(object)).c_str());
        }
    }).detach();
}

std::function<std::string(const void*)> g_regionOf;

void SetRegionLookup(std::function<std::string(const void*)> lookup)
{
    g_regionOf = std::move(lookup);
}

// The big battle object the phases and the director reach through their
// task's +0xE8 (rva 0x307F90 locks a weak pointer there and returns +0x110).
uint8_t* BattleContext(uint8_t* task)
{
    using GetFn = uint8_t* (*)(uint8_t*);
    return reinterpret_cast<GetFn>(g_base + 0x307F90)(task + 0xE8);
}

void TestEmptyStageRequest() { g_testEmptyStage = true; }

std::string RoundEndState()
{
    char buf[160];
    const uint8_t* d = LiveRoundDirector();
    const uint8_t* m = LivePhaseMachine();
    // The game's battle input recorder (global at rva 0x39DA0D0, 0x79580
    // bytes): 4 slots of 0xD0 from +0xE0, each a state (1, 2) and a round.
    const auto* recorder = *reinterpret_cast<const uint8_t* const*>(g_base + 0x39DA0D0);
    const bool haveRecorder = recorder && IsReadable(recorder, 0x420);
    std::snprintf(buf, sizeof(buf),
                  "tick %u, director %d kind %d stop %d, phase %d requested %d, journal %zu, recorder %d/%d %d/%d",
                  TickNow(), d ? Field<int32_t>(d, kDirectorState) : -1, d ? Field<int32_t>(d, kDirectorStopKind) : -1,
                  g_roundFlow.stopTicks, m ? Field<int32_t>(m, 0x130) : -1, m ? Field<int32_t>(m, 0x134) : -1,
                  g_roundOverJournal.size(), haveRecorder ? Field<int32_t>(recorder, 0xE0) : -1,
                  haveRecorder ? Field<int32_t>(recorder, 0xE4) : -1, haveRecorder ? Field<int32_t>(recorder, 0x1B0) : -1,
                  haveRecorder ? Field<int32_t>(recorder, 0x1B4) : -1);
    return buf;
}

uint8_t* PhaseMachineObject() { return LivePhaseMachine(); }

void LogPhaseMachine()
{
    uint8_t* m = g_phaseMachine;
    if (!m || !IsReadable(m, 0x140)) {
        Log("phases: no phase machine seen yet (it is known once a battle ticks)");
        return;
    }
    auto q = [](const uint8_t* p, uint32_t off) { return *reinterpret_cast<const uint64_t*>(p + off); };
    auto d = [](const uint8_t* p, uint32_t off) { return *reinterpret_cast<const int32_t*>(p + off); };
    auto rva = [](uint64_t v) { return v >= g_base && v < g_base + 0x5000000 ? v - g_base : 0; };
    auto where = [](const void* p) { return g_regionOf ? g_regionOf(p) : std::string(); };
    Log("phases: machine %p (vtable rva 0x%llX) [%s], current %d, requested %d, +0x138 %d, +0x13C %02x, current object %p",
        static_cast<void*>(m), static_cast<unsigned long long>(rva(q(m, 0))), where(m + 0x130).c_str(), d(m, 0x130),
        d(m, 0x134), d(m, 0x138), m[0x13C], reinterpret_cast<void*>(q(m, 0x120)));
    if (uint8_t* battle = BattleContext(m)) {
        Log("phases: battle context %p, +0x18D0 %08x [%s]", static_cast<void*>(battle), d(battle, 0x18D0),
            where(battle + 0x18D0).c_str());
        // What the round over writes: in the snapshot or not.
        for (uint32_t off : {0x1904u, 0x1908u, 0x1910u, 0x1910u + 0x10FC8u, 0x78788u, 0x78798u, 0x787ACu, 0x787B8u}) {
            Log("phases:   battle context +0x%X [%s]", off, where(battle + off).c_str());
        }
    }
    // A vector of shared pointers: begin at +0xF8, end at +0x100.
    const auto* phases = reinterpret_cast<const uint8_t*>(q(m, 0xF8));
    const int count = phases ? static_cast<int>((q(m, 0x100) - q(m, 0xF8)) / 16) : 0;
    if (!phases || count <= 0 || count > 64 || !IsReadable(phases, count * 16)) {
        Log("phases: no phase array (%p, %d)", static_cast<const void*>(phases), count);
        return;
    }
    for (int i = 0; i < count; i++) {
        const auto* obj = reinterpret_cast<const uint8_t*>(q(phases, i * 16));
        if (!obj || !IsReadable(obj, 0x40)) {
            Log("phases: [%d] %p", i, static_cast<const void*>(obj));
            continue;
        }
        // +0x20: the id (the flow event code); +0x50 / +0x60 / +0x68: the
        // callback leaving the phase calls and clears; +0x70 / +0x78: reset then.
        Log("phases: [%d] %p [%s] vtable rva 0x%llX, id %d, +0x8 %d, +0x50 rva 0x%llX, +0x68 rva 0x%llX, +0x70 %016llx, +0x78 %08x",
            i, static_cast<const void*>(obj), where(obj + 0x70).c_str(), static_cast<unsigned long long>(rva(q(obj, 0))), d(obj, 0x20), d(obj, 8),
            static_cast<unsigned long long>(rva(q(obj, 0x50))), static_cast<unsigned long long>(rva(q(obj, 0x68))),
            static_cast<unsigned long long>(q(obj, 0x70)), d(obj, 0x78));
    }
}

std::string FlowHistory()
{
    std::lock_guard lock(g_mutex);
    std::string s;
    for (const auto& l : g_history) s += l + " | ";
    return s;
}

} // namespace sfv::mod
