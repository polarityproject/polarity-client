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
#include <cfloat>
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

enum Kind : uint8_t { Empty, Idle, Counter, Active, FirstActive, Recovery, Iasa, Parry, Stun, ProxGuard, Invincible, StrikeInv, ProjInv, Armor, ArmorFree };
// Runs count FirstActive with Active, ArmorFree with Armor; Empty (not
// recorded) is neutral too. Armor while free to act (outside a move, or in
// its IASA frames) is armor to see and neutral to count.
Kind Group(Kind k) { return k == FirstActive ? Active : k == ArmorFree ? Armor : k == Empty ? Idle : k; }
bool Actionable(Kind k) { return k == Idle || k == Iasa || k == Empty || k == ArmorFree; }
const char* KindName(Kind k)
{
    static const char* names[] = {"-", "idle", "cnt", "act", "act1", "rec", "iasa", "par", "stun", "prox", "inv", "sinv", "pinv", "arm", "armf"};
    return names[k];
}

constexpr int N = 90;                // blocks: 1.5 s
constexpr ULONGLONG kStaleMs = 250;  // no refresh this long: the battle is gone
constexpr ULONGLONG kPausedMs = 150; // no battle tick this long out of a replay: the game is paused

// A block's milestones, drawn as a white border on its top, right and bottom.
enum Edge : uint8_t { EdgeAirborne = 1, EdgeLanding = 2, EdgeEnd = 4 };

struct Meter {
    Kind kind[2][N]{};
    uint16_t count[2][N]{}; // a run's length, over its last block (0: none)
    bool lane[2][N]{};      // the player's projectile is out
    uint8_t edge[2][N]{};   // Edge bits
    bool mark[N]{};         // a super freeze or cinematic held the meter before this block
    int startup[2] = {-1, -1}, total[2] = {-1, -1}, adv[2] = {0, 0};
    bool advKnown[2] = {false, false};
};

// One battle frame as read: what the meter is built from. A viewed replay
// keeps one per recording frame, so that a step or a seek rebuilds the meter
// as the frames led there.
struct Record {
    bool valid = false;
    uint8_t hold = 0; // 0 counted; 1 hitstop; 2 a super freeze or a cinematic; 3 outside a round's fight
    Kind kind[2] = {Idle, Idle};
    bool lane[2] = {false, false};
    bool air[2] = {false, false};
    bool attack[2] = {false, false}; // the script has hitboxes of its own
    uint32_t script[2] = {0, 0};
    int frame[2] = {0, 0};             // the script's frame (a hand-over lands past 0, a new move at 0)
    int interrupt[2] = {0, 0}, firstHit[2] = {-1, -1}; // fmlog: what classified it
    bool hit[2] = {false, false}, counter[2] = {false, false};
    uint32_t side[2] = {0, 0};
    int round = 0; // a replay's round, from its call (the meter starts again with each); training: 0
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
    bool air = false;        // the last frame was airborne
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
int g_written = 0; // blocks written in this bar (the next one's index)
bool g_hasContent = false, g_ended = true, g_pendingMark = false, g_exchange = false;
bool g_armed = false; // both fighters have been free to act since the battle (or a reset) began
std::vector<Record> g_records; // a viewed replay's, by recording frame
int g_applied = -1;            // the last recording frame the meter is built to
int g_round = 0;               // the round of the last frame applied (Record::round)

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
    bool hit = false, strike = false, projectile = false, hurt = false, parry = false, armor = false, counter = false, air = false;
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
        const uint32_t flags = StatusFlags(script, s.frame);
        s.counter = (flags & kStatusCounter) != 0;
        s.air = (flags & kStatusAir) != 0;
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
        if ((id >> 16) == s.script && (!s.parry || !s.armor)) {
            const int effect = ArmorEffect(script, (id >> 8) & 0xFF);
            if (effect == kArmorParry) s.parry = true;
            if (effect == kArmorArmor) s.armor = true;
        }
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
    const bool stand = s.script == kScriptStand; // see `action` below
    const bool past = interruptible && s.frame >= s.interrupt;
    // The jump's scripts (pre-jump, airborne, landing: the same indices in
    // every character's move file) are actions too: the character is
    // committed to them, even where the script has no interrupt frame.
    const bool jump = s.script >= kScriptFirstJump && s.script <= kScriptLastJump;
    // STAND is standing, free to act, whatever its move file carries:
    // Akuma's and Birdie's STAND have a first hitbox frame (Akuma's at 401,
    // the long idle pose standing still plays) with no attack ever out, and
    // read as recovery, so the meter never began (both were never free to
    // act) and the idle counted as a move.
    const bool action = !stand && (interruptible || jump || s.firstHit >= 0 || s.counter);
    // Proximity guard (the move file's FGUARD scripts) holds the guard pose
    // with no attack blocked yet: the state object counts it as stun too.
    if (s.script >= kScriptFirstFakeGuard && s.script <= kScriptLastFakeGuard) return ProxGuard;
    if (s.stun) return past ? Idle : Stun;
    // (Akuma's long idle pose has a strike box out on its STAND frames 401-402.)
    if (s.hit && !stand) return Active;
    if (s.parry) return Parry;
    if (action || !s.hurt) {
        if (!s.strike && !s.projectile) return Invincible;
        if (!s.strike) return StrikeInv;
        if (!s.projectile) return ProjInv;
    }
    // Armor (a live hurtbox that takes hits without stun), whatever else the
    // frame is: in a move's startup or recovery, or while free to act.
    if (s.armor) return action && !past ? Armor : ArmorFree;
    if (s.counter && !stand) return Counter;
    if (!action) return Idle;
    return past ? Iasa : Recovery;
}

