#include "mod/frame_meter.h"

#include "mod/battle_actors.h"
#include "mod/config.h"
#include "mod/flow.h"
#include "mod/image.h"
#include "mod/log.h"
#include "mod/replay_driver.h"
#include "mod/sfv_game.h"
#include "mod/ui_kit.h"

#include <imgui.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sfv::mod {

namespace {

using namespace actor; // battle_actors.h
using namespace ui;    // ui_kit.h

enum Kind : uint8_t { Empty, Idle, Counter, Active, FirstActive, Recovery, Iasa, Parry, Stun, Invincible, StrikeInv, ProjInv };
// Runs count FirstActive with Active; Empty (not recorded) is neutral too.
Kind Group(Kind k) { return k == FirstActive ? Active : k == Empty ? Idle : k; }
const char* KindName(Kind k)
{
    static const char* names[] = {"-", "idle", "cnt", "act", "act1", "rec", "iasa", "par", "stun", "inv", "sinv", "pinv"};
    return names[k];
}

constexpr int N = 120;            // blocks: 2 s
constexpr int kClear = 10;        // blocks left clear after the newest once it wraps
constexpr int kNeutralLimit = 120; // neutral frames after which the next action starts a new bar
constexpr ULONGLONG kStaleMs = 250;  // no refresh this long: the battle is gone
constexpr ULONGLONG kPausedMs = 150; // no battle tick this long out of a replay: the game is paused

struct Meter {
    Kind kind[2][N]{};
    uint16_t count[2][N]{}; // a run's length, over its last block (0: none)
    bool lane[2][N]{};      // the player's projectile is out
    bool mark[N]{};         // a super freeze or cinematic held the meter before this block
    int startup[2] = {-1, -1}, total[2] = {-1, -1}, adv[2] = {0, 0};
    bool advKnown[2] = {false, false};
};

struct Run {
    Kind group = Idle;
    int length = 0;
    int cell = -1;
};

struct Player {
    const uint8_t* actor = nullptr; // R
    const uint8_t* state = nullptr; // S (battle_actors.h)
    Run run;
    Kind lastWritten = Idle;
    uint32_t script = ~0u;
    bool doing = false;      // the last frame was an action
    bool actionable = true;  // the last frame was neutral or IASA
    int actionableSince = 0; // the frame it became so
    int moveStart = -1;      // the open move's first frame, -1 none
};

uintptr_t g_base = 0;
std::atomic<int> g_training{-1}, g_replays{-1};
std::atomic<bool> g_forced{false};
std::atomic<int> g_log{0};

// Game thread.
Meter g_m;
Player g_p[2];
uint64_t g_layout = ~0ull;
int g_written = 0;      // blocks written in this bar (the next one's index)
int g_pendingNeutral = 0;
bool g_hasContent = false, g_pendingMark = false, g_exchange = false;
bool g_armed = false; // both fighters have been free to act since the battle (or a reset) began
int g_replayFrame = -1;

// Shared with the overlay.
std::mutex g_mutex;
Meter g_shown;
std::atomic<ULONGLONG> g_tickAt{0};

template <class T> T At(const uint8_t* p, size_t offset) { T v; std::memcpy(&v, p + offset, sizeof(T)); return v; }

int ReadChoice(std::atomic<int>& cache, const wchar_t* key, int fallback)
{
    int v = cache;
    if (v < 0) cache = v = ReadMainIni(L"framemeter", key, fallback) ? 1 : 0;
    return v;
}

bool IsActor(const uint8_t* r, uint32_t vtable)
{
    return r && IsReadable(r - kPosition, kPosition) && At<uintptr_t>(r - kVtableAt, 0) == g_base + vtable;
}

// The move file's section of that type in a script, or nullptr.
const uint8_t* Section(const uint8_t* script, int type, uint32_t* count)
{
    if (!IsReadable(script, 64)) return nullptr;
    const uint32_t n = At<uint32_t>(script, kScriptSections);
    const uint32_t at = At<uint32_t>(script, kScriptSectionsAt);
    if (n == 0 || n > 64 || at > 0x100000 || !IsReadable(script + at, n * kSectionSize)) return nullptr;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* s = script + at + i * kSectionSize;
        if (At<int16_t>(s, 0) != type) continue;
        *count = At<uint16_t>(s, 2);
        return *count <= 1024 ? s : nullptr;
    }
    return nullptr;
}

