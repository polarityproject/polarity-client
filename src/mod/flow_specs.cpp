#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

namespace {

constexpr uint32_t kTableRowStructOff = 0x28, kTableRowsOff = 0x40;
constexpr size_t kTableBytes = kTableRowsOff + kArrayBytes;
constexpr size_t kRowElementBytes = 0x18;
constexpr uint32_t kRowElementRowOff = 0x8;
constexpr uint32_t kStructPropertiesOff = 0x38, kPropertyNextOff = 0x28;
constexpr size_t kPropertyBytes = 0x50;
constexpr int kMaxProperties = 60;
constexpr uint32_t kPersonalNameOff = 0x28;
constexpr uint32_t kCommentsOff = 0x48;
constexpr int kMaxComments = 32;

} // namespace

namespace flow {

std::optional<DataTable> FindDataTable(const char* name) {
    const auto* table = static_cast<const uint8_t*>(ue4::FindObject(name, "DataTable"));
    if (!table || !IsReadable(table, kTableBytes)) return std::nullopt;
    DataTable t;
    t.rowStruct = *reinterpret_cast<const uint8_t* const*>(table + kTableRowStructOff);
    t.elements = *reinterpret_cast<const uint8_t* const*>(table + kTableRowsOff);
    t.count = t.elements ? *reinterpret_cast<const int32_t*>(table + kTableRowsOff + kArrayNumOff) : 0;
    return t;
}

const uint8_t* DataTable::Row(int i) const {
    return *reinterpret_cast<const uint8_t* const*>(elements + i * kRowElementBytes + kRowElementRowOff);
}

int32_t DataTable::RowName(int i) const { return *reinterpret_cast<const int32_t*>(elements + i * kRowElementBytes); }

std::vector<const uint8_t*> StructProperties(const uint8_t* rowStruct) {
    std::vector<const uint8_t*> out;
    const uint8_t* f = rowStruct ? *reinterpret_cast<const uint8_t* const*>(rowStruct + kStructPropertiesOff) : nullptr;
    for (int k = 0; f && k < kMaxProperties && IsReadable(f, kPropertyBytes); k++) {
        out.push_back(f);
        f = *reinterpret_cast<const uint8_t* const*>(f + kPropertyNextOff);
    }
    return out;
}

int PropertyOffset(const uint8_t* rowStruct, const char* name) {
    for (const uint8_t* f : StructProperties(rowStruct)) {
        if (ue4::ObjectName(f) == name) return *reinterpret_cast<const int32_t*>(f + kPropertyOffsetOff);
    }
    return -1;
}

} // namespace flow

