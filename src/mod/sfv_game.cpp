#include "mod/sfv_game.h"

#include "core/state.h"
#include "mod/battle_driver.h"
#include "mod/camera_rollback.h"
#include "mod/frame_meter.h"
#include "mod/sim_version.h"
#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/ue4.h"
#include "mod/watch.h"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace sfv::mod {

namespace {

namespace mgr = game::manager;

template <typename T>
T Read(const uint8_t* base, uint32_t offset) {
    return *reinterpret_cast<const T*>(base + offset);
}

constexpr size_t kMaxRegions = 100000;
constexpr size_t kMaxGroups = 64;

// Sorted committed address ranges, for cheap "is this a pointer" checks.
std::vector<std::pair<uintptr_t, uintptr_t>> CommittedRanges() {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi))) {
        const auto start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = start + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) {
            if (!out.empty() && out.back().second == start) out.back().second = end;
            else out.emplace_back(start, end);
        }
        if (end <= addr) break;
        addr = end;
    }
    return out;
}

bool InRanges(const std::vector<std::pair<uintptr_t, uintptr_t>>& ranges, uint64_t v) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), std::make_pair(static_cast<uintptr_t>(v), UINTPTR_MAX));
    if (it == ranges.begin()) return false;
    --it;
    return v >= it->first && v < it->second;
}

// Address ranges seen in use (committed or reserved) and 64 KB granules seen
// free; the free ones are forgotten every few seconds as the heaps grow.
std::mutex g_addressMutex;
std::vector<std::pair<uintptr_t, uintptr_t>> g_addressRanges;
std::unordered_set<uint64_t> g_freeGranules;
ULONGLONG g_freeGranulesSince = 0;

} // namespace

bool LooksLikeAddress(uint64_t v) {
    // Below 4 GB are mostly plain integers; nothing of the game's lives there.
    if (v < 0x100000000ull || v >= 0x800000000000ull) return false;
    std::lock_guard lock(g_addressMutex);
    if (InRanges(g_addressRanges, v)) return true;
    // Free granules are forgotten quickly: a pointer to memory allocated since
    // must not stay "not an address" on one machine only.
    const ULONGLONG now = GetTickCount64();
    if (now - g_freeGranulesSince > 250) {
        g_freeGranules.clear();
        g_freeGranulesSince = now;
    }
    if (g_freeGranules.count(v >> 16)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(v), &mbi, sizeof(mbi)) || mbi.State == MEM_FREE) {
        g_freeGranules.insert(v >> 16);
        return false;
    }
    // Remember the whole reservation, not just this part of it: heaps commit
    // more of their reservations as they grow.
    const auto base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    MEMORY_BASIC_INFORMATION next;
    while (VirtualQuery(reinterpret_cast<const void*>(end), &next, sizeof(next)) && next.AllocationBase == mbi.AllocationBase &&
           next.State != MEM_FREE) {
        end = reinterpret_cast<uintptr_t>(next.BaseAddress) + next.RegionSize;
    }
    const auto range = std::make_pair(std::min(base, reinterpret_cast<uintptr_t>(mbi.BaseAddress)), end);
    g_addressRanges.insert(std::upper_bound(g_addressRanges.begin(), g_addressRanges.end(), range), range);
    return true;
}

// ---------------------------------------------------------------------------
// StateMap

bool StateMap::Load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    bool ok = true;
    auto get = [&](void* p, size_t n) { ok = ok && std::fread(p, 1, n, f) == n; };

    uint32_t magic = 0, version = 0, count = 0;
    get(&magic, 4);
    get(&version, 4);
    get(&count, 4);
    ok = ok && magic == 0x4D564653 /* SFVM */ && version == 1 && count < kMaxRegions;
    regions.clear();
    for (uint32_t i = 0; ok && i < count; i++) {
        Region r;
        uint32_t len = 0;
        get(&r.size, 8);
        get(&len, 4);
        ok = ok && len <= (r.size + 63) / 64;
        if (!ok) break;
        r.ignore.resize(len);
        get(r.ignore.data(), len);
        regions.push_back(std::move(r));
    }
    std::fclose(f);
    if (!ok) regions.clear();
    return ok;
}

bool StateMap::Save(const std::wstring& path) const {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const uint32_t header[3] = {0x4D564653, 1, static_cast<uint32_t>(regions.size())};
    std::fwrite(header, 4, 3, f);
    for (const auto& r : regions) {
        const uint32_t len = static_cast<uint32_t>(r.ignore.size());
        std::fwrite(&r.size, 8, 1, f);
        std::fwrite(&len, 4, 1, f);
        std::fwrite(r.ignore.data(), 1, len, f);
    }
    return std::fclose(f) == 0;
}

uint64_t StateMap::Fingerprint() const {
    uint64_t h = regions.size();
    for (const auto& r : regions) {
        h = Hash64(&r.size, sizeof(r.size), h);
        h = Hash64(r.ignore.data(), r.ignore.size(), h);
    }
    return h;
}

// ---------------------------------------------------------------------------
// SfvGame

void SfvGame::OnInputUnit(uint8_t* inputUnit) {
    lastInputMs_ = GetTickCount64();
    if (inputUnit != inputUnit_) {
        inputUnit_ = inputUnit;
        Log("InputUnit at %p, %d pads, local mask %x", inputUnit, Read<int32_t>(inputUnit, game::inputunit::kPadCount),
            Read<uint32_t>(inputUnit, game::inputunit::kLocalMask));
    }
    // The values the game computed from the pads for this tick; then, when
    // driving the tick, what the players actually play with.
    auto* values = reinterpret_cast<uint32_t*>(inputUnit + game::inputunit::kValues);
    const int pads = std::clamp(Read<int32_t>(inputUnit, game::inputunit::kPadCount), 0, 4);
    for (int i = 0; i < pads; i++) sampled_[i] = values[i];
    // Research (`inputlog <ticks>`): what the game computed per pad, on change.
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

namespace {

// watchregion: a write watch on a region of the next battle, armed as soon as
// its region list exists (a region's address is only known then).
std::mutex g_regionWatchMutex;
std::string g_regionWatchName;
size_t g_regionWatchOffset = 0;
int g_regionWatchLength = 0;

} // namespace

void WatchRegionOfNextBattle(const std::string& name, size_t offset, int length) {
    std::lock_guard lock(g_regionWatchMutex);
    g_regionWatchName = name;
    g_regionWatchOffset = offset;
    g_regionWatchLength = length;
    Log("watch: armed for region %s +0x%zx (%d bytes) in the next region list", name.c_str(), offset, length);
}

// A name ending in ".*" (a pool: which slot the game takes is not known in
// advance) is armed by CheckChangeWatch instead, on the first region of that
// prefix whose watched bytes change.
bool IsPoolWatch() { return g_regionWatchName.size() > 2 && g_regionWatchName.compare(g_regionWatchName.size() - 2, 2, ".*") == 0; }

std::map<const uint8_t*, std::vector<uint8_t>> g_poolWatchSeen;

void CheckChangeWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase) {
    std::lock_guard lock(g_regionWatchMutex);
    if (g_regionWatchName.empty() || !IsPoolWatch()) return;
    const std::string prefix = g_regionWatchName.substr(0, g_regionWatchName.size() - 1); // "s44."
    for (const StateRegion& r : regions) {
        if (r.name.compare(0, prefix.size(), prefix) != 0 || g_regionWatchOffset + g_regionWatchLength > r.size) continue;
        const uint8_t* p = r.ptr + g_regionWatchOffset;
        auto& seen = g_poolWatchSeen[r.ptr];
        const bool changed = !seen.empty() && std::memcmp(seen.data(), p, seen.size()) != 0;
        seen.assign(p, p + g_regionWatchLength);
        if (!changed) continue;
        // From another thread: arming sets every thread's debug registers,
        // and the game thread (this one) cannot set its own while it runs.
        const uint8_t* at = r.ptr + g_regionWatchOffset;
        const int length = g_regionWatchLength;
        std::thread([at, length, imageBase] {
            ThreadGuard running;
            int slot = 0;
            for (int done = 0; done < length && slot < 4; done += 8, slot++) {
                ArmWatch(slot, at + done, std::min(8, length - done), imageBase);
            }
        }).detach();
        Log("watch: region %s at %p changed at +0x%zx; watching it", r.name.c_str(), static_cast<void*>(r.ptr),
            g_regionWatchOffset);
        g_regionWatchName.clear();
        g_poolWatchSeen.clear();
        return;
    }
}

void ArmPendingRegionWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase) {
    std::lock_guard lock(g_regionWatchMutex);
    if (g_regionWatchName.empty() || IsPoolWatch()) return;
    for (const StateRegion& r : regions) {
        if (r.name != g_regionWatchName || g_regionWatchOffset >= r.size) continue;
        // Debug registers watch up to 8 bytes each: 16 bytes take two.
        int slot = 0;
        for (int done = 0; done < g_regionWatchLength && slot < 4; done += 8, slot++) {
            ArmWatch(slot, r.ptr + g_regionWatchOffset + done, std::min(8, g_regionWatchLength - done), imageBase);
        }
        Log("watch: region %s at %p, watching +0x%zx", r.name.c_str(), static_cast<void*>(r.ptr), g_regionWatchOffset);
        g_regionWatchName.clear();
        return;
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

    // The current battle's objects: the newest (highest numbered) instances
    // in the persistent level; older battles' may not be collected yet.
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

// The sound manager's post-restore callback (flow_tasks.cpp kSoundReconcileTask).
constexpr uintptr_t kSoundRestoreCallback = 0x4B6230;

// Rose's soul entity (vtable rva 0x2C5E2A8): the game snapshots 884 bytes of
// it from +0xF0; +0x5C8..+0x66F (counters, the soul level countdown and
// table; plain integers, a pointer follows) is added (RefreshRegions).
constexpr uint32_t kSoulEntityVtable = 0x2C5E2A8;
constexpr size_t kSoulEntityRegionSize = 884, kSoulEntityRegionAt = 0xF0;
constexpr size_t kSoulEntityTailAt = 0x5C8, kSoulEntityTailSize = 0xA8;
// The battle camera: the game snapshots 528 bytes of it from +0x100 (its
// sub-object with vtable rva 0x2C4F4A0 sits at +0xE8). The screen shake's
// flip period, counter and stop flag (+0x310, +0x314, +0x318: 0x339514,
// 0x33E0A3) lie after it, so after a rollback the shake flipped direction on
// another frame (a Rose mirror match, both PCs). +0x310..+0x31F is added.
constexpr uint32_t kCameraPartVtable = 0x2C4F4A0;
constexpr size_t kCameraRegionSize = 528, kCameraRegionAt = 0x100, kCameraPartAt = 0xE8;
constexpr size_t kCameraShakeAt = 0x310, kCameraShakeSize = 0x10;
// The rest of the shake, inside the game's region: +0x2E0..+0x30F (rva
// 0x33A7C0 starts a shake, 0x33E090 runs and ends it).
constexpr size_t kCameraShakeFieldsAt = 0x2E0;

bool IsBattleCamera(const uint8_t* camera, uintptr_t imageBase) {
    return IsReadable(camera + kCameraPartAt, 8) &&
           *reinterpret_cast<const uintptr_t*>(camera + kCameraPartAt) == imageBase + kCameraPartVtable &&
           IsReadable(camera + kCameraShakeAt, kCameraShakeSize);
}

// A fighter: the game snapshots its 372-byte vitality block from +0xC0 (the
// block's owner, vtable rva 0x2C51718, sits at +0xB0). +0x338 is a per-tick
// event flag (cleared with +0x334 at rva 0x362A64, set at 0x328E13) that no
// region covers, and Rose's soul entity reads it (rva 0x3C10D0, 0x372940)
// before the fighter's tick clears it, so it carries from one tick to the
// next: after a rollback it held the newest tick's value and Rose's entity
// set the fighter's +0x110 bit differently (Urien vs Rose 136d1a40). Added.
constexpr uint32_t kVitalityOwnerVtable = 0x2C51718;
constexpr size_t kVitalityRegionSize = 372, kVitalityRegionAt = 0xC0, kVitalityOwnerAt = 0xB0;
constexpr size_t kFighterEventAt = 0x338, kFighterEventSize = 4;
// A fighter's four effect switches: records of 0x48 bytes from +0x340. Each
// tick rva 0x362520 picks the entry type a switch shows (by its kind at
// +0x28, or its override at +0x40 when that is not -1), turns off the type
// it showed before (+0x3C) when the pick changed, and turns the pick on in
// the fighter's effect list (s1624 +0x0, its count at +0x2C0; rva 0x364800).
// The move scripts set the override (rva 0x35EB40) and the level (+0x38,
// 0x35EB80) and can install a record (0x35EBF0). The game's regions cover
// only +0x2C..+0x33 and +0x44 of each: after a rollback the override and the
// type shown kept the newest tick's values, the resimulated tick took its
// pick as already shown and added it without turning the old one off (M.
// Bison vs Ken ed359e49 and five more 0.3.x matches: a count of 2 against 1),
// and a warm-up that set them left both games apart from frame 0 (Rose vs G
// 3936e71b, e5cc4189). Added: +0x28..+0x43 of each record.
constexpr size_t kEffectSwitchesAt = 0x340, kEffectSwitchStride = 0x48, kEffectSwitchCount = 4;
constexpr size_t kEffectSwitchFieldsAt = 0x28, kEffectSwitchFieldsSize = 0x1C;
// The owner of a fighter's effect slots: the game snapshots the slots (392
// bytes) from +0x6D8 (the owner's vtable rva 0x2C53598 sits at +0xB0). +0x108
// is set each tick from the fighter (rva 0x3A00C5; kept while 0x377710 holds)
// and no region covers it. Poison's unique part (Z33, CharaUniquePart_035)
// draws slot 0 or 1 from it (rva 0x3BDA38), so after a rollback it held the
// newest tick's value and the slot states came out differently (Urien vs
// Poison d704c27a; Seth vs Poison x5 on 0.2.3, before this). Added.
constexpr uint32_t kEffectOwnerVtable = 0x2C53598;
constexpr size_t kEffectSlotsSize = 392, kEffectSlotsAt = 0x6D8, kEffectOwnerVtableAt = 0xB0;
constexpr size_t kEffectOwnerValueAt = 0x108, kEffectOwnerValueSize = 4;
// The start probe (SfvGame::ArmStartProbe): the battle camera that pans from
// frame 0 (its sub-object, vtable rva 0x2C5F520, at +0xE8; the game snapshots
// 112 bytes from +0x1C0; its tick is rva 0x3E4AF0) and each fighter's
// simulation object (300 bytes snapshot from +0x764, its position at +0x790).
constexpr uint32_t kBattleCameraVtable = 0x2C5F520;
constexpr size_t kBattleCameraRegionSize = 112, kBattleCameraRegionAt = 0x1C0, kBattleCameraSize = 0x400;
constexpr size_t kFighterSimRegionSize = 300, kFighterSimRegionAt = 0x764, kFighterSimSize = 0x1000;

bool SfvGame::RefreshRegions() {
    // The game's table, in its order, plus our extra regions; only rebuilt
    // when that changes (a new battle).
    std::vector<std::pair<uint8_t*, size_t>> raw;
    if (uint8_t* m = Manager()) {
        auto* begin = Read<mgr::Region*>(m, mgr::kRegionsBegin);
        auto* end = Read<mgr::Region*>(m, mgr::kRegionsEnd);
        const size_t count = begin && end > begin ? static_cast<size_t>(end - begin) : 0;
        if (count <= kMaxRegions && IsReadable(begin, count * sizeof(mgr::Region))) {
            for (size_t i = 0; i < count; i++) raw.emplace_back(begin[i].ptr, static_cast<size_t>(begin[i].size));
        }
    }
    const size_t gameRegions = raw.size();
    const bool extras = includeSyncCounters && !raw.empty();
    if (extras && inputUnit_) raw.emplace_back(inputUnit_ + game::inputunit::kCountersBegin, game::inputunit::kCountersSize);
    if (extras && battleFrame_) raw.emplace_back(battleFrame_ + game::battleframe::kStateBegin, game::battleframe::kStateSize);
    uint8_t* singletonTicks = extras ? SingletonTicks() : nullptr;
    if (singletonTicks) raw.emplace_back(singletonTicks, game::singleton::kTicksSize);
    std::vector<const ExtraRegion*> presentation;
    for (const auto& e : presentation_) {
        if (extras && IsReadable(e.ptr, e.size)) presentation.push_back(&e);
    }
    for (const auto* e : presentation) raw.emplace_back(e->ptr, e->size);
    // The round's flow phase and a copy of the round director (flow.h). Not
    // the director itself: it is only known once its task runs, and a region
    // appearing mid-session would change the state size under the session.
    std::vector<ExtraRegion> flow;
    if (extras) {
        flow.push_back({"flow.round", FlowRoundFlowState(), kRoundFlowStateSize});
        // Rose's soul entity: the game's region is its +0xF0..+0x463; the
        // countdown that applies a new soul level (+0x61C, rva 0x3C14F9 and
        // 0x3C1990), the level table and its counters lie after it. A
        // rollback left them run on, and the level came a frame early in
        // the resimulation (both PCs desynced in every Rose match).
        for (size_t i = 0; i < gameRegions; i++) {
            if (raw[i].second != kSoulEntityRegionSize) continue;
            uint8_t* entity = raw[i].first - kSoulEntityRegionAt;
            if (IsReadable(entity, 8) && *reinterpret_cast<const uintptr_t*>(entity) == image_.base + kSoulEntityVtable &&
                IsReadable(entity + kSoulEntityTailAt, kSoulEntityTailSize)) {
                flow.push_back({"rose.soul", entity + kSoulEntityTailAt, kSoulEntityTailSize});
            }
        }
        for (size_t i = 0; i < gameRegions; i++) {
            if (raw[i].second != kCameraRegionSize) continue;
            uint8_t* camera = raw[i].first - kCameraRegionAt;
            if (IsBattleCamera(camera, image_.base)) {
                flow.push_back({"camera.shake", camera + kCameraShakeAt, kCameraShakeSize});
            }
        }
    }
    for (const auto& e : flow) raw.emplace_back(e.ptr, e.size);
    if (raw == rawLayout_) return false;
    rawLayout_ = raw;
    layoutVersion_++;

    // The registration order differs between battles (and so between peers),
    // so regions get a canonical order: by size, then by contents with
    // pointer-like words ignored. Names follow that order. Pooled effects
    // (out of the checksum, IsEffectRegion) come first of their size whatever
    // they hold: the peers can hold the same effects in other pool objects,
    // and the regions after them then get the same names on both.
    struct Item {
        StateRegion region;
        uint64_t key;
    };

    std::vector<Item> items;
    const size_t gameCount = raw.size() - (extras && inputUnit_ ? 1 : 0) - (extras && battleFrame_ ? 1 : 0) -
                             (singletonTicks ? 1 : 0) - presentation.size() - flow.size();
    for (size_t i = 0; i < gameCount; i++) {
        uint8_t* p = raw[i].first;
        const size_t size = raw[i].second;
        if (IsEffectRegion({"", p, size, {}})) {
            items.push_back({{"", p, size, {}}, 0});
            continue;
        }
        std::vector<uint8_t> copy(p, p + size);
        for (size_t w = 0; w + 8 <= size; w += 8) {
            uint64_t v;
            std::memcpy(&v, &copy[w], 8);
            if (LooksLikeAddress(v)) std::memset(&copy[w], 0, 8);
        }
        items.push_back({{"", p, size, {}}, Hash64(copy.data(), copy.size())});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.region.size != b.region.size ? a.region.size < b.region.size : a.key < b.key;
    });

    std::vector<StateRegion> fresh;
    size_t lastSize = 0;
    int ordinal = 0;
    for (auto& it : items) {
        ordinal = it.region.size == lastSize ? ordinal + 1 : 0;
        lastSize = it.region.size;
        char name[32];
        std::snprintf(name, sizeof(name), "s%zu.%d", it.region.size, ordinal);
        it.region.name = name;
        fresh.push_back(std::move(it.region));
    }
    if (extras && inputUnit_) {
        fresh.push_back({"inputunit.counters", inputUnit_ + game::inputunit::kCountersBegin, game::inputunit::kCountersSize, {}});
        // Rolled back with the counters, the capture runs again on the
        // corrected inputs (Urien's thong, sfv_offsets.h kHeldBegin).
        fresh.push_back({"inputunit.held", inputUnit_ + game::inputunit::kHeldBegin, game::inputunit::kHeldSize, {}});
    }
    if (extras && battleFrame_) {
        fresh.push_back({"battleframe.state", battleFrame_ + game::battleframe::kStateBegin, game::battleframe::kStateSize, {}});
    }
    if (singletonTicks) fresh.push_back({"singleton.ticks", singletonTicks, game::singleton::kTicksSize, {}});
    for (const auto* e : presentation) fresh.push_back({e->name, e->ptr, e->size, {}});
    for (const auto& e : flow) fresh.push_back({e.name, e.ptr, e.size, {}});
    // Each fighter's event flag and its effect slot owner's value, named after
    // the canonical order of the regions they sit next to (the game registers
    // the fighters in a different order per peer).
    if (extras) {
        auto owned = [&](uint8_t* object, size_t vtableAt, uint32_t vtable, size_t at, size_t size) {
            return IsReadable(object + vtableAt, 8) &&
                   *reinterpret_cast<const uintptr_t*>(object + vtableAt) == image_.base + vtable &&
                   IsReadable(object + at, size);
        };
        // Gathered apart and appended after: adding to `fresh` while reading
        // it moved the regions being read (0.4.3: std::out_of_range in the
        // warm-up, the game ended itself, three crash reports).
        std::vector<StateRegion> named;
        for (const StateRegion& r : fresh) {
            if (r.size == kVitalityRegionSize && r.name.rfind("s372.", 0) == 0) {
                uint8_t* fighter = r.ptr - kVitalityRegionAt;
                if (owned(fighter, kVitalityOwnerAt, kVitalityOwnerVtable, kFighterEventAt, kFighterEventSize)) {
                    named.push_back({"fighter.event." + r.name.substr(5), fighter + kFighterEventAt, kFighterEventSize, {}});
                }
                for (size_t s = 0; s < kEffectSwitchCount; s++) {
                    const size_t at = kEffectSwitchesAt + s * kEffectSwitchStride + kEffectSwitchFieldsAt;
                    if (!owned(fighter, kVitalityOwnerAt, kVitalityOwnerVtable, at, kEffectSwitchFieldsSize)) continue;
                    named.push_back({"fighter.switch" + std::to_string(s) + "." + r.name.substr(5), fighter + at, kEffectSwitchFieldsSize, {}});
                }
            }
            if (r.size == kEffectSlotsSize && r.name.rfind("s392.", 0) == 0) {
                uint8_t* owner = r.ptr - kEffectSlotsAt;
                if (owned(owner, kEffectOwnerVtableAt, kEffectOwnerVtable, kEffectOwnerValueAt, kEffectOwnerValueSize)) {
                    named.push_back({"effects.owner." + r.name.substr(5), owner + kEffectOwnerValueAt, kEffectOwnerValueSize, {}});
                }
            }
        }
        fresh.insert(fresh.end(), named.begin(), named.end());
    }

    regions_ = std::move(fresh);
    CameraJournalReset();
    ArmPendingRegionWatch(regions_, image_.base);
    SetStateMap(stateMap_);
    return true;
}

// Visual effects the game keeps in pools: motion trails, effect objects and
// an effect pool. Which pool object an effect gets is decided outside the
// snapshot (the trail manager's pending requests, rva 0x484C70), so after a
// rollback across a request the same effect can land in another object, and
// the objects it did not take keep a finished effect's leftovers: the two
// games then hold the same effects in different objects (R. Mika's trail,
// netplay-testing.md finding 33). Nothing in a fight reads them back. The
// script's effect records follow: a reused one goes on counting from an old
// effect, and an effect's record can stay live in one game and be reused in
// the other. A divergence in the fight still shows in the fighters'
// regions. Each region of such an object is known by its size and where its
// owner's vtable sits before it.
// Vega's claw and mask once knocked off are drawn the same way: they fly from
// his hand or head, which his mesh's pose gives (rva 0x3A4B74, outside the
// snapshot, posed once per drawn frame, so a resimulated knock-off launches
// from a later frame's pose), land, and fade; nothing in the fight reads them
// (tester matches 2026-11-02: the two games' items parted for thousands of
// frames, the fighters never).
struct EffectRegionKind {
    size_t size;
    size_t ownerBefore; // the owner's vtable pointer, this many bytes before the region
    uint32_t vtable;    // rva
};

constexpr EffectRegionKind kEffectRegions[] = {
    {44, 0x10, 0x2C6FC18},    // trail object (vtable 0x2C6FB88) +0xC0: timer, state
    {136, 0x40, 0x2C6FC18},   // trail object +0xF0: transforms
    {40, 0x10, 0x2C6EAD0},    // effect object: flags (a slot's live flag, Menat vs Lucia 8882ac21)
    {68, 0x90, 0x2C6EAD0},    // effect object
    {208, 0xE8, 0x2C6EAD0},   // effect object: transforms
    {164, 0x1C8, 0x2C6EAD0},  // effect object: handles
    {15808, 0x18, 0x2C6E6D8}, // effect pool
    {224, 0x30, 0x3231618},   // script effect record
    {208, 0x40, 0x2C54490},   // Vega's CharaUniquePart_021 (vtable 0x2C541A0; 0x2C54490 at its +0xB0) +0xF0:
                              // two 0x60-byte entries, his knocked-off claw and mask (state, launch point,
                              // target, time), then +0x1A0..+0x1AF, an address on some machines
};

bool SfvGame::IsEffectRegion(const StateRegion& r) const {
    for (const EffectRegionKind& k : kEffectRegions) {
        if (r.size != k.size) continue;
        const uint8_t* at = r.ptr - k.ownerBefore;
        if (IsReadable(at, 8) && *reinterpret_cast<const uintptr_t*>(at) == image_.base + k.vtable) return true;
    }
    return false;
}

void SfvGame::SetStateMap(const StateMap* map) {
    stateMap_ = map;
    const bool fits = map && map->regions.size() == regions_.size() &&
                      std::equal(regions_.begin(), regions_.end(), map->regions.begin(),
                                 [](const StateRegion& r, const StateMap::Region& m) { return r.size == m.size; });
    if (map && !regions_.empty() && !fits) {
        Log("state map does not fit this battle's %zu regions; hashes include pointers (expect false desyncs)",
            regions_.size());
    }
    for (size_t i = 0; i < regions_.size(); i++) {
        StateRegion& r = regions_[i];
        r.ignore = fits ? map->regions[i].ignore : std::vector<uint8_t>{};
        if (r.name == "inputunit.counters") {
            // +0x217c tags the InputUnit's input ring with a reset counter
            // that differs between machines; the logic only compares it with
            // itself. The word's other half (+0x2178) is forced (Advance).
            r.ignore.resize(1, 0);
            r.ignore[0] |= uint8_t(1u << ((game::inputunit::kRingTag - game::inputunit::kCountersBegin) / 8));
        }
        if (r.name == "viewtarget.clock" || r.name == "pawn.clock") {
            // The camera's and the pawns' animation clocks follow how ticks
            // fall into rendered frames, different on each machine (and more
            // so with time sync stretching frames): in intros, round calls and
            // win poses they part by a few microseconds while the whole rest
            // of the state agrees (2026-10-29: 400 false desyncs a match).
            // Restored on a rollback like the rest, but out of the checksum.
            r.ignore.resize(1, 0);
            r.ignore[0] |= 1;
        }
        if (r.name == "camera.shake" || (r.size == kCameraRegionSize && IsBattleCamera(r.ptr - kCameraRegionAt, image_.base))) {
            // The screen shake (camera +0x2E0..+0x31F: amplitude, offset,
            // frame counter and length, flip period and count). Stages start
            // shakes too: an animation notify on an SX1 prop runs its
            // Blueprint's ShakeCamera (rva 0x531870), on the prop's world-time
            // animation, so on another frame on each PC whenever a frame
            // hitches (2026-11-04: 5 desync events in the round call). Only
            // the drawn view reads the shake. Restored, but out of the checksum.
            if (r.name == "camera.shake") {
                r.ignore.assign((r.size / 8 + 8) / 8, 0xFF);
            } else {
                r.ignore.resize((r.size / 8 + 8) / 8, 0);
                for (size_t at = kCameraShakeFieldsAt; at < kCameraShakeAt; at += 8) {
                    const size_t word = (at - kCameraRegionAt) / 8;
                    r.ignore[word / 8] |= uint8_t(1u << (word % 8));
                }
            }
        }
        if (r.name == "rose.soul") {
            // The entity's +0x620..+0x62F (this region's +0x58, +0x60): left
            // from the last match (floats, at times an address), constant
            // through the next and different on each machine; none of the
            // soul code writes them. Restored, but out of the checksum.
            if (r.ignore.size() < 2) r.ignore.resize(2, 0);
            r.ignore[1] |= uint8_t(1u << (0x58 / 8 - 8)) | uint8_t(1u << (0x60 / 8 - 8));
        }
        if (IsEffectRegion(r)) {
            // Restored on a rollback like the rest, but out of the checksum.
            r.ignore.assign((r.size / 8 + 8) / 8, 0xFF);
            continue;
        }
        auto it = noise_.find(r.size);
        if (it == noise_.end()) continue;
        r.ignore.resize((r.size / 8 + 8) / 8, 0);
        for (size_t offset : it->second) {
            const size_t word = offset / 8;
            if (word / 8 < r.ignore.size()) r.ignore[word / 8] |= uint8_t(1u << (word % 8));
        }
    }
}

bool SfvGame::LoadNoise(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    LoadNoiseText(text.data(), text.size());
    return true;
}

void SfvGame::LoadNoiseText(const char* text, size_t len) {
    noise_.clear();
    const char* end = text + len;
    for (const char* p = text; p < end;) {
        const char* eol = std::find(p, end, '\n');
        const std::string line(p, eol);
        p = eol < end ? eol + 1 : end;
        unsigned long long size = 0, offset = 0;
        if (line.empty() || line[0] == '#') continue;
        if (std::sscanf(line.c_str(), "%llu %llx", &size, &offset) == 2) noise_[size_t(size)].insert(size_t(offset) & ~size_t(7));
    }
    SetStateMap(stateMap_);
}

uint64_t SfvGame::NoiseHash() const {
    uint64_t noise = 0;
    for (const auto& [size, offsets] : noise_) {
        for (size_t o : offsets) {
            const uint64_t pair[2] = {size, o};
            noise = Hash64(pair, sizeof(pair), noise);
        }
    }
    return noise;
}

bool SfvGame::SaveNoise(const std::wstring& path) const {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return false;
    std::fprintf(f, "# Words that differ when a frame is resimulated (values derived from engine-side\n"
                    "# transforms). Left out of desync checksums.\n"
                    "# <region size> <word offset, hex>\n");
    for (const auto& [size, offsets] : noise_) {
        for (size_t o : offsets) std::fprintf(f, "%zu 0x%zx\n", size, o);
    }
    return std::fclose(f) == 0;
}

bool SfvGame::AddNoise(size_t regionSize, size_t offset) {
    if (!noise_[regionSize].insert(offset & ~size_t(7)).second) return false;
    SetStateMap(stateMap_);
    return true;
}

void SfvGame::LearnPointers(StateMap& map) const {
    if (map.regions.size() != regions_.size()) {
        map.regions.assign(regions_.size(), {});
        for (size_t i = 0; i < regions_.size(); i++) map.regions[i].size = regions_[i].size;
    }
    const auto ranges = CommittedRanges();
    for (size_t i = 0; i < regions_.size(); i++) {
        const StateRegion& r = regions_[i];
        auto& ignore = map.regions[i].ignore;
        ignore.resize((r.size / 8 + 7) / 8, 0);
        for (size_t w = 0; w + 1 <= r.size / 8; w++) {
            uint64_t v;
            std::memcpy(&v, r.ptr + w * 8, 8);
            if (v >= 0x10000 && v < 0x800000000000ull && InRanges(ranges, v)) ignore[w / 8] |= uint8_t(1u << (w % 8));
        }
    }
}

SfvGame::Inventory SfvGame::Inspect() const {
    Inventory inv;
    uint8_t* m = Manager();
    if (!m) return inv;
    inv.regionCount = regions_.size();
    inv.stateBytes = StateSize(regions_);
    auto* groups = Read<mgr::Group*>(m, mgr::kGroupsBegin);
    auto* groupsEnd = Read<mgr::Group*>(m, mgr::kGroupsEnd);
    inv.groupCount = groups && groupsEnd > groups ? static_cast<size_t>(groupsEnd - groups) : 0;
    inv.ringCount = Read<uint64_t>(m, mgr::kRingCount);
    inv.ringMapSize = Read<uint64_t>(m, mgr::kRingMapSize);
    inv.currentId = Read<game::FrameId>(m, mgr::kCurrentId);

    const uint8_t* sentinel = Read<uint8_t*>(m, mgr::kPostRestoreList);
    if (IsReadable(sentinel, 8)) {
        for (const uint8_t* node = Read<uint8_t*>(sentinel, mgr::callback::kNext);
             node && node != sentinel && inv.postRestoreCallbacks < 1000 && IsReadable(node, 0x38);
             node = Read<uint8_t*>(node, mgr::callback::kNext)) {
            inv.postRestoreCallbacks++;
        }
    }
    return inv;
}

void SfvGame::LogLayout() const {
    uint8_t* m = Manager();
    if (!m) {
        Log("layout: no rollback manager");
        return;
    }
    const Inventory inv = Inspect();
    Log("layout: manager %p, %zu regions, %llu bytes, %zu groups, ring %llu frames (map %llu), current id %d:%d, "
        "%zu post-restore callbacks",
        m, inv.regionCount, static_cast<unsigned long long>(inv.stateBytes), inv.groupCount,
        static_cast<unsigned long long>(inv.ringCount), static_cast<unsigned long long>(inv.ringMapSize),
        inv.currentId.lo, inv.currentId.hi, inv.postRestoreCallbacks);

    auto* groups = Read<mgr::Group*>(m, mgr::kGroupsBegin);
    for (size_t g = 0; g < inv.groupCount; g++) {
        Log("  group %zu: regions %llu..%llu -> %p", g, static_cast<unsigned long long>(groups[g].first),
            static_cast<unsigned long long>(groups[g].last), groups[g].dst);
    }

    const auto ranges = CommittedRanges();
    for (size_t i = 0; i < regions_.size(); i++) {
        const StateRegion& r = regions_[i];
        size_t pointers = 0;
        for (size_t w = 0; w + 1 <= r.size / 8; w++) {
            uint64_t v;
            std::memcpy(&v, r.ptr + w * 8, 8);
            if (v >= 0x10000 && v < 0x800000000000ull && InRanges(ranges, v)) pointers++;
        }
        const uintptr_t p = reinterpret_cast<uintptr_t>(r.ptr);
        const bool inImage = p >= image_.base && p < image_.base + 0x5000000;
        Log("  region %-10s %p %8zu bytes  %s  %zu pointer-like words%s", r.name.c_str(), r.ptr, r.size,
            inImage ? "image" : "heap ", pointers, (p & 7) ? "  (unaligned)" : "");
    }

    const uint8_t* sentinel = Read<uint8_t*>(m, mgr::kPostRestoreList);
    if (IsReadable(sentinel, 8)) {
        int n = 0;
        for (const uint8_t* node = Read<uint8_t*>(sentinel, mgr::callback::kNext);
             node && node != sentinel && n < 64 && IsReadable(node, 0x38);
             node = Read<uint8_t*>(node, mgr::callback::kNext), n++) {
            const auto fn = Read<uintptr_t>(node, mgr::callback::kFunction);
            // Both thunks seen in practice call ctx->target(object): log the target.
            const auto* ctx = Read<const uint8_t*>(node, mgr::callback::kContext);
            const uintptr_t target = IsReadable(ctx, 8) ? Read<uintptr_t>(ctx, 0) : 0;
            Log("  post-restore callback %d: thunk rva 0x%llX target rva 0x%llX", n,
                static_cast<unsigned long long>(fn - image_.base),
                static_cast<unsigned long long>(target ? target - image_.base : 0));
        }
    }
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

std::vector<uint64_t> DefaultSkippedLoadCallbacks() {
    // thunk rva << 32 | target rva. About 90% of the time the callbacks take
    // after a load (0.9-1.2 ms a load on the test PC): the fighters'
    // animation re-sync (0x3587D0) and two at 0x448B30 / 0x448B40. The ticks
    // resimulated after the load set the same state: two-PC sessions and
    // stress replays with them skipped had no desync (2026-10-30).
    return {0x4C1D10ull << 32 | 0x3587D0, 0x447A60ull << 32 | 0x448B30, 0x447A60ull << 32 | 0x448B40};
}

void SetSkippedLoadCallbacks(std::vector<uint64_t> keys) {
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
    // Mirrors the loop at 0x48A45C in Restore.
    uint8_t* m = Manager();
    if (!m) return;
    const uint8_t* sentinel = Read<uint8_t*>(m, mgr::kPostRestoreList);
    if (!IsReadable(sentinel, 8)) return;
    using CallbackFn = void (*)(void* ctx, const game::FrameId*);
    for (const uint8_t* node = Read<uint8_t*>(sentinel, mgr::callback::kNext); node && node != sentinel;
         node = Read<uint8_t*>(node, mgr::callback::kNext)) {
        auto fn = Read<CallbackFn>(node, mgr::callback::kFunction);
        if (!fn || Read<uint64_t>(node, mgr::callback::kToken) == 0) continue;
        // Timed per function (LoadCosts): where the time after a load goes,
        // by the thunk and the function its context names (its first word,
        // an rva in the image; 0 when not): 50 callbacks share two thunks.
        // The game's thunks (rva 0x4C1D10, 0x447A60) read the context to make
        // the call, so it is readable whenever the call is made: no
        // VirtualQuery per callback (about 50 a load, ~1 us each).
        void* ctx = Read<void*>(node, mgr::callback::kContext);
        uintptr_t target = ctx ? *static_cast<const uintptr_t*>(ctx) : 0;
        target = target > image_.base && target < image_.base + 0x8000000 ? target - image_.base : 0;
        const uint64_t key = ((reinterpret_cast<uintptr_t>(fn) - image_.base) << 32) | target;
        // Its reset of the camera views, replaced (camera_rollback.h).
        if (target == kCameraResetCallback && CameraRollbackOn()) continue;
        if (g_skipAny) {
            std::lock_guard lock(g_skipMutex);
            if (std::find(g_skipCallbacks.begin(), g_skipCallbacks.end(), key) != g_skipCallbacks.end()) continue;
        }
        // Two callbacks read the restored frame from id +4 and compare it
        // with frames the game stamped from InputUnit +0x2180: the sound
        // manager's (rva 0x4B6230: sounds played after it are replay
        // candidates, flow_tasks.cpp kSoundReconcileTask) and 0x4D5820 (flags the
        // items of its objects that started after it). They get the game's
        // own id, as every callback does (soundrollback 0 / frameids 0: the
        // mod's old id, for comparison).
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
    // The battle itself is set up and entered beforehand (battle_setup.h); a
    // session starts once that battle runs and its objects have been seen.
    (void)setup;
    RefreshRegions();
    return CanRunTicks() && !regions_.empty();
}

PackedInput SfvGame::ReadLocalInput(int player) {
    if (inputOverride) return inputOverride(player);
    // What the game computed from the physical pad on the last tick. Netplay
    // plays the local player with the input manager's pad 0 (keyboard and
    // pads, with the player's button config) on both machines.
    if (localInput && !bothPadsLocal) return localInput(localPadSlot);
    return sampled_[bothPadsLocal ? std::clamp(player, 0, 3) : localPadSlot];
}

void SfvGame::Advance(const FrameInputs& inputs, bool rollingBack) {
    if (!CanRunTicks()) return;
    hitch::AddTick(rollingBack); // the frame-drop log: ticks per frame, and how many replayed
    BattleSetReplayingTick(rollingBack);
    // Start reaches the game only when both players pressed it outside the
    // fight (flow.h): one player cannot skip the intro, or pause the fight.
    const PackedInput start = FlowPresentationStart(inputs.p[0], inputs.p[1]);
    inject_[0] = (inputs.p[0] & ~kStart) | start;
    inject_[1] = (inputs.p[1] & ~kStart) | start;
    // Set at round start from a machine-specific global (rva 0x438EBD): it
    // differs between peers (0 on one, 1 on the other) and the battle
    // diverges when the fight starts. Same value everywhere.
    if (inputUnit_) *reinterpret_cast<int32_t*>(inputUnit_ + game::inputunit::kRoundParam) = 0;
    injecting_ = true;
    FlowBeginDrivenTick();
    BattleSeedTickRandom();
    CameraTickBegin();
    RunBattleTick(rollingBack);
    CameraTickEnd();
    injecting_ = false;
    // The round director's check for this tick, and its state into the snapshot.
    FlowRunRoundDirector();
    LatchInputs();
    if (probeTicks_ > 0 && !rollingBack) {
        CaptureStartProbe(probeTicks_ == 2 ? "after the first tick" : "after the second tick");
        probeTicks_--;
    }
    CheckChangeWatch(regions_, image_.base);
    // Between ticks the presentation (intro and round call skipping, once
    // per rendered frame) reads the current input values. Rendered frames and
    // ticks line up differently on each machine, so it must see nothing: a
    // skip pressed during an intro would otherwise start the fight on a
    // different tick per machine.
    if (inputUnit_ && exclusiveTicks) {
        auto* values = reinterpret_cast<uint32_t*>(inputUnit_ + game::inputunit::kValues);
        for (int i = 0; i < 4; i++) values[i] = 0;
    }
}

namespace {
std::atomic<bool> g_leanResim{true};
} // namespace

void SetLeanResim(bool on) { g_leanResim = on; }

bool LeanResimOn() { return g_leanResim; }

void SfvGame::RunBattleTick(bool replayed) {
    namespace bf = game::battleframe;
    uint8_t* frame = battleFrame_;
    if (!replayed || !g_leanResim) {
        // One tick: BattleFrame::Update runs 1 + [this+0x1A8] ticks.
        *reinterpret_cast<int32_t*>(frame + bf::kExtraTicks) = 0;
        runFrame_(frame);
        return;
    }
    // A replayed tick: BattleFrame::Update's parts without scheduler phase 8,
    // and phase 4 without the tasks flow_tasks.cpp leaves out of replayed ticks
    // (kLeanResimTasks); the newest tick of the frame is a whole update and
    // runs them. Measured 2026-11-08 (stress replays): what can be left out
    // is presentation the newest tick makes up for, and leaving it out
    // keeps every frame's state equal to a straight run's. Phase 1 (the round
    // state's clocks, rva 0x4BB540), phase 2 (the battle) and the fighters'
    // phase-4 tasks (their box ring: rva 0x386950; and 0x37D1C0, which
    // costs more once after many ticks than once per tick) run every tick.
    // The update's own counters (battleframe.state, in the snapshot) move
    // as a whole update moves them.
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
    // The battle's round state is the one 228-byte region: +0xb4 the round,
    // +0xc4 / +0xc8 the matches each side won since character select (one
    // more as the match is decided; found from recordings, 2026-10-28).
    constexpr size_t kRoundStateSize = 228;
    for (const StateRegion& r : regions_) {
        if (r.size != kRoundStateSize || !IsReadable(r.ptr, r.size)) continue;
        std::memcpy(&left, r.ptr + 0xC4, 4);
        std::memcpy(&right, r.ptr + 0xC8, 4);
        return true;
    }
    return false;
}

namespace {
std::atomic<int> g_warmupSleepMs{0};
std::atomic<int> g_warmupExtra{0};
} // namespace

void SetWarmupSleep(int ms) { g_warmupSleepMs = std::max(0, ms); }

void SetGameFrameIds(bool on) { g_gameFrameIds = on; }

void SetWarmupExtra(int ticks) { g_warmupExtra = std::max(0, ticks); }

std::atomic<bool> g_watchCamera{false};

void WatchCameraAtFreeze() { g_watchCamera = true; }

// The battle camera's constructor: (this, the object initializer, ...).
constexpr uint32_t kBattleCameraCtor = 0x3C72A0;
using CameraCtorFn = void* (*)(void*, void*, void*, void*);
CameraCtorFn g_origCameraCtor = nullptr;
std::atomic<bool> g_watchNewCamera{false};

void* CameraCtorDetour(void* self, void* a, void* b, void* c) {
    void* r = g_origCameraCtor(self, a, b, c);
    if (g_watchNewCamera.exchange(false)) {
        auto* cam = static_cast<uint8_t*>(self);
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        Log("watch: battle camera constructed at %p (thread %lu); watching +0x90, +0xE0, +0x1D0, +0x1F0", self,
            GetCurrentThreadId());
        // Armed from another thread (a thread cannot set its own debug
        // registers while it runs); this one waits so nothing is missed.
        std::thread arm([cam, base] {
            ThreadGuard running;
            ArmWatch(0, cam + 0x90, 8, base);
            ArmWatch(1, cam + 0xE0, 8, base);
            ArmWatch(2, cam + 0x1D0, 8, base);
            ArmWatch(3, cam + 0x1F0, 8, base);
        });
        arm.join();
    }
    return r;
}

void WatchNextCamera() {
    if (!g_origCameraCtor) {
        void* target = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + kBattleCameraCtor;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&CameraCtorDetour), reinterpret_cast<void**>(&g_origCameraCtor)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            Log("watch: could not hook the battle camera's constructor");
            g_origCameraCtor = nullptr;
            return;
        }
    }
    g_watchNewCamera = true;
}

const uint8_t* SfvGame::FindBattleCamera() const {
    for (const StateRegion& r : regions_) {
        if (r.size == kBattleCameraRegionSize && IsReadable(r.ptr - (kBattleCameraRegionAt - 0xE8), 8) &&
            *reinterpret_cast<const uintptr_t*>(r.ptr - (kBattleCameraRegionAt - 0xE8)) == image_.base + kBattleCameraVtable) {
            const uint8_t* camera = r.ptr - kBattleCameraRegionAt;
            if (IsReadable(camera, kBattleCameraSize)) return camera;
        }
    }
    return nullptr;
}

std::string SfvGame::CameraSetupText() const {
    const uint8_t* cam = FindBattleCamera();
    if (!cam) return "no battle camera";
    uint32_t flags, set = 0;
    float e4, x;
    std::memcpy(&flags, cam + 0x94, 4);
    std::memcpy(&e4, cam + 0xE4, 4);
    std::memcpy(&x, cam + kBattleCameraRegionAt, 4);
    for (size_t at = kBattleCameraRegionAt + 0x28; at < kBattleCameraRegionAt + 0x60; at += 4) {
        uint32_t v;
        std::memcpy(&v, cam + at, 4);
        set += v != 0;
    }
    char text[160];
    std::snprintf(text, sizeof(text), "battle camera: +0x94 %08x, +0xE4 %.3f, %u of 14 setup words set, x %.4f", flags, e4, set, x);
    return text;
}

bool SfvGame::CameraNotStarted() const {
    const uint8_t* cam = FindBattleCamera();
    if (!cam) return false;
    uint32_t flags;
    std::memcpy(&flags, cam + 0x94, 4);
    for (size_t at = kBattleCameraRegionAt + 0x28; at < kBattleCameraRegionAt + 0x60; at += 4) {
        uint32_t v;
        std::memcpy(&v, cam + at, 4);
        if (v) return false;
    }
    return flags == 0x100;
}

void SfvGame::WatchCameraIfAsked() {
    if (!g_watchCamera.exchange(false)) return;
    const uint8_t* cam = FindBattleCamera();
    if (!cam) {
        Log("watch: no battle camera to watch");
        return;
    }
    const uintptr_t base = image_.base;
    Log("watch: battle camera at %p (%s); watching +0x90 and +0xE0", static_cast<const void*>(cam), CameraSetupText().c_str());
    // From another thread: arming sets every thread's debug registers, and
    // this (the game) thread cannot set its own while it runs.
    std::thread([cam, base] {
        ThreadGuard running;
        ArmWatch(0, cam + 0x90, 8, base);
        ArmWatch(1, cam + 0xE0, 8, base);
    }).detach();
}

bool SfvGame::CameraSetUp() const {
    const uint8_t* cam = FindBattleCamera();
    if (!cam) return false;
    uint32_t flags;
    std::memcpy(&flags, cam + 0x94, 4);
    return (flags & 0x80000000u) != 0;
}

bool SfvGame::WarmupStep() {
    RefreshRegions();
    if (Regions().empty() || !InputUnit()) {
        if (const int ms = g_warmupSleepMs; ms > 0) Sleep(ms);
        Advance(FrameInputs{}, false);
        warmupTicks_++;
        return true;
    }
    // The battle's state can still be registering as its first regions show
    // up: a host whose objects came after 3 ticks had 369 regions, its joiner
    // 549 (90 of a pool of 128 objects, vtable rva 0x2C4E598, one 4-byte and
    // one 8-byte region each, not there yet), and nine matches between them
    // parted from frame 0 (0.4.2, 0826be3b and eight more, 2026-11-13). So the
    // freeze waits until the count has held for kSettleSteps calls, without
    // ticking (more ticks would move the battle: warmupextra), at most
    // kSettleMax calls (whatever registers only in ticks never comes).
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
    // Test only (warmupextra): more ticks once the objects exist. They move
    // the fighters, the round's flow and the camera (40 regions differ after
    // 2), and a session started so desyncs from frame 0 even after the
    // reconcile (2026-11-05). (43c9c9cb, the 3-tick start, was a battle
    // started without its stage loaded first: flow_jump.cpp.)
    const int after = warmupTicks_ - objectsAt_;
    if (after < g_warmupExtra) {
        Advance(FrameInputs{}, false);
        warmupTicks_++;
        cameraWait_++;
        return true;
    }
    return false;
}

// What the next tick reads as the pads' values for its frame (the intro's
// Start check), latched from the input ring as the game does once per
// rendered frame after the battle's update: after every driven tick, and
// after a load, so a replayed tick sees what it saw the first time.
void SfvGame::LatchInputs() {
    if (!inputUnit_ || !latchEachTick) return;
    using LatchFn = void (*)(uint8_t*);
    image_.At<LatchFn>(game::kInputUnitLatch.rva)(inputUnit_);
}

void SfvGame::ArmStartProbe() {
    {
        std::lock_guard lock(probeMutex_);
        probe_.clear();
        probeLogged_ = false;
    }
    CaptureStartProbe("at the freeze");
    probeTicks_ = 2;
}

void SfvGame::CaptureStartProbe(const char* when) {
    ProbeCapture capture{when, {}};
    for (const StateRegion& r : regions_) {
        if (r.size == kBattleCameraRegionSize && IsReadable(r.ptr - (kBattleCameraRegionAt - 0xE8), 8) &&
            *reinterpret_cast<const uintptr_t*>(r.ptr - (kBattleCameraRegionAt - 0xE8)) == image_.base + kBattleCameraVtable) {
            const uint8_t* camera = r.ptr - kBattleCameraRegionAt;
            if (IsReadable(camera, kBattleCameraSize)) capture.objects.push_back({"camera (" + r.name + ")", {camera, camera + kBattleCameraSize}});
        }
        if (r.size == kFighterSimRegionSize && r.name.rfind("s300.", 0) == 0) {
            const uint8_t* sim = r.ptr - kFighterSimRegionAt;
            if (IsReadable(sim, kFighterSimSize)) capture.objects.push_back({"fighter sim (" + r.name + ")", {sim, sim + kFighterSimSize}});
        }
    }
    std::lock_guard lock(probeMutex_);
    if (probe_.size() < 6) probe_.push_back(std::move(capture));
}

void SfvGame::LogStartProbe(const char* why) {
    std::lock_guard lock(probeMutex_);
    if (probeLogged_ || probe_.empty()) return;
    probeLogged_ = true;
    Log("start probe (%s; objects after %d neutral ticks): %zu captures, words that are not zero or an address",
        why, objectsAt_, probe_.size());
    for (const ProbeCapture& c : probe_) {
        for (const auto& [name, bytes] : c.objects) {
            std::string line;
            int words = 0;
            for (size_t at = 0; at + 8 <= bytes.size(); at += 8) {
                uint64_t q;
                std::memcpy(&q, &bytes[at], 8);
                if (q == 0 || LooksLikeAddress(q)) continue;
                for (size_t half = 0; half < 8; half += 4) {
                    uint32_t v;
                    std::memcpy(&v, &bytes[at + half], 4);
                    if (!v) continue;
                    char b[24];
                    std::snprintf(b, sizeof(b), " %zx=%x", at + half, v);
                    line += b;
                    if (++words % 48 == 0) {
                        Log("start probe %s, %s:%s", c.when.c_str(), name.c_str(), line.c_str());
                        line.clear();
                    }
                }
            }
            if (!line.empty() || words == 0) Log("start probe %s, %s:%s", c.when.c_str(), name.c_str(), line.c_str());
        }
    }
}

void SfvGame::OnStateLoaded(int frame) {
    // The callbacks receive the id of the restored frame: the game's own, from
    // the InputUnit's counters (restored with the snapshot), as its Restore
    // passes it; {frame, 0x5346} was the mod's until 2026-11-08.
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
    // Every 200 loads, the callbacks by cost (the game's own, and ours).
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