// The script's status flags on that frame (sections whose condition needs no
// script state).
uint32_t StatusFlags(const uint8_t* script, int frame)
{
    uint32_t count = 0;
    const uint8_t* s = Section(script, kSectionStatus, &count);
    if (!s) return 0;
    const uint8_t* ticks = s + At<uint32_t>(s, 4);
    const uint8_t* data = s + At<uint32_t>(s, 8);
    const uint32_t flagsAt = At<uint32_t>(s, 12);
    if (!IsReadable(ticks, count * 8) || !IsReadable(data, count * 8) || (flagsAt && !IsReadable(s + flagsAt, count * 16))) return 0;
    uint32_t flags = 0;
    for (uint32_t e = 0; e < count; e++) {
        if (frame < At<int32_t>(ticks, e * 8) || frame >= At<int32_t>(ticks, e * 8 + 4)) continue;
        if (flagsAt && At<int32_t>(s + flagsAt, e * 16) != 0) continue; // active only in some script state
        flags |= At<uint32_t>(data, e * 8);
    }
    return flags;
}

// A hurtbox's armor effect, from the move file (its id: box << 8 | script << 16).
int ArmorEffect(const uint8_t* script, uint32_t box)
{
    uint32_t count = 0;
    const uint8_t* s = Section(script, kSectionHurtboxes, &count);
    if (!s || box >= count) return 0;
    const uint8_t* rec = s + At<uint32_t>(s, 8) + box * kHurtboxRecord;
    return IsReadable(rec, kHurtboxRecord) ? At<int16_t>(rec, kHurtboxArmorEffect) : 0;
}

struct Sample {
    bool ok = false;
    uint32_t script = 0;
    int frame = 0, interrupt = -1, firstHit = -1;
    bool held = false, cinematic = false, frozen = false, stun = false;
    bool hit = false, strike = false, projectile = false, hurt = false, parry = false, counter = false;
    uint32_t side = 0;
};

Sample Read(const uint8_t* r, const uint8_t* state)
{
    Sample s;
    const auto* runner = At<const uint8_t*>(r, kRunner);
    if (!runner || !IsReadable(runner, kRunnerSpeed + 4)) return s;
    const auto* script = At<const uint8_t*>(runner, kRunnerScript);
    s.script = At<uint32_t>(runner, kRunnerIndex);
    s.frame = At<int32_t>(runner, kRunnerFrame) >> 16;
    s.held = At<int32_t>(runner, kRunnerSpeed) == 0;
    if (IsReadable(script, 64)) {
        s.firstHit = At<int32_t>(script, kScriptFirstHit);
        s.interrupt = At<int32_t>(script, kScriptInterrupt);
        s.counter = (StatusFlags(script, s.frame) & kStatusCounter) != 0;
    }
    const auto* pos = At<const uint8_t*>(r - kPosition, 0);
    s.frozen = pos && IsReadable(pos + kSuperFreeze, 4) && At<uint32_t>(pos, kSuperFreeze) != 0;
    s.cinematic = At<uint32_t>(r, kCinematic) != 0;
    s.stun = IsReadable(state + kStateFree, 4) && At<uint32_t>(state, kStateFree) == 0;
    s.side = At<uint32_t>(r, kSide);
    const uintptr_t entryVtable = g_base + kEntryVtable;
    for (size_t at = kHitList; at + kEntrySize <= kHitListEnd; at += kEntrySize) {
        const uint8_t* e = r + at;
        if (At<uintptr_t>(e, kEntryVtableAt) != entryVtable || At<uint32_t>(e, kEntryId) == 0xFFFFFFFFu) continue;
        const uint8_t type = e[kEntryType];
        if (type != 4 && type <= 5) s.hit = true; // proximity guard boxes are not an attack
    }
    for (size_t at = kHurtList; at + kEntrySize <= kHurtListEnd; at += kEntrySize) {
        const uint8_t* e = r + at;
        if (At<uintptr_t>(e, kEntryVtableAt) != entryVtable) continue;
        const uint32_t id = At<uint32_t>(e, kEntryId);
        if (id == 0xFFFFFFFFu) continue;
        const uint32_t type = At<uint32_t>(e, kEntryType);
        s.hurt = true;
        if (type & 1) s.strike = true;
        if (type & 2) s.projectile = true;
        if (!s.parry && (id >> 16) == s.script && ArmorEffect(script, (id >> 8) & 0xFF) == kArmorParry) s.parry = true;
    }
    s.ok = true;
    return s;
}

