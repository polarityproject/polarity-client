#pragma once

#include "mod/flow.h"

#include "core/random_stage.h"
#include "mod/benchmark.h"
#include "mod/battle_setup.h"
#include "mod/boot.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/crash_report.h"
#include "mod/entitlements.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/overlay.h"
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
#include <optional>
#include <thread>
#include <vector>

namespace sfv::mod {

uint8_t* BattleContext(uint8_t* task);

void WaitForStageThenSwitch(void* manager, void* stateClass, uint8_t* loader, int frames, uint64_t sceneAt, int generation);

void StartLoaderWhenReady(int frames, uint8_t* seen, int seenAt);

void JumpOnGameThread(int generation, int setupFrames);

std::string PropertyText(const uint8_t* property, const uint8_t* data);

extern std::function<std::string(const void*)> g_regionOf;

uint8_t* BattleContext(uint8_t* task);

} // namespace sfv::mod

namespace sfv::mod::flow {

constexpr uint32_t kEngineGlobal = 0x3D878C8; // engine; +0x1E0 KiwiGameSingleton

constexpr uint32_t kSingleton = 0x1E0;

constexpr uint32_t kSingletonBattleFlow = 0xB8;

constexpr uint32_t kBattleFlowLoader = 0x10;

constexpr uint32_t kLoaderPhase = 0x8;

constexpr int kLoaderStartDelay = 30; // frames

constexpr uint32_t kLoaderStepDone = 0x710E40; // bool (loader, step): step done and its loading finished

constexpr int kStageWaitFrames = 60 * 30;

constexpr int kSetupWaitFrames = 60 * 30;

constexpr int kSetupSettleFrames = 30;

constexpr uint32_t kEnsureLoader = 0x4AA510;

constexpr int kVersusLoaderType = 0xE;

constexpr int kSceneSettleFrames = 60;

constexpr int kLoaderWaitFrames = 60 * 20;

constexpr uint32_t kSingletonStageTable = 0x68; // KWStageEntryDataAsset; entries at +0x28 (TArray)

constexpr size_t kStageEntrySize = 0x50; // five FStrings: code, then level/asset paths

constexpr uint32_t kCharaCache = 0x68CEA0;

constexpr uint32_t kCacheSlotsInUse = 0x1D0; // TArray<Slot*>

constexpr uint32_t kCacheSlotsFree = 0x1E0; // TArray<Slot*>

constexpr uint32_t kKiwiSingleton = 0x521690;

constexpr uint32_t kSingletonVersions = 0x60;

constexpr uint32_t kResolveCharaVersion = 0x56B360;

constexpr uint32_t kCharaVersionInfo = 0x57F030;

constexpr uint32_t kAssetManager = 0x3A91FA8;

constexpr uint32_t kAssetManagerQueue = 0x108;

constexpr uint32_t kQueueState = 0x8;

constexpr uint32_t kQueueAddChara = 0x6996B0;

constexpr uint32_t kQueueStart = 0x69C500;

constexpr int kQueueLoadBattle = 3;

using ChangeSceneStateFn = void (*)(void* self, const void* stateClass, void* param);

inline ChangeSceneStateFn g_origChange = nullptr;

inline std::atomic<bool> g_loaderAfterReset{false};

inline std::atomic<uint64_t> g_sceneTicks{0};

inline std::atomic<uint64_t> g_pumps{0};

inline std::atomic<uint64_t> g_lastPumpUs{0};

using StepFn = uint64_t (*)(void* self, int step, uint8_t force);

inline StepFn g_origStep = nullptr;

inline uintptr_t g_base = 0;

using TaskThunkFn = uint64_t (*)(void* closure, void* a2, void* a3, void* a4);

inline TaskThunkFn g_origThunk = nullptr;

inline std::atomic<uint32_t> g_roundEnds{0};

inline std::atomic<bool> g_roundCall{false};

inline std::atomic<int> g_roundCallPhase{-1};

constexpr int32_t kResults = 13;

bool MatchOver(int32_t code);

struct RoundFlow {
    int32_t inFight;
    int32_t lastCode;
    uint32_t startPressed; // presentation: who pressed Start (bit per player)
    int32_t startTicks;    // presentation: ticks Start still goes to the game
    int32_t directorSeen;
    int32_t director[14];       // +0xF8..+0x130: state, then the round's stop record
    int32_t directorTimers[16]; // +0x110C4..+0x11104: four timers its check advances
    int32_t directorCounts[5];  // +0x11128..+0x1113C: counts the round's end adds to
    int32_t stopTicks;          // ticks the director has been stopped for a KO (or time over)
    uint32_t ticks;             // driven ticks run: keys the round's end copies (FlowBeginDrivenTick)
    int32_t unused[2];
};

inline alignas(16) RoundFlow g_roundFlow{};

constexpr uintptr_t kRoundDirectorTask = 0x487CD0;

constexpr uint32_t kDirectorState = 0xF8;

constexpr int32_t kDirectorStopped = 2;

constexpr int32_t kDirectorRoundOver = 3;

constexpr uint32_t kStopFlags = 0x190; // battle frame: two flag words

constexpr uint32_t kStopBit = 0x20;

inline std::atomic<uint8_t*> g_roundDirector{nullptr};

constexpr uintptr_t kPhaseMachineTask = 0x3DD890;

inline std::atomic<uint8_t*> g_phaseMachine{nullptr};

constexpr uint32_t kDirectorRecord = 0xF8;

constexpr uint32_t kDirectorTimers = 0x110C4;

constexpr uint32_t kDirectorCounts = 0x11128;

constexpr uint32_t kDirectorStopKind = 0x108; // 0: a plain stop, ended by the next tick; else a KO or time over

constexpr int32_t kRoundOverAfterTicks = 10;

using DirectorCheckFn = void (*)(uint8_t* director);

inline DirectorCheckFn g_origDirectorCheck = nullptr;

inline std::atomic<bool> g_directorPerTick{false};

inline thread_local bool t_directorFromTick = false;

inline std::atomic<bool> g_holdResults{false};

inline std::atomic<bool> g_resultsHeld{false};

uint8_t* LiveRoundDirector();

inline std::atomic<int> g_taskCensus{0}; // calls still to record

inline std::mutex g_censusMutex;

inline std::vector<std::pair<uintptr_t, uintptr_t>> g_census; // (fn, object)

inline std::mutex g_mutex;

inline std::vector<std::string> g_history;

inline std::atomic<void*> g_manager{nullptr}; // the GameScene seen switching states

inline std::mutex g_sceneMutex;

inline std::string g_lastScene;

inline std::atomic<uint64_t> g_sceneChangedAt{0}; // queue run (about a frame) of the last scene change

inline std::atomic<uint64_t> g_battleSwitches{0}; // jumps switched to the battle scene (FlowBattleSceneSwitches)

void RememberScene(const std::string& stateClassName);

std::string LastScene();

inline std::mutex g_stageMutex;

inline std::string g_pendingStage;

inline std::string g_jumpStage;

inline std::mutex g_taskMutex;

inline std::deque<std::function<void()>> g_tasks;

void TraceSceneSteps(const std::string& name);

using CharaCacheRequestFn = uint64_t (*)(void* cache, const void* code, int costume, int color, const void* x, int y);

inline CharaCacheRequestFn g_origCacheRequest = nullptr;

uint64_t CharaCacheRequestDetour(void* cache, const void* code, int costume, int color, const void* x, int y);

void PreloadTick();

constexpr uint32_t kKoWaitCtor = 0x3C7AE0;

constexpr uint32_t kSharedAssign = 0x2F02D0; // (slot, &block): the new block counted up, the old one down (freed at zero)

constexpr uint8_t kSharedAssignBytes[16] = {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
                                            0xEC, 0x20, 0x48, 0x8B, 0x32, 0x48, 0x8B, 0xF9};

using SharedAssignFn = void (*)(void** slot, void* const* block);

inline SharedAssignFn g_sharedAssign = nullptr;

inline uint8_t* g_koWait = nullptr; // the newest such object

inline std::atomic<bool> g_roundEndRollback{false}; // on: no hold; the copies and the journal below instead

constexpr uint32_t kRoundCount = 0x1904; // +0x1904 the round's time, +0x1908 rounds recorded (at most 7)

constexpr size_t kRoundCountSize = 0x0C;

constexpr uint32_t kRoundHistory = 0x1910; // 7 records of 0x10FC8

constexpr size_t kRoundRecord = 0x10FC8;

constexpr uint32_t kLastRecord = 0x787AC; // the latest round's record

constexpr uint32_t kRoundResult = 0x78788;

constexpr size_t kRoundResultSize = 0x10;

struct RoundOverUndo {
    uint32_t tick;
    uint8_t* context;
    uint8_t* slot;
    std::vector<uint8_t> count, slotBytes, lastRecord, result;
};

inline std::vector<RoundOverUndo> g_roundOverJournal;

inline RoundOverUndo g_roundOverPending; // filled before a director tick that may end the round

uint32_t TickNow();

inline std::atomic<bool> g_testEmptyStage{false};

uint8_t* LivePhaseMachine();

template <typename T>
T Field(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const T*>(p + off); }

void PrepareRoundOverUndo(uint8_t* director);

inline std::atomic<int> g_schedPhase{0};

uint64_t TaskThunkDetour(void* closure, void* a2, void* a3, void* a4);

inline int g_otherDirectors = 0;

std::string FStringText(const uint8_t* fstring);

struct FStringArg {
    explicit FStringArg(const std::wstring& text) : chars(text) {
        data = chars.c_str();
        num = max = static_cast<int32_t>(chars.size() + 1);
    }