namespace {

std::mutex g_specMutex;

std::map<int, std::map<int, CostumeSpec>> g_specs;

bool g_specsRead = false;

void ReadCharaSpecs() {
    if (g_specsRead || !g_base) return;
    if (!ue4::EnsureObjects(g_base)) {
        Log("flow: CharaSpec: no object list yet");
        return;
    }
    const std::optional<DataTable> table = FindDataTable("CharaSpec");
    if (!table) {
        Log("flow: CharaSpec: no table yet");
        return;
    }
    g_specsRead = true;
    const uint8_t* rowStruct = table->rowStruct;
    const int chara = PropertyOffset(rowStruct, "CharaID"), costume = PropertyOffset(rowStruct, "CostumeId");
    const int text = PropertyOffset(rowStruct, "CostumeText"), color = PropertyOffset(rowStruct, "ColorId");
    const int charaCode = PropertyOffset(rowStruct, "RefAvailableCode"), visibleCode = PropertyOffset(rowStruct, "RefVisibleCode");
    const int costumeCode = PropertyOffset(rowStruct, "RefCostumeAvailableCode"), colorCode = PropertyOffset(rowStruct, "RefColorAvailableCode");
    if (chara < 0 || costume < 0 || text < 0 || color < 0) {
        Log("flow: the CharaSpec table has other fields than expected");
        return;
    }
    for (int i = 0; i < table->count; i++) {
        const uint8_t* row = table->Row(i);
        if (!row || !IsReadable(row, color + sizeof(int32_t))) continue;
        CostumeSpec& spec = g_specs[*reinterpret_cast<const int32_t*>(row + chara)][*reinterpret_cast<const int32_t*>(row + costume)];
        const std::string name = FTextText(row + text);
        if (spec.name.empty() && !name.empty()) spec.name = name;
        const int colorId = *reinterpret_cast<const int32_t*>(row + color);
        spec.colors.push_back(colorId);
        auto code = [row](int at) { return at >= 0 && IsReadable(row + at, kArrayBytes) ? FStringText(row + at) : std::string(); };
        if (spec.charaCode.empty()) spec.charaCode = code(charaCode);
        if (spec.charaVisibleCode.empty()) spec.charaVisibleCode = code(visibleCode);
        if (spec.costumeCode.empty()) spec.costumeCode = code(costumeCode);
        spec.colorCodes[colorId] = code(colorCode);
    }
    for (auto& [c, costumes] : g_specs) {
        for (auto& [k, spec] : costumes) std::sort(spec.colors.begin(), spec.colors.end());
    }
    Log("flow: costume names and colors of %zu characters", g_specs.size());
}

std::mutex g_commentMutex;

std::map<int, std::string> g_comments;

bool g_commentsRead = false;

uint64_t g_commentsTriedAt = 0;

void ReadLoungeComments() {
    if (g_commentsRead || !g_base) return;
    const uint64_t now = GetTickCount64();
    if (g_commentsTriedAt && now - g_commentsTriedAt < 2000) return;
    g_commentsTriedAt = now;
    if (!ue4::EnsureObjects(g_base)) return;
    const std::optional<DataTable> table = FindDataTable("FixedPhraseCommentSpec");
    if (!table) return;
    g_commentsRead = true;
    const uint8_t* rowStruct = table->rowStruct;
    const int id = PropertyOffset(rowStruct, "Id"), text = PropertyOffset(rowStruct, "CommentText");
    if (id < 0 || text < 0) {
        Log("flow: the FixedPhraseCommentSpec table has other fields than expected");
        return;
    }
    for (int i = 0; i < table->count; i++) {
        const uint8_t* row = table->Row(i);
        if (!row || !IsReadable(row, std::max(id, text) + sizeof(void*))) continue;
        std::string s = FTextText(row + text);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        g_comments[*reinterpret_cast<const int32_t*>(row + id)] = s;
    }
    Log("flow: %zu Battle Lounge comments (room titles)", g_comments.size());
}

std::atomic<bool> g_commentsReading{false};

} // namespace

std::string FlowLoungeCommentText(int id) {
    {
        std::unique_lock lock(g_commentMutex, std::try_to_lock);
        if (lock && g_commentsRead) {
            const auto it = g_comments.find(id);
            return it != g_comments.end() ? it->second : std::string();
        }
    }
    if (!g_commentsReading.exchange(true)) {
        std::thread([] {
            ThreadGuard running;
            {
                std::lock_guard lock(g_commentMutex);
                ReadLoungeComments();
            }
            g_commentsReading = false;
        }).detach();
    }
    return std::string();
}

void FlowLogAllVariants() {
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    int total = 0;
    for (int i = 0; entries && i < count; i++) {
        const uint8_t* e = entries + i * kCharaEntrySize;
        const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
        const int num = *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8);
        const auto specs = FlowCharaCostumeSpecs(i);
        for (int k = 0; paths && k < num; k++) {
            const auto it = specs.find(k);
            const int n = FlowCostumeVariants(i, k);
            total += n ? 1 : 0;
            Log("variants %s [%d] %s: %d (%s)", FlowCharaCode(i).c_str(), k, it == specs.end() ? "(not offered)" : it->second.name.c_str(), n,
                FStringText(paths + k * 16).c_str());
        }
    }
    Log("variants: %d costumes with variants", total);
    if (!ue4::EnsureObjects(g_base)) return;
    ue4::ForEachObject([](const void* object) {
        const auto* o = static_cast<const uint8_t*>(object);
        if (ue4::ObjectName(ue4::ClassOf(object)) != "KWBattleCharaCostumeDataAsset" || !IsReadable(o + 0xD8, 16)) return;
        Log("variants: loaded %s lists %d", ue4::ObjectName(object).c_str(), *reinterpret_cast<const int32_t*>(o + 0xE0));
        const auto* preset = *reinterpret_cast<const uint8_t* const*>(o + 0x40);
        if (!preset || !IsReadable(preset + 0x28, 16)) return;
        const auto* items = *reinterpret_cast<const uint8_t* const*>(preset + 0x28);
        const int n = *reinterpret_cast<const int32_t*>(preset + 0x30);
        for (int k = 0; items && k < n && k < 40; k++) {
            Log("variants:   preset %s [%d] '%s' materials %d", ue4::ObjectName(preset).c_str(), k, FStringText(items + k * 0x20).c_str(),
                *reinterpret_cast<const int32_t*>(items + k * 0x20 + 0x18));
        }
    });
}

