// The battle update's phases and jobs: their cost, and the jobs run in order.

#include "mod/battle_internal.h"

#include "mod/battle_actors.h"

namespace sfv::mod {

using namespace battle;

namespace {

constexpr game::CodeSite kUpdateStart{"battle update start", 0x4DC2D0, {0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xF1, 0x48, 0x8B, 0x49, 0x18, 0x48}};
constexpr game::CodeSite kSchedulerPhase{"scheduler phase", 0x4CD890, {0x4C, 0x8B, 0xDC, 0x53, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05, 0xDE, 0x49}};
constexpr game::CodeSite kTickProper{"battle tick", 0x4D2850, {0x83, 0xB9, 0x90, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC1, 0x74, 0x0C, 0x83, 0xB9, 0x94, 0x01}};
using UpdateStartFn = uint64_t (*)(void*);
using PhaseFn = uint64_t (*)(void* scheduler, int phase);
using TickProperFn = uint64_t (*)(void* battleFrame);
UpdateStartFn g_origUpdateStart = nullptr;
PhaseFn g_origPhase = nullptr;
TickProperFn g_origTickProper = nullptr;
std::atomic<bool> g_phaseTiming{false};
std::atomic<bool> g_skipResimPresentation{false};

enum PhasePart { kPartStart,
                 kPart1,
                 kPart2,
                 kPart4,
                 kPart8,
                 kPartTick,
                 kPartCount };

const char* const kPartNames[kPartCount] = {"start", "phase 1", "phase 2", "phase 4", "phase 8", "tick"};
uint64_t g_partTicks[2][kPartCount] = {};
uint64_t g_partCalls[2] = {};
uint64_t g_profiledTicks = 0;

template <typename F>
uint64_t Timed(PhasePart part, F call) {
    if (!g_phaseTiming || !g_inDriver) return call();
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const uint64_t r = call();
    QueryPerformanceCounter(&b);
    g_partTicks[g_replayingTick ? 1 : 0][part] += b.QuadPart - a.QuadPart;
    return r;
}

uint64_t UpdateStartDetour(void* frame) {
    return Timed(kPartStart, [&] { return g_origUpdateStart(frame); });
}

bool IsPresentationPhase(int phase) { return phase == 4 || phase == 8; }

uint64_t PhaseDetour(void* scheduler, int phase) {
    const PhasePart part = phase == 1 ? kPart1 : phase == 2 ? kPart2
                                             : phase == 4   ? kPart4
                                                            : kPart8;
    if (phase != 1 && phase != 2 && phase != 4 && phase != 8) return g_origPhase(scheduler, phase);
    if (g_skipResimPresentation && g_inDriver && g_replayingTick && IsPresentationPhase(phase)) return 0;
    if (g_inDriver) FlowSetSchedulerPhase(phase, g_replayingTick);
    const uint64_t r = Timed(part, [&] { return g_origPhase(scheduler, phase); });
    FlowSetSchedulerPhase(0);
    return r;
}

uint64_t TickProperDetour(void* frame) {
    return Timed(kPartTick, [&] { return g_origTickProper(frame); });
}

constexpr game::CodeSite kJobRunner{"battle job runner", 0x4CD420, {0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57}};
constexpr game::CodeSite kPhaseRunsQueue{"phase runs the job queue", 0x4CDA89, {0x48, 0x8B, 0x0D, 0x50, 0x76, 0x5A, 0x03, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xE8, 0x86, 0xF9, 0xFF}};
constexpr uint32_t kRunnerModeTest = 0x4CD46B;
constexpr uint8_t kRunnerModeTestBytes[] = {0x39, 0x6F, 0x74, 0x0F, 0x85};
constexpr uint32_t kJobQueueGlobal = 0x3A750E0;
constexpr size_t kJobQueueModeOff = 0x74;
constexpr size_t kJobQueueListOff = 0x08, kJobNextOff = 0x10, kJobGroupOff = 0x30, kJobPayloadOff = 0x28;
constexpr size_t kTaskNodeKeyBeforeOff = 0x08;
constexpr size_t kTaskClosureOff = 0x18;
constexpr size_t kClosureAdjustOff = 0x08, kClosureObjectOff = 0x10;
constexpr int kPhaseLogTicks = 600;
constexpr int kJobsLogged = 400;
constexpr intptr_t kNearFighter = 0x20000;
using JobRunnerFn = uint64_t (*)(uint8_t* queue, int mode);
JobRunnerFn g_origJobRunner = nullptr;
uint8_t* const* g_jobQueue = nullptr;
std::atomic<bool> g_serialJobs{true};
std::atomic<int> g_jobOrderLogs{0};

void LogJobOrder(uint8_t* queue) {
    std::vector<std::pair<uint8_t*, int>> fighters;
    for (const StateRegion& r : g_game->Regions()) {
        if (r.size != actor::kRegionSize || !IsReadable(r.ptr - actor::kVtableAt, 8)) continue;
        if (*reinterpret_cast<const uintptr_t*>(r.ptr - actor::kVtableAt) != g_imageBase + actor::kFighterVtable) continue;
        fighters.push_back({r.ptr - actor::kVtableAt, *reinterpret_cast<const int32_t*>(r.ptr + actor::kSide)});
    }
    std::string sides;
    for (const auto& [owner, side] : fighters) {
        char b[48];
        std::snprintf(b, sizeof(b), " %p side %d", static_cast<void*>(owner), side);
        sides += b;
    }
    Log("joborder: tick %u%s, fighters%s", FlowDrivenTicks(), g_replayingTick ? " replayed" : "", sides.c_str());
    int n = 0;
    for (uint8_t* job = *reinterpret_cast<uint8_t**>(queue + kJobQueueListOff); job && n < kJobsLogged; n++) {
        if (!IsReadable(job, 0x48)) break;
        const int32_t group = *reinterpret_cast<const int32_t*>(job + kJobGroupOff);
        uint64_t key = 0;
        uintptr_t fn = 0, object = 0;
        uint8_t* payload = *reinterpret_cast<uint8_t**>(job + kJobPayloadOff);
        uint8_t* task = payload && IsReadable(payload, 8) ? *reinterpret_cast<uint8_t**>(payload) : nullptr;
        if (task && IsReadable(task - kTaskNodeKeyBeforeOff, 8 + kTaskClosureOff + 8)) {
            key = *reinterpret_cast<const uint64_t*>(task - kTaskNodeKeyBeforeOff);
            const uint8_t* closure = *reinterpret_cast<uint8_t* const*>(task + kTaskClosureOff);
            if (closure && IsReadable(closure, 0x18)) {
                fn = *reinterpret_cast<const uintptr_t*>(closure);
                object = *reinterpret_cast<const uintptr_t*>(closure + kClosureObjectOff) + *reinterpret_cast<const int32_t*>(closure + kClosureAdjustOff);
            }
        }
        std::string near_ = "-";
        for (size_t i = 0; i < fighters.size(); i++) {
            const intptr_t d = static_cast<intptr_t>(object) - reinterpret_cast<intptr_t>(fighters[i].first);
            if (d > -kNearFighter && d < kNearFighter) {
                char b[48];
                std::snprintf(b, sizeof(b), "fighter %zu %s0x%llx", i, d < 0 ? "-" : "+", static_cast<unsigned long long>(d < 0 ? -d : d));
                near_ = b;
            }
        }
        const bool inImage = ImageHolds(fn);
        Log("joborder:   %3d key %u.%u group %d fn %s%llx on %p (%s)", n, static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key),
            group, inImage ? "rva " : "", static_cast<unsigned long long>(inImage ? fn - g_imageBase : fn),
            reinterpret_cast<void*>(object), near_.c_str());
        job = *reinterpret_cast<uint8_t**>(job + kJobNextOff);
    }
}

uint64_t JobRunnerDetour(uint8_t* queue, int mode) {
    if (!g_inDriver || !queue || !g_jobQueue || queue != *g_jobQueue) return g_origJobRunner(queue, mode);
    if (g_jobOrderLogs > 0) {
        g_jobOrderLogs--;
        LogJobOrder(queue);
    }
    if (!g_serialJobs) return g_origJobRunner(queue, mode);
    auto* threads = reinterpret_cast<int32_t*>(queue + kJobQueueModeOff);
    const int32_t was = *threads;
    *threads = 0;
    const uint64_t r = g_origJobRunner(queue, mode);
    *threads = was;
    return r;
}

} // namespace

