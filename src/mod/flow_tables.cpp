#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

namespace {

constexpr uint32_t kPreviewColorsOff = 0x28, kPreviewExColorsOff = 0x38;

} // namespace

namespace flow {

const uint8_t* StageEntries(int* count) {
    uint8_t* singleton = GameSingleton();
    if (!singleton || !IsReadable(singleton + kSingletonStageTable, sizeof(void*))) return nullptr;
    return AssetList(*reinterpret_cast<uint8_t* const*>(singleton + kSingletonStageTable), count);
}

} // namespace flow

int FlowStageCount() {
    int count = 0;
    return StageEntries(&count) ? count : 0;
}

std::string FlowStageCode(int index) {
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    return FStringText(entries + index * kStageEntrySize);
}

bool FlowIsPlayableStage(int index) {
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries || index < 0 || index >= count) return false;
    const uint8_t* e = entries + index * kStageEntrySize;
    return FStringText(e).size() == kStageCodeLength && !FStringText(e + kStageLevelOff).empty() && !FStringText(e + kStageAssetOff).empty();
}

namespace {

std::mutex g_arrangeMutex;

std::map<int, std::vector<int>> g_arrange;

bool g_arrangeRead = false;

const std::map<int, std::vector<int>>& StageArrange() {
    std::lock_guard lock(g_arrangeMutex);
    if (g_arrangeRead || !g_base || !ue4::EnsureObjects(g_base)) return g_arrange;
    const auto* asset = static_cast<const uint8_t*>(ue4::FindObject("DA_StageArrange", "KWStageArrangeDataAsset"));
    constexpr size_t kInfoBytes = 0x18, kItemBytes = 0x30;
    constexpr uint32_t kInfoItemsOff = 0x8;
    int n = 0;
    const uint8_t* infos = AssetList(asset, &n);
    if (!infos) return g_arrange;
    g_arrangeRead = true;
    for (int i = 0; i < n && IsReadable(infos + i * kInfoBytes, kInfoBytes); i++) {
        const uint8_t* info = infos + i * kInfoBytes;
        const auto* items = *reinterpret_cast<const uint8_t* const*>(info + kInfoItemsOff);
        const int m = *reinterpret_cast<const int32_t*>(info + kInfoItemsOff + kArrayNumOff);
        for (int k = 0; items && k < m && IsReadable(items + k * kItemBytes, 4); k++) {
            g_arrange[*reinterpret_cast<const int32_t*>(info)].push_back(*reinterpret_cast<const int32_t*>(items + k * kItemBytes));
        }
    }
    Log("flow: %zu stages have settings", g_arrange.size());
    return g_arrange;
}

} // namespace

std::vector<int> FlowStageSettings(int base) {
    const auto& arrange = StageArrange();
    const auto it = arrange.find(base);
    return it == arrange.end() ? std::vector<int>{} : it->second;
}

int FlowStageSettingBase(int index) {
    for (const auto& [base, settings] : StageArrange()) {
        if (std::find(settings.begin(), settings.end(), index) != settings.end()) return base;
    }
    return -1;
}

bool FlowIsMenuStage(int index) {
    if (!FlowIsPlayableStage(index) || FlowStageSettingBase(index) >= 0) return false;
    int count = 0;
    const uint8_t* e = StageEntries(&count) + index * kStageEntrySize;
    const std::string code = FStringText(e);
    return code != "RND" && FStringText(e + kStageAssetOff).find("/" + code + "/") != std::string::npos && !FlowStageName(index).empty();
}

bool FlowIsSelectableStage(int index) {
    if (FlowIsMenuStage(index)) return true;
    const int base = FlowStageSettingBase(index);
    return base >= 0 && FlowIsMenuStage(base) && FlowIsPlayableStage(index);
}

int FlowRandomStage(uint64_t roll, bool ownedOnly, const std::vector<std::string>& off) {
    std::vector<RandomStageChoice> stages;
    for (int i = 0; i < FlowStageCount(); i++) {
        if (!FlowIsSelectableStage(i) || (ownedOnly && !OwnsEntitlement(FlowStageAvailableCode(i)))) continue;
        const int base = FlowStageSettingBase(i);
        stages.push_back({i, FlowStageCode(i), base >= 0 ? FlowStageCode(base) : ""});
    }
    return PickRandomStage(stages, roll, off);
}