    std::wstring chars;
    const wchar_t* data;
    int32_t num;
    int32_t max;

    const void* Get() const { return &data; }
};

std::wstring WideOf(const std::string& text);

inline std::atomic<bool> g_preloading{false}; // a preload holds the asset queue

inline std::atomic<uint64_t> g_jumpRequestedAt{0}; // queue run (≈ frame) of the last jump request

void* KnownObject(const std::string& name, const std::string& className);

void* CachedObject(const std::string& name, const std::string& className);

const uint8_t* StageEntries(int* count);

constexpr size_t kCharaEntrySize = 0xE8;

constexpr uint32_t kCharaCostumes = 0x50;

const uint8_t* CharaEntries(int* count);

bool PackageExists(const std::string& path);

std::string FTextText(const uint8_t* text);

uint8_t* GameSingleton();

constexpr uint32_t kAssetListOff = 0x28;
const uint8_t* AssetList(const uint8_t* asset, int* count);

struct DataTable {
    const uint8_t* rowStruct = nullptr;
    const uint8_t* elements = nullptr;
    int count = 0;

    const uint8_t* Row(int i) const;
    int32_t RowName(int i) const;
};

std::optional<DataTable> FindDataTable(const char* name);
std::vector<const uint8_t*> StructProperties(const uint8_t* rowStruct);
int PropertyOffset(const uint8_t* rowStruct, const char* name);
constexpr uint32_t kPropertyOffsetOff = 0x4C;

constexpr uint32_t kStageLevelOff = 0x10, kStageAssetOff = 0x20;
constexpr size_t kStageCodeLength = 3;
constexpr uint32_t kCharaProgIdOff = 0x10, kCharaUniqueIdOff = 0x14;
constexpr size_t kFTextBytes = 0x28;

constexpr uint32_t kArrayNumOff = 0x8, kArrayMaxOff = 0xC;
constexpr size_t kArrayBytes = 0x10;

constexpr uint32_t kSlotCodeOff = 0x8, kSlotCostumeOff = 0x18, kSlotVersionOff = 0x1C, kSlotYOff = 0x20;

constexpr size_t kBattleFlowBytes = 0x20, kLoaderBytes = 0x40;

uintptr_t ImageEnd();
bool InImage(uint64_t v);

constexpr uint32_t kMachinePhasesOff = 0xF8, kMachinePhasesEndOff = 0x100;
constexpr uint32_t kMachineCurrentOff = 0x120, kMachineCurrentRefOff = 0x128;
constexpr uint32_t kMachineIndexOff = 0x130, kMachineRequestedOff = 0x134;
constexpr size_t kMachineBytes = 0x140;

constexpr uint32_t kTaskRefOff = 0xE8, kTaskRefObjectOff = 0xF0;
constexpr size_t kTaskHeaderBytes = 0x18;

constexpr uint32_t kThunkAdjustOff = 0x8, kThunkObjectOff = 0x10;
constexpr uint32_t kClassKey = 0x80000000u;

} // namespace sfv::mod::flow
