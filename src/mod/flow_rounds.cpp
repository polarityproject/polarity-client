#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

namespace {

constexpr uintptr_t kPhaseMachineVtable = 0x2C5F838;

constexpr uint32_t kMachineFields = 0x120;

constexpr size_t kMachineFieldsSize = 0x20;

constexpr uint32_t kPhaseTimerDestroy = 0x50;

constexpr uint32_t kPhaseTimerClone = 0x58;

constexpr uint32_t kPhaseTimerArgument = 0x60;

constexpr uint32_t kPhaseTimerCall = 0x68;

constexpr size_t kMaxPhases = 24;

constexpr size_t kPhaseBytesLarge = 0xC0, kPhaseBytesSmall = 0xA0;
constexpr uintptr_t kLargePhaseVtables[] = {0x2C6A668, 0x2C6A878, 0x2C6AA18};

constexpr size_t kKoWaitTailBytes = 0x48;

constexpr size_t kPhaseRing = 64;

struct PhaseCopy {
    uint8_t bytes[kPhaseBytesLarge];
    uint8_t argument[16];
    bool hasArgument;
};

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
    uint8_t koWait[kKoWaitTailBytes];
    void* koWaitHeld;
};

std::vector<MachineCopy> g_phaseRing(kPhaseRing);

struct FreezeCopy {
    bool valid;
    bool hasDirector;
    int32_t director[14];
    int32_t directorTimers[16];
    int32_t directorCounts[5];
    MachineCopy machine;
};

FreezeCopy g_freeze{};

constexpr uintptr_t kKoWaitVtable = 0x2C602F8;

constexpr uint32_t kKoWaitState = 0xF8;

constexpr size_t kKoWaitStateSize = kKoWaitTailBytes;

constexpr size_t kKoWaitLatch = 0x100 - kKoWaitState, kKoWaitMode = 0x104 - kKoWaitState;

constexpr uint32_t kKoWaitObject = 0x110, kKoWaitRef = 0x118;

std::atomic<DWORD> g_gameThread{0};

int g_waitRefBacks = 0, g_waitRefEmptied = 0, g_waitRefLeft = 0;

std::atomic<bool> g_koWaitRollback{true};

int g_koWaitLogs = 0;

int g_phaseRestoreSkips = 0;

} // namespace

namespace flow {

uint32_t TickNow() {
    return g_roundFlow.ticks;
}

uint8_t* LivePhaseMachine() {
    uint8_t* m = g_phaseMachine;
    if (!m || !IsReadable(m, kMachineBytes)) return nullptr;
    return *reinterpret_cast<const uintptr_t*>(m) == g_base + kPhaseMachineVtable ? m : nullptr;
}

} // namespace flow