namespace {

constexpr uint32_t kStaticLoadObject = 0x1242910;

constexpr uint8_t kStaticLoadObjectBytes[] = {0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x49, 0x8D, 0xAB,
                                              0x18, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xD8};

std::string Utf8Of(const uint8_t* fstring) {
    const auto* data = *reinterpret_cast<const wchar_t* const*>(fstring);
    const int n = *reinterpret_cast<const int32_t*>(fstring + 8);
    if (!data || n <= 1) return "";
    std::string out(static_cast<size_t>(n) * 3, '\0');
    const int len = WideCharToMultiByte(CP_UTF8, 0, data, n - 1, out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(std::max(len, 0));
    return out;
}

} // namespace

void* FlowLoadAsset(const std::string& className, const std::string& path) {
    if (!g_base || std::memcmp(reinterpret_cast<const void*>(g_base + kStaticLoadObject), kStaticLoadObjectBytes,
                               sizeof(kStaticLoadObjectBytes)) != 0) {
        Log("flow: StaticLoadObject not where expected");
        return nullptr;
    }
    static std::mutex classMutex;
    static std::map<std::string, const void*> classes;
    const void* cls = nullptr;
    {
        std::lock_guard lock(classMutex);
        const void*& known = classes[className];
        if (!known) known = ue4::FindObject(className, "Class");
        cls = known;
    }
    if (!cls) return nullptr;
    using LoadFn = void* (*)(const void* cls, void* outer, const wchar_t* name, const wchar_t* file, uint32_t flags, void* sandbox, bool reconcile);
    return reinterpret_cast<LoadFn>(g_base + kStaticLoadObject)(cls, nullptr, WideOf(path).c_str(), nullptr, 0, nullptr, true);
}

namespace {

void LogPresetsOf(int index) {
    using LoadFn = void* (*)(const void* cls, void* outer, const wchar_t* name, const wchar_t* file, uint32_t flags, void* sandbox, bool reconcile);
    static const void* presetClass = ue4::FindObject("KWMaterialPresetDataAsset", "Class");
    int count = 0;
    const uint8_t* e = CharaEntries(&count) + index * kCharaEntrySize;
    const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
    const int num = *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8);
    const auto specs = FlowCharaCostumeSpecs(index);
    for (int k = 0; paths && k < num; k++) {
        const std::string path = FStringText(paths + k * 16);
        const size_t slash = path.find('/'), name = path.rfind("/DA_"), at = path.find("_Costume_");
        if (slash == std::string::npos || name == std::string::npos || at == std::string::npos) continue;
        const auto it = specs.find(k);
        const std::string label = it == specs.end() ? "(not offered)" : it->second.name;
        for (const std::string& code : {path.substr(name + 4, 3), path.substr(0, slash)}) {
            const std::string asset = "DA_" + code + "_MaterialPreset_" + path.substr(at + 9);
            const std::string package = "/Game/Chara/" + path.substr(0, name + 1) + asset;
            if (!PackageExists(package)) continue;
            const std::wstring full = WideOf(package + "." + asset);
            const auto* preset = static_cast<const uint8_t*>(
                reinterpret_cast<LoadFn>(g_base + kStaticLoadObject)(presetClass, nullptr, full.c_str(), nullptr, 0, nullptr, true));
            if (!preset || !IsReadable(preset + 0x28, 16)) {
                Log("presets %s [%d] %s: %s did not load", FlowCharaCode(index).c_str(), k, label.c_str(), asset.c_str());
                continue;
            }
            const auto* items = *reinterpret_cast<const uint8_t* const*>(preset + 0x28);
            const int n = *reinterpret_cast<const int32_t*>(preset + 0x30);
            std::string list;
            for (int j = 0; items && j < n && j < 40; j++) {
                list += " [" + Utf8Of(items + j * 0x20) + " " + std::to_string(*reinterpret_cast<const int32_t*>(items + j * 0x20 + 0x18)) + "]";
            }
            Log("presets %s [%d] %s (variants %d) %s:%s", FlowCharaCode(index).c_str(), k, label.c_str(), FlowCostumeVariants(index, k),
                asset.c_str(), list.c_str());
            if (code == path.substr(0, slash)) break;
        }
    }
}

void LogPresetsFrom(int index, std::string code) {
    int count = 0;
    if (!CharaEntries(&count)) return;
    for (; index < count; index++) {
        if (code.empty() || FlowCharaCode(index) == code) break;
    }
    if (index >= count) {
        Log("presets: done");
        return;
    }
    LogPresetsOf(index);
    FlowRunOnGameThreadAfter(5, [index, code] { LogPresetsFrom(index + 1, code); });
}

} // namespace

