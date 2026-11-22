// The battle's setup: a match's players, rules and stage written into the battle data (docs/flow.md).

#include "mod/battle_setup.h"

#include "mod/flow.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/stack.h"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace sfv::mod {

namespace {

namespace bd = battledata;

constexpr uint32_t kEngineGlobal = 0x3D878C8;
constexpr uint32_t kGameInstance = 0x1E0;
constexpr uint32_t kGetBattleSettingSlot = 0x238;
constexpr uint32_t kBattleDataPtr = 0x30;
constexpr int kDressRounds = 7;
constexpr int kUniqueParamCount = 8;
constexpr int kUrienLookParam = 2, kUrienLookSpecial = 2;
constexpr int kAkiraLookParam = 0, kAkiraLookSpecial = 1;
constexpr size_t kCodeChars = 3, kCodeWide = 4, kCodeField = 12;
constexpr int kMaxBestOf = 5, kMaxTimerSeconds = 99, kFramesPerSecond = 60;
constexpr int kPickFailuresLogged = 10;

constexpr game::CodeSite kCheckPlayers{
    "BattleSetting::CheckPlayers", 0x7B33B0, {0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xF9, 0xE8, 0xD0, 0xE2, 0xD6, 0xFF}};
constexpr game::CodeSite kReadSettings{
    "BattleActor::ReadSettings", 0x6A45E0, {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x4B, 0x08, 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48}};

GameImage g_image;
uint64_t (*g_origCheckPlayers)(uint8_t*, void*, void*, void*) = nullptr;
uint64_t (*g_origReadSettings)(uint8_t*, void*, void*, void*) = nullptr;
std::mutex g_mutex;
bool g_pending = false;
MatchSetup g_setup;
int g_uniqueParams[kNumPlayers][kUniqueParamCount] = {{-1, -1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1}};
int g_specRoundsForTest = 0;
std::atomic<int> g_setups{0};
std::atomic<int> g_creations{0};

template <typename T>
T Get(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const T*>(p + off); }

template <typename T>
void Put(uint8_t* p, uint32_t off, T v) { *reinterpret_cast<T*>(p + off) = v; }

void ApplyPending(const char* when) {
    std::lock_guard lock(g_mutex);
    if (!g_pending) return;
    if (BattleSetupApply(g_setup)) Log("battle setup applied %s: %s", when, BattleSetupDescribe(g_setup).c_str());
}

uint64_t CheckPlayersDetour(uint8_t* self, void* a2, void* a3, void* a4) {
    HookGuard guard;
    g_setups++;
    LogStack("CheckPlayers", reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    ApplyPending("at character select end");
    return g_origCheckPlayers(self, a2, a3, a4);
}

uint64_t ReadSettingsDetour(uint8_t* self, void* a2, void* a3, void* a4) {
    HookGuard guard;
    g_creations++;
    LogStack("ReadSettings", reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    ApplyPending("at battle creation");
    if (BattleSetupHasPending() || !FlowPendingStage().empty()) Log("battle setup: pending setup and stage used up");
    BattleSetupClearPending();
    FlowSetPendingStage("");
    return g_origReadSettings(self, a2, a3, a4);
}

bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original) {
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("battle setup: signature mismatch at %s", site.name);
        return false;
    }
    return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

} // namespace

bool BattleSetupInstall(const GameImage& image) {
    g_image = image;
    const bool ok = Hook(image, kCheckPlayers, reinterpret_cast<void*>(&CheckPlayersDetour),
                         reinterpret_cast<void**>(&g_origCheckPlayers)) &&
                    Hook(image, kReadSettings, reinterpret_cast<void*>(&ReadSettingsDetour),
                         reinterpret_cast<void**>(&g_origReadSettings));
    Log("battle setup hooks %s", ok ? "installed" : "NOT installed");
    return ok;
}

namespace {

uint8_t* BattleSetting() {
    if (!g_image.base) return nullptr;
    auto* engine = Get<uint8_t*>(reinterpret_cast<uint8_t*>(g_image.base), kEngineGlobal);
    if (!IsReadable(engine, kGameInstance + 8)) return nullptr;
    auto* instance = Get<uint8_t*>(engine, kGameInstance);
    if (!IsReadable(instance, 8)) return nullptr;
    auto* vtable = Get<uint8_t*>(instance, 0);
    if (!IsReadable(vtable, kGetBattleSettingSlot + 8)) return nullptr;
    const auto fn = Get<uintptr_t>(vtable, kGetBattleSettingSlot);
    if (fn < g_image.base || fn >= g_image.base + 0x5000000) return nullptr;
    auto* setting = reinterpret_cast<uint8_t* (*)(void*)>(fn)(instance);
    return IsReadable(setting, situation::kObject + 8) ? setting : nullptr;
}

uint8_t* SituationData() {
    uint8_t* setting = BattleSetting();
    auto* object = setting ? Get<uint8_t*>(setting, situation::kObject) : nullptr;
    if (!IsReadable(object, situation::kData + 8)) return nullptr;
    auto* data = Get<uint8_t*>(object, situation::kData);
    return IsReadable(data, situation::kBattleCount + 4) ? data : nullptr;
}

} // namespace

uint8_t* BattleData() {
    uint8_t* setting = BattleSetting();
    auto* data = setting ? Get<uint8_t*>(setting, kBattleDataPtr) : nullptr;
    return IsReadable(data, 2 * bd::kRecordStride) ? data : nullptr;
}

bool BattleSetupRead(MatchSetup* out) {
    uint8_t* data = BattleData();
    if (!data) return false;
    MatchSetup s;
    for (int p = 0; p < kNumPlayers; p++) {
        const uint8_t* r = data + p * bd::kRecordStride;
        char code[5] = {};
        std::memcpy(code, r + bd::kCharaCode, 4);
        s.players[p].character = PackCode(code);
        s.players[p].costume = static_cast<uint16_t>(Get<int32_t>(r, bd::kCostume));
        s.players[p].color = static_cast<uint8_t>(Get<int32_t>(r, bd::kColor));
        const int32_t comment = Get<int32_t>(r, bd::kComment);
        s.players[p].comment = static_cast<uint8_t>(comment < 0 ? 0 : comment + 1);
        s.players[p].vskill = static_cast<uint8_t>(Get<int32_t>(r, bd::kVSkill));
        s.players[p].vtrigger = static_cast<uint8_t>(Get<int32_t>(r, bd::kVTrigger));
        uint8_t variation = Get<int32_t>(r, bd::kUseDressState) && Get<int32_t>(r, bd::kDressStates) ? kVariantDress : 0;
        if ((std::strcmp(code, "URN") == 0 && Get<int32_t>(r, bd::kUniqueParams + 4 * kUrienLookParam) == kUrienLookSpecial) ||
            (std::strcmp(code, "Z41") == 0 && Get<int32_t>(r, bd::kUniqueParams + 4 * kAkiraLookParam) == kAkiraLookSpecial)) {
            variation |= kVariantSpecial;
        }
        s.players[p].variation = variation;
        s.players[p].version = static_cast<int16_t>(Get<int32_t>(r, bd::kCharaVersion));
    }
    s.stage = static_cast<uint16_t>(Get<int32_t>(data, bd::kStage));
    *out = s;
    return true;
}

bool BattleSetupApply(const MatchSetup& setup) {
    uint8_t* data = BattleData();
    if (!data) {
        Log("battle setup: no battle data to write to");
        return false;
    }
    for (int p = 0; p < kNumPlayers; p++) {
        uint8_t* r = data + p * bd::kRecordStride;
        const PlayerSetup& ps = setup.players[p];
        if (ps.character) {
            const std::string code = UnpackCode(ps.character);
            uint8_t field[kCodeField] = {};
            for (size_t i = 0; i < code.size() && i < kCodeChars; i++) {
                field[i] = static_cast<uint8_t>(code[i]);
                field[kCodeWide + 2 * i] = static_cast<uint8_t>(code[i]);
            }
            std::memcpy(r + bd::kCharaCode, field, sizeof(field));
        }
        Put<int32_t>(r, bd::kCharaVersion, ps.version >= 0 ? ps.version : -1);
        Put<int32_t>(r, bd::kCostume, ps.costume);
        Put<int32_t>(r, bd::kColor, ps.color);
        Put<int32_t>(r, bd::kComment, ps.comment == 0 ? -1 : ps.comment - 1);
        Put<int32_t>(r, bd::kVSkill, ps.vskill);
        Put<int32_t>(r, bd::kVTrigger, ps.vtrigger);
        Put<int32_t>(r, bd::kHandicap, 0);
        const int dress = ps.variation & kVariantDress ? 1 : 0;
        for (int round = 0; round < kDressRounds; round++) Put<int32_t>(r, bd::kDressStates + 4 * round, dress);
        Put<int32_t>(r, bd::kUseDressState, dress);
        const bool special = ps.variation & kVariantSpecial;
        const std::string chara(reinterpret_cast<const char*>(r + bd::kCharaCode), kCodeChars);
        if (chara == "URN") Put<int32_t>(r, bd::kUniqueParams + 4 * kUrienLookParam, special ? kUrienLookSpecial : 0);
        if (chara == "Z41") Put<int32_t>(r, bd::kUniqueParams + 4 * kAkiraLookParam, special ? kAkiraLookSpecial : 0);
        if (const int voice = FlowVoiceLanguageFor(chara); voice >= 0) Put<int32_t>(r, bd::kVoiceLanguage, voice);
        for (int k = 0; k < kUniqueParamCount; k++) {
            if (g_uniqueParams[p][k] >= 0) Put<int32_t>(r, bd::kUniqueParams + 4 * k, g_uniqueParams[p][k]);
        }
        Put<int32_t>(r, bd::kWins, setup.wins[p]);
        Put<int32_t>(r, bd::kWinStreak, setup.streaks[p]);
        Put<int32_t>(r, bd::kCtrlType, 0);
        Put<int32_t>(r, bd::kPadId, p);
    }
    Put<int32_t>(data, bd::kMode, 1);
    const int bestOf = std::clamp(setup.roundsToWin * 2 - 1, 1, kMaxBestOf);
    const int seconds = setup.timerSeconds ? std::clamp<int>(setup.timerSeconds, 1, kMaxTimerSeconds) : kMaxTimerSeconds;
    Put<int32_t>(data, bd::kRounds, bestOf);
    Put<int32_t>(data, bd::kTimer, seconds * kFramesPerSecond);
    Put<int32_t>(data, bd::kSpecRounds, g_specRoundsForTest ? g_specRoundsForTest : bestOf);
    Put<int32_t>(data, bd::kVersusFlag, 1);
    if (setup.stage != kKeepStage) {
        const int32_t before = Get<int32_t>(data, bd::kStage);
        Put<int32_t>(data, bd::kStage, setup.stage);
        if (before != setup.stage) Log("battle setup: stage index %d -> %u", before, setup.stage);
    }
    const std::string stageCode = FlowJumpStageCode();
    if (stageCode.size() == kCodeChars && IsReadable(data + bd::kStageName, bd::kStageNameSize)) {
        char* name = reinterpret_cast<char*>(data + bd::kStageName);
        if (std::strncmp(name, stageCode.c_str(), bd::kStageNameSize) != 0) {
            char before[bd::kStageNameSize + 1] = {};
            std::memcpy(before, name, bd::kStageNameSize);
            std::memset(name, 0, bd::kStageNameSize);
            std::memcpy(name, stageCode.data(), stageCode.size());
            Log("battle setup: stage name '%s' -> '%s'", before, stageCode.c_str());
        }
    }
    if (uint8_t* s = SituationData()) {
        const int32_t before = Get<int32_t>(s, situation::kBattleCount);
        Put<int32_t>(s, situation::kBattleCount, setup.played);
        if (before != setup.played) Log("battle setup: matches played %d -> %u", before, setup.played);
    } else {
        Log("battle setup: no battle situation to write the matches played to");
    }
    return true;
}

void BattleSetupSetRoundsForTest(int rounds) { g_specRoundsForTest = rounds; }

bool BattleSetupIsEleven(const PlayerSetup& p) {
    const int index = FlowCharaIndex(UnpackCode(p.character));
    return index >= 0 && p.costume == FlowCostumeInFolder(index, "0C");
}

LookRules BattleSetupLookRules() {
    LookRules rules;
    rules.isEleven = BattleSetupIsEleven;
    rules.elevenOtherColor = 1;
    rules.colorsOf = [](const PlayerSetup& p) { return BattleSetupColorIds(FlowCharaIndex(UnpackCode(p.character)), p.costume); };
    return rules;
}

bool BattleSetupIsFighter(int index) {
    if (FlowCharaCostumeCount(index) < 10 || FlowCharaCode(index).rfind('X', 0) == 0) return false;
    const std::string name = FlowCharaName(index);
    return name.empty() || (name[0] != '*' && name.rfind("ID_", 0) != 0);
}

std::vector<int> BattleSetupColorIds(int index, int costume) {
    const auto specs = FlowCharaCostumeSpecs(index);
    const auto spec = specs.find(costume);
    if (spec != specs.end() && !spec->second.colors.empty()) return spec->second.colors;
    std::vector<int> ids;
    for (int k = 0; k < FlowCostumeColorCount(index, costume); k++) ids.push_back(k);
    return ids;
}

PickTables BattleSetupPickTables() {
    PickTables t;
    t.charaIndex = FlowCharaIndex;
    t.isFighter = BattleSetupIsFighter;
    t.costumeCount = FlowCharaCostumeCount;
    t.colorCount = FlowCostumeColorCount;
    t.colorIds = BattleSetupColorIds;
    t.isEleven = BattleSetupIsEleven;
    t.skillCount = FlowSkillCountKnown;
    t.commentCount = FlowVCommentCount;
    t.variants = FlowCostumeVariantsKnown;
    t.isPlayableStage = FlowIsPlayableStage;
    return t;
}

void BattleSetupLogPickCheck() {
    const PickTables tables = BattleSetupPickTables();
    int checked = 0, failed = 0;
    for (int index = 0; index < FlowCharaCount(); index++) {
        if (!BattleSetupIsFighter(index) || FlowCharaName(index).empty()) continue;
        const std::string code = FlowCharaCode(index);
        PlayerSetup pick;
        pick.character = PackCode(code.c_str());
        pick.vskill = static_cast<uint8_t>(std::max<size_t>(tables.skillCount(code, false), 2) - 1);
        pick.vtrigger = static_cast<uint8_t>(std::max<size_t>(tables.skillCount(code, true), 2) - 1);
        pick.comment = static_cast<uint8_t>(FlowVCommentNames(code).size());
        const int eleven = FlowCostumeInFolder(index, "0C");
        for (int costume = 0; costume < FlowCharaCostumeCount(index); costume++) {
            const bool offered = FlowCostumeColorCount(index, costume) > 0;
            if (!offered && costume != eleven) continue;
            pick.costume = static_cast<uint16_t>(costume);
            pick.variation = FlowCostumeVariants(index, costume);
            const std::vector<int> colors = offered ? BattleSetupColorIds(index, costume) : std::vector<int>{0, 1};
            for (int color : colors) {
                pick.color = static_cast<uint8_t>(color);
                checked++;
                const PickVerdict v = PickCheck(pick, tables);
                if (!v.Fine() && ++failed <= kPickFailuresLogged) Log("pickcheck: refused %s", v.kind == PickVerdict::Kind::Bad ? v.why.c_str() : "(V-Comments not loaded)");
            }
        }
    }
    int stages = 0, stagesFailed = 0;
    for (int stage = 0; stage < FlowStageCount(); stage++) {
        if (!FlowIsSelectableStage(stage)) continue;
        stages++;
        if (!StageRuleCheck(static_cast<uint16_t>(stage), tables).Fine()) stagesFailed++;
    }
    Log("pickcheck: %d offered picks, %d refused; %d selectable stages, %d refused", checked, failed, stages, stagesFailed);
}

void BattleSetupSetUniqueParam(int player, int index, int value) {
    if (player >= 0 && player < kNumPlayers && index >= 0 && index < kUniqueParamCount) g_uniqueParams[player][index] = value;
}

void BattleSetupSetPending(const MatchSetup& setup) {
    std::lock_guard lock(g_mutex);
    g_setup = setup;
    g_pending = true;
}

bool BattleSetupHasPending() {
    std::lock_guard lock(g_mutex);
    return g_pending;
}

void BattleSetupClearPending() {
    std::lock_guard lock(g_mutex);
    g_pending = false;
}

int BattleSetupCount() { return g_setups; }

int BattleSetupCreations() { return g_creations; }

std::string BattleSetupDescribe(const MatchSetup& s) {
    std::string out;
    char buf[160];
    for (int p = 0; p < kNumPlayers; p++) {
        const PlayerSetup& ps = s.players[p];
        const std::string version = ps.version >= 0 ? " version " + std::to_string(ps.version) : "";
        std::snprintf(buf, sizeof(buf), "P%d %s costume %u color %u comment %u V-Skill %u V-Trigger %u%s%s | ", p + 1,
                      UnpackCode(ps.character).c_str(), ps.costume, ps.color + 1, ps.comment, ps.vskill + 1,
                      ps.vtrigger + 1, ps.variation ? (" " + FlowVariantLabel(UnpackCode(ps.character), ps.variation)).c_str() : "",
                      version.c_str());
        out += buf;
    }
    if (s.stage == kKeepStage) return out + "stage as prepared";
    std::snprintf(buf, sizeof(buf), "stage %u %s", s.stage, FlowStageCode(s.stage).c_str());
    return out + buf;
}

void BattleSetupLog(const char* when) {
    MatchSetup s;
    if (!BattleSetupRead(&s)) {
        Log("battle setup %s: not available", when);
        return;
    }
    const uint8_t* data = BattleData();
    Log("battle setup %s: %s (rounds %d, timer %d, versions %d/%d)", when, BattleSetupDescribe(s).c_str(),
        Get<int32_t>(data, bd::kRounds), Get<int32_t>(data, bd::kTimer), Get<int32_t>(data, bd::kCharaVersion),
        Get<int32_t>(data + bd::kRecordStride, bd::kCharaVersion));
}

} // namespace sfv::mod