namespace {

size_t PhaseSize(const uint8_t* phase) {
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(phase) - g_base;
    for (const uintptr_t large : kLargePhaseVtables) {
        if (vtable == large) return kPhaseBytesLarge;
    }
    return kPhaseBytesSmall;
}

uint8_t* LiveKoWait() {
    uint8_t* w = g_koWaitRollback ? g_koWait : nullptr;
    if (!w || !IsReadable(w, kKoWaitState + kKoWaitStateSize)) return nullptr;
    return *reinterpret_cast<const uintptr_t*>(w) == g_base + kKoWaitVtable ? w : nullptr;
}

void HoldRef(void** slot, void* block) {
    if (*slot == block) return;
    if (g_sharedAssign) g_sharedAssign(slot, &block);
    else *slot = nullptr;
}

void ReleaseHeld(MachineCopy& c) {
    void* block = c.koWaitHeld;
    if (!block) return;
    c.koWaitHeld = nullptr;
    const DWORD game = g_gameThread;
    if (!game || GetCurrentThreadId() == game) {
        HoldRef(&block, nullptr);
        return;
    }
    FlowRunOnGameThread([block]() mutable { HoldRef(&block, nullptr); });
}

void ForgetCopies() {
    for (MachineCopy& c : g_phaseRing) {
        c.valid = false;
        ReleaseHeld(c);
    }
}

uint8_t* LiveRecorder() {
    auto* recorder = *reinterpret_cast<uint8_t* const*>(g_base + kRecorderGlobal);
    return recorder && IsReadable(recorder, kRecorderHead) ? recorder : nullptr;
}

bool TimerSet(const uint8_t* phase) {
    return Field<uintptr_t>(phase, kPhaseTimerDestroy) && Field<uintptr_t>(phase, kPhaseTimerCall) &&
           Field<uintptr_t>(phase, kPhaseTimerArgument);
}

int PhaseVector(const uint8_t* m, uint8_t* const** begin) {
    auto* b = Field<uint8_t* const*>(m, kMachinePhasesOff);
    auto* e = Field<uint8_t* const*>(m, kMachinePhasesEndOff);
    const int count = b && e > b ? static_cast<int>((e - b) / 2) : 0;
    if (count <= 0 || count > static_cast<int>(kMaxPhases) || !IsReadable(b, count * 16)) return 0;
    *begin = b;
    return count;
}

bool CaptureMachine(MachineCopy& c, uint32_t tick) {
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
    auto* block = wait ? Field<uint8_t*>(wait, kKoWaitRef) : nullptr;
    if (block && (!IsReadable(block, 16) || Field<int32_t>(block, 8) <= 0)) block = nullptr;
    HoldRef(&c.koWaitHeld, block);
    c.valid = true;
    return true;
}

void CapturePhases() {
    const uint32_t tick = TickNow();
    if (!CaptureMachine(g_phaseRing[tick % kPhaseRing], tick)) return;
    const uint8_t* wait = LiveKoWait();
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

} // namespace

namespace flow {

uintptr_t ImageEnd() {
    static uintptr_t end = 0;
    if (!end && g_base) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        end = g_base + nt->OptionalHeader.SizeOfImage;
    }
    return end;
}

bool InImage(uint64_t v) { return g_base && v >= g_base && v < ImageEnd(); }

} // namespace flow

