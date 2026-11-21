#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

namespace {

constexpr uint32_t kStateStepOff = 0x338, kStateStepFramesOff = 0x378;
constexpr uint32_t kEventCodeOff = 0xC;

constexpr game::CodeSite kChangeSceneState{
    "SceneManager::ChangeSceneState", 0x584B70, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x99, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89}};

constexpr game::CodeSite kLaunchNextGameScene{
    "LaunchNextGameScene", 0x56C100, {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x33, 0xC0, 0x48, 0x8B, 0xF9, 0x48}};

constexpr game::CodeSite kSetupStep{
    "BattleSetupLoader::Step", 0x714BD0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}};

constexpr game::CodeSite kSetupStep0Call{
    "BattleSetupLoader::Step0Call", 0x69ADA0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48}};

constexpr game::CodeSite kTaskThunk{
    "BattleTask::CallThunk", 0x4C1D10, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xC1, 0x48, 0x63, 0x49, 0x08, 0x48, 0x03, 0x48, 0x10, 0xFF}};

constexpr game::CodeSite kRoundEvent{
    "BattleNode::OnEvent", 0x4818C0, {0x40, 0x55, 0x53, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC}};

constexpr game::CodeSite kSceneTick{
    "GameScene::Tick", 0x5C9A90, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}};

constexpr game::CodeSite kCharaCacheRequest{
    "CharaCache::Request", 0x6A4880, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}};

constexpr game::CodeSite kFlowReset{
    "BattleFlow::Reset", 0x71A790, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55}};

using LaunchFn = uint64_t (*)(void* self, void* a2, void* a3);

using FlowResetFn = void (*)(void* flow);

FlowResetFn g_origFlowReset = nullptr;

using SceneTickFn = void (*)(void* self, float deltaSeconds);

SceneTickFn g_origSceneTick = nullptr;

using Step0Fn = uint64_t (*)(void* self, void* data, uint8_t a, uint8_t b);

Step0Fn g_origStep0 = nullptr;

LaunchFn g_origLaunch = nullptr;

using EventFn = uint64_t (*)(void* node, void* event, void* a3, void* a4);

EventFn g_origEvent = nullptr;

constexpr uintptr_t kFlowEventType = 0x2C5F8F8;

constexpr int32_t kFightStarts = 6;

constexpr int32_t kRoundCall = 5;

constexpr int32_t kRoundEnd = 7;

} // namespace

namespace flow {

bool MatchOver(int32_t code) { return code == 11 || code == 12; }

} // namespace flow

namespace {

bool EndsFight(int32_t code) {
    return (code >= 1 && code <= 5) || (code >= 7 && code <= 12);
}

static_assert(sizeof(RoundFlow) == kRoundFlowStateSize);

constexpr uintptr_t kRoundDirectorVtable = 0x2C70E30;

constexpr game::CodeSite kRoundDirectorCheck{
    "RoundDirector::Check", 0x485240, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}};

constexpr game::CodeSite kRoundHitTally{
    "RoundDirector::OnHitEvent", 0x482010, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55}};

using HitTallyFn = uint64_t (*)(uint8_t*, void*, void*, void*);

HitTallyFn g_origHitTally = nullptr;

std::recursive_mutex g_hitTallyMutex;

} // namespace

namespace flow {

uint8_t* LiveRoundDirector() {
    uint8_t* d = g_roundDirector;
    if (!d || !IsReadable(d + kTaskRefOff, kTaskHeaderBytes)) return nullptr;
    if (*reinterpret_cast<const uintptr_t*>(d + kTaskRefOff) != g_base + kRoundDirectorVtable) return nullptr;
    if (*reinterpret_cast<uint8_t* const*>(d + kTaskRefObjectOff) != d) return nullptr;
    return d;
}

void RememberScene(const std::string& stateClassName) {
    g_holdResults = false;
    g_resultsHeld = false;
    std::lock_guard lock(g_sceneMutex);
    if (stateClassName == "TrainingBattleState" && g_lastScene != stateClassName) NoteSessionActivity("training");
    g_lastScene = stateClassName;
    g_sceneChangedAt = g_pumps.load();
}

std::string LastScene() {
    std::lock_guard lock(g_sceneMutex);
    return g_lastScene;
}

} // namespace flow