namespace {

constexpr uint32_t kCharaPersonalData = 0x18;

constexpr uint32_t kPersonalEnglishName = 0x50;

std::atomic<int> g_charaTableOffset{-1};

} // namespace

namespace flow {

const uint8_t* CharaEntries(int* count) {
    constexpr int kSingletonFields = 0x300;
    uint8_t* singleton = GameSingleton();
    if (!singleton || !IsReadable(singleton, kSingletonFields)) return nullptr;
    if (g_charaTableOffset < 0) {
        for (int off = kAssetListOff; off < kSingletonFields; off += sizeof(void*)) {
            const void* p = *reinterpret_cast<void* const*>(singleton + off);
            if (p && IsReadable(p, kAssetListOff + kArrayBytes) && ue4::ObjectName(ue4::ClassOf(p)) == "KWCharaEntryDataAsset") {
                g_charaTableOffset = off;
                break;
            }
        }
        if (g_charaTableOffset < 0) return nullptr;
    }
    return AssetList(*reinterpret_cast<uint8_t* const*>(singleton + g_charaTableOffset), count);
}

} // namespace flow

int FlowCharaCount() {
    int count = 0;
    return CharaEntries(&count) ? count : 0;
}

std::string FlowCharaCode(int index) {
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    return FStringText(entries + index * kCharaEntrySize);
}

std::string FlowCharaName(int index) {
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    const uint8_t* e = entries + index * kCharaEntrySize;
    const uint8_t* personal = IsReadable(e + kCharaPersonalData, 8) ? *reinterpret_cast<const uint8_t* const*>(e + kCharaPersonalData) : nullptr;
    if (!personal || !IsReadable(personal + kPersonalEnglishName, 8)) return "";
    const uint8_t* display = *reinterpret_cast<const uint8_t* const*>(personal + kPersonalEnglishName);
    return display ? FStringText(display) : "";
}

