#include "mod/profile.h"

#include "mod/image.h"
#include "mod/log.h"
#include "mod/ue4.h"

#include <windows.h>

#include <chrono>
#include <unordered_map>

namespace sfv::mod {

namespace {

// KWUserProfileDetails.
constexpr size_t kIsMainUser = 0x28;          // bit 0
constexpr size_t kFavorites = 0x68;           // KWFavoriteBattleSettings
constexpr size_t kFavoriteChara = kFavorites; // FString CharaCode
constexpr size_t kFavoriteStage = 0x90;       // FString StageCode
constexpr size_t kCharaSettings = 0xA0;       // TArray of 0x38-byte entries
constexpr size_t kRandomMask = 0x108;         // KWRandomStageSelectSetting: MaskStageCodes TArray<FString>
constexpr size_t kProfileSize = 0x118;
constexpr int kMaxMask = 128;
// One CharaSettings entry.
constexpr size_t kEntrySize = 0x38;
constexpr size_t kEntryCostume = 0x10, kEntryColor = 0x14, kEntryComment = 0x28, kEntryVTrigger = 0x2C, kEntryVSkill = 0x30;
constexpr int kMaxEntries = 128;

// An FString's text (ASCII codes), empty if it is not readable.
std::string Code(const uint8_t* field) {
    const auto* data = *reinterpret_cast<const wchar_t* const*>(field);
    const int32_t n = *reinterpret_cast<const int32_t*>(field + 8);
    std::string out;
    if (!data || n <= 0 || n > 64 || !IsReadable(data, size_t(n) * 2)) return out;
    for (int i = 0; i < n && data[i]; i++) out += static_cast<char>(data[i] < 128 ? data[i] : '?');
    return out;
}

} // namespace

bool ReadProfileFavorites(ProfileFavorites* out) {
    if (!ue4::EnsureObjects(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) return false;
    const auto start = std::chrono::steady_clock::now();
    const uint8_t* main = nullptr;
    // Each class's name once (a name per object held the game ~180 ms).
    std::unordered_map<const void*, bool> isProfile;
    ue4::ForEachObject([&](const void* object) {
        if (main) return;
        const void* cls = ue4::ClassOf(object);
        auto [it, added] = isProfile.try_emplace(cls, false);
        if (added) it->second = ue4::ObjectName(cls) == "KWUserProfileDetails";
        if (!it->second) return;
        const auto* p = static_cast<const uint8_t*>(object);
        if (IsReadable(p, kProfileSize) && (p[kIsMainUser] & 1)) main = p;
    });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!main) {
        Log("profile: no main profile loaded (%.1f ms)", ms);
        return false;
    }
    ProfileFavorites f;
    f.loaded = true;
    f.chara = Code(main + kFavoriteChara);
    f.stage = Code(main + kFavoriteStage);
    const auto* entries = *reinterpret_cast<const uint8_t* const*>(main + kCharaSettings);
    const int32_t count = *reinterpret_cast<const int32_t*>(main + kCharaSettings + 8);
    for (int32_t i = 0; entries && i < count && i < kMaxEntries; i++) {
        const uint8_t* e = entries + size_t(i) * kEntrySize;
        if (!IsReadable(e, kEntrySize)) break;
        auto at = [e](size_t offset) { return *reinterpret_cast<const int32_t*>(e + offset); };
        FavoriteChara c;
        c.code = Code(e);
        c.costume = at(kEntryCostume);
        c.color = at(kEntryColor);
        c.comment = at(kEntryComment);
        c.vtrigger = at(kEntryVTrigger);
        c.vskill = at(kEntryVSkill);
        if (!c.code.empty()) f.charas.push_back(std::move(c));
    }
    const auto* mask = *reinterpret_cast<const uint8_t* const*>(main + kRandomMask);
    const int32_t masked = *reinterpret_cast<const int32_t*>(main + kRandomMask + 8);
    std::string maskText;
    for (int32_t i = 0; mask && i < masked && i < kMaxMask && IsReadable(mask + size_t(i) * 16, 16); i++) {
        std::string code = Code(mask + size_t(i) * 16);
        if (code.empty()) continue;
        maskText += (maskText.empty() ? "" : ",") + code;
        f.randomOff.push_back(std::move(code));
    }
    Log("profile: favorite character '%s', stage '%s', %zu characters' settings, random leaves out %d stages (%s) (%.1f ms)",
        f.chara.c_str(), f.stage.c_str(), f.charas.size(), masked, maskText.c_str(), ms);
    *out = std::move(f);
    return true;
}

} // namespace sfv::mod