// A side's fighter actors, when it has more than one: a helper is a fighter
// too. R. Mika's Nadeshiko is a third fighter actor on R. Mika's side, with
// R. Mika's move file, standing in STAND at frame 0 where the round began,
// her script not run unless she is called in (and her state object's side
// is -1). With three fighters the meter never found its two and stayed off.
struct Candidate {
    const uint8_t* actor;
    uint32_t script;
    int32_t frame;
};
std::vector<Candidate> g_candidates[2];
uint64_t g_candidatesLayout = ~0ull;

bool RunnerNow(const uint8_t* r, uint32_t* script, int32_t* frame)
{
    const auto* runner = At<const uint8_t*>(r, kRunner);
    if (!runner || !IsReadable(runner, kRunnerSpeed + 4)) return false;
    *script = At<uint32_t>(runner, kRunnerIndex);
    *frame = At<int32_t>(runner, kRunnerFrame);
    return true;
}

// The two fighters' actors and state objects, P1 first, paired by side: at
// a battle's start P1 is on the left. Not yet (false) while the sides are
// not set apart, or while a side with a helper has not shown which of its
// fighters plays: the one whose script runs while the other's stands still
// (a player's STAND loops its frames; an idle helper's stays at 0).
bool FindFighters(SfvGame& game)
{
    std::vector<const uint8_t*> actors[2];
    const uint8_t* states[2] = {};
    int ns[2] = {0, 0};
    for (const StateRegion& r : game.Regions()) {
        if (r.size == kRegionSize && IsActor(r.ptr, kFighterVtable)) {
            const uint32_t side = At<uint32_t>(r.ptr, kSide);
            if (side <= 1 && actors[side].size() < 4) actors[side].push_back(r.ptr);
        }
        else if (r.size == kStateRegionSize && IsReadable(r.ptr - kStateRegionAt, kStateSide + 4) &&
                 At<uintptr_t>(r.ptr - kStateRegionAt, 0) == g_base + kStateVtable) {
            // A helper's state object has no side (-1).
            const uint32_t side = At<uint32_t>(r.ptr - kStateRegionAt, kStateSide);
            if (side > 1) continue;
            states[side] = r.ptr - kStateRegionAt;
            ns[side]++;
        }
    }
    if (actors[0].empty() || actors[1].empty() || ns[0] != 1 || ns[1] != 1) return false;
    if (g_candidatesLayout != game.LayoutVersion()) {
        g_candidatesLayout = game.LayoutVersion();
        g_candidates[0].clear();
        g_candidates[1].clear();
    }
    const uint8_t* chosen[2] = {};
    for (int side = 0; side < 2; side++) {
        if (actors[side].size() == 1) {
            chosen[side] = actors[side][0];
            continue;
        }
        // Which of them runs its script since the last look.
        std::vector<Candidate>& seen = g_candidates[side];
        std::vector<Candidate> now;
        const uint8_t* running = nullptr;
        int runningCount = 0;
        for (const uint8_t* a : actors[side]) {
            Candidate c{a, 0, 0};
            if (!RunnerNow(a, &c.script, &c.frame)) continue;
            for (const Candidate& before : seen) {
                if (before.actor == a && (before.script != c.script || before.frame != c.frame)) {
                    running = a;
                    runningCount++;
                }
            }
            now.push_back(c);
        }
        seen = now;
        if (runningCount != 1) return false;
        chosen[side] = running;
        Log("framemeter: side %d has %zu fighters (a helper): the one whose script runs, %p", side, actors[side].size(),
            static_cast<const void*>(running));
    }
    for (int p = 0; p < 2; p++) {
        g_p[p].actor = chosen[p];
        g_p[p].state = states[p];
    }
    return true;
}