void FlowLogCharaCostumes(const std::string& code) {
    if (!ue4::EnsureObjects(g_base)) return;
    std::map<std::string, std::pair<int, int>> previews;
    std::vector<const uint8_t*> sponsored, general;
    ue4::ForEachObject([&](const void* object) {
        const std::string kind = ue4::ObjectName(ue4::ClassOf(object));
        const auto* o = static_cast<const uint8_t*>(object);
        if (kind == "KWCharaCostumeColorPreviewDataAsset" && IsReadable(o + 0x28, 0x20)) {
            previews[ue4::ObjectName(object)] = {*reinterpret_cast<const int32_t*>(o + 0x30), *reinterpret_cast<const int32_t*>(o + 0x40)};
        }
        if ((kind == "KWCostumeDataAsset" || kind == "KWBattleCharaCostumeDataAsset") && IsReadable(o + 0x98, 0x20)) {
            const auto* cluster = *reinterpret_cast<const uint8_t* const*>(o + 0x50);
            int maxColors = -1, maxSpecial = -1;
            if (cluster && IsReadable(cluster + 0x28, 16)) {
                const auto* items = *reinterpret_cast<const uint8_t* const*>(cluster + 0x28);
                for (int j = 0; items && j < *reinterpret_cast<const int32_t*>(cluster + 0x30); j++) {
                    maxColors = std::max(maxColors, *reinterpret_cast<const int32_t*>(items + j * 0x50 + 0x18));
                    maxSpecial = std::max(maxSpecial, *reinterpret_cast<const int32_t*>(items + j * 0x50 + 0x28));
                }
            }
            Log("costumes: loaded %s: material presets %d, sponsored %d, max colours %d, special %d", ue4::ObjectName(object).c_str(),
                *reinterpret_cast<const int32_t*>(o + 0xA0), *reinterpret_cast<const int32_t*>(o + 0xB0), maxColors, maxSpecial);
        }
        if (kind == "KWCharaSponsoredCostumeDataAsset") sponsored.push_back(o);
        if (kind == "KWGeneralCostumeAvailableCheckDataAsset") general.push_back(o);
    });
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    for (int i = 0; entries && i < count; i++) {
        if (FlowCharaCode(i) != code) continue;
        const uint8_t* e = entries + i * kCharaEntrySize;
        const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + 0x50);
        const int num = *reinterpret_cast<const int32_t*>(e + 0x58);
        const auto* colors = *reinterpret_cast<const uint8_t* const*>(e + 0x70);
        const int colorNum = *reinterpret_cast<const int32_t*>(e + 0x78);
        if (!previews.empty()) Log("costumes: %zu colour previews, first %s", previews.size(), previews.begin()->first.c_str());
        Log("costumes %s (prog id %d, unique id %d): %d", code.c_str(), *reinterpret_cast<const int32_t*>(e + kCharaProgIdOff),
            *reinterpret_cast<const int32_t*>(e + kCharaUniqueIdOff), num);
        for (int k = 0; k < num; k++) {
            const std::string preview = colors && k < colorNum ? FStringText(colors + k * 16) : "";
            auto it = preview.size() == 5 ? previews.find("DA_" + preview.substr(0, 3) + "_CostumeColorPreview_" + preview.substr(3))
                                          : previews.end();
            Log("costumes %s:   [%d] %s variants %d colors %s %d special %d", code.c_str(), k, FStringText(paths + k * 16).c_str(),
                FlowCostumeVariants(i, k),
                it == previews.end() ? preview.c_str() : it->first.c_str(),
                it == previews.end() ? -1 : it->second.first, it == previews.end() ? -1 : it->second.second);
        }
        for (const uint8_t* o : sponsored) {
            const auto* rows = *reinterpret_cast<const uint8_t* const*>(o + 0x28);
            for (int r = 0; rows && r < *reinterpret_cast<const int32_t*>(o + 0x30); r++) {
                const uint8_t* row = rows + r * 0x18;
                if (*reinterpret_cast<const int32_t*>(row) != *reinterpret_cast<const int32_t*>(e + kCharaProgIdOff) &&
                    *reinterpret_cast<const int32_t*>(row) != *reinterpret_cast<const int32_t*>(e + kCharaUniqueIdOff)) continue;
                std::string similar;
                const auto* ids = *reinterpret_cast<const int32_t* const*>(row + 8);
                for (int j = 0; ids && j < *reinterpret_cast<const int32_t*>(row + 0x10); j++) similar += " " + std::to_string(ids[j]);
                Log("costumes %s: sponsored (%s) chara %d costume %d similar%s", code.c_str(), ue4::ObjectName(o).c_str(),
                    *reinterpret_cast<const int32_t*>(row), *reinterpret_cast<const int32_t*>(row + 4), similar.c_str());
            }
        }
        for (const uint8_t* o : general) {
            const auto* rows = *reinterpret_cast<const uint8_t* const*>(o + 0x28);
            for (int r = 0; rows && r < *reinterpret_cast<const int32_t*>(o + 0x30); r++) {
                const uint8_t* row = rows + r * 0x20;
                if (FStringText(row) != code) continue;
                std::string list;
                const auto* ids = *reinterpret_cast<const int32_t* const*>(row + 0x10);
                for (int j = 0; ids && j < *reinterpret_cast<const int32_t*>(row + 0x18); j++) list += " " + std::to_string(ids[j]);
                Log("costumes %s: general story available (%s):%s", code.c_str(), ue4::ObjectName(o).c_str(), list.c_str());
            }
        }
        return;
    }
    Log("costumes: no character %s", code.c_str());
}

int FlowCharaCostumeCount(int index) {
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return 0;
    const uint8_t* e = entries + index * kCharaEntrySize + kCharaCostumes;
    return IsReadable(e, 16) ? *reinterpret_cast<const int32_t*>(e + 8) : 0;
}

namespace {

constexpr uint32_t kDoesPackageExist = 0x11EE840;

constexpr uint8_t kDoesPackageExistBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                              0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};

} // namespace

namespace flow {

bool PackageExists(const std::string& path) {
    using ExistsFn = bool (*)(const void* name, const void* guid, void* filename);
    static const bool ok = g_base && std::memcmp(reinterpret_cast<const void*>(g_base + kDoesPackageExist),
                                                 kDoesPackageExistBytes, sizeof(kDoesPackageExistBytes)) == 0;
    if (!ok) return false;
    const FStringArg name(std::wstring(path.begin(), path.end()));
    return reinterpret_cast<ExistsFn>(g_base + kDoesPackageExist)(name.Get(), nullptr, nullptr);
}

} // namespace flow