void FlowLogCostumePresets(const std::string& code) {
    if (!g_base || std::memcmp(reinterpret_cast<const void*>(g_base + kStaticLoadObject), kStaticLoadObjectBytes,
                               sizeof(kStaticLoadObjectBytes)) != 0) {
        Log("presets: StaticLoadObject not where expected");
        return;
    }
    FlowRunOnGameThread([code] { LogPresetsFrom(0, code); });
}

std::map<int, CostumeSpec> FlowCharaCostumeSpecs(int index) {
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return {};
    const int progId = *reinterpret_cast<const int32_t*>(entries + index * kCharaEntrySize + kCharaProgIdOff);
    std::lock_guard lock(g_specMutex);
    for (int tries = 0; tries < 5 && !g_specsRead; tries++) {
        if (tries) Sleep(200);
        ReadCharaSpecs();
    }
    const auto it = g_specs.find(progId);
    return it == g_specs.end() ? std::map<int, CostumeSpec>{} : it->second;
}

void FlowLogDataTable(const std::string& name, const std::string& filter, int limit) {
    if (!ue4::EnsureObjects(g_base)) return;
    const std::optional<DataTable> table = FindDataTable(name.c_str());
    if (!table) {
        Log("datatable %s: not found", name.c_str());
        return;
    }
    Log("datatable %s: %d rows of %s", name.c_str(), table->count, ue4::ObjectName(table->rowStruct).c_str());
    const std::vector<const uint8_t*> properties = StructProperties(table->rowStruct);
    int shown = 0;
    for (int i = 0; i < table->count && shown < limit; i++) {
        const uint8_t* row = table->Row(i);
        if (!row || !IsReadable(row, sizeof(void*))) continue;
        std::string line = ue4::NameText(table->RowName(i)) + ":";
        for (const uint8_t* f : properties) line += " " + ue4::ObjectName(f) + "=" + PropertyText(f, row);
        if (!filter.empty() && line.find(filter) == std::string::npos) continue;
        Log("  %s", line.c_str());
        shown++;
    }
}

std::string FlowStageAvailableCode(int index) {
    static std::mutex m;
    static std::map<int, std::string> codes;
    static bool read = false;
    std::lock_guard lock(m);
    if (!read && g_base && ue4::EnsureObjects(g_base)) {
        if (const std::optional<DataTable> table = FindDataTable("StageSpec")) {
            read = true;
            const int id = PropertyOffset(table->rowStruct, "StageId"), code = PropertyOffset(table->rowStruct, "RefAvailableCode");
            for (int i = 0; id >= 0 && code >= 0 && i < table->count; i++) {
                const uint8_t* row = table->Row(i);
                const size_t needed = std::max<size_t>(id + sizeof(int32_t), code + kArrayBytes);
                if (row && IsReadable(row, needed)) codes[*reinterpret_cast<const int32_t*>(row + id)] = FStringText(row + code);
            }
            Log("flow: stage codes of %zu stages", codes.size());
        }
    }
    const auto it = codes.find(index);
    return it == codes.end() ? std::string() : it->second;
}

