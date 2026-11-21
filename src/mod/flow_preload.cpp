#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

namespace {

std::mutex g_cacheArgsMutex;

std::wstring g_cacheX;

int g_cacheY = 0;

bool g_cacheArgsSeen = false;

} // namespace

namespace flow {

std::wstring WideOf(const std::string& text) { return std::wstring(text.begin(), text.end()); }

uint64_t CharaCacheRequestDetour(void* cache, const void* code, int costume, int color, const void* x, int y) {
    HookGuard guard;
    const std::string xText = FStringText(static_cast<const uint8_t*>(x));
    {
        std::lock_guard lock(g_cacheArgsMutex);
        g_cacheX = WideOf(xText);
        g_cacheY = y;
        g_cacheArgsSeen = true;
    }
    const auto* c = static_cast<const uint8_t*>(cache);
    Log("flow: character cache request %s costume %d version %d x '%s' y %d (slots %d in use, %d free)",
        FStringText(static_cast<const uint8_t*>(code)).c_str(), costume, color, xText.c_str(), y,
        *reinterpret_cast<const int32_t*>(c + kCacheSlotsInUse + 8), *reinterpret_cast<const int32_t*>(c + kCacheSlotsFree + 8));
    return g_origCacheRequest(cache, code, costume, color, x, y);
}

std::string FStringText(const uint8_t* fstring) {
    const wchar_t* data = nullptr;
    int32_t num = 0;
    if (!IsReadable(fstring, 16)) return "";
    std::memcpy(&data, fstring, 8);
    std::memcpy(&num, fstring + 8, 4);
    std::wstring wide;
    for (int i = 0; data && i < num && i < 200 && IsReadable(data + i, 2) && data[i]; i++) wide += data[i];
    if (wide.empty()) return "";
    std::string out(wide.size() * 3, '\0');
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                                        static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(static_cast<size_t>(std::max(len, 0)));
    return out;
}

} // namespace flow