// A projectile's owner: the fighter it faced like when it appeared (an
// effect is spawned facing as its owner faces), kept for the effect's life,
// which its spawn point marks (its position object's +0x1D0, fixed while it
// lives; a slot used again starts elsewhere). Told by side on each frame
// instead, a projectile left out while its owner crossed over (Urien's
// Aegis Reflector, then a jump over the opponent) counted for the other
// player. The game keeps no owner link near the effect (research
// 2026-11-12: no pointer to either fighter around the 16 slots).
struct EffectOwner {
    const uint8_t* slot = nullptr;
    uint64_t spawn = 0;
    int owner = 0;
};
std::vector<EffectOwner> g_effectOwners;

float FighterX(int i)
{
    const auto* pos = At<const uint8_t*>(g_p[i].actor - kPosition, 0);
    return pos && IsReadable(pos + kFighterXY, 4) ? At<float>(pos, kFighterXY) : 0.0f;
}

// The owner of the live effect at `slot` (side, spawn point as read now).
int EffectOwnerOf(const uint8_t* slot, uint32_t side, uint64_t spawn, const Sample s[2])
{
    for (EffectOwner& e : g_effectOwners) {
        if (e.slot != slot) continue;
        if (e.spawn == spawn) return e.owner;
        g_effectOwners.erase(g_effectOwners.begin() + (&e - g_effectOwners.data()));
        break;
    }
    // Appeared now: the fighter on its side; with both on one side (as one
    // passes the other), the one nearer where it appeared.
    int owner = 0;
    if (s[0].side != s[1].side) owner = s[0].side == side ? 0 : 1;
    else {
        float x;
        std::memcpy(&x, &spawn, sizeof(x));
        owner = std::fabs(FighterX(0) - x) <= std::fabs(FighterX(1) - x) ? 0 : 1;
    }
    g_effectOwners.push_back({slot, spawn, owner});
    return owner;
}