void battle::InstallPhaseHooks(const GameImage& image) {
    if (!Hook(image, kUpdateStart, reinterpret_cast<void*>(&UpdateStartDetour), reinterpret_cast<void**>(&g_origUpdateStart)) ||
        !Hook(image, kSchedulerPhase, reinterpret_cast<void*>(&PhaseDetour), reinterpret_cast<void**>(&g_origPhase)) ||
        !Hook(image, kTickProper, reinterpret_cast<void*>(&TickProperDetour), reinterpret_cast<void**>(&g_origTickProper))) {
        Log("battle driver: phasecost unavailable");
    }
    const auto* phaseEnd = image.At<const uint8_t*>(kPhaseRunsQueue.rva);
    const auto* modeTest = image.At<const uint8_t*>(kRunnerModeTest);
    if (std::memcmp(phaseEnd, kPhaseRunsQueue.bytes, sizeof(kPhaseRunsQueue.bytes)) != 0 ||
        std::memcmp(modeTest, kRunnerModeTestBytes, sizeof(kRunnerModeTestBytes)) != 0) {
        Log("battle driver: signature mismatch at %s: the battle's jobs run on its threads as they come", kPhaseRunsQueue.name);
        return;
    }
    g_jobQueue = image.At<uint8_t* const*>(kJobQueueGlobal);
    if (!Hook(image, kJobRunner, reinterpret_cast<void*>(&JobRunnerDetour), reinterpret_cast<void**>(&g_origJobRunner))) {
        g_jobQueue = nullptr;
        Log("battle driver: the battle's jobs run on its threads as they come");
    }
}

void battle::CountPhaseTick() {
    if (!g_phaseTiming) return;
    g_partCalls[g_replayingTick ? 1 : 0]++;
    g_profiledTicks++;
    if (g_partCalls[0] + g_partCalls[1] < kPhaseLogTicks) return;
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

void BattleSetSerialJobs(bool on) {
    g_serialJobs = on;
    Log("battle driver: driven ticks run the battle's jobs %s%s", on ? "in order on the game thread" : "on its threads as they come",
        g_jobQueue ? "" : " (the job runner is not hooked: always on its threads)");
}

bool BattleSerialJobs() { return g_serialJobs && g_jobQueue; }

void BattleLogJobOrderNext(int calls) {
    g_jobOrderLogs = calls;
    Log("joborder: the next %d driven job runs", calls);
}

void BattleSetResimPresentationSkip(bool on) {
    g_skipResimPresentation = on;
    Log("resimskip: replayed ticks %s scheduler phases 4 and 8", on ? "leave out" : "run");
}

void BattleSetPhaseTiming(bool on) {
    g_phaseTiming = on;
    FlowSetTaskProfiling(on, g_profiledTicks);
    g_profiledTicks = 0;
    Log("phasecost: %s", on ? "timing the battle update's parts" : "off");
}

} // namespace sfv::mod