uint8_t FlowCostumeVariants(int index, int costume) {
    static std::mutex mutex;
    static std::map<std::string, uint8_t> known;
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return 0;
    const uint8_t* e = entries + index * kCharaEntrySize;
    const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
    if (!paths || costume < 0 || costume >= *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8)) return 0;
    const std::string path = FStringText(paths + costume * 16);
    std::lock_guard lock(mutex);
    if (const auto it = known.find(path); it != known.end()) return it->second;
    auto listed = [&path](const auto& table) {
        return std::any_of(std::begin(table), std::end(table), [&path](const char* p) { return path == p; });
    };
    uint8_t variants = listed(variant_table::kPresetVariants) ? kVariantDress : 0;
    if (listed(variant_table::kUrienThong) || listed(variant_table::kAkiraHelmet)) variants |= kVariantSpecial;
    std::vector<std::string> bases{path};
    const size_t slash = path.find('/'), name = path.rfind("/DA_");
    if (slash != std::string::npos && name != std::string::npos && path.size() > name + 8 && path[name + 7] == '_') {
        std::string other = path;
        other.replace(name + 4, 3, path.substr(0, slash));
        if (other != path) bases.push_back(other);
    }
    for (const std::string& base : bases) {
        if (PackageExists("/Game/Chara/" + base + "_DressState_01")) variants |= kVariantDress;
    }
    known[path] = variants;
    return variants;
}

std::optional<uint8_t> FlowCostumeVariantsKnown(int index, int costume) {
    static std::mutex mutex;
    static std::map<std::pair<int, int>, int> known;
    const std::pair<int, int> key{index, costume};
    {
        std::lock_guard lock(mutex);
        const auto [it, asked] = known.try_emplace(key, -1);
        if (!asked) return it->second < 0 ? std::nullopt : std::optional<uint8_t>(static_cast<uint8_t>(it->second));
    }
    std::thread([key] {
        ThreadGuard running;
        const uint8_t variants = FlowCostumeVariants(key.first, key.second);
        std::lock_guard lock(mutex);
        known[key] = variants;
    }).detach();
    return std::nullopt;
}

const char* FlowSpecialVariantName(const std::string& code) {
    if (code == "URN") return "Thong";
    if (code == "Z41") return "Helmet off";
    return nullptr;
}

std::string FlowVariantLabel(const std::string& code, uint8_t variation) {
    const char* special = FlowSpecialVariantName(code);
    const bool dress = variation & kVariantDress, other = (variation & kVariantSpecial) && special;
    if (dress && other) return std::string("Variant + ") + special;
    if (other) return special;
    return dress ? "Variant" : "Normal";
}

namespace {

std::mutex g_previewMutex;

std::map<std::string, int> g_previewColors;

std::map<std::string, const uint8_t*> g_previewAssets;

bool g_previewsRead = false;

int PreviewColors(const std::string& name) {
    std::lock_guard lock(g_previewMutex);
    if (!g_previewsRead && ue4::EnsureObjects(g_base)) {
        g_previewsRead = true;
        ue4::ForEachObject([](const void* object) {
            const auto* o = static_cast<const uint8_t*>(object);
            if (ue4::ObjectName(ue4::ClassOf(object)) == "KWCharaCostumeColorPreviewDataAsset" && IsReadable(o + kPreviewColorsOff, kArrayBytes)) {
                g_previewColors[ue4::ObjectName(object)] = *reinterpret_cast<const int32_t*>(o + kPreviewColorsOff + kArrayNumOff);
                g_previewAssets[ue4::ObjectName(object)] = o;
            }
        });
        if (g_previewColors.empty()) Log("flow: no costume colour previews loaded");
    }
    const auto it = g_previewColors.find(name);
    return it == g_previewColors.end() ? -1 : it->second;
}

} // namespace

namespace {

std::string CostumePreviewKey(int index, int costume) {
    constexpr uint32_t kEntryPreviewsOff = 0x70;
    constexpr size_t kKeyLength = 5;
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return "";
    const uint8_t* e = entries + index * kCharaEntrySize;
    const auto* keys = *reinterpret_cast<const uint8_t* const*>(e + kEntryPreviewsOff);
    if (!keys || costume < 0 || costume >= *reinterpret_cast<const int32_t*>(e + kEntryPreviewsOff + kArrayNumOff)) return "";
    const std::string key = FStringText(keys + costume * kArrayBytes);
    return key.size() == kKeyLength ? key : "";
}

} // namespace

int FlowCostumeColorCount(int index, int costume) {
    const std::string key = CostumePreviewKey(index, costume);
    if (key.empty()) return 0;
    const int colors = PreviewColors("DA_" + key.substr(0, 3) + "_CostumeColorPreview_" + key.substr(3));
    return colors > 0 ? colors : 2;
}