// The frame the game is on now, read into a record (false: the fighters are
// gone, a new battle). `fight`: in a round's fight (replays; else always).
bool ReadFrame(SfvGame& game, bool fight, Record* out)
{
    Record r;
    Sample s[2];
    for (int i = 0; i < 2; i++) {
        if (!IsActor(g_p[i].actor, kFighterVtable)) return false;
        s[i] = Read(g_p[i].actor, g_p[i].state);
        if (!s[i].ok) return false;
    }
    r.valid = true;
    // Hitstop (both held), a super freeze and a cinematic are not counted.
    const bool freeze = s[0].frozen || s[1].frozen || s[0].cinematic || s[1].cinematic;
    r.hold = !fight ? 3 : freeze ? 2 : s[0].held && s[1].held ? 1 : 0;
    // The players' projectiles: live effect slots with an attacking box, each
    // in its owner's lane (EffectOwnerOf; every frame, freezes too, so that
    // one spawned in a super freeze is seen as it appears).
    for (const StateRegion& reg : game.Regions()) {
        if (reg.size != kRegionSize || !IsActor(reg.ptr, kEffectVtable)) continue;
        const uint32_t side = At<uint32_t>(reg.ptr, kSide);
        if (side > 1 || (!At<uint32_t>(reg.ptr, kEffectLiveA) && !At<uint32_t>(reg.ptr, kEffectLiveB))) {
            g_effectOwners.erase(std::remove_if(g_effectOwners.begin(), g_effectOwners.end(),
                                                [&](const EffectOwner& e) { return e.slot == reg.ptr; }),
                                 g_effectOwners.end());
            continue;
        }
        const auto* pos = At<const uint8_t*>(reg.ptr - kPosition, 0);
        const uint64_t spawn = pos && IsReadable(pos + kEffectSpawnXY, 8) ? At<uint64_t>(pos, kEffectSpawnXY) : 0;
        const int owner = EffectOwnerOf(reg.ptr, side, spawn, s);
        if (r.hold != 0) continue;
        bool hit = false;
        for (size_t at = kHitList; at + kEntrySize <= kHitListEnd && !hit; at += kEntrySize) {
            const uint8_t* e = reg.ptr + at;
            hit = At<uintptr_t>(e, kEntryVtableAt) == g_base + kEntryVtable && At<uint32_t>(e, kEntryId) != 0xFFFFFFFFu &&
                  e[kEntryType] != 4 && e[kEntryType] <= 5;
        }
        if (hit) r.lane[owner] = true;
    }
    for (int i = 0; i < 2; i++) {
        r.kind[i] = Classify(s[i]);
        r.air[i] = s[i].air;
        r.attack[i] = s[i].firstHit >= 0;
        r.script[i] = s[i].script;
        r.frame[i] = s[i].frame;
        r.interrupt[i] = s[i].interrupt;
        r.firstHit[i] = s[i].firstHit;
        r.hit[i] = s[i].hit;
        r.counter[i] = s[i].counter;
        r.side[i] = s[i].side;
    }
    *out = r;
    return true;
}

void EndRun(int i)
{
    Run& run = g_p[i].run;
    // A single block needs no count (and its label would run into the next).
    if (run.group != Idle && run.length > 1 && run.cell >= 0) g_m.count[i][run.cell] = static_cast<uint16_t>(std::min(run.length, 9999));
    run = Run{};
}

// An empty bar; the numbers of the last moves stay.
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
    g_hasContent = false;
    g_ended = true;
    g_pendingMark = false;
    g_exchange = false;
}

// As at a battle's start: nothing recorded, nothing known.
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