namespace {

uint8_t* AssetQueue() {
    auto* manager = *reinterpret_cast<uint8_t**>(g_base + kAssetManager);
    if (!manager || manager == reinterpret_cast<uint8_t*>(-1)) return nullptr;
    return *reinterpret_cast<uint8_t**>(manager + kAssetManagerQueue);
}

std::vector<int> VersionsOf(const std::string& code) {
    std::vector<int> out;
    uint8_t* singleton = reinterpret_cast<uint8_t* (*)()>(g_base + kKiwiSingleton)();
    void* versions = singleton ? *reinterpret_cast<void**>(singleton + kSingletonVersions) : nullptr;
    if (!versions) return out;
    const FStringArg codeArg(WideOf(code));
    auto* info = reinterpret_cast<uint8_t* (*)(void*, const void*)>(g_base + kCharaVersionInfo)(versions, codeArg.Get());
    constexpr uint32_t kInfoListOff = 0x80, kListEntriesOff = 0x28;
    constexpr size_t kEntryBytes = 0x18, kMaxEntries = 64;
    const uint8_t* list = info ? *reinterpret_cast<uint8_t* const*>(info + kInfoListOff) : nullptr;
    if (!list) return out;
    const uint8_t* entries = *reinterpret_cast<uint8_t* const*>(list + kListEntriesOff);
    const int count = *reinterpret_cast<const int32_t*>(list + kListEntriesOff + kArrayNumOff);
    for (int i = 0; entries && i < count && i < static_cast<int>(kMaxEntries); i++) {
        out.push_back(*reinterpret_cast<const int32_t*>(entries + i * kEntryBytes));
    }
    return out;
}

int LatestVersion(const std::string& code) {
    int version = -1;
    uint8_t* singleton = reinterpret_cast<uint8_t* (*)()>(g_base + kKiwiSingleton)();
    void* versions = singleton ? *reinterpret_cast<void**>(singleton + kSingletonVersions) : nullptr;
    const FStringArg codeArg(WideOf(code));
    if (versions) reinterpret_cast<bool (*)(void*, const void*, int*)>(g_base + kResolveCharaVersion)(versions, codeArg.Get(), &version);
    return version;
}

const std::map<std::string, int> kFinalVersions = {
    {"RYU", 60},
    {"CNL", 61},
    {"KEN", 61},
    {"KRN", 60},
    {"ZGF", 61},
    {"DSM", 61},
    {"NSH", 61},
    {"BLR", 61},
    {"JRI", 61},
    {"BRD", 60},
    {"RMK", 60},
    {"RSD", 60},
    {"FAN", 60},
    {"LAR", 60},
    {"NCL", 61},
    {"CMY", 60},
    {"GUL", 60},
    {"IBK", 61},
    {"BSN", 60},
    {"URN", 60},
    {"ALX", 61},
    {"NDK", 60},
    {"VEG", 60},
    {"Z20", 61},
    {"Z21", 61},
    {"Z22", 61},
    {"Z23", 61},
    {"Z24", 60},
    {"Z25", 61},
    {"Z26", 60},
    {"Z27", 61},
    {"Z28", 60},
    {"Z29", 61},
    {"Z30", 61},
    {"Z31", 60},
    {"Z32", 60},
    {"Z33", 60},
    {"Z34", 60},
    {"Z35", 61},
    {"Z36", 62},
    {"Z37", 61},
    {"Z38", 60},
    {"Z39", 60},
    {"Z40", 60},
    {"Z41", 61},
    {"Z42", 61},
};

std::mutex g_localVersionsMutex;

std::map<std::string, std::vector<int>> g_localVersions;

std::atomic<bool> g_localVersionsRead{false};

std::string g_localVersionsText = "not read yet";

void ReadLocalVersions() {
    if (FlowCharaCount() <= 0 || LatestVersion("RYU") < 0) return;
    std::map<std::string, std::vector<int>> lists;
    for (int i = 0; i < FlowCharaCount(); i++) {
        const std::string code = FlowCharaCode(i);
        if (!code.empty()) lists[code] = VersionsOf(code);
    }
    std::string older;
    int olderCount = 0;
    for (const auto& [code, final] : kFinalVersions) {
        const auto it = lists.find(code);
        const int newest = it == lists.end() || it->second.empty() ? -1 : it->second.back();
        if (newest == final) continue;
        olderCount++;
        older += (older.empty() ? "" : ", ") + code + " " + std::to_string(newest) + "/" + std::to_string(final);
    }
    const std::string text = olderCount == 0 ? "the final game's"
                                             : std::to_string(olderCount) + " not the final game's (newest/final): " + older;
    Log("flow: character versions: %s", text.c_str());
    {
        std::lock_guard lock(g_localVersionsMutex);
        g_localVersions = std::move(lists);
        g_localVersionsText = text;
    }
    g_localVersionsRead = true;
}

int MatchVersion(const std::string& code, int version) {
    if (version >= 0) return version;
    const int final = FlowFinalCharaVersion(code);
    return final >= 0 && FlowHasCharaVersion(code, final) ? final : LatestVersion(code);
}

bool InCharaCache(const std::string& code, int costume, int version, int y) {
    const auto* cache = reinterpret_cast<const uint8_t* (*)()>(g_base + kCharaCache)();
    if (!cache) return false;
    const auto slots = *reinterpret_cast<uint8_t* const* const*>(cache + kCacheSlotsInUse);
    const int inUse = *reinterpret_cast<const int32_t*>(cache + kCacheSlotsInUse + kArrayNumOff);
    for (int i = 0; i < inUse && slots; i++) {
        const uint8_t* slot = slots[i];
        if (FStringText(slot + kSlotCodeOff) == code && *reinterpret_cast<const int32_t*>(slot + kSlotCostumeOff) == costume &&
            *reinterpret_cast<const int32_t*>(slot + kSlotVersionOff) == version && *reinterpret_cast<const int32_t*>(slot + kSlotYOff) == y) {
            return true;
        }
    }
    return false;
}

std::mutex g_preloadMutex;

std::function<std::vector<PreloadPick>()> g_preloadSource;

std::string g_preloadSeen;

uint64_t g_preloadSeenAt = 0;

std::map<std::string, int> g_preloadTries;

std::atomic<bool> g_preloadsDone{false};

std::string g_preloadsDoneFor;

std::map<std::string, int> g_preloadStatus;

std::string g_preloadingKey;

std::chrono::steady_clock::time_point g_preloadingSince;

constexpr int kPreloadStableFrames = 60;

constexpr int kPreloadMaxTries = 2;

void CacheArgs(std::wstring* x, int* y) {
    std::lock_guard lock(g_cacheArgsMutex);
    *x = g_cacheArgsSeen ? g_cacheX : L"S25";
    *y = g_cacheArgsSeen ? g_cacheY : -1;
}

bool StartPreload(const std::string& code, int costume, int setupVersion, const char* why) {
    uint8_t* queue = AssetQueue();
    if (!queue || g_preloading || *reinterpret_cast<int32_t*>(queue + kQueueState) != 0) return false;
    std::wstring x;
    int y = 0;
    CacheArgs(&x, &y);
    const int version = MatchVersion(code, setupVersion);
    if (InCharaCache(code, costume, version, y)) {
        Log("flow: preload %s costume %d (%s): already loaded", code.c_str(), costume, why);
        return true;
    }
    Log("flow: preloading %s costume %d version %d (%s)", code.c_str(), costume, version, why);
    g_preloading = true;
    {
        std::lock_guard lock(g_preloadMutex);
        g_preloadingKey = code + "/" + std::to_string(costume);
        g_preloadingSince = std::chrono::steady_clock::now();
    }
    {
        const FStringArg codeArg(WideOf(code));
        const FStringArg xArg(x);
        reinterpret_cast<void (*)(void*, const void*, int, int, const void*, int)>(g_base + kQueueAddChara)(
            queue, codeArg.Get(), costume, version, xArg.Get(), y);
    }
    reinterpret_cast<void (*)(void*, int)>(g_base + kQueueStart)(queue, kQueueLoadBattle);

    struct Waiter {
        static void Step(std::string code, int costume, int version, std::wstring x, int y, uint8_t* queue,
                         std::chrono::steady_clock::time_point started, int frames) {
            if (*reinterpret_cast<int32_t*>(queue + kQueueState) != 0 && frames < 60 * 60) {
                FlowRunOnGameThreadAfter(1, [=] { Step(code, costume, version, x, y, queue, started, frames + 1); });
                return;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (void* cache = reinterpret_cast<void* (*)()>(g_base + kCharaCache)()) {
                const FStringArg codeArg(WideOf(code));
                const FStringArg xArg(x);
                CharaCacheRequestDetour(cache, codeArg.Get(), costume, version, xArg.Get(), y);
            }
            Log("flow: preloaded %s costume %d in %.1f s (%d frames)%s", code.c_str(), costume, seconds, frames,
                InCharaCache(code, costume, version, y) ? "" : "; NOT in the character cache");
            {
                std::lock_guard lock(g_preloadMutex);
                g_preloadingKey.clear();
            }
            g_preloading = false;
        }
    };

    Waiter::Step(code, costume, version, x, y, queue, std::chrono::steady_clock::now(), 0);
    return true;
}

} // namespace

namespace flow {

void PreloadTick() {
    std::function<std::vector<PreloadPick>()> source;
    {
        std::lock_guard lock(g_preloadMutex);
        source = g_preloadSource;
    }
    if (!g_localVersionsRead) ReadLocalVersions();
    std::vector<PreloadPick> picks;
    if (source) picks = source();
    std::string seen;
    for (const auto& p : picks) seen += p.code + "/" + std::to_string(p.costume) + " ";
    const uint64_t now = g_pumps;
    {
        std::wstring x;
        int y = 0;
        CacheArgs(&x, &y);
        bool done = !g_preloading;
        std::map<std::string, int> status;
        for (const auto& p : picks) {
            const std::string key = p.code + "/" + std::to_string(p.costume);
            const bool cached = !p.code.empty() && InCharaCache(p.code, p.costume, MatchVersion(p.code, p.version), y);
            const bool tried = g_preloadTries[key] >= kPreloadMaxTries;
            status[key] = cached ? 2 : tried ? -1
                                             : 0;
            done = done && (p.code.empty() || cached || tried);
        }
        if (LastScene() != "MainMenuState" && !g_preloading) done = true;
        std::lock_guard lock(g_preloadMutex);
        g_preloadsDone = done;
        g_preloadsDoneFor = seen;
        g_preloadStatus = std::move(status);
    }
    if (seen != g_preloadSeen) {
        g_preloadSeen = seen;
        g_preloadSeenAt = now;
        return;
    }
    static std::string lastWhy;
    auto wait = [&](const std::string& why) {
        if (why != lastWhy && !picks.empty()) Log("flow: preload of %swaits: %s", seen.c_str(), why.c_str());
        lastWhy = why;
    };
    if (picks.empty() || g_preloading || now - g_preloadSeenAt < kPreloadStableFrames) return;
    if (LastScene() != "MainMenuState") return wait("scene " + LastScene());
    if (now - g_sceneChangedAt < kSceneSettleFrames * 2) return wait("scene settling");
    if (g_jumpRequestedAt && now - g_jumpRequestedAt < kSceneSettleFrames * 10) return wait("a jump was requested");
    std::wstring x;
    int y = 0;
    CacheArgs(&x, &y);
    for (const auto& p : picks) {
        if (p.code.empty() || InCharaCache(p.code, p.costume, MatchVersion(p.code, p.version), y)) {
            wait("loaded");
            continue;
        }
        int& tries = g_preloadTries[p.code + "/" + std::to_string(p.costume)];
        if (tries >= kPreloadMaxTries) {
            wait("tried enough");
            continue;
        }
        if (StartPreload(p.code, p.costume, p.version, "pick")) {
            tries++;
            return;
        }
        static int loggedState = 0;
        const uint8_t* queue = AssetQueue();
        const int state = queue ? *reinterpret_cast<const int32_t*>(queue + kQueueState) : -1;
        if (state != loggedState) Log("flow: preload %s waits for the asset queue (state %d)", p.code.c_str(), state);
        loggedState = state;
        return;
    }
}

} // namespace flow

void FlowSetPreloadSource(std::function<std::vector<PreloadPick>()> source) {
    std::lock_guard lock(g_preloadMutex);
    g_preloadSource = std::move(source);
}

bool FlowPreloadsDone() { return g_preloadsDone; }

bool FlowPreloadsDoneFor(const std::string& picks) {
    std::lock_guard lock(g_preloadMutex);
    return g_preloadsDone && g_preloadsDoneFor == picks;
}

int FlowPreloadStatus(const std::string& code, int costume, double* seconds) {
    const std::string key = code + "/" + std::to_string(costume);
    std::lock_guard lock(g_preloadMutex);
    *seconds = 0;
    if (g_preloadingKey == key) {
        *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_preloadingSince).count();
        return 1;
    }
    const auto it = g_preloadStatus.find(key);
    return it == g_preloadStatus.end() ? -1 : it->second;
}

