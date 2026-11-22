// The mod's menu sounds, the game's own cues and volumes, and muting the game (docs/overlay.md).

#include "mod/ui_sound.h"

#include "mod/battle_driver.h"
#include "mod/image.h"
#include "mod/log.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace sfv::mod {
namespace {

constexpr game::CodeSite kPlayGameSe{
    "PlayGameSE", 0x52AE30, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9}};
using PlayFn = void (*)(const uint8_t* cue, const uint8_t* mode);
PlayFn g_play = nullptr;
uintptr_t g_base = 0;

constexpr uint32_t kCategoryVolumeByName = 0x39C10D8;
using CategoryVolumeFn = void (*)(const char* category, float volume);
constexpr uint32_t kApplySoundOptions = 0x79BDE0;
constexpr uint32_t kGameInstance = 0x521690;
constexpr uint32_t kSystemSave = 0x68E0D0;
constexpr uint32_t kOptionsOfSystemSave = 0x60;
bool g_muted = false;

enum Cue : uint8_t {
    MAINMENU_DECISION1 = 1,
    MAINMENU_CURSOR1 = 2,
    MAINMENU_CURSOR_LR = 4,
    MAINMENU_CANCEL1 = 5,
    MAINMENU_NG1 = 6,
    MAINMENU_WINDOW_OP1 = 7,
    MAINMENU_WINDOW_CLS1 = 8,
    DECISION1 = 9,
    CURSOR1 = 10,
    CURSOR_LR = 11,
    CANCEL1 = 12,
    NG1 = 13,
    WINDOW_OP1 = 14,
    WINDOW_CLS1 = 15,
    FIGHT_REQUEST_FOUND_OPPONENT = 22,
    NOTICE_INFO = 24,
    NOTICE_ERROR = 26,
    NOTICE_INVITE = 29,
    CHARASELECT_DECISION = 31,
    CHARASELECT_CURSOR = 32,
    CHARASELECT_CANCEL = 33,
    LOUNGE_CHAT_USER_POST = 68,
    LOUNGE_CHAT_SYSTEM_ENTER = 69,
    LOUNGE_CHAT_SYSTEM_LEAVE = 70,
    LOUNGE_READY_ON = 80,
    LOUNGE_READY_OFF = 81,
    LOUNGE_PLAYERS_TURN_COUNT_DOWN_SE = 86,
    LOUNGE_COMMING_PLAYERS_TURN_SE = 87,
    kCueCount = 103,
};

constexpr size_t kSounds = static_cast<size_t>(UiSound::Count);

uint8_t CueOf(UiSound s, UiSoundSet set) {
    const bool main = set == UiSoundSet::MainMenu, chara = set == UiSoundSet::CharaSelect;
    switch (s) {
        case UiSound::Cursor: return chara ? CHARASELECT_CURSOR : main ? MAINMENU_CURSOR1
                                                                       : CURSOR1;
        case UiSound::Step: return chara ? CHARASELECT_CURSOR : main ? MAINMENU_CURSOR_LR
                                                                     : CURSOR_LR;
        case UiSound::Decide: return chara ? CHARASELECT_DECISION : main ? MAINMENU_DECISION1
                                                                         : DECISION1;
        case UiSound::Cancel: return chara ? CHARASELECT_CANCEL : main ? MAINMENU_CANCEL1
                                                                       : CANCEL1;
        case UiSound::Error: return main ? MAINMENU_NG1 : NG1;
        case UiSound::Open: return main ? MAINMENU_WINDOW_OP1 : WINDOW_OP1;
        case UiSound::Close: return main ? MAINMENU_WINDOW_CLS1 : WINDOW_CLS1;
        case UiSound::Notice: return NOTICE_INFO;
        case UiSound::NoticeError: return NOTICE_ERROR;
        case UiSound::Invite: return NOTICE_INVITE;
        case UiSound::ChatPost: return LOUNGE_CHAT_USER_POST;
        case UiSound::ReadyOn: return LOUNGE_READY_ON;
        case UiSound::ReadyOff: return LOUNGE_READY_OFF;
        case UiSound::YourTurn: return LOUNGE_COMMING_PLAYERS_TURN_SE;
        case UiSound::Countdown: return LOUNGE_PLAYERS_TURN_COUNT_DOWN_SE;
        case UiSound::Found: return FIGHT_REQUEST_FOUND_OPPONENT;
        case UiSound::Joined: return LOUNGE_CHAT_SYSTEM_ENTER;
        case UiSound::Left: return LOUNGE_CHAT_SYSTEM_LEAVE;
        default: return CURSOR1;
    }
}

const char* const kNames[kSounds] = {"cursor", "step", "decide", "cancel", "error", "open", "close", "notice",
                                     "noticeerror", "invite", "chat", "readyon", "readyoff", "yourturn", "countdown", "found", "joined", "left"};

std::mutex g_mutex;
std::array<bool, kSounds> g_pending{};
UiSoundSet g_set = UiSoundSet::Menu;
bool g_modalNow = false, g_modalBefore = false;

void PlayCue(uint8_t cue) {
    if (!g_play || cue >= kCueCount) return;
    const uint8_t mode = 0;
    BattleSetSoundCaptureExempt(true);
    g_play(&cue, &mode);
    BattleSetSoundCaptureExempt(false);
}

} // namespace

void UiSoundInstall(const GameImage& image) {
    const auto* code = image.At<const uint8_t*>(kPlayGameSe.rva);
    if (std::memcmp(code, kPlayGameSe.bytes, sizeof(kPlayGameSe.bytes)) != 0) {
        Log("ui sound: signature mismatch at %s: the mod's screens stay silent", kPlayGameSe.name);
        return;
    }
    g_play = reinterpret_cast<PlayFn>(image.base + kPlayGameSe.rva);
    g_base = image.base;
}

