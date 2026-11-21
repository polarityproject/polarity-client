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

// BattleSetting::CheckPlayers(this): once, when character select ends.
constexpr game::CodeSite kCheckPlayers{
    "BattleSetting::CheckPlayers", 0x7B33B0, {0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xF9, 0xE8, 0xD0, 0xE2, 0xD6, 0xFF}};
// BattleActor::ReadSettings(this): when the battle is created.
constexpr game::CodeSite kReadSettings{
    "BattleActor::ReadSettings", 0x6A45E0, {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x4B, 0x08, 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48}};

GameImage g_image;
uint64_t (*g_origCheckPlayers)(uint8_t*, void*, void*, void*) = nullptr;
uint64_t (*g_origReadSettings)(uint8_t*, void*, void*, void*) = nullptr;
std::mutex g_mutex;
bool g_pending = false;
MatchSetup g_setup;
// Research (`uniqueparam`): KWBattlePlayerSetting::SetUniqueParam values
// (+0x128, 8 ints) written with the next setups; -1 leaves one alone.
int g_uniqueParams[2][8] = {{-1, -1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1}};
int g_specRoundsForTest = 0; // setrounds: battledata::kSpecRounds as a game that kept another count writes it (0: the match's)
std::atomic<int> g_setups{0};
std::atomic<int> g_creations{0};

template <typename T>
T Get(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const T*>(p + off); }

template <typename T>
void Put(uint8_t* p, uint32_t off, T v) { *reinterpret_cast<T*>(p + off) = v; }

