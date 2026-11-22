// SfvGame: a driven tick, the inputs, warm-up and loading a snapshot.

#include "mod/sfv_game_internal.h"

namespace sfv::mod {

using namespace snapshot;

void SfvGame::OnInputUnit(uint8_t* inputUnit) {
    lastInputMs_ = GetTickCount64();
    if (inputUnit != inputUnit_) {
        inputUnit_ = inputUnit;
        Log("InputUnit at %p, %d pads, local mask %x", inputUnit, Read<int32_t>(inputUnit, game::inputunit::kPadCount),
            Read<uint32_t>(inputUnit, game::inputunit::kLocalMask));
    }
    auto* values = reinterpret_cast<uint32_t*>(inputUnit + game::inputunit::kValues);
    const int pads = std::clamp(Read<int32_t>(inputUnit, game::inputunit::kPadCount), 0, 4);
    for (int i = 0; i < pads; i++) sampled_[i] = values[i];
    if (inputLog > 0) {
        inputLog--;
        static uint32_t last[4] = {};
        if (std::memcmp(last, sampled_, sizeof(last)) != 0) {
            std::memcpy(last, sampled_, sizeof(last));
            Log("inputlog: pads %d, values %08X %08X %08X %08X", pads, sampled_[0], sampled_[1], sampled_[2], sampled_[3]);
        }
    }
    BattleActorLogTick();
    BattleCineLogTick();
    BattleCamTraceRecord(TraceAt::TickEnd);
    FrameMeterTick(*this);
    BattleFtraceTick();
    if (injecting_) {
        values[0] = inject_[0];
        values[1] = inject_[1];
    }
}

void SfvGame::OnRollbackUnitTick(uint8_t* unit) {
    if (unit != unit_) {
        unit_ = unit;
        FlowResetRoundFlow();
        Log("RollbackUnit at %p (new battle)", unit);
    }
}

uint8_t* SfvGame::Manager() const {
    if (!unit_) return nullptr;
    using GetManagerFn = uint8_t* (*)(void*);
    auto getManager = image_.At<GetManagerFn>(game::kUnitGetManager.rva);
    uint8_t* m = getManager(unit_ + game::unit::kManagerContext);
    return IsReadable(m, mgr::kGroupsEnd + 8) ? m : nullptr;
}

void SfvGame::FindPresentationState() {
    presentation_.clear();
    if (!ue4::EnsureObjects(image_.base)) return;

    struct Found {
        uint8_t* object;
        int number;
    };

    std::vector<Found> pawns, targets;
    ue4::ForEachObject([&](const void* object) {
        const std::string cls = ue4::ObjectName(ue4::ClassOf(object));
        const bool pawn = cls == "KBP_BattlePawn_C";
        if (!pawn && cls != "KBP_BattleViewTarget_C") return;
        if (ue4::ObjectName(ue4::OuterOf(object)) != "PersistentLevel") return;
        const std::string name = ue4::ObjectName(object);
        const size_t us = name.rfind('_');
        const int number = us == std::string::npos ? -1 : std::atoi(name.c_str() + us + 1);
        (pawn ? pawns : targets).push_back({static_cast<uint8_t*>(const_cast<void*>(object)), number});
    });
    auto newest = [](std::vector<Found>& v) {
        std::sort(v.begin(), v.end(), [](const Found& a, const Found& b) { return a.number > b.number; });
    };
    newest(pawns);
    newest(targets);
    for (size_t i = 0; i < pawns.size() && i < 2; i++) {
        presentation_.push_back({"pawn.clock", pawns[i].object + game::presentation::kPawnClock, 8});
    }
    if (!targets.empty()) presentation_.push_back({"viewtarget.clock", targets[0].object + game::presentation::kViewTargetClock, 8});
    Log("presentation state: %zu pawns, %zu view targets; %zu regions", pawns.size(), targets.size(), presentation_.size());
}

uint8_t* SfvGame::SingletonTicks() const {
    auto* engine = *image_.At<uint8_t* const*>(game::singleton::kEngineGlobal);
    if (!engine || !IsReadable(engine + game::singleton::kSingleton, 8)) return nullptr;
    uint8_t* singleton = *reinterpret_cast<uint8_t* const*>(engine + game::singleton::kSingleton);
    return singleton && IsReadable(singleton + game::singleton::kTicks, game::singleton::kTicksSize)
               ? singleton + game::singleton::kTicks
               : nullptr;
}

bool SfvGame::RestoreRingFrame(const game::FrameId& id, bool discardNewer) {
    uint8_t* m = Manager();
    if (!m) return false;
    using RestoreFn = int (*)(void*, const game::FrameId*, int);
    return image_.At<RestoreFn>(game::kManagerRestore.rva)(m, &id, discardNewer ? 1 : 0) != 0;
}

namespace {

std::mutex g_skipMutex;
std::vector<uint64_t> g_skipCallbacks;
std::atomic<bool> g_skipAny{false};
std::atomic<bool> g_gameFrameIds{true};

} // namespace

std::vector<uint64_t> GameDefaultSkippedLoadCallbacks() {
    return {0x4C1D10ull << 32 | 0x3587D0, 0x447A60ull << 32 | 0x448B30, 0x447A60ull << 32 | 0x448B40};
}

void GameSetSkippedLoadCallbacks(std::vector<uint64_t> keys) {
    std::lock_guard lock(g_skipMutex);
    g_skipCallbacks = std::move(keys);
    g_skipAny = !g_skipCallbacks.empty();
    std::string text;
    for (uint64_t k : g_skipCallbacks) {
        char part[48];
        std::snprintf(part, sizeof(part), " 0x%llx/0x%llx", static_cast<unsigned long long>(k >> 32),
                      static_cast<unsigned long long>(k & 0xFFFFFFFF));
        text += part;
    }
    Log("loads skip %zu game callbacks:%s", g_skipCallbacks.size(), text.c_str());
}

void SfvGame::RunPostRestoreCallbacks(const game::FrameId& gameId, const game::FrameId& oldId) {
    uint8_t* m = Manager();
    if (!m) return;
    const uint8_t* sentinel = Read<uint8_t*>(m, mgr::kPostRestoreList);
    if (!IsReadable(sentinel, 8)) return;
    using CallbackFn = void (*)(void* ctx, const game::FrameId*);
    for (const uint8_t* node = Read<uint8_t*>(sentinel, mgr::callback::kNext); node && node != sentinel;
         node = Read<uint8_t*>(node, mgr::callback::kNext)) {
        auto fn = Read<CallbackFn>(node, mgr::callback::kFunction);
        if (!fn || Read<uint64_t>(node, mgr::callback::kToken) == 0) continue;
        void* ctx = Read<void*>(node, mgr::callback::kContext);
        uintptr_t target = ctx ? *static_cast<const uintptr_t*>(ctx) : 0;
        target = target > image_.base && target < image_.base + kImageSpan ? target - image_.base : 0;
        const uint64_t key = ((reinterpret_cast<uintptr_t>(fn) - image_.base) << 32) | target;
        if (target == kCameraResetCallback && CameraRollbackOn()) continue;
        if (g_skipAny) {
            std::lock_guard lock(g_skipMutex);
            if (std::find(g_skipCallbacks.begin(), g_skipCallbacks.end(), key) != g_skipCallbacks.end()) continue;
        }
        const bool sound = target == kSoundRestoreCallback;
        const bool useGame = sound ? FlowSoundRollbackOn() : g_gameFrameIds.load();
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        fn(ctx, useGame ? &gameId : &oldId);
        QueryPerformanceCounter(&b);
        CallbackCost& c = callbackCosts_[key];
        c.ticks += b.QuadPart - a.QuadPart;
        c.calls++;
    }
}

std::string SfvGame::BuildId() const {
    char buf[128];
    const uint64_t noise = NoiseHash();
    std::snprintf(buf, sizeof(buf), "sfv-%08X/adapter-13/sim-%d/map-%016llx/noise-%016llx", image_.timestamp, kSimVersion,
                  static_cast<unsigned long long>(stateMap_ ? stateMap_->Fingerprint() : 0),
                  static_cast<unsigned long long>(noise));
    return buf;
}

bool SfvGame::BeginMatch(const MatchSetup& setup) {
    (void)setup;
    RefreshRegions();
    return CanRunTicks() && !regions_.empty();
}

PackedInput SfvGame::ReadLocalInput(int player) {
    if (inputOverride) return inputOverride(player);
    if (localInput && !bothPadsLocal) return localInput(localPadSlot);
    return sampled_[bothPadsLocal ? std::clamp(player, 0, 3) : localPadSlot];
}

void SfvGame::Advance(const FrameInputs& inputs, bool rollingBack) {
    if (!CanRunTicks()) return;
    hitch::AddTick(rollingBack);
    BattleSetReplayingTick(rollingBack);
    const PackedInput start = FlowPresentationStart(inputs.p[0], inputs.p[1]);
    inject_[0] = (inputs.p[0] & ~kStart) | start;
    inject_[1] = (inputs.p[1] & ~kStart) | start;
    if (inputUnit_) *reinterpret_cast<int32_t*>(inputUnit_ + game::inputunit::kRoundParam) = 0;
    injecting_ = true;
    FlowBeginDrivenTick();
    BattleSeedTickRandom();
    CameraTickBegin();
    RunBattleTick(rollingBack);
    CameraTickEnd();
    injecting_ = false;
    FlowRunRoundDirector();
    LatchInputs();
    if (probeTicks_ > 0 && !rollingBack) {
        CaptureStartProbe(probeTicks_ == 2 ? "after the first tick" : "after the second tick");
        probeTicks_--;
    }
    GameCheckChangeWatch(regions_, image_.base);
    if (inputUnit_ && exclusiveTicks) {
        auto* values = reinterpret_cast<uint32_t*>(inputUnit_ + game::inputunit::kValues);
        for (int i = 0; i < 4; i++) values[i] = 0;
    }
}

namespace {

std::atomic<bool> g_leanResim{true};
std::atomic<int> g_warmupSleepMs{0};
std::atomic<int> g_warmupExtra{0};

} // namespace

void GameSetLeanResim(bool on) { g_leanResim = on; }

void GameSetWarmupSleep(int ms) { g_warmupSleepMs = std::max(0, ms); }

void GameSetFrameIds(bool on) { g_gameFrameIds = on; }

void GameSetWarmupExtra(int ticks) { g_warmupExtra = std::max(0, ticks); }

bool GameLeanResimOn() { return g_leanResim; }

void SfvGame::RunBattleTick(bool replayed) {
    namespace bf = game::battleframe;
    uint8_t* frame = battleFrame_;
    if (!replayed || !g_leanResim) {
        *reinterpret_cast<int32_t*>(frame + bf::kExtraTicks) = 0;
        runFrame_(frame);
        return;
    }
    using StartFn = uint64_t (*)(void*);
    using PhaseFn = uint64_t (*)(void*, int);
    using TickFn = uint64_t (*)(void*);
    auto u32 = [frame](uint32_t at) -> uint32_t& { return *reinterpret_cast<uint32_t*>(frame + at); };
    void* scheduler = *reinterpret_cast<void**>(frame + bf::kScheduler);
    auto phase = [&](int p) {
        if (scheduler) image_.At<PhaseFn>(bf::kSchedulerPhaseRva)(scheduler, p);
    };
    image_.At<StartFn>(bf::kUpdateStartRva)(frame);
    phase(1);
    const bool stopped = u32(bf::kStopped) != 0 && u32(bf::kStopReleased) == 0;
    const uint32_t slow = u32(bf::kSlowEvery);
    const bool slowed = slow != 0 && u32(bf::kUpdates) % (slow + 1) != 0;
    if (!stopped && !slowed) {
        const uint32_t extra = u32(bf::kExtraTicks), least = u32(bf::kMinExtraTicks);
        const uint32_t ticks = (extra >= least ? extra : least) + 1;
        for (uint32_t i = 0; i < ticks; i++) {
            u32(bf::kTickCount)++;
            phase(2);
            if (!image_.At<TickFn>(bf::kTickProperRva)(frame)) break;
        }
        phase(4);
        u32(bf::kFightUpdates)++;
        const uint32_t step = u32(bf::kClockStep);
        float add = static_cast<float>(static_cast<int32_t>(step & 0xFFFF)) * *image_.At<const float*>(bf::kClockScaleRva);
        add += static_cast<float>(static_cast<int32_t>(step) >> 16);
        float& clock = *reinterpret_cast<float*>(frame + bf::kFrameClock);
        clock = add + clock;
    }
    u32(bf::kUpdates)++;
    *reinterpret_cast<uint64_t*>(frame + bf::kExtraTicks) = 0;
}

bool SfvGame::OutcomeDecided() { return FlowMatchDecided(); }

uint8_t SfvGame::FrameFlags() { return FlowRoundInFight() ? kFrameFight : 0; }

bool SfvGame::MatchWins(uint32_t& left, uint32_t& right) const {
    constexpr size_t kRoundStateSize = 228;
    for (const StateRegion& r : regions_) {
        if (r.size != kRoundStateSize || !IsReadable(r.ptr, r.size)) continue;
        std::memcpy(&left, r.ptr + 0xC4, 4);
        std::memcpy(&right, r.ptr + 0xC8, 4);
        return true;
    }
    return false;
}

bool SfvGame::WarmupStep() {
    RefreshRegions();
    if (Regions().empty() || !InputUnit()) {
        if (const int ms = g_warmupSleepMs; ms > 0) Sleep(ms);
        Advance(FrameInputs{}, false);
        warmupTicks_++;
        return true;
    }
    constexpr int kSettleSteps = 12, kSettleMax = 240;
    if (Regions().size() != settleCount_) {
        if (settleCount_ && settleLogs_++ < 6) {
            Log("warmup: %zu state regions (was %zu) at neutral tick %d, %d calls after the first", Regions().size(), settleCount_,
                warmupTicks_, settleCalls_);
        }
        settleCount_ = Regions().size();
        settleSame_ = 0;
    }
    if (settleCalls_++ < kSettleMax && settleSame_++ < kSettleSteps) return true;
    if (settleCalls_ == kSettleMax + 1) Log("warmup: the state regions still changed after %d calls; going on with %zu", kSettleMax, Regions().size());
    if (objectsAt_ < 0) objectsAt_ = warmupTicks_;
    const int after = warmupTicks_ - objectsAt_;
    if (after < g_warmupExtra) {
        Advance(FrameInputs{}, false);
        warmupTicks_++;
        cameraWait_++;
        return true;
    }
    return false;
}

void SfvGame::LatchInputs() {
    if (!inputUnit_ || !latchEachTick) return;
    using LatchFn = void (*)(uint8_t*);
    image_.At<LatchFn>(game::kInputUnitLatch.rva)(inputUnit_);
}

void SfvGame::OnStateLoaded(int frame) {
    LARGE_INTEGER a, b, c;
    QueryPerformanceCounter(&a);
    const game::FrameId oldId{frame, 0x5346};
    game::FrameId gameId = oldId;
    if (inputUnit_) {
        gameId.lo = Read<int32_t>(inputUnit_, game::inputunit::kFrameTag);
        gameId.hi = Read<int32_t>(inputUnit_, game::inputunit::kSimFrame);
        if (restoresLogged_ < 5) {
            restoresLogged_++;
            Log("restore: to frame %d (InputUnit tag %d, simulated frame %d, driven tick %u)", frame, gameId.lo, gameId.hi,
                FlowDrivenTicks());
        }
    }
    RunPostRestoreCallbacks(gameId, oldId);
    CameraLoaded(FlowDrivenTicks());
    QueryPerformanceCounter(&b);
    FlowRoundDirectorFromSnapshot();
    FlowMatchStopToDirector(battleFrame_);
    LatchInputs();
    QueryPerformanceCounter(&c);
    ownLoadTicks_ += c.QuadPart - b.QuadPart;
    if (++loadsTimed_ % 200 == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double us = 1e6 / static_cast<double>(f.QuadPart);
        std::vector<std::pair<double, uintptr_t>> byCost;
        double total = 0;
        for (const auto& [rva, cost] : callbackCosts_) {
            byCost.push_back({cost.ticks * us / 200.0, rva});
            total += cost.ticks * us / 200.0;
        }
        std::sort(byCost.rbegin(), byCost.rend());
        std::string text;
        for (size_t k = 0; k < byCost.size() && k < 10; k++) {
            const uintptr_t key = byCost[k].second;
            const int calls = callbackCosts_[key].calls / 200;
            char part[80];
            std::snprintf(part, sizeof(part), "%s0x%zx/0x%zx x%d %.0f us", k ? ", " : "", static_cast<size_t>(key >> 32),
                          static_cast<size_t>(key & 0xFFFFFFFF), calls, byCost[k].first);
            text += part;
        }
        Log("load callbacks per load (last 200): %.0f us in %zu kinds of game callback (%s), %.0f us round state", total,
            callbackCosts_.size(), text.c_str(), ownLoadTicks_ * us / 200.0);
        callbackCosts_.clear();
        ownLoadTicks_ = 0;
    }
}

} // namespace sfv::mod