bool UiSoundGameMuted() { return g_muted; }

void UiSoundMuteGame(bool mute) {
    if (!g_base || mute == g_muted) return;
    auto setVolume = *reinterpret_cast<CategoryVolumeFn*>(g_base + kCategoryVolumeByName);
    if (!setVolume) return;
    if (mute) {
        for (const char* category : {"M_SE", "M_VOICE", "M_SYS_VOICE", "M_EXTRA_VOICE"}) setVolume(category, 0.0f);
        g_muted = true;
        BattleBeginSoundCapture();
        Log("ui sound: the game's sounds muted (music stays)");
        return;
    }
    const int stopped = BattleStopCapturedSounds();
    if (stopped > 0) Sleep(35);
    using GetFn = void* (*)();
    using OfFn = void* (*)(void*);
    void* instance = reinterpret_cast<GetFn>(g_base + kGameInstance)();
    void* system = instance ? reinterpret_cast<OfFn>(g_base + kSystemSave)(instance) : nullptr;
    void* options = system && IsReadable(static_cast<uint8_t*>(system) + kOptionsOfSystemSave, 8)
                        ? *reinterpret_cast<void**>(static_cast<uint8_t*>(system) + kOptionsOfSystemSave)
                        : nullptr;
    if (!options) {
        for (const char* category : {"M_SE", "M_VOICE", "M_SYS_VOICE", "M_EXTRA_VOICE"}) setVolume(category, 1.0f);
        Log("ui sound: the options save not found; sounds unmuted at full volume");
    } else {
        using ApplyFn = void (*)(void*);
        reinterpret_cast<ApplyFn>(g_base + kApplySoundOptions)(options);
        Log("ui sound: the game's sounds back at the player's volumes (%d asked for while muted, stopped)", stopped);
    }
    g_muted = false;
}

void UiSoundPlay(UiSound sound) {
    if (sound >= UiSound::Count) return;
    std::lock_guard lock(g_mutex);
    g_pending[static_cast<size_t>(sound)] = true;
}

void UiSoundUseSet(UiSoundSet set) { g_set = set; }

void UiSoundNoteModal() { g_modalNow = true; }

void UiSoundFlush() {
    std::array<bool, kSounds> now;
    {
        std::lock_guard lock(g_mutex);
        now = g_pending;
        g_pending = {};
    }
    const UiSoundSet set = std::exchange(g_set, UiSoundSet::Menu);
    auto has = [&](UiSound s) { return now[static_cast<size_t>(s)]; };
    auto drop = [&](UiSound s) { now[static_cast<size_t>(s)] = false; };
    const bool modal = std::exchange(g_modalNow, false);
    if (modal && !g_modalBefore) now[static_cast<size_t>(UiSound::Open)] = true;
    if (!modal && g_modalBefore && !has(UiSound::Decide) && !has(UiSound::Cancel) && !has(UiSound::Error)) {
        now[static_cast<size_t>(UiSound::Close)] = true;
    }
    g_modalBefore = modal;
    if (has(UiSound::ReadyOn) || has(UiSound::ReadyOff)) drop(UiSound::Decide), drop(UiSound::Cancel);
    if (has(UiSound::Error)) now[static_cast<size_t>(UiSound::Decide)] = false;
    if (has(UiSound::Decide) || has(UiSound::Cancel) || has(UiSound::Error)) {
        now[static_cast<size_t>(UiSound::Cursor)] = now[static_cast<size_t>(UiSound::Step)] = false;
    }
    if (has(UiSound::Cursor)) now[static_cast<size_t>(UiSound::Step)] = false;
    for (size_t i = 0; i < kSounds; i++) {
        if (now[i]) PlayCue(CueOf(static_cast<UiSound>(i), set));
    }
}

bool UiSoundPlayNamed(const std::string& name) {
    for (size_t i = 0; i < kSounds; i++) {
        if (name == kNames[i]) {
            const uint8_t cue = CueOf(static_cast<UiSound>(i), UiSoundSet::Menu);
            Log("ui sound: %s (GameSoundSECue %d)", kNames[i], cue);
            PlayCue(cue);
            return true;
        }
    }
    if (name.rfind("cat=", 0) == 0 && g_base) {
        const size_t colon = name.find(':');
        auto fn = *reinterpret_cast<CategoryVolumeFn*>(g_base + kCategoryVolumeByName);
        if (!fn || colon == std::string::npos) return false;
        const std::string category = name.substr(4, colon - 4);
        const float volume = std::strtof(name.c_str() + colon + 1, nullptr);
        fn(category.c_str(), volume);
        Log("ui sound: category %s volume %.2f", category.c_str(), volume);
        return true;
    }
    if (name.rfind("volume=", 0) == 0 && g_base) {
        const float setting = std::strtof(name.c_str() + 7, nullptr);
        auto fn = *reinterpret_cast<CategoryVolumeFn*>(g_base + kCategoryVolumeByName);
        if (!fn || setting < 0 || setting > 1) return false;
        Log("ui sound: category M_SE volume %.2f (setting %.2f, as the options set it)", setting * 0.9f, setting);
        fn("M_SE", setting * 0.9f);
        return true;
    }
    char* end = nullptr;
    const long cue = std::strtol(name.c_str(), &end, 10);
    if (end && *end == '\0' && !name.empty() && cue >= 0 && cue < kCueCount) {
        Log("ui sound: GameSoundSECue %ld", cue);
        PlayCue(static_cast<uint8_t>(cue));
        return true;
    }
    return false;
}

} // namespace sfv::mod