Kind Classify(const Sample& s)
{
    // A script's interrupt frame is where the character can act again, as
    // sfvsim has it: a damage script's speed is scaled so that it comes as
    // the hit or block stun ends. 0 means none (basic scripts).
    const bool interruptible = s.interrupt > 0;
    const bool past = interruptible && s.frame >= s.interrupt;
    // The jump's scripts (pre-jump, airborne, landing: the same indices in
    // every character's move file) are actions too: the character is
    // committed to them, even where the script has no interrupt frame.
    const bool jump = s.script >= kScriptFirstJump && s.script <= kScriptLastJump;
    const bool action = interruptible || jump || s.firstHit >= 0 || s.counter;
    if (s.stun) return past ? Idle : Stun;
    if (s.hit) return Active;
    if (s.parry) return Parry;
    if (action || !s.hurt) {
        if (!s.strike && !s.projectile) return Invincible;
        if (!s.strike) return StrikeInv;
        if (!s.projectile) return ProjInv;
    }
    if (s.counter) return Counter;
    if (!action) return Idle;
    return past ? Iasa : Recovery;
}

void EndRun(int i)
{
    Run& run = g_p[i].run;
    // A single block needs no count (and its label would run into the next).
    if (run.group != Idle && run.length > 1 && run.cell >= 0) g_m.count[i][run.cell] = static_cast<uint16_t>(std::min(run.length, 9999));
    run = Run{};
}

void ClearBar()
{
    const Meter keep = g_m;
    g_m = Meter{};
    for (int i = 0; i < 2; i++) {
        g_m.startup[i] = keep.startup[i];
        g_m.total[i] = keep.total[i];
        g_m.adv[i] = keep.adv[i];
        g_m.advKnown[i] = keep.advKnown[i];
        g_p[i].run = Run{};
        g_p[i].lastWritten = Idle;
        g_p[i].moveStart = -1;
    }
    g_written = 0;
    g_pendingNeutral = 0;
    g_hasContent = false;
    g_pendingMark = false;
    g_exchange = false;
}

void Reset()
{
    ClearBar();
    g_armed = false;
    g_m = Meter{};
    for (Player& p : g_p) {
        const uint8_t* actor = p.actor;
        const uint8_t* state = p.state;
        p = Player{};
        p.actor = actor;
        p.state = state;
    }
}

void Write(const Kind (&kinds)[2], const bool (&lanes)[2])
{
    const int cell = g_written % N;
    for (int i = 0; i < 2; i++) {
        Kind k = kinds[i];
        if (k == Active && Group(g_p[i].lastWritten) != Active) k = FirstActive;
        if (Group(k) != g_p[i].run.group) EndRun(i);
        Run& run = g_p[i].run;
        run.group = Group(k);
        run.length++;
        run.cell = cell;
        g_m.kind[i][cell] = k;
        g_m.count[i][cell] = 0;
        g_m.lane[i][cell] = lanes[i];
        g_p[i].lastWritten = k;
    }
    g_m.mark[cell] = g_pendingMark;
    g_pendingMark = false;
    if (g_written + 1 >= N) {
        // Wrapped: the blocks after the newest are cleared.
        for (int j = 1; j <= kClear; j++) {
            const int c = (cell + j) % N;
            for (int i = 0; i < 2; i++) {
                g_m.kind[i][c] = Empty;
                g_m.count[i][c] = 0;
                g_m.lane[i][c] = false;
            }
            g_m.mark[c] = false;
        }
    }
    g_written++;
    g_hasContent = true;
}