namespace {

std::mutex g_loadedNamesMutex;

std::map<std::string, std::string> g_loadedNames;

std::atomic<int> g_stageNamesState{0};

} // namespace

bool FlowStageNamesReady() {
    int expected = 0;
    if (g_stageNamesState.compare_exchange_strong(expected, 1)) {
        FlowRunOnGameThread([] {
            int count = 0;
            const uint8_t* entries = StageEntries(&count);
            if (!entries || FlowStageAvailableCode(0).empty()) {
                g_stageNamesState = 0;
                return;
            }
            std::string loaded;
            for (int i = 0; i < count; i++) {
                const uint8_t* e = entries + i * kStageEntrySize;
                const std::string code = FStringText(e), path = FStringText(e + kStageAssetOff);
                const size_t dot = path.rfind('.');
                if (code.size() != kStageCodeLength || code == "RND" || dot == std::string::npos || path.find("/" + code + "/") == std::string::npos ||
                    !FlowIsPlayableStage(i) || FlowStageAvailableCode(i).empty() || !FlowStageName(i).empty()) {
                    continue;
                }
                const auto* asset = static_cast<const uint8_t*>(FlowLoadAsset("KWStagePersonalDataAsset", path));
                const std::string name = asset && IsReadable(asset + kPersonalNameOff, sizeof(void*)) ? FTextText(asset + kPersonalNameOff) : "";
                if (name.empty()) continue;
                std::lock_guard lock(g_loadedNamesMutex);
                g_loadedNames[path.substr(dot + 1)] = name;
                loaded += (loaded.empty() ? "" : ", ") + code + " " + name;
            }
            Log("flow: stage names loaded: %s", loaded.empty() ? "none needed" : loaded.c_str());
            g_stageNamesState = 2;
        });
    }
    return g_stageNamesState == 2;
}

std::string FlowStageName(int index) {
    static std::mutex mutex;
    static std::map<std::string, const uint8_t*> personal;
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    const std::string path = FStringText(entries + index * kStageEntrySize + kStageAssetOff);
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::lock_guard lock(mutex);
    if (personal.empty() && ue4::EnsureObjects(g_base)) {
        ue4::ForEachObject([&](const void* object) {
            if (ue4::ObjectName(ue4::ClassOf(object)) == "KWStagePersonalDataAsset") {
                personal[ue4::ObjectName(object)] = static_cast<const uint8_t*>(object);
            }
        });
    }
    const auto it = personal.find(path.substr(dot + 1));
    if (it != personal.end()) return FTextText(it->second + kPersonalNameOff);
    std::lock_guard loadedLock(g_loadedNamesMutex);
    const auto loaded = g_loadedNames.find(path.substr(dot + 1));
    return loaded == g_loadedNames.end() ? "" : loaded->second;
}

size_t FlowSkillCountKnown(const std::string& charaCode, bool vtrigger) {
    const char* name = vtrigger ? "DA_VTriggerNameAsset" : "DA_VSkillNameAsset";
    if (KnownObject(name, "KWVersionDataAsset")) return FlowSkillNames(charaCode, vtrigger).size();
    static std::atomic<ULONGLONG> askedAt[2] = {};
    std::atomic<ULONGLONG>& asked = askedAt[vtrigger ? 1 : 0];
    const ULONGLONG now = GetTickCount64();
    ULONGLONG before = asked;
    if (now - before > 5000 && asked.compare_exchange_strong(before, now)) {
        std::thread([name] {
            ThreadGuard running;
            CachedObject(name, "KWVersionDataAsset");
        }).detach();
    }
    return 0;
}