CostumeSwatches FlowCostumeSwatchColors(int index, int costume) {
    CostumeSwatches out;
    const std::string key = CostumePreviewKey(index, costume);
    if (key.empty()) return out;
    PreviewColors("");
    std::lock_guard lock(g_previewMutex);
    const auto it = g_previewAssets.find("DA_" + key.substr(0, 3) + "_CostumeColorPreview_" + key.substr(3));
    if (it == g_previewAssets.end()) return out;
    auto read = [](const uint8_t* array, std::vector<std::pair<uint32_t, uint32_t>>& to) {
        constexpr int kMaxColors = 64;
        constexpr uint32_t kOpaque = 0xFF000000u;
        if (!IsReadable(array, kArrayBytes)) return;
        const auto* data = *reinterpret_cast<const uint32_t* const*>(array);
        const int n = *reinterpret_cast<const int32_t*>(array + kArrayNumOff);
        if (!data || n <= 0 || n > kMaxColors || !IsReadable(data, n * 2 * sizeof(uint32_t))) return;
        for (int k = 0; k < n; k++) to.emplace_back(data[k * 2] | kOpaque, data[k * 2 + 1] | kOpaque);
    };
    read(it->second + kPreviewColorsOff, out.colors);
    read(it->second + kPreviewExColorsOff, out.ex);
    return out;
}

int FlowCostumeInFolder(int index, const std::string& folder) {
    int count = 0;
    const uint8_t* entries = CharaEntries(&count);
    if (!entries || index < 0 || index >= count) return -1;
    const uint8_t* e = entries + index * kCharaEntrySize;
    const auto* paths = *reinterpret_cast<const uint8_t* const*>(e + kCharaCostumes);
    const int n = *reinterpret_cast<const int32_t*>(e + kCharaCostumes + 8);
    const std::string part = "/SkelMesh/" + folder + "/";
    for (int k = 0; paths && k < n && k < 64; k++) {
        if (FStringText(paths + k * 16).find(part) != std::string::npos) return k;
    }
    return -1;
}

bool FlowElevenStandIn(uint32_t roll, std::string* code, int* costume) {
    std::vector<std::pair<std::string, int>> fighters;
    for (int i = 0; i < FlowCharaCount(); i++) {
        if (FlowCharaCostumeCount(i) < 10 || FlowCharaCode(i).rfind('X', 0) == 0) continue;
        const int k = FlowCostumeInFolder(i, "0C");
        if (k >= 0) fighters.emplace_back(FlowCharaCode(i), k);
    }
    if (fighters.empty()) return false;
    const auto& f = fighters[roll % fighters.size()];
    *code = f.first;
    *costume = f.second;
    return true;
}

int FlowCharaIndex(const std::string& code) {
    for (int i = 0; i < FlowCharaCount(); i++) {
        if (FlowCharaCode(i) == code) return i;
    }
    return -1;
}

namespace flow {

std::string FTextText(const uint8_t* text) {
    if (!text || !IsReadable(text, 8)) return "";
    const uint8_t* display = *reinterpret_cast<const uint8_t* const*>(text);
    return display ? FStringText(display) : "";
}

} // namespace flow

std::string PropertyText(const uint8_t* property, const uint8_t* data) {
    const std::string type = ue4::ObjectName(ue4::ClassOf(property));
    const uint8_t* p = data + *reinterpret_cast<const int32_t*>(property + kPropertyOffsetOff);
    char buf[64];
    if (type == "IntProperty") return std::to_string(*reinterpret_cast<const int32_t*>(p));
    if (type == "ByteProperty") return std::to_string(*p);
    if (type == "BoolProperty") return *p ? "true" : "false";
    if (type == "FloatProperty") {
        std::snprintf(buf, sizeof(buf), "%g", *reinterpret_cast<const float*>(p));
        return buf;
    }
    if (type == "NameProperty") return ue4::NameText(*reinterpret_cast<const int32_t*>(p));
    if (type == "StrProperty") return "\"" + FStringText(p) + "\"";
    if (type == "TextProperty") return "'" + FTextText(p) + "'";
    if (type == "ArrayProperty") return "[" + std::to_string(*reinterpret_cast<const int32_t*>(p + 8)) + "]";
    return "(" + type + ")";
}

} // namespace sfv::mod