void Write(const Record& r)
{
    const int cell = g_written % N;
    for (int i = 0; i < 2; i++) {
        Kind k = r.kind[i];
        if (k == Active && Group(g_p[i].lastWritten) != Active) k = FirstActive;
        if (Group(k) != g_p[i].run.group) EndRun(i);
        Run& run = g_p[i].run;
        run.group = Group(k);
        run.length++;
        run.cell = cell;
        g_m.kind[i][cell] = k;
        g_m.count[i][cell] = 0;
        g_m.lane[i][cell] = r.lane[i];
        // The first airborne frame after the pre-jump, the first landing frame.
        g_m.edge[i][cell] = r.air[i] && !g_p[i].air ? EdgeAirborne : !r.air[i] && g_p[i].air ? EdgeLanding : 0;
        g_p[i].lastWritten = k;
    }
    g_m.mark[cell] = g_pendingMark;
    g_pendingMark = false;
    if (g_written + 1 >= N) {
        // Wrapped: the block after the newest is cleared (where the next goes).
        const int c = (cell + 1) % N;
        for (int i = 0; i < 2; i++) {
            g_m.kind[i][c] = Empty;
            g_m.count[i][c] = 0;
            g_m.lane[i][c] = false;
            g_m.edge[i][c] = 0;
        }
        g_m.mark[c] = false;
    }
    g_written++;
    g_hasContent = true;
    g_ended = false;
}

// Each move's startup, total and the frame advantage after an exchange.
void Count(const Record& r, int index)
{
    for (int i = 0; i < 2; i++) {
        Player& p = g_p[i];
        const Kind k = r.kind[i];
        const bool actionable = Actionable(k);
        const bool doing = !actionable && k != Stun && k != ProxGuard;
        // A move starts when the character acts from neutral or IASA, or
        // cancels into a script with hitboxes of its own, from that script's
        // start, with startup frames before its first hitbox. A script
        // handing over to its follow-up, a landing or a recovery is the same
        // move, and so are hand-overs of a held move let go of: Urien's st.HP
        // begins as 5HP_HOLD and goes on as 5HP from its frame 7 (counted
        // from there, its startup read 5F, not 12F); his st.HK begins as #605
        // and goes on as #606 from its frame 0 with its hitbox out on that
        // very frame (counted from there, 1F, not 9F). No move that starts
        // anew has a hitbox on its first frame.
        if (doing && (!p.doing || (r.script[i] != p.script && r.attack[i] && r.frame[i] == 0 && r.firstHit[i] > 0))) {
            p.moveStart = index;
            g_m.startup[i] = -1;
            g_m.total[i] = -1;
            g_m.advKnown[i] = false;
        }
        if (p.moveStart >= 0) {
            if (g_m.startup[i] < 0 && (Group(k) == Active || r.lane[i])) g_m.startup[i] = index - p.moveStart + 1;
            if (actionable) {
                g_m.total[i] = index - p.moveStart;
                p.moveStart = -1;
            }
            else if (k == Stun || k == ProxGuard) p.moveStart = -1;
        }
        if (actionable && !p.actionable) p.actionableSince = index;
        p.actionable = actionable;
        p.doing = doing;
        p.script = r.script[i];
        p.air = r.air[i];
        if (k == Stun) g_exchange = true;
    }
    if (g_exchange && g_p[0].actionable && g_p[1].actionable) {
        g_m.adv[0] = g_p[1].actionableSince - g_p[0].actionableSince;
        g_m.adv[1] = -g_m.adv[0];
        g_m.advKnown[0] = g_m.advKnown[1] = true;
        g_exchange = false;
    }
}