namespace {

void Remember(const std::string& line) {
    std::lock_guard lock(g_mutex);
    g_history.push_back(line);
    if (g_history.size() > 64) g_history.erase(g_history.begin());
}

void FlowResetDetour(void* flow) {
    HookGuard guard;
    g_origFlowReset(flow);
    if (g_loaderAfterReset.exchange(false)) {
        reinterpret_cast<void (*)(void*, int)>(g_base + kEnsureLoader)(flow, kVersusLoaderType);
        Log("flow: the old scene reset the battle flow; Versus battle loader made again");
    }
}

void SceneTickDetour(void* self, float deltaSeconds) {
    HookGuard guard;
    g_origSceneTick(self, deltaSeconds);
    g_sceneTicks++;
    NoteGameTick();
    FlowPumpGameThread();
    if (g_sceneTicks % 30 == 0) PreloadTick();
}

void ChangeDetour(void* self, const void* stateClass, void* param) {
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

std::atomic<int> g_sceneGeneration{0};

void TraceStep(int generation, const std::string& name, uint8_t* state, uintptr_t lastStep, int frame) {
    if (generation != g_sceneGeneration || Stopping() || frame > 60 * 120) return;
    if (!state) {
        if (!ue4::EnsureObjects(g_base)) return;
        ue4::ForEachObject([&](const void* object) {
            if (ue4::ObjectName(ue4::ClassOf(object)) == name) state = static_cast<uint8_t*>(const_cast<void*>(object));
        });
        if (!state) return;
    }
    if (!IsReadable(state + kStateStepOff, kStateStepFramesOff + sizeof(int32_t) - kStateStepOff)) return;
    const uintptr_t step = *reinterpret_cast<const uintptr_t*>(state + kStateStepOff);
    if (step != lastStep) {
        Log("flow: %s step rva %llx (delay %d) at frame %d", name.c_str(), static_cast<unsigned long long>(step - g_base),
            *reinterpret_cast<const int32_t*>(state + kStateStepFramesOff), frame);
    }
    FlowRunOnGameThreadAfter(1, [=] { TraceStep(generation, name, state, step, frame + 1); });
}

} // namespace

namespace flow {

void TraceSceneSteps(const std::string& name) {
    const int generation = ++g_sceneGeneration;
    FlowRunOnGameThreadAfter(1, [=] { TraceStep(generation, name, nullptr, 0, 0); });
}

} // namespace flow

namespace {

uint64_t LaunchDetour(void* self, void* a2, void* a3) {
    HookGuard guard;
    const std::string line = "LaunchNextGameScene on " + ue4::Describe(self) + " (" + ue4::Describe(a2) + ")";
    Log("flow: %s (this %p a2 %p a3 %p)", line.c_str(), self, a2, a3);
    Remember(line);
    return g_origLaunch(self, a2, a3);
}

uint64_t StepDetour(void* self, int step, uint8_t force) {
    HookGuard guard;
    Log("flow: setup step %d (force %d) on %s (%p)", step, force, ue4::Describe(self).c_str(), self);
    LogStack("setup step", g_base);
    return g_origStep(self, step, force);
}

uint64_t Step0Detour(void* self, void* data, uint8_t a, uint8_t b) {
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

std::mutex g_burstOnMutex;

int g_burstOnCode = -1, g_burstOnFrames = 0, g_burstOnScale = 4, g_burstOnLeft = 0, g_burstOnIndex = 0;

std::string g_burstOnDir;

uint64_t EventDetour(void* node, void* event, void* a3, void* a4) {
    if (event && IsReadable(static_cast<uint8_t*>(event), kEventCodeOff + sizeof(int32_t))) {
        const int code = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(event) + kEventCodeOff);
        const uintptr_t vtable = *static_cast<const uintptr_t*>(event);
        if (vtable - g_base == kFlowEventType && g_holdResults && (code == kResults || code == kResults + 1)) {
            if (!g_resultsHeld.exchange(true)) Log("flow: results screen held (flow event %d dropped)", code);
            return 0;
        }
        if (vtable - g_base == kFlowEventType) {
            g_roundCall = code == kRoundCall;
            if (code == kRoundCall) {
                if (const uint8_t* m = g_phaseMachine; m && IsReadable(m + kMachineIndexOff, 4)) g_roundCallPhase = *reinterpret_cast<const int32_t*>(m + kMachineIndexOff);
            }
            if (code == kRoundEnd) g_roundEnds++;
            if (code == kFightStarts || EndsFight(code) || code == kResults) {
                g_roundFlow.lastCode = code;
                g_roundFlow.inFight = code == kFightStarts;
            }
            Log("flow: round flow event %d", code);
            {
                std::lock_guard lock(g_burstOnMutex);
                if (code == g_burstOnCode && g_burstOnLeft > 0) {
                    g_burstOnLeft--;
                    const std::string dir = g_burstOnDir + "-" + std::to_string(++g_burstOnIndex);
                    OverlayRequestShotBurst(dir, g_burstOnFrames, g_burstOnScale);
                    Log("flow: frame burst into %s on flow event %d", dir.c_str(), code);
                }
            }
        }
    }
    return g_origEvent(node, event, a3, a4);
}

uint64_t HitTallyDetour(uint8_t* self, void* event, void* a3, void* a4) {
    HookGuard guard;
    std::unique_lock lock(g_hitTallyMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        static std::atomic<int> waits{0};
        const int n = ++waits;
        if (n == 1 || n == 10 || n == 100 || n == 1000) Log("flow: hit tallies: %d calls waited for another thread's", n);
        lock.lock();
    }
    return g_origHitTally(self, event, a3, a4);
}

void DirectorCheckDetour(uint8_t* director) {
    HookGuard guard;
    if (g_directorPerTick && !t_directorFromTick) {
        if (director != g_roundDirector && g_otherDirectors++ < 3) {
            Log("flow: director check skipped for %p, not the round director (%p)", static_cast<void*>(director),
                static_cast<void*>(g_roundDirector.load()));
        }
        return;
    }
    g_origDirectorCheck(director);
}

bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original) {
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("flow: signature mismatch at %s", site.name);
        return false;
    }
    return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

constexpr game::CodeSite kKoWaitCtorSite{"round end wait object", kKoWaitCtor, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x42, 0x45, 0xF2, 0xFF, 0xC7, 0x83}};