void FlowPreloadCharacter(const std::string& code, int costume) {
    FlowRunOnGameThread([code, costume] {
        if (!StartPreload(code, costume, -1, "command")) Log("flow: preload %s: the asset queue is busy", code.c_str());
    });
}

void FlowPreloadCharacters(std::vector<PreloadPick> picks, std::function<void()> then) {
    struct Chain {
        static void Step(std::vector<PreloadPick> picks, size_t next, std::function<void()> then, int frames) {
            if (next == picks.size() && !g_preloading) {
                if (then) then();
                return;
            }
            if (frames >= 60 * 60) {
                Log("flow: preloads gave up waiting for the asset queue");
                if (then) then();
                return;
            }
            if (next < picks.size() && LatestVersion(picks[next].code) >= 0 &&
                StartPreload(picks[next].code, picks[next].costume, picks[next].version, "replay")) {
                next++;
            }
            FlowRunOnGameThreadAfter(1, [=] { Step(picks, next, then, frames + 1); });
        }
    };

    FlowRunOnGameThread([picks = std::move(picks), then = std::move(then)] { Chain::Step(picks, 0, then, 0); });
}

void FlowLogCharaVersions() {
    FlowRunOnGameThread([] {
        for (int i = 0; i < FlowCharaCount(); i++) {
            const std::string code = FlowCharaCode(i);
            if (code.empty()) continue;
            std::string list;
            for (int v : VersionsOf(code)) list += " " + std::to_string(v);
            Log("flow: versions %s:%s (latest %d)", code.c_str(), list.c_str(), LatestVersion(code));
        }
    });
}