// Each move's startup, total and the frame advantage after an exchange.
void Count(const Sample (&s)[2], const Kind (&k)[2], const bool (&lanes)[2], int index)
{
    for (int i = 0; i < 2; i++) {
        Player& p = g_p[i];
        const bool actionable = k[i] == Idle || k[i] == Iasa;
        const bool doing = !actionable && k[i] != Stun;
        // A move starts when the character acts from neutral or IASA, or
        // cancels into a script with hitboxes of its own (a script handing
        // over to its follow-up, a landing or a recovery, is the same move).
        const bool attack = s[i].firstHit >= 0;
        if (doing && (!p.doing || (s[i].script != p.script && attack))) {
            p.moveStart = index;
            g_m.startup[i] = -1;
            g_m.total[i] = -1;
            g_m.advKnown[i] = false;
        }
        if (p.moveStart >= 0) {
            if (g_m.startup[i] < 0 && (Group(k[i]) == Active || lanes[i])) g_m.startup[i] = index - p.moveStart + 1;
            if (actionable) {
                g_m.total[i] = index - p.moveStart;
                p.moveStart = -1;
            }
            else if (k[i] == Stun) p.moveStart = -1;
        }
        if (actionable && !p.actionable) p.actionableSince = index;
        p.actionable = actionable;
        p.doing = doing;
        p.script = s[i].script;
        if (k[i] == Stun) g_exchange = true;
    }
    if (g_exchange && g_p[0].actionable && g_p[1].actionable) {
        g_m.adv[0] = g_p[1].actionableSince - g_p[0].actionableSince;
        g_m.adv[1] = -g_m.adv[0];
        g_m.advKnown[0] = g_m.advKnown[1] = true;
        g_exchange = false;
    }
}

void Publish()
{
    std::lock_guard lock(g_mutex);
    g_shown = g_m;
}

bool Wanted(bool* replay, int* frame, bool* seeking, bool* fighting = nullptr)
{
    const ReplayView v = ReplayViewNow();
    *replay = v.active;
    *frame = v.frame;
    *seeking = v.seeking;
    // A replay's round: from its FIGHT to its KO (not the intro, the KO and
    // the win poses).
    if (fighting) *fighting = !v.active || v.roundFrame >= 0;
    if (g_forced) return true;
    if (v.active) return !v.benchmark && FrameMeterInReplays();
    return FrameMeterInTraining() && CurrentScene() == "TrainingBattleState";
}

// The two fighters' actors and state objects, P1 first, paired by side: at
// a battle's start P1 is on the left. Not yet (false) while the sides are
// not set apart.
bool FindFighters(SfvGame& game)
{
    const uint8_t* actors[2] = {};
    const uint8_t* states[2] = {};
    int na = 0, ns = 0;
    for (const StateRegion& r : game.Regions()) {
        if (r.size == kRegionSize && IsActor(r.ptr, kFighterVtable)) {
            if (na < 2) actors[na] = r.ptr;
            na++;
        }
        else if (r.size == kStateRegionSize && IsReadable(r.ptr - kStateRegionAt, kStateSide + 4) &&
                 At<uintptr_t>(r.ptr - kStateRegionAt, 0) == g_base + kStateVtable) {
            if (ns < 2) states[ns] = r.ptr - kStateRegionAt;
            ns++;
        }
    }
    if (na != 2 || ns != 2) {
        static ULONGLONG logged = 0;
        if (GetTickCount64() - logged > 2000) {
            logged = GetTickCount64();
            Log("framemeter: %d fighter actors, %d state objects", na, ns);
        }
        return false;
    }
    const uint32_t a0 = At<uint32_t>(actors[0], kSide), a1 = At<uint32_t>(actors[1], kSide);
    const uint32_t s0 = At<uint32_t>(states[0], kStateSide), s1 = At<uint32_t>(states[1], kStateSide);
    if (a0 > 1 || a1 > 1 || a0 == a1 || s0 > 1 || s1 > 1 || s0 == s1) {
        static ULONGLONG logged = 0;
        if (GetTickCount64() - logged > 2000) {
            logged = GetTickCount64();
            Log("framemeter: fighters not apart yet (actor sides %X/%X, state sides %X/%X)", a0, a1, s0, s1);
        }
        return false;
    }
    for (int p = 0; p < 2; p++) {
        g_p[p].actor = a0 == static_cast<uint32_t>(p) ? actors[0] : actors[1];
        g_p[p].state = s0 == static_cast<uint32_t>(p) ? states[0] : states[1];
    }
    return true;
}

} // namespace

