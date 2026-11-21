#pragma once
// The game's own menu sounds for the mod's screens. They play through the
// call the game's menus make (KWGameBlueprintFunction::PlayGameSE: a
// GameSoundSECue, rva 0x52AE30), so they are the same cues on the same
// player, and follow the game's own sound settings (Options, Sound: the SE
// volume) exactly as its menus do.
//
// The screens' input (ui_kit.h) notes the common ones by itself: Up and Down
// the cursor, Left and Right the step, A decide, B cancel. A screen names
// the others (an error, a window opening, a notice) with PlayUi. Each frame
// plays at most one of each, and a cursor or step gives way to a decide,
// cancel or error in the same frame.

#include <cstdint>
#include <string>

namespace sfv::mod {

struct GameImage;

enum class UiSound : uint8_t {
    Cursor,      // up and down through a list
    Step,        // left and right: a value, a tab
    Decide,      // A
    Cancel,      // B
    Error,       // an action that cannot be done
    Open,        // a window (modal, panel) opens
    Close,       // a window closes
    Notice,      // a toast
    NoticeError, // a toast about something that went wrong
    Invite,      // someone wants you: a rematch request, a call to a table
    ChatPost,    // a chat line from someone else
    ReadyOn,     // ready
    ReadyOff,    // no longer ready
    YourTurn,    // your turn at a table
    Countdown,   // a call's last seconds
    Found,       // an opponent found: a session starts
    Joined,      // someone came into the room
    Left,        // someone left the room
    Count
};

// Which of the game's sets the common sounds come from.
enum class UiSoundSet : uint8_t {
    Menu,       // the game's sub-menus: Polarity's screens
    MainMenu,   // the main menu's: the hub
    CharaSelect // character select's: the select screen
};

void InstallUiSound(const GameImage& image);
// Any thread. Played at the end of the render thread's frame.
void PlayUi(UiSound sound);
// The render thread, while a screen draws: its set for this frame's sounds.
void SetUiSoundSet(UiSoundSet set);
// The render thread: a modal (ui::Modal) is on screen this frame. One that
// appears plays Open; one that goes away plays Close (unless the press that
// closed it already made its own sound).
void NoteModalDrawn();
// The render thread, once a frame after every screen drew: plays what came.
void FlushUiSounds();
// The game's sounds but its music silenced (true) or back to the player's
// settings (false): for a replay's fast runs, whose ticks would play every
// hit and voice. Mutes the CriWare categories M_SE, M_VOICE, M_SYS_VOICE and
// M_EXTRA_VOICE; unmuting runs the game's own apply of its Sound options
// (rva 0x79BDE0), so the player's volumes come back exactly. Game thread.
void MuteGameSounds(bool mute);
bool GameSoundsMuted();
// Research and tests (`uisound <name|cue number>`): plays now, logs the cue.
bool PlayUiNamed(const std::string& name);

} // namespace sfv::mod