void ApplyPending(const char* when) {
    std::lock_guard lock(g_mutex);
    if (!g_pending) return;
    if (ApplyMatchSetup(g_setup)) Log("battle setup applied %s: %s", when, DescribeSetup(g_setup).c_str());
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
    // Used up: the battle jumped into has its setup and its stage (requested
    // before it is created). Left pending, every later battle the player
    // starts (Arcade, Versus, Training) got them: the last session's stage
    // in every stage request (even the menus' BLK), and after Battle now Ryu
    // and Ken. A session sets both again before each match's jump.
    if (HasPendingSetup() || !PendingStage().empty()) Log("battle setup: pending setup and stage used up");
    ClearPendingSetup();
    SetPendingStage("");
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

bool InstallBattleSetup(const GameImage& image) {
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

bool ReadMatchSetup(MatchSetup* out) {
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
        if ((std::strcmp(code, "URN") == 0 && Get<int32_t>(r, bd::kUniqueParams + 4 * 2) == 2) ||
            (std::strcmp(code, "Z41") == 0 && Get<int32_t>(r, bd::kUniqueParams) == 1)) {
            variation |= kVariantSpecial;
        }
        s.players[p].variation = variation;
        // Once the battle is set up, the version it resolved to (a version the
        // game lacks becomes its latest).
        s.players[p].version = static_cast<int16_t>(Get<int32_t>(r, bd::kCharaVersion));
    }
    s.stage = static_cast<uint16_t>(Get<int32_t>(data, bd::kStage));
    *out = s;
    return true;
}

bool ApplyMatchSetup(const MatchSetup& setup) {
    uint8_t* data = BattleData();
    if (!data) {
        Log("battle setup: no battle data to write to");
        return false;
    }
    for (int p = 0; p < kNumPlayers; p++) {
        uint8_t* r = data + p * bd::kRecordStride;
        const PlayerSetup& ps = setup.players[p];
        if (ps.character) {
            // ASCII then UTF-16 copy of the same code, both NUL padded.
            const std::string code = UnpackCode(ps.character);
            uint8_t field[12] = {};
            for (size_t i = 0; i < code.size() && i < 3; i++) {
                field[i] = static_cast<uint8_t>(code[i]);
                field[4 + 2 * i] = static_cast<uint8_t>(code[i]);
            }
            std::memcpy(r + bd::kCharaCode, field, sizeof(field));
        }
        // The setup's version (sessions: the final game's). Without one: the
        // game keeps the version in the record when the new character has it
        // too (rva 0x56B2E0), so it would depend on who played before; -1
        // makes it resolve to the last entry of the character's version list.
        Put<int32_t>(r, bd::kCharaVersion, ps.version >= 0 ? ps.version : -1);
        Put<int32_t>(r, bd::kCostume, ps.costume);
        Put<int32_t>(r, bd::kColor, ps.color);
        Put<int32_t>(r, bd::kComment, ps.comment == 0 ? -1 : ps.comment - 1);
        Put<int32_t>(r, bd::kVSkill, ps.vskill);
        Put<int32_t>(r, bd::kVTrigger, ps.vtrigger);
        Put<int32_t>(r, bd::kHandicap, 0);
        // The costume's variant (the menus' held buttons at load): dress
        // state 1 in every round, and the character's special look, a
        // unique param (flow.h kVariantSpecial).
        const int dress = ps.variation & kVariantDress ? 1 : 0;
        for (int round = 0; round < 7; round++) Put<int32_t>(r, bd::kDressStates + 4 * round, dress);
        Put<int32_t>(r, bd::kUseDressState, dress);
        const bool special = ps.variation & kVariantSpecial;
        const std::string chara(reinterpret_cast<const char*>(r + bd::kCharaCode), 3);
        if (chara == "URN") Put<int32_t>(r, bd::kUniqueParams + 4 * 2, special ? 2 : 0);
        if (chara == "Z41") Put<int32_t>(r, bd::kUniqueParams, special ? 1 : 0);
        // Not part of the match: each game's own player's choice.
        if (const int voice = VoiceLanguageFor(chara); voice >= 0) Put<int32_t>(r, bd::kVoiceLanguage, voice);
        for (int k = 0; k < 8; k++) {
            if (g_uniqueParams[p][k] >= 0) Put<int32_t>(r, bd::kUniqueParams + 4 * k, g_uniqueParams[p][k]);
        }
        Put<int32_t>(r, bd::kWins, setup.wins[p]);
        Put<int32_t>(r, bd::kWinStreak, setup.streaks[p]);
        Put<int32_t>(r, bd::kCtrlType, 0);
        // Each player's own pad, as local Versus sets them: the marker over
        // each fighter at the round's start names the pad ("Player 1" over
        // both with 0, 2026-11-04). The battle's input does not come from
        // here: the mod injects each side's (control.cpp clears the pads).
        Put<int32_t>(r, bd::kPadId, p);
    }
    // A battle entered straight from another (flow.h JumpToBattle) gets its
    // data from defaults that are not local Versus (infinite time, P2 a CPU).
    Put<int32_t>(data, bd::kMode, 1);
    // The match's rules (core/rules.h, the M0 spike): best of 1, 3 or 5
    // rounds, and the timer in frames (it has no "infinite": 0 and below end
    // the round at once). The round count goes to +0x08 too, not left to
    // this game's own setting: each hit's gauge gain is divided by it.
    const int bestOf = std::clamp(setup.roundsToWin * 2 - 1, 1, 5);
    const int seconds = setup.timerSeconds ? std::clamp<int>(setup.timerSeconds, 1, 99) : 99;
    Put<int32_t>(data, bd::kRounds, bestOf);
    Put<int32_t>(data, bd::kTimer, seconds * 60);
    Put<int32_t>(data, bd::kSpecRounds, g_specRoundsForTest ? g_specRoundsForTest : bestOf);
    Put<int32_t>(data, bd::kVersusFlag, 1);
    // The level itself comes from the stage request (flow.h SetPendingStage);
    // the index here does not change it. Kept the same as the level, so
    // what reads the index (and ReadMatchSetup) sees the stage played.
    if (setup.stage != kKeepStage) {
        const int32_t before = Get<int32_t>(data, bd::kStage);
        Put<int32_t>(data, bd::kStage, setup.stage);
        if (before != setup.stage) Log("battle setup: stage index %d -> %u", before, setup.stage);
    }
    // The stage's code, as the menus write it: a battle from another
    // battle's results has none, so its fighters had no stage data (no stage
    // transition; a spectator or replay from the menus had them).
    const std::string stageCode = JumpStageCode();
    if (stageCode.size() == 3 && IsReadable(data + bd::kStageName, bd::kStageNameSize)) {
        char* name = reinterpret_cast<char*>(data + bd::kStageName);
        if (std::strncmp(name, stageCode.c_str(), bd::kStageNameSize) != 0) {
            char before[bd::kStageNameSize + 1] = {};
            std::memcpy(before, name, bd::kStageNameSize);
            std::memset(name, 0, bd::kStageNameSize);
            std::memcpy(name, stageCode.data(), stageCode.size());
            Log("battle setup: stage name '%s' -> '%s'", before, stageCode.c_str());
        }
    }
    // Left alone, the count stayed as the menus or the last battle left it
    // (0 after a jump: both sides read 100%).
    if (uint8_t* s = SituationData()) {
        const int32_t before = Get<int32_t>(s, situation::kBattleCount);
        Put<int32_t>(s, situation::kBattleCount, setup.played);
        if (before != setup.played) Log("battle setup: matches played %d -> %u", before, setup.played);
    } else {
        Log("battle setup: no battle situation to write the matches played to");
    }
    return true;
}

void SetMatchRoundsForTest(int rounds) { g_specRoundsForTest = rounds; }

bool IsElevenPick(const PlayerSetup& p) {
    const int index = CharaIndex(UnpackCode(p.character));
    return index >= 0 && p.costume == CostumeInFolder(index, "0C");
}

LookRules GameLookRules() {
    LookRules rules;
    rules.isEleven = IsElevenPick;
    rules.elevenOtherColor = 1; // the 0C costume's color 2 of 2
    rules.colorsOf = [](const PlayerSetup& p) { return CostumeColorIds(CharaIndex(UnpackCode(p.character)), p.costume); };
    return rules;
}

bool IsFighter(int index) {
    // Entries with a handful of costumes or fewer are random select, dummies,
    // assist and event characters; "*NDK" and untranslated "ID_..." names are
    // placeholders, and X codes are helpers (X01: the partner Akira's
    // V-Trigger calls in).
    if (CharaCostumeCount(index) < 10 || CharaCode(index).rfind('X', 0) == 0) return false;
    const std::string name = CharaName(index);
    return name.empty() || (name[0] != '*' && name.rfind("ID_", 0) != 0);
}

std::vector<int> CostumeColorIds(int index, int costume) {
    const auto specs = CharaCostumeSpecs(index);
    const auto spec = specs.find(costume);
    if (spec != specs.end() && !spec->second.colors.empty()) return spec->second.colors;
    std::vector<int> ids;
    for (int k = 0; k < CostumeColorCount(index, costume); k++) ids.push_back(k);
    return ids;
}

void SetUniqueParamOverride(int player, int index, int value) {
    if (player >= 0 && player < 2 && index >= 0 && index < 8) g_uniqueParams[player][index] = value;
}

void SetPendingSetup(const MatchSetup& setup) {
    std::lock_guard lock(g_mutex);
    g_setup = setup;
    g_pending = true;
}

bool HasPendingSetup() {
    std::lock_guard lock(g_mutex);
    return g_pending;
}

void ClearPendingSetup() {
    std::lock_guard lock(g_mutex);
    g_pending = false;
}

int BattleSetupCount() { return g_setups; }

int BattleCreationCount() { return g_creations; }

std::string DescribeSetup(const MatchSetup& s) {
    std::string out;
    char buf[160];
    for (int p = 0; p < kNumPlayers; p++) {
        const PlayerSetup& ps = s.players[p];
        const std::string version = ps.version >= 0 ? " version " + std::to_string(ps.version) : "";
        std::snprintf(buf, sizeof(buf), "P%d %s costume %u color %u comment %u V-Skill %u V-Trigger %u%s%s | ", p + 1,
                      UnpackCode(ps.character).c_str(), ps.costume, ps.color + 1, ps.comment, ps.vskill + 1,
                      ps.vtrigger + 1, ps.variation ? (" " + VariantLabel(UnpackCode(ps.character), ps.variation)).c_str() : "",
                      version.c_str());
        out += buf;
    }
    if (s.stage == kKeepStage) return out + "stage as prepared";
    std::snprintf(buf, sizeof(buf), "stage %u %s", s.stage, StageCode(s.stage).c_str());
    return out + buf;
}

void LogBattleSetup(const char* when) {
    MatchSetup s;
    if (!ReadMatchSetup(&s)) {
        Log("battle setup %s: not available", when);
        return;
    }
    const uint8_t* data = BattleData();
    Log("battle setup %s: %s (rounds %d, timer %d, versions %d/%d)", when, DescribeSetup(s).c_str(),
        Get<int32_t>(data, bd::kRounds), Get<int32_t>(data, bd::kTimer), Get<int32_t>(data, bd::kCharaVersion),
        Get<int32_t>(data + bd::kRecordStride, bd::kCharaVersion));
}

} // namespace sfv::mod