namespace {

std::atomic<bool> g_legacyPointerTest{false};

bool LooksLikePointer(uint64_t v) {
    constexpr uint64_t kLowest = 0x10000, kUserEnd = 0x800000000000ull, kAlign = 8;
    if (v < kLowest || v >= kUserEnd || v % kAlign != 0 || !IsReadable(reinterpret_cast<const void*>(v), 8)) return false;
    if ((v >> 32) || g_legacyPointerTest) return true;
    return InImage(v) || InImage(*reinterpret_cast<const uint64_t*>(v));
}

void RestorePhase(uint8_t* phase, const PhaseCopy& saved) {
    const size_t size = PhaseSize(phase);
    const auto* liveArgument = Field<const uint8_t*>(phase, kPhaseTimerArgument);
    const bool liveTimer = TimerSet(phase) && IsReadable(liveArgument, 16);
    const bool sameTimer = liveTimer && saved.hasArgument &&
                           Field<uintptr_t>(phase, kPhaseTimerArgument) == Field<uintptr_t>(saved.bytes, kPhaseTimerArgument) &&
                           std::memcmp(liveArgument, saved.argument, 16) == 0;
    if (std::memcmp(phase, saved.bytes, size) == 0 && (sameTimer || (!liveTimer && !saved.hasArgument))) return;
    if (liveTimer && !sameTimer) {
        using DestroyFn = void (*)(void*);
        Field<DestroyFn>(phase, kPhaseTimerDestroy)(const_cast<uint8_t*>(liveArgument));
    }
    for (uint32_t off = 8; off + 8 <= size; off += 8) {
        if (off == kPhaseTimerArgument) continue;
        const uint64_t want = Field<uint64_t>(saved.bytes, off);
        const uint64_t have = Field<uint64_t>(phase, off);
        if (want == have) continue;
        const bool timerFunction = off == kPhaseTimerDestroy || off == kPhaseTimerClone || off == kPhaseTimerCall;
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

void RestoreMachine(const MachineCopy& c) {
    uint8_t* m = LivePhaseMachine();
    uint8_t* const* phases = nullptr;
    const int count = m ? PhaseVector(m, &phases) : 0;
    if (!count || !c.valid || c.count != count) return;
    const int liveIndex = Field<int32_t>(m, kMachineIndexOff);
    const int savedIndex = Field<int32_t>(c.fields, kMachineIndexOff - kMachineFields);
    for (int i = 0; i < count; i++) RestorePhase(phases[i * 2], c.phases[i]);
    if (savedIndex != liveIndex && savedIndex >= 0 && savedIndex < count) {
        uint8_t* object = phases[savedIndex * 2];
        uint8_t* block = phases[savedIndex * 2 + 1];
        auto* oldBlock = Field<uint8_t*>(m, kMachineCurrentRefOff);
        if (block) InterlockedIncrement(reinterpret_cast<volatile LONG*>(block + 8));
        *reinterpret_cast<uint8_t**>(m + kMachineCurrentOff) = object;
        *reinterpret_cast<uint8_t**>(m + kMachineCurrentRefOff) = block;
        if (oldBlock) InterlockedDecrement(reinterpret_cast<volatile LONG*>(oldBlock + 8));
        Log("flow: phase machine back to phase %d (was %d) by a load", savedIndex, liveIndex);
    }
    std::memcpy(m + kMachineIndexOff, c.fields + (kMachineIndexOff - kMachineFields), kMachineFieldsSize - (kMachineIndexOff - kMachineFields));
    if (uint8_t* wait = c.hasKoWait ? LiveKoWait() : nullptr) {
        if (std::memcmp(wait + kKoWaitState, c.koWait, kKoWaitStateSize) != 0) {
            if (g_koWaitLogs++ < 8) {
                Log("flow: round end wait back to mode %d latch %d (was mode %d latch %d) by a load to tick %u",
                    Field<int32_t>(c.koWait, kKoWaitMode), Field<int32_t>(c.koWait, kKoWaitLatch),
                    Field<int32_t>(wait, kKoWaitState + kKoWaitMode), Field<int32_t>(wait, kKoWaitState + kKoWaitLatch), c.tick);
            }
            uint8_t live[16];
            std::memcpy(live, wait + kKoWaitObject, sizeof(live));
            std::memcpy(wait + kKoWaitState, c.koWait, kKoWaitStateSize);
            std::memcpy(wait + kKoWaitObject, live, sizeof(live));
            void* const want = Field<void*>(c.koWait, kKoWaitRef - kKoWaitState);
            void* const have = Field<void*>(wait, kKoWaitRef);
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
            } else if (want != have) {
                g_waitRefLeft++;
                static int s_kept = 0;
                if (s_kept++ < 3) Log("flow: round end wait's object left as it is by a load (no reference kept for it)");
            }
        }
    }
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

void RestorePhases(uint32_t tick) {
    const MachineCopy& c = g_phaseRing[tick % kPhaseRing];
    if (c.valid && c.tick == tick) RestoreMachine(c);
}

} // namespace

namespace flow {

void PrepareRoundOverUndo(uint8_t* director) {
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

} // namespace flow

namespace {

void UndoRoundOvers(uint32_t tick) {
    while (!g_roundOverJournal.empty() && g_roundOverJournal.back().tick > tick) {
        const RoundOverUndo& u = g_roundOverJournal.back();
        std::memcpy(u.context + kRoundCount, u.count.data(), u.count.size());
        std::memcpy(u.slot, u.slotBytes.data(), u.slotBytes.size());
        std::memcpy(u.context + kLastRecord, u.lastRecord.data(), u.lastRecord.size());
        std::memcpy(u.context + kRoundResult, u.result.data(), u.result.size());
        Log("flow: round over of tick %u taken back by a load to tick %u", u.tick, tick);
        g_roundOverJournal.pop_back();
    }
    while (!g_roundOverJournal.empty() && g_roundOverJournal.front().tick + kPhaseRing < tick) {
        g_roundOverJournal.erase(g_roundOverJournal.begin());
    }
}

uint8_t* RoundContextNow() {
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

constexpr size_t kRoundChunk = 4096;

void CopyRoundHistory(const uint8_t* context, uint8_t* out) {
    std::memcpy(out, context + kRoundCount, kRoundHistoryPart);
    std::memcpy(out + kRoundHistoryPart, context + kLastRecord, kRoundRecord);
}

void PutRoundHistory(uint8_t* context, const uint8_t* in) {
    std::memcpy(context + kRoundCount, in, kRoundHistoryPart);
    std::memcpy(context + kLastRecord, in + kRoundHistoryPart, kRoundRecord);
}

} // namespace

void FlowSetLegacyPointerTest(bool on) {
    g_legacyPointerTest = on;
    Log("flow: phase restores take %s for pointers", on ? "any mapped aligned word (as before 2026-11-13)" : "only image addresses, objects with image vtables and 64-bit addresses");
}

void FlowSetKoWaitRollback(bool on) {
    g_koWaitRollback = on;
    Log("flow: the round end wait's mode %s", on ? "is put back by loads" : "stays as it is on loads (as before 2026-11-08)");
}

bool FlowRoundCallRunning() {
    const int phase = g_roundCallPhase;
    if (const uint8_t* m = phase >= 0 ? LivePhaseMachine() : nullptr) return Field<int32_t>(m, kMachineIndexOff) == phase;
    return g_roundCall;
}

uint32_t FlowRoundEndEvents() { return g_roundEnds; }

bool FlowRoundInFight() {
    if (!g_roundFlow.inFight) return false;
    const uint8_t* d = LiveRoundDirector();
    const int32_t state = d ? *reinterpret_cast<const int32_t*>(d + kDirectorState) : 1;
    return state != kDirectorStopped && state != kDirectorRoundOver;
}

void FlowMatchStopToDirector(uint8_t* battleFrame) {
    const uint8_t* d = LiveRoundDirector();
    if (!d || !battleFrame) return;
    const bool stopped = *reinterpret_cast<const int32_t*>(d + kDirectorState) == kDirectorStopped;
    auto* flags = reinterpret_cast<uint32_t*>(battleFrame + kStopFlags);
    const bool set = (flags[0] & kStopBit) != 0;
    if (set == stopped) return;
    if (stopped) {
        flags[0] |= kStopBit;
    } else {
        flags[0] &= ~kStopBit;
        flags[1] &= ~kStopBit;
    }
    Log("flow: stop bit %s after a load, as the round director has it", stopped ? "set" : "cleared");
}

void FlowSetRoundDirectorPerTick(bool on, bool roundEndRollback) {
    if (g_waitRefBacks || g_waitRefEmptied || g_waitRefLeft) {
        Log("flow: round end wait's object put back by loads: %d times to a kept object, %d to none, %d left as it was",
            g_waitRefBacks, g_waitRefEmptied, g_waitRefLeft);
    }
    g_waitRefBacks = g_waitRefEmptied = g_waitRefLeft = 0;
    g_directorPerTick = on && g_origDirectorCheck;
    g_roundEndRollback = g_directorPerTick && roundEndRollback;
    ForgetCopies();
    g_roundOverJournal.clear();
    g_roundFlow.ticks = 0;
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
        g_freeze.machine.valid ? "kept" : "missing", m ? Field<int32_t>(m, kMachineIndexOff) : -1, m ? Field<int32_t>(m, kMachineRequestedOff) : -1);
}

namespace {

void RestoreFreeze() {
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

void FlowBeginDrivenTick() {
    if (g_directorPerTick) g_roundFlow.ticks++;
    g_gameThread = GetCurrentThreadId();
}

uint32_t FlowDrivenTicks() {
    return g_roundFlow.ticks;
}

void FlowRunRoundDirector() {
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

void FlowRoundDirectorFromSnapshot() {
    const RoundFlow& f = g_roundFlow;
    uint8_t* d = LiveRoundDirector();
    if (!d) return;
    if (!f.directorSeen) return RestoreFreeze();
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
                c.valid ? "valid" : "missing", c.tick, Field<int32_t>(c.fields, kMachineIndexOff - kMachineFields), m ? Field<int32_t>(m, kMachineIndexOff) : -1);
        }
        UndoRoundOvers(tick);
        RestorePhases(tick);
    }
}

namespace {

using Chunks = std::vector<std::shared_ptr<const std::vector<uint8_t>>>;

void SaveChunks(const uint8_t* src, size_t size, const Chunks* before, Chunks* out) {
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

bool PutChunks(const Chunks& chunks, uint8_t* dst, size_t size) {
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

bool FlowSaveOutsideState(OutsideState* out, const OutsideState* previous) {
    out->machine.assign(sizeof(MachineCopy), 0);
    auto* c = reinterpret_cast<MachineCopy*>(out->machine.data());
    if (!CaptureMachine(*c, TickNow())) {
        out->machine.clear();
        return false;
    }
    ReleaseHeld(*c);
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
    thread_local std::vector<uint8_t> now;
    now.resize(kRoundHistoryBytes);
    CopyRoundHistory(context, now.data());
    SaveChunks(now.data(), kRoundHistoryBytes, previous ? &previous->rounds : nullptr, &out->rounds);
    return true;
}

bool FlowReadRoundResults(std::vector<RoundResult>* rounds, uint8_t wins[2]) {
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

void FlowRestoreOutsideState(const OutsideState& s) {
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
    ForgetCopies();
    g_roundOverJournal.clear();
}

uint32_t FlowPresentationStart(uint32_t p1, uint32_t p2) {
    constexpr uint32_t kGameStart = 0x4000;
    constexpr int kStartTicks = 10;
    constexpr int kUiStartMs = 150;
    constexpr uint16_t kUiButtons = 0x1000;
    RoundFlow& f = g_roundFlow;
    if (FlowRoundInFight()) {
        f.startPressed = 0;
        f.startTicks = 0;
        return 0;
    }
    if (f.startTicks > 0) {
        f.startTicks--;
        return kGameStart;
    }
    f.startPressed |= ((p1 & kGameStart) ? 1u : 0u) | ((p2 & kGameStart) ? 2u : 0u);
    if (f.startPressed != 3) return 0;
    f.startPressed = 0;
    f.startTicks = kStartTicks - 1;
    static int loggedFor = -1;
    if (loggedFor != f.lastCode) Log("flow: both players pressed Start during the presentation (after flow event %d): skipping", f.lastCode);
    loggedFor = f.lastCode;
    if (MatchOver(f.lastCode)) {
        Log("flow: both players pressed Start after the match: confirm on each game's pad input");
        InputPressUi(kUiButtons, kUiStartMs);
    }
    return kGameStart;
}

bool FlowMatchDecided() { return MatchOver(g_roundFlow.lastCode) || g_roundFlow.lastCode == kResults; }

void FlowSetHoldResults(bool hold) {
    g_holdResults = hold;
    if (!hold) g_resultsHeld = false;
}

bool FlowResultsHeld() { return g_resultsHeld; }

uint8_t* FlowRoundFlowState() {
    return reinterpret_cast<uint8_t*>(&g_roundFlow);
}

uint8_t* FlowRoundDirector() { return LiveRoundDirector(); }

void FlowResetRoundFlow() {
    g_roundFlow = {};
    g_roundCall = false;
    g_roundCallPhase = -1;
    g_roundDirector = nullptr;
}

} // namespace sfv::mod