// One frame into the meter.
void Apply(const Record& r)
{
    // A frame not read (a replay's frame never played): what follows is not
    // joined to what came before.
    if (!r.valid) {
        ClearBar();
        g_armed = false;
        return;
    }
    // A replay's next round, from its call (after the round's set-up, the
    // screen's load): the meter as at a battle's start, bars and numbers.
    if (r.round != g_round) {
        Reset();
        g_round = r.round;
    }
    if (r.hold == 3) return;
    if (r.hold == 2) {
        if (g_hasContent && !g_ended) g_pendingMark = true;
        return;
    }
    if (r.hold == 1) return;
    // Not before both have been free to act (a battle's or training's start).
    const bool bothFree = Actionable(r.kind[0]) && Actionable(r.kind[1]);
    if (!g_armed && !bothFree) return;
    g_armed = true;
    const bool neutral = r.kind[0] == Idle && r.kind[1] == Idle && !r.lane[0] && !r.lane[1];
    if (neutral) {
        // Both neutral: the meter holds, its last frame marked.
        if (g_hasContent && !g_ended) {
            const int last = (g_written - 1) % N;
            for (int i = 0; i < 2; i++) {
                g_m.edge[i][last] |= EdgeEnd;
                EndRun(i);
            }
            g_ended = true;
        }
        Count(r, g_written);
        return;
    }
    // An action that does not carry on an exchange (the other player free to
    // act, not in stun or in an action of their own) starts a new bar.
    bool fresh = !g_hasContent;
    for (int i = 0; i < 2; i++) {
        const Kind k = r.kind[i];
        const bool starts = !Actionable(k) && k != Stun && k != ProxGuard && g_p[i].actionable;
        if (starts && Actionable(r.kind[1 - i]) && g_p[1 - i].actionable) fresh = true;
    }
    if (fresh) ClearBar();
    const int index = g_written;
    Write(r);
    Count(r, index);
}

void Publish()
{
    std::lock_guard lock(g_mutex);
    g_shown = g_m;
}

bool Wanted(bool* replay, int* frame, bool* fight)
{
    const ReplayView v = ReplayViewNow();
    *replay = v.active;
    *frame = v.frame;
    // A replay's round: from its FIGHT to its KO (not the intro, the KO and
    // the win poses).
    *fight = !v.active || v.roundFrame >= 0;
    if (g_forced) return true;
    if (v.active) return !v.benchmark && FrameMeterInReplays();
    return FrameMeterInTraining() && CurrentScene() == "TrainingBattleState";
}

// A viewed replay (not the benchmark's) with its meter hidden still records.
bool RecordsHidden(bool replay) { return replay && !ReplayViewNow().benchmark; }

// A new battle's fighters (the region layout changed): false while not there.
bool Ready(SfvGame& game, bool replay)
{
    if (game.LayoutVersion() == g_layout) return true;
    if (!FindFighters(game)) return false;
    g_layout = game.LayoutVersion();
    g_records.clear();
    g_effectOwners.clear(); // (only here: a replay's rebuilds keep the live projectiles' owners)
    g_applied = -1;
    Reset();
    Log("framemeter: %s, P1 %p, P2 %p", replay ? "replay" : CurrentScene().c_str(), static_cast<const void*>(g_p[0].actor),
        static_cast<const void*>(g_p[1].actor));
    return true;
}

void Store(int frame, const Record& r)
{
    if (frame < 0 || frame > 1000000) return;
    if (static_cast<size_t>(frame) >= g_records.size()) g_records.resize(frame + 1);
    g_records[frame] = r;
}

// A replay's meter as the frames up to `frame` built it: on from where it
// is when they follow on, else again from the start.
void BuildTo(int frame)
{
    if (frame == g_applied) return;
    if (frame < g_applied || g_applied < 0) {
        Reset();
        g_applied = -1;
    }
    for (int f = g_applied + 1; f <= frame; f++) Apply(static_cast<size_t>(f) < g_records.size() ? g_records[f] : Record{});
    g_applied = frame;
    Publish();
}

void LogRecord(int index, const Record& r)
{
    if (g_log <= 0) return;
    g_log--;
    char side[2][96];
    for (int i = 0; i < 2; i++) {
        std::snprintf(side[i], sizeof(side[i]), "#%u f%d %s%s%s (side %u, int %d, hit at %d%s%s)", r.script[i], r.frame[i], KindName(r.kind[i]),
                      r.lane[i] ? "+proj" : "", r.air[i] ? "+air" : "", r.side[i], r.interrupt[i], r.firstHit[i], r.hit[i] ? ", box" : "",
                      r.counter[i] ? ", cnt" : "");
    }
    Log("fmlog %d: hold %d | P1 %s | P2 %s | written %d | startup %d/%d, total %d/%d", index, r.hold, side[0], side[1], g_written,
        g_m.startup[0], g_m.startup[1], g_m.total[0], g_m.total[1]);
}