std::vector<std::string> FlowSkillNames(const std::string& charaCode, bool vtrigger) {
    std::vector<std::string> names;
    const auto* asset = static_cast<const uint8_t*>(
        CachedObject(vtrigger ? "DA_VTriggerNameAsset" : "DA_VSkillNameAsset", "KWVersionDataAsset"));
    constexpr size_t kEntryBytes = 0x20;
    constexpr uint32_t kEntryTextsOff = 0x10;
    constexpr int kMaxEntries = 256, kMaxNames = 8;
    int count = 0;
    const uint8_t* entries = AssetList(asset, &count);
    for (int i = 0; entries && i < count && i < kMaxEntries; i++) {
        const uint8_t* e = entries + i * kEntryBytes;
        if (!IsReadable(e, kEntryBytes) || FStringText(e) != charaCode) continue;
        const uint8_t* texts = *reinterpret_cast<const uint8_t* const*>(e + kEntryTextsOff);
        const int n = *reinterpret_cast<const int32_t*>(e + kEntryTextsOff + kArrayNumOff);
        for (int k = 0; texts && k < n && k < kMaxNames; k++) names.push_back(FTextText(texts + k * kFTextBytes));
        break;
    }
    return names;
}

std::vector<std::string> FlowVCommentNames(const std::string& charaCode) {
    const std::string name = "DA_" + charaCode + "_Subtitle";
    const auto* asset = static_cast<const uint8_t*>(CachedObject(name, "KWSubtitleDataAsset"));
    if (!asset) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        const std::string path = "/Game/Chara/" + charaCode + "/DataAsset/" + name + "." + name;
        FlowRunOnGameThread([path, done] {
            FlowLoadAsset("KWSubtitleDataAsset", path);
            *done = true;
        });
        for (int i = 0; i < 200 && !*done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        asset = static_cast<const uint8_t*>(CachedObject(name, "KWSubtitleDataAsset"));
    }
    std::vector<std::string> names;
    if (!asset || !IsReadable(asset + kCommentsOff, kArrayBytes)) return names;
    const uint8_t* texts = *reinterpret_cast<const uint8_t* const*>(asset + kCommentsOff);
    const int n = *reinterpret_cast<const int32_t*>(asset + kCommentsOff + kArrayNumOff);
    for (int k = 0; texts && k < n && k < kMaxComments; k++) {
        std::string text = FTextText(texts + k * kFTextBytes);
        std::replace(text.begin(), text.end(), '\n', ' ');
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        names.push_back(std::move(text));
    }
    return names;
}

std::optional<size_t> FlowVCommentCount(const std::string& charaCode) {
    const std::string name = "DA_" + charaCode + "_Subtitle";
    const auto count = [&]() -> std::optional<size_t> {
        const auto* asset = static_cast<const uint8_t*>(KnownObject(name, "KWSubtitleDataAsset"));
        if (!asset) return std::nullopt;
        if (!IsReadable(asset + kCommentsOff, kArrayBytes)) return 0;
        return static_cast<size_t>(std::clamp(*reinterpret_cast<const int32_t*>(asset + kCommentsOff + kArrayNumOff), 0, kMaxComments));
    };
    if (const auto n = count()) return n;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<std::atomic<bool>>> loads;
    std::shared_ptr<std::atomic<bool>> done;
    {
        std::lock_guard lock(mutex);
        auto& load = loads[charaCode];
        if (!load) {
            load = std::make_shared<std::atomic<bool>>(false);
            FlowRunOnGameThread([path = "/Game/Chara/" + charaCode + "/DataAsset/" + name + "." + name, name, load] {
                FlowLoadAsset("KWSubtitleDataAsset", path);
                CachedObject(name, "KWSubtitleDataAsset");
                *load = true;
            });
        }
        done = load;
    }
    if (!*done) return std::nullopt;
    const auto n = count();
    return n ? n : 0;
}

int FlowStageIndex(const std::string& code) {
    for (int i = 0; i < FlowStageCount(); i++) {
        if (FlowStageCode(i) == code) return i;
    }
    return -1;
}

} // namespace sfv::mod
