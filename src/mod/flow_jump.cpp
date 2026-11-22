#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

void FlowRunOnGameThreadAfter(int frames, std::function<void()> task) {
    if (frames <= 0) {
        FlowRunOnGameThread(std::move(task));
        return;
    }
    FlowRunOnGameThread([frames, task = std::move(task)]() mutable { FlowRunOnGameThreadAfter(frames - 1, std::move(task)); });
}

void FlowRunOnGameThread(std::function<void()> task) {
    std::lock_guard lock(g_taskMutex);
    g_tasks.push_back(std::move(task));
}

std::string FlowPumpStats() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "scene ticks %llu, queue runs %llu", static_cast<unsigned long long>(g_sceneTicks.load()),
                  static_cast<unsigned long long>(g_pumps.load()));
    return buf;
}

void FlowPumpGameThread() {
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

void FlowSetPendingStage(const std::string& code) {
    std::lock_guard lock(g_stageMutex);
    g_pendingStage = code;
}

std::string FlowPendingStage() {
    std::lock_guard lock(g_stageMutex);
    return g_pendingStage;
}

std::string FlowJumpStageCode() {
    std::lock_guard lock(g_stageMutex);
    return g_jumpStage;
}

namespace {

std::atomic<int> g_jumpGeneration{0};

std::atomic<int> g_jumpRetries{0};

constexpr int kJumpRetries = 5;

} // namespace

void FlowCancelBattleJump() {
    g_jumpGeneration++;
    Log("flow: jump to battle called off");
}

void FlowReturnToMainMenu(int delayFrames) {
    const int generation = g_jumpGeneration;
    FlowRunOnGameThreadAfter(delayFrames, [generation] {
        if (generation != g_jumpGeneration) return;
        {
            std::lock_guard lock(g_sceneMutex);
            if (g_lastScene == "MainMenuState") return;
        }
        FlowChangeScene("MainMenuState");
    });
}

std::string FlowCurrentScene() { return LastScene(); }

uint64_t FlowBattleSceneSwitches() { return g_battleSwitches; }

void FlowJumpToBattle() {
    if (!BattleSetupHasPending()) {
        MatchSetup setup;
        setup.players[0].character = PackCode("RYU");
        setup.players[1].character = PackCode("KEN");
        setup.stage = kKeepStage;
        BattleSetupSetPending(setup);
        Log("flow: jump to battle without a match setup: Ryu against Ken in local Versus");
    }
    const int generation = ++g_jumpGeneration;
    g_jumpRequestedAt = g_pumps.load();
    g_jumpRetries = 0;
    if (!ue4::Ready()) {
        Log("flow: jump to battle waits for the name table");
        std::thread([] {
            ThreadGuard running;
            for (int i = 0; i < 300 && !ue4::Ready() && !Stopping(); i++) Sleep(100);
            if (ue4::Ready()) FlowJumpToBattle();
            else Log("flow: no name table; cannot jump");
        }).detach();
        return;
    }
    FlowRunOnGameThread([generation] { JumpOnGameThread(generation, 0); });
}

namespace {

std::mutex g_cachedObjectsMutex;

std::map<std::string, void*> g_cachedObjects;

} // namespace

namespace flow {

void* KnownObject(const std::string& name, const std::string& className) {
    std::lock_guard lock(g_cachedObjectsMutex);
    const auto it = g_cachedObjects.find(className + "/" + name);
    return it == g_cachedObjects.end() ? nullptr : it->second;
}

void* CachedObject(const std::string& name, const std::string& className) {
    if (void* known = KnownObject(name, className)) return known;
    void* object = ue4::EnsureObjects(g_base) ? ue4::FindObject(name, className) : nullptr;
    std::lock_guard lock(g_cachedObjectsMutex);
    if (object) g_cachedObjects[className + "/" + name] = object;
    return object;
}

} // namespace flow

namespace {

uint8_t* BattleFlow() {
    uint8_t* singleton = GameSingleton();
    uint8_t* flow = singleton && IsReadable(singleton + kSingletonBattleFlow, 8)
                        ? *reinterpret_cast<uint8_t* const*>(singleton + kSingletonBattleFlow)
                        : nullptr;
    return flow && IsReadable(flow, kBattleFlowBytes) ? flow : nullptr;
}

uint8_t* BattleLoader() {
    uint8_t* flow = BattleFlow();
    uint8_t* loader = flow ? *reinterpret_cast<uint8_t* const*>(flow + kBattleFlowLoader) : nullptr;
    return loader && IsReadable(loader, kLoaderBytes) ? loader : nullptr;
}

constexpr uint32_t kLoaderStageHolderOff = 0x10;
constexpr size_t kHolderGetRequestSlot = 0x70 / sizeof(void*);
constexpr uint32_t kRequestStageOff = 0x10;

uint8_t* StageRequest(uint8_t* loader) {
    auto* holder = *reinterpret_cast<uint8_t* const*>(loader + kLoaderStageHolderOff);
    using GetFn = uint8_t* (*)(void*);
    return holder ? (*reinterpret_cast<GetFn* const*>(holder))[kHolderGetRequestSlot](holder) : nullptr;
}

} // namespace

void JumpOnGameThread(int generation, int setupFrames) {
    if (generation != g_jumpGeneration) {
        Log("flow: jump to battle cancelled (another jump came or it was called off)");
        return;
    }
    {
        void* stateClass = CachedObject("VersusBattleState", "Class");
        void* manager = g_manager;
        if (!manager) g_manager = manager = ue4::FindObject("GameScene_0", "GameScene");
        if (!setupFrames) {
            Log("flow: jump to battle: manager %s (%p), state class %s (%p)", ue4::Describe(manager).c_str(), manager,
                ue4::Describe(stateClass).c_str(), stateClass);
        }
        if (!manager || !stateClass || !g_origChange) return;
        if (!setupFrames && g_preloading) {
            static uint64_t loggedAt = 0;
            if (g_pumps - loggedAt > 60) Log("flow: jump to battle: waiting for a preload to end");
            loggedAt = g_pumps;
            FlowRunOnGameThreadAfter(1, [generation] { JumpOnGameThread(generation, 0); });
            return;
        }
        if (!setupFrames && g_pumps - g_sceneChangedAt < kSceneSettleFrames) {
            FlowRunOnGameThreadAfter(1, [generation] { JumpOnGameThread(generation, 0); });
            return;
        }
        const std::string scene = LastScene();
        const bool inVersus = scene == "VersusBattleState" || scene == "VersusBattleSetupState";
        uint8_t* loader = setupFrames || inVersus ? BattleLoader() : nullptr;
        static int loaderSeenAt = -1;
        if (!setupFrames) loaderSeenAt = -1;
        if (setupFrames && loader && loaderSeenAt < 0) loaderSeenAt = setupFrames;
        const bool setupReady = loader && (!setupFrames || setupFrames - loaderSeenAt >= kSetupSettleFrames);
        if (!setupFrames && !loader) {
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
            FlowRunOnGameThreadAfter(1, [generation] { JumpOnGameThread(generation, 1); });
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
            FlowRunOnGameThreadAfter(1, [generation, setupFrames] { JumpOnGameThread(generation, setupFrames + 1); });
            return;
        }
        if (setupFrames) Log("flow: jump to battle: battle setup ready after %d frames", setupFrames);
        if (!loader) {
            Log("flow: jump to battle: no battle loader");
            return;
        }
        std::string stage;
        {
            std::lock_guard lock(g_stageMutex);
            stage = g_pendingStage;
        }
        static wchar_t lent[kStageCodeLength + 1];
        uint8_t* lentTo = nullptr;
        uint8_t saved[kArrayBytes] = {};
        if (!stage.empty()) {
            uint8_t* request = StageRequest(loader);
            if (request && g_testEmptyStage.exchange(false) && IsReadable(request + kRequestStageOff + kArrayNumOff, 4)) {
                *reinterpret_cast<int32_t*>(request + kRequestStageOff + kArrayNumOff) = 0;
                Log("flow: test: the new loader's stage request emptied");
            }
            const std::string before = request ? FStringText(request + kRequestStageOff) : "";
            if (request && before.size() == kStageCodeLength && stage.size() == kStageCodeLength) {
                auto* chars = *reinterpret_cast<wchar_t* const*>(request + kRequestStageOff);
                for (size_t i = 0; i < kStageCodeLength; i++) chars[i] = static_cast<wchar_t>(stage[i]);
                Log("flow: stage %s -> %s", before.c_str(), stage.c_str());
            } else if (request && stage.size() == kStageCodeLength && IsReadable(request + kRequestStageOff, kArrayBytes)) {
                for (size_t i = 0; i < kStageCodeLength; i++) lent[i] = static_cast<wchar_t>(stage[i]);
                lent[kStageCodeLength] = 0;
                std::memcpy(saved, request + kRequestStageOff, kArrayBytes);
                const wchar_t* data = lent;
                const int32_t count = static_cast<int32_t>(kStageCodeLength) + 1;
                std::memcpy(request + kRequestStageOff, &data, sizeof(data));
                std::memcpy(request + kRequestStageOff + kArrayNumOff, &count, sizeof(count));
                std::memcpy(request + kRequestStageOff + kArrayMaxOff, &count, sizeof(count));
                lentTo = request;
                Log("flow: stage request '%s' -> %s for this request", before.c_str(), stage.c_str());
            } else {
                Log("flow: cannot set the stage (request '%s')", before.c_str());
            }
        }
        {
            std::string code = stage.size() == kStageCodeLength ? stage : std::string{};
            if (code.empty() && loader) {
                uint8_t* request = StageRequest(loader);
                const std::string prepared = request ? FStringText(request + kRequestStageOff) : "";
                if (prepared.size() == kStageCodeLength) code = prepared;
            }
            std::lock_guard lock(g_stageMutex);
            g_jumpStage = code;
        }
        if (g_origStep) g_origStep(loader, 0, 1);
        if (lentTo) std::memcpy(lentTo + kRequestStageOff, saved, kArrayBytes);
        WaitForStageThenSwitch(manager, stateClass, loader, 0, g_sceneChangedAt.load(), generation);
    }
}

void FlowChangeScene(const std::string& stateClassName, const std::string& paramName) {
    FlowRunOnGameThread([stateClassName, paramName] {
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
        TraceSceneSteps(stateClassName);
    });
}

void WaitForStageThenSwitch(void* manager, void* stateClass, uint8_t* loader, int frames, uint64_t sceneAt, int generation) {
    if (g_sceneChangedAt != sceneAt || g_jumpGeneration != generation) {
        Log("flow: jump to battle cancelled (%s while its stage loaded)",
            g_jumpGeneration != generation ? "another jump came or it was called off" : ("the scene changed to " + LastScene()).c_str());
        return;
    }
    if (BattleLoader() != loader) {
        if (g_sceneChangedAt < g_jumpRequestedAt && g_jumpRetries < kJumpRetries) {
            g_jumpRetries++;
            Log("flow: the battle loader went away without a scene change; jumping again (%d)", g_jumpRetries.load());
            FlowRunOnGameThreadAfter(kSceneSettleFrames, [generation] { JumpOnGameThread(generation, 0); });
            return;
        }
        Log("flow: jump to battle cancelled (the battle loader went away)");
        return;
    }
    using DoneFn = bool (*)(void*, int);
    const bool loaded = reinterpret_cast<DoneFn>(g_base + kLoaderStepDone)(loader, 0);
    if (!loaded && frames < kStageWaitFrames) {
        FlowRunOnGameThreadAfter(1, [=] { WaitForStageThenSwitch(manager, stateClass, loader, frames + 1, sceneAt, generation); });
        return;
    }
    Log("flow: stage %s after %d frames; switching to the battle", loaded ? "loaded" : "NOT loaded", frames);
    const std::string from = LastScene();
    g_loaderAfterReset = from != "VersusBattleState" && from != "VersusBattleSetupState";
    RememberScene("VersusBattleState");
    g_origChange(manager, stateClass, nullptr);
    g_battleSwitches++;
    StartLoaderWhenReady(0, nullptr, 0);
}

void StartLoaderWhenReady(int frames, uint8_t* seen, int seenAt) {
    uint8_t* loader = BattleLoader();
    if (loader != seen) {
        seen = loader;
        seenAt = frames;
    }
    if (!loader || frames - seenAt < kLoaderStartDelay) {
        if (frames < kLoaderWaitFrames) {
            FlowRunOnGameThreadAfter(1, [=] { StartLoaderWhenReady(frames + 1, seen, seenAt); });
        } else {
            g_loaderAfterReset = false;
            Log("flow: no battle loader to start after %d frames", frames);
        }
        return;
    }
    g_loaderAfterReset = false;
    auto* phase = reinterpret_cast<int32_t*>(loader + kLoaderPhase);
    Log("flow: battle loader %p was at phase %d; starting it", static_cast<void*>(loader), *phase);
    *phase = 1;
}

} // namespace sfv::mod