// Text over the fight: a thin black outline round it and a drop shadow under
// it, so it reads on any stage (`s`: the design scale).
void OutlinedText(ImDrawList* d, ImFont* f, float px, ImVec2 at, ImU32 color, const std::string& t, float align, float s)
{
    const ImVec2 size = f->CalcTextSizeA(px, FLT_MAX, 0, t.c_str());
    const ImVec2 p(std::round(at.x - size.x * align), std::round(at.y));
    const float o = std::max(1.0f, std::round(s));
    const float drop = std::max(1.0f, std::round(2.5f * s));
    d->AddText(f, px, ImVec2(p.x + o, p.y + drop), IM_COL32(0, 0, 0, 150), t.c_str());
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx || dy) d->AddText(f, px, ImVec2(p.x + dx * o, p.y + dy * o), IM_COL32(0, 0, 0, 230), t.c_str());
        }
    }
    d->AddText(f, px, p, color, t.c_str());
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
    bool replay = false, fight = true;
    int frame = 0;
    // A viewed replay keeps its records while the meter is hidden: shown
    // again it is built from them at once (it was reset, empty until the
    // next exchange, and a step back found nothing before the show).
    if (!Wanted(&replay, &frame, &fight) && !RecordsHidden(replay)) {
        g_layout = ~0ull;
        return;
    }
    if (!Ready(game, replay)) return;
    Record r;
    if (replay) {
        // The state this tick starts from is the one after the frame before
        // it: that frame's record (BuildTo applies it once it is on screen).
        const int f = ReplayTickFrame() - 1;
        if (f < 0 || !ReadFrame(game, ReplayTickInFight(), &r)) return;
        r.round = ReplayTickRound();
        Store(f, r);
        LogRecord(f, r);
        return;
    }
    if (!ReadFrame(game, true, &r)) {
        g_layout = ~0ull;
        return;
    }
    Apply(r);
    LogRecord(g_written, r);
    Publish();
}

