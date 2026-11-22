// SfvGame's state regions: the game's and the ones added, their names and masks, the state map.

#include "mod/sfv_game_internal.h"

#include "mod/image.h"

namespace sfv::mod {

using namespace snapshot;

namespace {

constexpr size_t kMaxRegions = 100000;
constexpr uint64_t kLowestAddress = 0x100000000ull, kUserSpaceEnd = 0x800000000000ull, kFirstPage = 0x10000;
constexpr int kGranuleShift = 16;
constexpr ULONGLONG kFreeGranulesKeptMs = 250;
constexpr uint32_t kStateMapMagic = 0x4D564653, kStateMapVersion = 1; // "SFVM"

std::vector<std::pair<uintptr_t, uintptr_t>> CommittedRanges() {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    uintptr_t addr = kFirstPage;
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

std::mutex g_addressMutex;
std::vector<std::pair<uintptr_t, uintptr_t>> g_addressRanges;
std::unordered_set<uint64_t> g_freeGranules;
ULONGLONG g_freeGranulesSince = 0;

} // namespace

bool GameLooksLikeAddress(uint64_t v) {
    if (v < kLowestAddress || v >= kUserSpaceEnd) return false;
    std::lock_guard lock(g_addressMutex);
    if (InRanges(g_addressRanges, v)) return true;
    const ULONGLONG now = GetTickCount64();
    if (now - g_freeGranulesSince > kFreeGranulesKeptMs) {
        g_freeGranules.clear();
        g_freeGranulesSince = now;
    }
    if (g_freeGranules.count(v >> kGranuleShift)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(v), &mbi, sizeof(mbi)) || mbi.State == MEM_FREE) {
        g_freeGranules.insert(v >> kGranuleShift);
        return false;
    }
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

bool StateMap::Load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    bool ok = true;
    auto get = [&](void* p, size_t n) { ok = ok && std::fread(p, 1, n, f) == n; };

    uint32_t magic = 0, version = 0, count = 0;
    get(&magic, 4);
    get(&version, 4);
    get(&count, 4);
    ok = ok && magic == kStateMapMagic && version == kStateMapVersion && count < kMaxRegions;
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
    const uint32_t header[3] = {kStateMapMagic, kStateMapVersion, static_cast<uint32_t>(regions.size())};
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

namespace {

std::mutex g_regionWatchMutex;
std::string g_regionWatchName;
size_t g_regionWatchOffset = 0;
int g_regionWatchLength = 0;

} // namespace

void GameWatchRegionOfNextBattle(const std::string& name, size_t offset, int length) {
    std::lock_guard lock(g_regionWatchMutex);
    g_regionWatchName = name;
    g_regionWatchOffset = offset;
    g_regionWatchLength = length;
    Log("watch: armed for region %s +0x%zx (%d bytes) in the next region list", name.c_str(), offset, length);
}

bool IsPoolWatch() { return g_regionWatchName.size() > 2 && g_regionWatchName.compare(g_regionWatchName.size() - 2, 2, ".*") == 0; }

std::map<const uint8_t*, std::vector<uint8_t>> g_poolWatchSeen;

void GameCheckChangeWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase) {
    std::lock_guard lock(g_regionWatchMutex);
    if (g_regionWatchName.empty() || !IsPoolWatch()) return;
    const std::string prefix = g_regionWatchName.substr(0, g_regionWatchName.size() - 1);
    for (const StateRegion& r : regions) {
        if (r.name.compare(0, prefix.size(), prefix) != 0 || g_regionWatchOffset + g_regionWatchLength > r.size) continue;
        const uint8_t* p = r.ptr + g_regionWatchOffset;
        auto& seen = g_poolWatchSeen[r.ptr];
        const bool changed = !seen.empty() && std::memcmp(seen.data(), p, seen.size()) != 0;
        seen.assign(p, p + g_regionWatchLength);
        if (!changed) continue;
        const uint8_t* at = r.ptr + g_regionWatchOffset;
        const int length = g_regionWatchLength;
        std::thread([at, length, imageBase] {
            ThreadGuard running;
            int slot = 0;
            for (int done = 0; done < length && slot < 4; done += 8, slot++) {
                WatchArm(slot, at + done, std::min(8, length - done), imageBase);
            }
        }).detach();
        Log("watch: region %s at %p changed at +0x%zx; watching it", r.name.c_str(), static_cast<void*>(r.ptr),
            g_regionWatchOffset);
        g_regionWatchName.clear();
        g_poolWatchSeen.clear();
        return;
    }
}

void GameArmPendingRegionWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase) {
    std::lock_guard lock(g_regionWatchMutex);
    if (g_regionWatchName.empty() || IsPoolWatch()) return;
    for (const StateRegion& r : regions) {
        if (r.name != g_regionWatchName || g_regionWatchOffset >= r.size) continue;
        int slot = 0;
        for (int done = 0; done < g_regionWatchLength && slot < 4; done += 8, slot++) {
            WatchArm(slot, r.ptr + g_regionWatchOffset + done, std::min(8, g_regionWatchLength - done), imageBase);
        }
        Log("watch: region %s at %p, watching +0x%zx", r.name.c_str(), static_cast<void*>(r.ptr), g_regionWatchOffset);
        g_regionWatchName.clear();
        return;
    }
}