using KoWaitCtorFn = void* (*)(uint8_t*);

KoWaitCtorFn g_origKoWaitCtor = nullptr;

void* KoWaitCtorDetour(uint8_t* self) {
    void* r = g_origKoWaitCtor(self);
    g_koWait = self;
    return r;
}

} // namespace

void FlowSetBurstOnEvent(int code, const std::string& dir, int frames, int scale, int count) {
    std::lock_guard lock(g_burstOnMutex);
    g_burstOnCode = code;
    g_burstOnDir = dir;
    g_burstOnFrames = frames;
    g_burstOnScale = scale;
    g_burstOnLeft = count;
    g_burstOnIndex = 0;
    Log("flow: a %d-frame burst into %s-N on each of the next %d flow events %d", frames, dir.c_str(), count, code);
}

bool FlowInstallProbes(const GameImage& image) {
    g_base = image.base;
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
    } else {
        Log("flow: the game's shared pointer assign is not at rva 0x%X: the round end wait's object is not put back by loads", kSharedAssign);
    }
    if (!Hook(image, kRoundHitTally, reinterpret_cast<void*>(&HitTallyDetour), reinterpret_cast<void**>(&g_origHitTally))) {
        Log("flow: the round's hit tallies stay unguarded (a trade can count differently on each side)");
    }
    Log("flow probes %s", ok ? "installed" : "NOT installed");
    return ok;
}

namespace flow {

uint8_t* GameSingleton() {
    if (!g_base) return nullptr;
    auto* engine = *reinterpret_cast<uint8_t* const*>(g_base + kEngineGlobal);
    if (!engine || !IsReadable(engine + kSingleton, sizeof(void*))) return nullptr;
    return *reinterpret_cast<uint8_t* const*>(engine + kSingleton);
}

const uint8_t* AssetList(const uint8_t* asset, int* count) {
    if (!asset || !IsReadable(asset + kAssetListOff, kArrayBytes)) return nullptr;
    *count = *reinterpret_cast<const int32_t*>(asset + kAssetListOff + kArrayNumOff);
    return *reinterpret_cast<uint8_t* const*>(asset + kAssetListOff);
}

} // namespace flow

} // namespace sfv::mod