int FlowLatestCharaVersion(const std::string& code) { return LatestVersion(code); }

int FlowFinalCharaVersion(const std::string& code) {
    const auto it = kFinalVersions.find(code);
    return it == kFinalVersions.end() ? -1 : it->second;
}

bool FlowHasCharaVersion(const std::string& code, int version) {
    if (version < 0 || !g_localVersionsRead) return true;
    std::lock_guard lock(g_localVersionsMutex);
    const auto it = g_localVersions.find(code);
    if (it == g_localVersions.end()) return true;
    return std::find(it->second.begin(), it->second.end(), version) != it->second.end();
}

int FlowNewestLocalVersion(const std::string& code) {
    std::lock_guard lock(g_localVersionsMutex);
    const auto it = g_localVersions.find(code);
    return it == g_localVersions.end() || it->second.empty() ? -1 : it->second.back();
}

std::string FlowCharaVersionsText() {
    std::lock_guard lock(g_localVersionsMutex);
    return g_localVersionsText;
}

namespace {

constexpr uint32_t kOptionVoiceMode = 0x40;

constexpr uint32_t kOptionCharaVoices = 0xC0;

std::atomic<int> g_voiceOverride{-1};

const uint8_t* OptionSettings() {
    static const void* found = nullptr;
    if (found && ue4::ObjectName(found) == "OptionSettingsSave") return static_cast<const uint8_t*>(found);
    found = nullptr;
    if (!g_base || !ue4::EnsureObjects(g_base)) return nullptr;
    ue4::ForEachObject([](const void* o) {
        if (found || ue4::ObjectName(o) != "OptionSettingsSave") return;
        const void* outer = ue4::OuterOf(o);
        if (outer && ue4::ObjectName(outer) == "GameSystemSave" && ue4::ObjectName(ue4::ClassOf(o)) == "KWOptionSettingsSave") found = o;
    });
    return static_cast<const uint8_t*>(found);
}

} // namespace