bool FrameMeterInTraining() { return ReadChoice(g_training, L"training", 1) != 0; }
bool FrameMeterInReplays() { return ReadChoice(g_replays, L"replays", 0) != 0; }

void SetFrameMeterInTraining(bool on)
{
    g_training = on ? 1 : 0;
    SaveMainIni(L"framemeter", L"training", on ? 1 : 0);
}

void SetFrameMeterInReplays(bool on)
{
    g_replays = on ? 1 : 0;
    SaveMainIni(L"framemeter", L"replays", on ? 1 : 0);
}

void SetFrameMeterForced(bool on)
{
    g_forced = on;
    Log("framemeter: %s in any battle", on ? "forced on" : "back to the choices");
}

void LogFrameMeter(int ticks) { g_log = ticks; }

void FrameMeterTick(SfvGame& game)
{
    if (!g_base) g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool replay = false, seeking = false, fighting = true;
    int frame = 0;
    if (!Wanted(&replay, &frame, &seeking, &fighting)) {
        g_layout = ~0ull;
        return;
    }
    if (game.LayoutVersion() != g_layout) {
        if (!FindFighters(game)) return;
        g_layout = game.LayoutVersion();
        g_replayFrame = -1;
        Reset();
        Log("framemeter: %s, P1 %p, P2 %p", replay ? "replay" : CurrentScene().c_str(), static_cast<const void*>(g_p[0].actor),
            static_cast<const void*>(g_p[1].actor));
    }
    // A replay's seek, step back or jump: what was recorded no longer leads here.
    if (replay) {
        if (seeking || frame < g_replayFrame || frame > g_replayFrame + 8) Reset();
        g_replayFrame = frame;
        if (seeking) return Publish();
    }
    Sample s[2];
    for (int i = 0; i < 2; i++) {
        if (!IsActor(g_p[i].actor, kFighterVtable)) {
            g_layout = ~0ull;
            return;
        }
        s[i] = Read(g_p[i].actor, g_p[i].state);
        if (!s[i].ok) return Publish();
    }
    // Hitstop (both held), a super freeze and a cinematic are not counted.
    const bool freeze = s[0].frozen || s[1].frozen || s[0].cinematic || s[1].cinematic;
    if (freeze || (s[0].held && s[1].held)) {
        if (freeze && g_hasContent) g_pendingMark = true;
        if (g_log > 0) {
            g_log--;
            Log("fmlog held: frozen %d/%d cinematic %d/%d held %d/%d", s[0].frozen, s[1].frozen, s[0].cinematic, s[1].cinematic, s[0].held, s[1].held);
        }
        return Publish();
    }
    // The players' projectiles: live effect slots with an attacking box, the
    // owner told by side (an effect faces as its owner did).
    bool lanes[2] = {false, false};
    for (const StateRegion& r : game.Regions()) {
        if (r.size != kRegionSize || !IsActor(r.ptr, kEffectVtable)) continue;
        if (!At<uint32_t>(r.ptr, kEffectLiveA) && !At<uint32_t>(r.ptr, kEffectLiveB)) continue;
        const uint32_t side = At<uint32_t>(r.ptr, kSide);
        if (side > 1 || s[0].side == s[1].side) continue;
        bool hit = false;
        for (size_t at = kHitList; at + kEntrySize <= kHitListEnd && !hit; at += kEntrySize) {
            const uint8_t* e = r.ptr + at;
            hit = At<uintptr_t>(e, kEntryVtableAt) == g_base + kEntryVtable && At<uint32_t>(e, kEntryId) != 0xFFFFFFFFu &&
                  e[kEntryType] != 4 && e[kEntryType] <= 5;
        }
        if (hit) lanes[s[0].side == side ? 0 : 1] = true;
    }
    const Kind k[2] = {Classify(s[0]), Classify(s[1])};
    // Not before both have been free to act (a battle's or training's start
    // scripts), nor outside a replay's rounds.
    const bool bothFree = (k[0] == Idle || k[0] == Iasa) && (k[1] == Idle || k[1] == Iasa);
    if (!g_armed && !bothFree) return Publish();
    g_armed = true;
    if (!fighting) return Publish();
    // Neutral holds the meter; IASA alone does not start a new sequence but
    // is drawn in one that runs.
    // After more than kNeutralLimit neutral frames, the first movement (any
    // script change: a walk, a crouch, a jump) clears the whole bar at once.
    if (g_hasContent && g_pendingNeutral > kNeutralLimit && (s[0].script != g_p[0].script || s[1].script != g_p[1].script)) {
        ClearBar();
    }
    const bool running = g_hasContent && g_pendingNeutral <= kNeutralLimit;
    const bool neutral = ((k[0] == Idle && k[1] == Idle) || (!running && bothFree)) && !lanes[0] && !lanes[1];
    int index = 0;
    if (neutral) {
        // Held: drawn only if someone acts within kNeutralLimit frames.
        for (int i = 0; i < 2; i++) {
            if (g_p[i].run.group != Idle) EndRun(i);
        }
        index = g_written + g_pendingNeutral;
        g_pendingNeutral++;
    }
    else {
        if (!g_hasContent || g_pendingNeutral > kNeutralLimit) ClearBar();
        const Kind idle[2] = {Idle, Idle};
        const bool none[2] = {false, false};
        for (; g_pendingNeutral > 0; g_pendingNeutral--) Write(idle, none);
        index = g_written;
        Write(k, lanes);
    }
    Count(s, k, lanes, index);
    if (g_log > 0) {
        g_log--;
        Log("fmlog %d: P1 #%u f%d %s%s | P2 #%u f%d %s%s | written %d", index, s[0].script, s[0].frame, KindName(k[0]),
            lanes[0] ? "+proj" : "", s[1].script, s[1].frame, KindName(k[1]), lanes[1] ? "+proj" : "", g_written);
    }
    Publish();
}

