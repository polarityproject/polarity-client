#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

namespace {

constexpr uintptr_t kVirtualTask = 0x3EB3F4;

constexpr uintptr_t kSoundReconcileTask = 0x4AFEF0;

std::atomic<bool> g_soundRollback{true};

std::atomic<bool> g_schedReplaying{false};

std::atomic<bool> g_taskProfiling{false};

std::mutex g_resimSkipMutex;

const std::vector<uintptr_t> kLeanResimTasks = {kClassKey | 0x2C63BB8, kClassKey | 0x2C63E00, kClassKey | 0x2C64100,
                                                0x3E3490, 0x4D6B40, 0x37D1B0};

std::vector<uintptr_t> g_resimSkip = kLeanResimTasks;

std::atomic<bool> g_resimSkipAny{true};

struct TaskCost {
    uint64_t ticks = 0, calls = 0;
    uintptr_t object = 0;
};

std::map<uint64_t, TaskCost> g_taskCosts;

} // namespace

namespace flow {

uint64_t TaskThunkDetour(void* closure, void* a2, void* a3, void* a4) {
    {
        const auto* c = static_cast<const uint8_t*>(closure);
        if (*reinterpret_cast<const uintptr_t*>(c) == g_base + kPhaseMachineTask) {
            g_phaseMachine = reinterpret_cast<uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + kThunkObjectOff) +
                                                        *reinterpret_cast<const int32_t*>(c + kThunkAdjustOff));
        }
        if (*reinterpret_cast<const uintptr_t*>(c) == g_base + kRoundDirectorTask) {
            auto* d = reinterpret_cast<uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + kThunkObjectOff) +
                                                 *reinterpret_cast<const int32_t*>(c + kThunkAdjustOff));
            g_roundDirector = d;
            const bool mayEnd = g_directorPerTick && *reinterpret_cast<const int32_t*>(d + kDirectorState) == kDirectorStopped &&
                                *reinterpret_cast<const int32_t*>(d + kDirectorStopKind) != kPlainStop;
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
            const auto* object = reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + kThunkObjectOff) +
                                                                  *reinterpret_cast<const int32_t*>(c + kThunkAdjustOff));
            key = kClassKey | (*reinterpret_cast<const uintptr_t*>(object) - g_base);
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
            const auto* object = reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uintptr_t*>(c + kThunkObjectOff) +
                                                                  *reinterpret_cast<const int32_t*>(c + kThunkAdjustOff));
            key = static_cast<uint64_t>(g_schedPhase.load()) << 32 | kClassKey |
                  (*reinterpret_cast<const uintptr_t*>(object) - g_base);
        }
        TaskCost& cost = g_taskCosts[key];
        cost.ticks += b.QuadPart - a.QuadPart;
        cost.calls++;
        if (!cost.object) cost.object = *reinterpret_cast<const uintptr_t*>(c + kThunkObjectOff) + *reinterpret_cast<const int32_t*>(c + kThunkAdjustOff);
    } else {
        result = g_origThunk(closure, a2, a3, a4);
    }
    if (g_taskCensus > 0) {
        g_taskCensus--;
        const auto* c = static_cast<const uint8_t*>(closure);
        const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(c);
        const uintptr_t object = *reinterpret_cast<const uintptr_t*>(c + kThunkObjectOff) + *reinterpret_cast<const int32_t*>(c + kThunkAdjustOff);
        std::lock_guard lock(g_censusMutex);
        g_census.emplace_back(fn, object);
    }
    return result;
}

} // namespace flow

void FlowSetSchedulerPhase(int phase, bool replaying) {
    g_schedPhase = phase;
    g_schedReplaying = replaying;
}

void FlowSetSoundRollback(bool on) {
    g_soundRollback = on;
    Log("sound: the game's own rollback of sounds %s", on ? "on (resimulated ticks' repeats are not played again)"
                                                          : "off (resimulated ticks play their sounds again)");
}

bool FlowSoundRollbackOn() { return g_soundRollback; }

std::vector<uintptr_t> FlowDefaultLeanResimTasks() { return kLeanResimTasks; }

void FlowSetResimTaskSkip(std::vector<uintptr_t> rvas) {
    std::lock_guard lock(g_resimSkipMutex);
    g_resimSkip = std::move(rvas);
    g_resimSkipAny = !g_resimSkip.empty();
    std::string text;
    for (uintptr_t r : g_resimSkip) text += " 0x" + [r] { char b[16]; std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(r)); return std::string(b); }();
    Log("resimtasks: replayed ticks leave out %zu tasks in phases 4 and 8:%s", g_resimSkip.size(), text.c_str());
}

void FlowSetTaskProfiling(bool on, uint64_t ticks) {
    if (on) {
        FlowRunOnGameThread([] { g_taskCosts.clear(); });
        g_taskProfiling = true;
        return;
    }
    g_taskProfiling = false;
    FlowRunOnGameThread([ticks] {
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
                Log("phasecost:   %s 0x%x: %.1f us per tick, %llu calls, on %s", low & kClassKey ? "virtual task, class vtable" : "rva",
                    low & ~kClassKey,
                    ticks ? c.ticks * 1e6 / f.QuadPart / ticks : 0.0, static_cast<unsigned long long>(c.calls),
                    ue4::Describe(reinterpret_cast<void*>(c.object)).c_str());
            }
        }
    });
}

void FlowTaskCensus(int calls) {
    {
        std::lock_guard lock(g_censusMutex);
        g_census.clear();
    }
    g_taskCensus = calls;
    std::thread([] {
        ThreadGuard running;
        for (int i = 0; i < 100 && g_taskCensus > 0 && !Stopping(); i++) Sleep(50);
        g_taskCensus = 0;
        std::vector<std::pair<uintptr_t, uintptr_t>> seen;
        {
            std::lock_guard lock(g_censusMutex);
            seen = g_census;
        }
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
            const bool inImage = InImage(fn);
            Log("  task fn %s%llx x%d on %p %s", inImage ? "rva " : "", static_cast<unsigned long long>(inImage ? fn - g_base : fn), n,
                reinterpret_cast<void*>(object), ue4::Describe(reinterpret_cast<void*>(object)).c_str());
        }
    }).detach();
}

std::function<std::string(const void*)> g_regionOf;

void FlowSetRegionLookup(std::function<std::string(const void*)> lookup) {
    g_regionOf = std::move(lookup);
}

constexpr uint32_t kLockBattleContext = 0x307F90;

uint8_t* BattleContext(uint8_t* task) {
    using GetFn = uint8_t* (*)(uint8_t*);
    return reinterpret_cast<GetFn>(g_base + kLockBattleContext)(task + kTaskRefOff);
}

void FlowTestEmptyStageRequest() { g_testEmptyStage = true; }

} // namespace sfv::mod