constexpr uint32_t kSoulEntityVtable = 0x2C5E2A8;
constexpr size_t kSoulEntityRegionSize = 884, kSoulEntityRegionAt = 0xF0;
constexpr size_t kSoulEntityTailAt = 0x5C8, kSoulEntityTailSize = 0xA8;
constexpr uint32_t kCameraPartVtable = 0x2C4F4A0;
constexpr size_t kCameraRegionSize = 528, kCameraRegionAt = 0x100, kCameraPartAt = 0xE8;
constexpr size_t kCameraShakeAt = 0x310, kCameraShakeSize = 0x10;
constexpr size_t kCameraShakeFieldsAt = 0x2E0;

bool IsBattleCamera(const uint8_t* camera, uintptr_t imageBase) {
    return IsReadable(camera + kCameraPartAt, 8) &&
           *reinterpret_cast<const uintptr_t*>(camera + kCameraPartAt) == imageBase + kCameraPartVtable &&
           IsReadable(camera + kCameraShakeAt, kCameraShakeSize);
}

constexpr uint32_t kVitalityOwnerVtable = 0x2C51718;
constexpr size_t kVitalityRegionSize = 372, kVitalityRegionAt = 0xC0, kVitalityOwnerAt = 0xB0;
constexpr size_t kFighterEventAt = 0x338, kFighterEventSize = 4;
constexpr size_t kEffectSwitchesAt = 0x340, kEffectSwitchStride = 0x48, kEffectSwitchCount = 4;
constexpr size_t kEffectSwitchFieldsAt = 0x28, kEffectSwitchFieldsSize = 0x1C;
constexpr uint32_t kEffectOwnerVtable = 0x2C53598;
constexpr size_t kEffectSlotsSize = 392, kEffectSlotsAt = 0x6D8, kEffectOwnerVtableAt = 0xB0;
constexpr size_t kEffectOwnerValueAt = 0x108, kEffectOwnerValueSize = 4;

bool SfvGame::RefreshRegions() {
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
    std::vector<ExtraRegion> flow;
    if (extras) {
        flow.push_back({"flow.round", FlowRoundFlowState(), kRoundFlowStateSize});
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
            if (GameLooksLikeAddress(v)) std::memset(&copy[w], 0, 8);
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
        fresh.push_back({"inputunit.held", inputUnit_ + game::inputunit::kHeldBegin, game::inputunit::kHeldSize, {}});
    }
    if (extras && battleFrame_) {
        fresh.push_back({"battleframe.state", battleFrame_ + game::battleframe::kStateBegin, game::battleframe::kStateSize, {}});
    }
    if (singletonTicks) fresh.push_back({"singleton.ticks", singletonTicks, game::singleton::kTicksSize, {}});
    for (const auto* e : presentation) fresh.push_back({e->name, e->ptr, e->size, {}});
    for (const auto& e : flow) fresh.push_back({e.name, e.ptr, e.size, {}});
    if (extras) {
        auto owned = [&](uint8_t* object, size_t vtableAt, uint32_t vtable, size_t at, size_t size) {
            return IsReadable(object + vtableAt, 8) &&
                   *reinterpret_cast<const uintptr_t*>(object + vtableAt) == image_.base + vtable &&
                   IsReadable(object + at, size);
        };
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
    GameArmPendingRegionWatch(regions_, image_.base);
    SetStateMap(stateMap_);
    return true;
}

struct EffectRegionKind {
    size_t size;
    size_t ownerBefore;
    uint32_t vtable;
};

constexpr EffectRegionKind kEffectRegions[] = {
    {44, 0x10, 0x2C6FC18},
    {136, 0x40, 0x2C6FC18},
    {40, 0x10, 0x2C6EAD0},
    {68, 0x90, 0x2C6EAD0},
    {208, 0xE8, 0x2C6EAD0},
    {164, 0x1C8, 0x2C6EAD0},
    {15808, 0x18, 0x2C6E6D8},
    {224, 0x30, 0x3231618},
    {208, 0x40, 0x2C54490},
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
            r.ignore.resize(1, 0);
            r.ignore[0] |= uint8_t(1u << ((game::inputunit::kRingTag - game::inputunit::kCountersBegin) / 8));
        }
        if (r.name == "viewtarget.clock" || r.name == "pawn.clock") {
            r.ignore.resize(1, 0);
            r.ignore[0] |= 1;
        }
        if (r.name == "camera.shake" || (r.size == kCameraRegionSize && IsBattleCamera(r.ptr - kCameraRegionAt, image_.base))) {
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
            if (r.ignore.size() < 2) r.ignore.resize(2, 0);
            r.ignore[1] |= uint8_t(1u << (0x58 / 8 - 8)) | uint8_t(1u << (0x60 / 8 - 8));
        }
        if (IsEffectRegion(r)) {
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
            if (v >= kFirstPage && v < kUserSpaceEnd && InRanges(ranges, v)) ignore[w / 8] |= uint8_t(1u << (w % 8));
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
            if (v >= kFirstPage && v < kUserSpaceEnd && InRanges(ranges, v)) pointers++;
        }
        const uintptr_t p = reinterpret_cast<uintptr_t>(r.ptr);
        const bool inImage = ImageHolds(p);
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
            const auto* ctx = Read<const uint8_t*>(node, mgr::callback::kContext);
            const uintptr_t target = IsReadable(ctx, 8) ? Read<uintptr_t>(ctx, 0) : 0;
            Log("  post-restore callback %d: thunk rva 0x%llX target rva 0x%llX", n,
                static_cast<unsigned long long>(fn - image_.base),
                static_cast<unsigned long long>(target ? target - image_.base : 0));
        }
    }
}

} // namespace sfv::mod