void FrameMeterFrame(SfvGame& game)
{
    if (!g_base) g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool replay = false, seeking = false;
    int frame = 0;
    if (!Wanted(&replay, &frame, &seeking) || game.LayoutVersion() != g_layout) return;
    // Training's pause menu stops the ticks: no meter over the game's menu.
    if (replay || GetTickCount64() - game.LastTickMs() <= kPausedMs) g_tickAt = GetTickCount64();
}

bool FrameMeterVisible()
{
    const ULONGLONG at = g_tickAt.load();
    return at && GetTickCount64() - at < kStaleMs;
}

void DrawFrameMeter(ImDrawList* d, ImFont* bold)
{
    if (!FrameMeterVisible() || !bold) return;
    Meter m;
    {
        std::lock_guard lock(g_mutex);
        m = g_shown;
    }
    BeginScreen();
    // Design units: the bars at x 80..1520 just above the V-Trigger gauges;
    // the blocks are laid on whole pixels (one pitch and gap for every
    // block, the 120 centred on the design's place), so no gap differs.
    constexpr float x0 = 80, y0 = 652, pitch = 12, gap = 2, h = 20;
    constexpr float kTextSize = 21, kCountSize = 20;
    constexpr float kCounts[2] = {y0 + 25, y0 + 103}, kLanes[2] = {y0 + 46, y0 + 98}, kBars[2] = {y0 + 52, y0 + 76};
    const float s = g_v.s;
    const float pitchPx = std::max(3.0f, std::round(pitch * s));
    const float gapPx = std::max(1.0f, std::round(gap * s));
    const float wPx = pitchPx - gapPx;
    const float left = std::round(g_v.P(x0 + N * pitch / 2, 0).x - N * pitchPx / 2);
    const float right = left + (N - 1) * pitchPx + wPx;
    const float hPx = std::max(2.0f, std::round(h * s)), lanePx = std::max(2.0f, std::round(4 * s));
    const float drop = std::max(1.0f, std::round(2 * s));
    auto cellX = [&](int c) { return left + c * pitchPx; };
    auto rowY = [&](float y) { return std::round(g_v.P(0, y).y); };
    const ImU32 shadow = Rgba(0, 0, 0, 120);
    auto color = [](Kind k) {
        switch (k) {
        case Counter: return Rgba(31, 163, 126);
        case Active: return Rgba(229, 67, 90);
        case FirstActive: return Rgba(255, 163, 174);
        case Recovery: return Rgba(95, 220, 242);
        case Iasa: return Rgba(42, 99, 196);
        case Parry: return Rgba(134, 87, 224);
        case Stun: return Rgba(242, 217, 74);
        case Invincible: return Rgba(244, 241, 232);
        case StrikeInv: return Rgba(229, 67, 90);
        case ProjInv: return Rgba(240, 160, 48);
        default: return Rgba(10, 12, 16, 158);
        }
    };
    for (int i = 0; i < 2; i++) {
        const float y = rowY(kBars[i]), ly = rowY(kLanes[i]);
        for (int c = 0; c < N; c++) {
            const float x = cellX(c);
            const Kind k = m.kind[i][c];
            d->AddRectFilled(ImVec2(x + drop / 2, y + drop), ImVec2(x + wPx + drop / 2, y + hPx + drop), shadow);
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + wPx, y + hPx), color(k));
            if (k == StrikeInv || k == ProjInv) {
                const float stripe = std::max(1.0f, std::round(3 * s));
                for (float sy = 0; sy < hPx; sy += 2 * stripe) d->AddRectFilled(ImVec2(x, y + sy), ImVec2(x + wPx, y + std::min(sy + stripe, hPx)), look::kCream);
            }
            if (m.lane[i][c]) {
                d->AddRectFilled(ImVec2(x + drop / 2, ly + drop / 2), ImVec2(x + wPx + drop / 2, ly + lanePx + drop / 2), shadow);
                d->AddRectFilled(ImVec2(x, ly), ImVec2(x + wPx, ly + lanePx), Rgba(240, 160, 48));
            }
            if (m.count[i][c]) {
                Text(d, bold, TextPx(kCountSize), ImVec2(x + wPx / 2, rowY(kCounts[i])), look::kCream, std::to_string(m.count[i][c]), 0.5f, true);
            }
        }
    }
    for (int c = 0; c < N; c++) {
        if (!m.mark[c]) continue;
        const float x = cellX(c) - std::round(gapPx / 2) - 1, w2 = std::max(2.0f, std::round(2 * s));
        const float top = rowY(y0 + 44), bottom = rowY(y0 + 102);
        d->AddRectFilled(ImVec2(x + 1, top + 1), ImVec2(x + w2 + 1, bottom + 1), shadow);
        d->AddRectFilled(ImVec2(x, top), ImVec2(x + w2, bottom), look::kCream);
    }
    // The numbers: P1's at the left, P2's at the right ("P1 Startup 6F ·
    // Total 20F · Advantage +5F"); the kicker on the outer side.
    struct Piece {
        std::string text;
        ImU32 color;
        float gap; // design units before it
    };
    for (int i = 0; i < 2; i++) {
        std::vector<Piece> pieces;
        auto add = [&](const char* label, const std::string& value, ImU32 c) {
            if (!pieces.empty()) pieces.push_back({"\xC2\xB7", look::kMuted, 10});
            pieces.push_back({std::string(label) + " ", look::kSoft, pieces.empty() ? 0.0f : 10.0f});
            pieces.push_back({value, c, 0});
        };
        if (m.startup[i] > 0) add("Startup", std::to_string(m.startup[i]) + "F", look::kCream);
        if (m.total[i] > 0) add("Total", std::to_string(m.total[i]) + "F", look::kCream);
        if (m.advKnown[i]) {
            const int a = m.adv[i];
            add("Advantage", (a > 0 ? "+" : "") + std::to_string(a) + "F", a > 0 ? look::kOk : a < 0 ? look::kDanger : look::kCream);
        }
        const Piece kicker{i == 0 ? "P1" : "P2", look::kGold, 0};
        if (i == 0) {
            if (!pieces.empty()) pieces.front().gap = 10;
            pieces.insert(pieces.begin(), kicker);
        }
        else {
            pieces.push_back(kicker);
            pieces.back().gap = pieces.size() > 1 ? 10.0f : 0.0f;
        }
        float width = 0; // screen pixels
        for (const Piece& p : pieces) width += (p.gap + TextW(bold, kTextSize, p.text)) * s;
        float x = i == 0 ? left : right - width;
        const float ty = rowY(y0 - 2);
        for (const Piece& p : pieces) {
            x += p.gap * s;
            Text(d, bold, TextPx(kTextSize), ImVec2(x, ty), p.color, p.text, 0, true);
            x += TextW(bold, kTextSize, p.text) * s;
        }
    }
}
} // namespace sfv::mod