void FrameMeterFrame(SfvGame& game)
{
    if (!g_base) g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool replay = false, fight = true;
    int frame = 0;
    const bool wanted = Wanted(&replay, &frame, &fight);
    if ((!wanted && !RecordsHidden(replay)) || !Ready(game, replay)) return;
    if (replay) {
        // The frame on screen: its record (the state now), then the meter
        // built to it, whichever way the replay got there (hidden: only the
        // record).
        Record r;
        if (ReadFrame(game, fight, &r)) {
            r.round = ReplayViewNow().callRound;
            Store(frame, r);
        }
        if (!wanted) return;
        if (!ReplayViewNow().seeking) BuildTo(frame);
        g_tickAt = GetTickCount64();
        return;
    }
    // Training's pause menu stops the ticks: no meter over the game's menu.
    if (GetTickCount64() - game.LastTickMs() <= kPausedMs) g_tickAt = GetTickCount64();
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
    // Design units: the bars centred on the screen (x 260..1340), above the
    // V-Trigger gauges; the blocks are laid on whole pixels (one pitch and
    // gap for every block, the N centred on x 800), so no gap differs. P1's
    // numbers above its bar, P2's under its own (both on the left).
    constexpr float cx = 800, y0 = 624, pitch = 12, gap = 2, h = 20;
    constexpr float kTextSize = 21, kCountSize = 20;
    constexpr float kCounts[2] = {y0 + 25, y0 + 103}, kLanes[2] = {y0 + 46, y0 + 98}, kBars[2] = {y0 + 52, y0 + 76};
    constexpr float kTexts[2] = {y0 - 2, y0 + 127};
    const float s = g_v.s;
    const float pitchPx = std::max(3.0f, std::round(pitch * s));
    const float gapPx = std::max(1.0f, std::round(gap * s));
    const float wPx = pitchPx - gapPx;
    const float left = std::round(g_v.P(cx, 0).x - N * pitchPx / 2);
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
        case ProxGuard: return Rgba(156, 104, 58);
        case Armor:
        case ArmorFree: return Rgba(214, 98, 208);
        case Invincible: return Rgba(244, 241, 232);
        case StrikeInv: return Rgba(229, 67, 90);
        case ProjInv: return Rgba(240, 160, 48);
        default: return Rgba(10, 12, 16, 158);
        }
    };
    // A solid black bar under each row (the gaps between blocks black too),
    // a thin black border round it, a drop shadow under that.
    const float border = std::max(1.0f, std::round(1.5f * s));
    const float edgePx = std::max(1.0f, std::round(1.5f * s));
    for (int i = 0; i < 2; i++) {
        const float y = rowY(kBars[i]);
        d->AddRectFilled(ImVec2(left - border, y - border + drop), ImVec2(right + border, y + hPx + border + drop), shadow);
        d->AddRectFilled(ImVec2(left - border, y - border), ImVec2(right + border, y + hPx + border), IM_COL32(0, 0, 0, 255));
    }
    for (int i = 0; i < 2; i++) {
        const float y = rowY(kBars[i]), ly = rowY(kLanes[i]);
        for (int c = 0; c < N; c++) {
            const float x = cellX(c);
            const Kind k = m.kind[i][c];
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + wPx, y + hPx), color(k));
            if (k == StrikeInv || k == ProjInv) {
                const float stripe = std::max(1.0f, std::round(3 * s));
                for (float sy = 0; sy < hPx; sy += 2 * stripe) d->AddRectFilled(ImVec2(x, y + sy), ImVec2(x + wPx, y + std::min(sy + stripe, hPx)), look::kCream);
            }
            // Milestones (the first airborne frame, the first landing frame,
            // a sequence's last frame): white on the top, right and bottom.
            if (m.edge[i][c]) {
                const ImU32 white = IM_COL32(255, 255, 255, 255);
                d->AddRectFilled(ImVec2(x, y), ImVec2(x + wPx, y + edgePx), white);
                d->AddRectFilled(ImVec2(x + wPx - edgePx, y), ImVec2(x + wPx, y + hPx), white);
                d->AddRectFilled(ImVec2(x, y + hPx - edgePx), ImVec2(x + wPx, y + hPx), white);
            }
            if (m.lane[i][c]) {
                d->AddRectFilled(ImVec2(x - border, ly - border), ImVec2(x + wPx + border, ly + lanePx + border), IM_COL32(0, 0, 0, 255));
                d->AddRectFilled(ImVec2(x, ly), ImVec2(x + wPx, ly + lanePx), Rgba(240, 160, 48));
            }
            if (m.count[i][c]) {
                OutlinedText(d, bold, TextPx(kCountSize), ImVec2(x + wPx / 2, rowY(kCounts[i])), look::kCream, std::to_string(m.count[i][c]), 0.5f, s);
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
    // The numbers, each at the left: P1's above the meter, P2's under it
    // ("P1 Startup 6F · Total 20F · Advantage +5F").
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
        if (!pieces.empty()) pieces.front().gap = 10;
        // The player colours (overlay.cpp kP1 / kP2): P1 blue, P2 red.
        pieces.insert(pieces.begin(), Piece{i == 0 ? "P1" : "P2", i == 0 ? Rgba(92, 158, 255) : Rgba(255, 92, 77), 0});
        float x = left;
        const float ty = rowY(kTexts[i]);
        for (const Piece& p : pieces) {
            x += p.gap * s;
            OutlinedText(d, bold, TextPx(kTextSize), ImVec2(x, ty), p.color, p.text, 0, s);
            x += TextW(bold, kTextSize, p.text) * s;
        }
    }
}
} // namespace sfv::mod