int FlowVoiceLanguageFor(const std::string& code) {
    if (const int forced = g_voiceOverride; forced >= 0) return forced;
    const uint8_t* options = OptionSettings();
    if (!options || !IsReadable(options + kOptionVoiceMode, kOptionCharaVoices + kArrayBytes - kOptionVoiceMode)) return -1;
    const int mode = options[kOptionVoiceMode];
    uint8_t* singleton = reinterpret_cast<uint8_t* (*)()>(g_base + kKiwiSingleton)();
    void* versions = singleton ? *reinterpret_cast<void**>(singleton + kSingletonVersions) : nullptr;
    const FStringArg codeArg(WideOf(code));
    const uint8_t* info = versions ? reinterpret_cast<uint8_t* (*)(void*, const void*)>(g_base + kCharaVersionInfo)(versions, codeArg.Get()) : nullptr;
    constexpr uint32_t kInfoUniqueIdOff = 0x14;
    const int id = info ? *reinterpret_cast<const int32_t*>(info + kInfoUniqueIdOff) : -1;
    const auto* langs = *reinterpret_cast<const uint8_t* const*>(options + kOptionCharaVoices);
    const int count = *reinterpret_cast<const int32_t*>(options + kOptionCharaVoices + kArrayNumOff);
    if (langs && id >= 0 && id < count && IsReadable(langs + id, 1)) return langs[id];
    return mode <= 1 ? mode : -1;
}

void FlowSetVoiceLanguageOverride(int language) { g_voiceOverride = language; }

void FlowLogCharaCache() {
    FlowRunOnGameThread([] {
        const auto* cache = reinterpret_cast<const uint8_t* (*)()>(g_base + kCharaCache)();
        if (!cache) return;
        const auto slots = *reinterpret_cast<uint8_t* const* const*>(cache + kCacheSlotsInUse);
        const int inUse = *reinterpret_cast<const int32_t*>(cache + kCacheSlotsInUse + kArrayNumOff);
        const int free = *reinterpret_cast<const int32_t*>(cache + kCacheSlotsFree + kArrayNumOff);
        Log("flow: character cache %p: %d slots in use (oldest first), %d free; battle data %p", static_cast<const void*>(cache), inUse, free, static_cast<void*>(BattleData()));
        for (int i = 0; i < inUse && slots; i++) {
            const uint8_t* slot = slots[i];
            const auto* queue = *reinterpret_cast<const uint8_t* const*>(slot);
            Log("  %s costume %d version %d y %d, queue state %d", FStringText(slot + kSlotCodeOff).c_str(),
                *reinterpret_cast<const int32_t*>(slot + kSlotCostumeOff), *reinterpret_cast<const int32_t*>(slot + kSlotVersionOff),
                *reinterpret_cast<const int32_t*>(slot + kSlotYOff), queue ? *reinterpret_cast<const int32_t*>(queue + kQueueState) : -1);
        }
    });
}

} // namespace sfv::mod
