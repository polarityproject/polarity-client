#pragma once
// The game's scene flow: which scene state runs (title, menus, stage and
// character select, battle, results) and how it moves on.
//
// SceneManager::ChangeSceneState(stateClass, param) (rva 0x584B70) switches
// to a scene state class (a UClass); LaunchNextGameScene (rva 0x56C100) moves
// to the next game scene. Both are logged with the class names.

#include "mod/image.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sfv::mod {

// offlineBoot: when the title asks for InitialState (which logs in to the
// game's servers, then shows the login result dialogs), switch to the main
// menu instead.
bool InstallFlowProbes(const GameImage& image, bool offlineBoot);

// Runs `task` on the game thread at its next frame: PumpGameThread runs the
// queue from the scene manager's tick and from the pad poll (neither runs
// every frame in every screen), at most once per 10 ms.
void RunOnGameThread(std::function<void()> task);
// Loads one asset by path ("/Game/UI/Foo/T_Foo.T_Foo") as an object of the
// named class through the game's StaticLoadObject. Game thread; null on failure.
void* LoadAsset(const std::string& className, const std::string& path);
void PumpGameThread();
std::string PumpStats(); // diagnostics: how often each source ran it
void RunOnGameThreadAfter(int frames, std::function<void()> task);

// Switches the scene flow straight to the Versus battle scene
// (ChangeSceneState(VersusBattleState)), skipping stage and character select.
// The battle uses the battle settings as they are (and a pending match setup).
void JumpToBattle();
// Back to the main menu after `delayFrames`, unless a jump to a battle starts
// in the meantime.
void ReturnToMainMenu(int delayFrames);
// Switches the scene flow to a scene state class by name ("MainMenuState"),
// with an init message object by name (none when empty).
void ChangeScene(const std::string& stateClassName, const std::string& paramName = "");
// The scene state class the game last switched to ("MainMenuState",
// "VersusBattleState"); empty before the first switch.
std::string CurrentScene();
// How many jumps switched to the battle scene so far: the screen goes black
// for the battle's loading right after (the menus fade out just before).
uint64_t BattleSceneSwitches();
// The stage the next stage request loads, by code from the stage table
// ("S25", "LVS"; empty: what the game asks for).
void SetPendingStage(const std::string& code);
std::string PendingStage();
// Loads a character (its latest version) the way battle setup does and puts it
// in the game's character cache (4 slots, least recently used dropped), so a
// battle with it later finds its assets loaded. Takes the asset queue for a
// few seconds (the game thread is busy meanwhile); a jump waits for it.
void PreloadCharacter(const std::string& code, int costume);
// What to keep preloaded: polled at the main menu, preloaded once unchanged
// for a second (the local pick, from the netplay driver).
struct PreloadPick {
    std::string code;
    int costume = 0;
    int version = -1; // the match setup's; -1: the final game's if this game has it, else its latest
};
// Preloads each in turn, then runs `then` on the game thread (a replay's jump).
void PreloadCharacters(std::vector<PreloadPick> picks, std::function<void()> then);
void SetPreloadSource(std::function<std::vector<PreloadPick>()> source);
// Every pick of the source is in the character cache (or would not stay
// there), as of the last check (about twice a second).
bool PreloadsDone();
// PreloadsDone, and the check was made for these picks as the source gives
// them ("RYU/0 KEN/0 "): false until it has seen a change of picks.
bool PreloadsDoneFor(const std::string& picks);
// One of the source's picks at the menus (My character's card): 2 in the
// character cache (as of the last check), 1 loading now (`seconds` since it
// started), 0 waiting for its turn, -1 not a pick of the source, or it does
// not stay cached (tried enough: the match loads it).
int PreloadStatus(const std::string& code, int costume, double* seconds);
// The version a battle resolves this character's to when asked for its latest.
int LatestCharaVersion(const std::string& code);
// The final game's balance version of a playable character (60, 61 or 62),
// -1 for others: what a session's match setup plays.
int FinalCharaVersion(const std::string& code);
// Whether this game has that version of the character (any thread; true
// until its version lists are read, at the main menu, and for -1).
bool HasCharaVersion(const std::string& code, int version);
// The last entry of this game's version list for the character, -1 if not known.
int NewestLocalVersion(const std::string& code);
// This game's version lists against the final game's, for logs and reports.
std::string CharaVersionsText();
// The language this game's player set this character to speak (Options,
// Sound: 0 Japanese, 1 English), -1 if not known; game thread. A voice is
// this player's own choice: each side sets its own. The override (testing,
// -1 off) answers for every character.
int VoiceLanguageFor(const std::string& code);
void SetVoiceLanguageOverride(int language);
// Diagnostics: logs every character's versions.
void LogCharaVersions();
// Diagnostics: logs the character cache's slots.
void LogCharaCache();
// The stage table (singleton +0x68, KWStageEntryDataAsset): code and level paths.
void LogStageTable();
// The stage table: entries, the code of entry `index` ("" if none), and whether
// it is a complete stage (a code and both level paths), for random picks.
int StageCount();
std::string StageCode(int index);
bool IsPlayableStage(int index);
// The stages the game's stage select lists (by stage table index), each
// with its settings (other stage indices: The Grid's The Grid Alternative),
// as the game's DA_StageArrange has them. IsSelectableStage: a menu stage or
// one of their settings (for random picks). The first call walks every object.
bool IsMenuStage(int index);
std::vector<int> StageSettings(int base);
int StageSettingBase(int index); // the menu stage a setting belongs to, or -1
bool IsSelectableStage(int index);
// The character table (KWCharaEntryDataAsset): entries, the code of entry
// `index` ("RYU"; "" if none) and how many costumes it has.
int CharaCount();
std::string CharaCode(int index);
int CharaCostumeCount(int index);
// How many colours the game offers for costume `costume` (an index into the
// character's costume list) of character `index`; 0 for a costume the menus do
// not offer (no colour preview: 0C, which random select's Eleven wears, and
// 0E). The first call walks every object.
int CostumeColorCount(int index, int costume);
// A costume's variants, as PlayerSetup::variation bits: kVariantDress, its
// dress state 1 (a dress state asset, asked of the paks, or its material
// preset's variant entry), and kVariantSpecial, its character's second look
// (Urien's thong, Akira's helmet off). Not for the render thread.
constexpr uint8_t kVariantDress = 1;
constexpr uint8_t kVariantSpecial = 2;
uint8_t CostumeVariants(int index, int costume);
// The character's special variant ("Thong", "Helmet off"), or nullptr.
const char* SpecialVariantName(const std::string& code);
// A variation's name for the panel and logs: "Normal", "Variant", "Thong",
// "Variant + Thong".
std::string VariantLabel(const std::string& code, uint8_t variation);
// The game's own name and color ids of each costume of character `index`
// (by costume index): colors 0.. are the costume's colors, 100.. its EX
// colors (the battle setup's color is the id). Empty when the game's table
// is not loaded. The first call walks every object.
struct CostumeSpec {
    std::string name;        // "Battle Outfit 1", "Track Suit", a team's name
    std::vector<int> colors; // sorted
    // Entitlement codes (entitlements.h, OwnsEntitlement): the character's
    // (playable, listed), the costume's, each color's (by color id).
    std::string charaCode, charaVisibleCode, costumeCode;
    std::map<int, std::string> colorCodes;
};
std::map<int, CostumeSpec> CharaCostumeSpecs(int index);
// A stage's entitlement code (StageSpec RefAvailableCode, by its index in the
// stage list; "" when it has none or the table is not loaded).
std::string StageAvailableCode(int index);
// The character's English display name ("" if its data is not loaded).
std::string CharaName(int index);
// The game's Battle Lounge comment id (FixedPhraseCommentSpec) in the game's
// language, {common1}/{common2} where its arguments go; "" until the table is
// loaded or for an unknown Id. The rooms' titles (keeper_protocol.h RoomTitle).
std::string LoungeCommentText(int id);
// A stage's display name ("Training Stage"; "" if unknown). The first call
// walks every object.
std::string StageName(int index);
// A character's V-Skill (or V-Trigger) names in the game's order ("" for one
// without a name; empty when unknown).
std::vector<std::string> SkillNames(const std::string& charaCode, bool vtrigger);
// A character's V-Comments as the menus list them after Auto (the battle
// setup's comment n is the n-th; empty when unknown). Loads the character's
// subtitle asset (DA_<code>_Subtitle) on the game thread when needed and
// waits for it, so not from the game thread.
std::vector<std::string> VCommentNames(const std::string& charaCode);
// The colour preview's two swatch colours (0xAARRGGBB) per colour of a costume:
// its colours, then its EX colours (ids 100..). Empty without a preview.
struct CostumeSwatches {
    std::vector<std::pair<uint32_t, uint32_t>> colors, ex;
};
CostumeSwatches CostumeSwatchColors(int index, int costume);
// The index of character `index`'s costume in SkelMesh folder `folder` ("0C",
// the silver model Eleven wears), or -1.
int CostumeInFolder(int index, const std::string& folder);
// Index of a character code in the character table, or -1.
int CharaIndex(const std::string& code);
// Eleven's stand-in: the `roll`-th (modulo) of the fighters that have a 0C
// costume, and that costume's index. False when there is none.
bool ElevenStandIn(uint32_t roll, std::string* code, int* costume);
// Index of a stage code, or -1.
int StageIndex(const std::string& code);
constexpr uint16_t kKeepStage = 0xFFFF; // MatchSetup::stage: whatever the game has prepared
// The round's flow, from the battle's flow events: whether the fight runs
// (between "FIGHT!" and the round's end) or a presentation phase (intro, round
// call, KO and win poses). Inputs can skip parts of the presentation, and its
// objects are not in the snapshot, so netplay keeps rollbacks out of it.
// The state lives in a snapshot region of its own, so a rollback restores it.
bool RoundInFight();
// The round call ("ROUND 1"): from its flow event (5) until "FIGHT!" (6).
bool RoundCallRunning();
// After a snapshot load: makes the battle frame's stop bit (KO and other
// stops) agree with the round director, which is not in the snapshot.
void MatchStopToDirector(uint8_t* battleFrame);
// The round director's check for a KO (or time over) runs once per rendered
// frame, outside the tick, and the director is not in the game's snapshot: a
// KO first reached in a resimulation was found only after it, frames late,
// and one a rollback took back left the director stopped (netplay-testing.md,
// finding 24). While a session owns the ticks (on), the check runs after each
// tick instead (RunRoundDirector, which also copies the director into the
// round flow state), and a load writes it back (RoundDirectorFromSnapshot).
// roundEndRollback: the round's end may happen on predicted frames (the
// phase machine copied per tick and the round over journaled, put back by a
// load) instead of being held until no rollback can reach it.
void SetRoundDirectorPerTick(bool on, bool roundEndRollback);
// Before each driven tick: counts it (the count keys the round's end copies).
void BeginDrivenTick();
// The driven ticks run so far (in the snapshot: restored with a load).
uint32_t DrivenTicks();
void RunRoundDirector();
void RoundDirectorFromSnapshot();
uint8_t* RoundFlowState();
// The battle's round director task object (research: replay traps), or nullptr.
uint8_t* RoundDirector();
// Start during a netplay match, for the tick about to run (from both
// players' inputs): outside the fight (intro, round call, KO, win poses) once
// both players have pressed it, Start for a few ticks (for both players'
// values: the intro takes it from player 1) to skip the intro or the round's
// win pose; otherwise nothing. After the match's end it also presses Start
// on each game's own pad input: the win screens read it there. The
// presses are counted in the round flow state, so a rollback restores them.
uint32_t PresentationStart(uint32_t p1, uint32_t p2);
// The match is over (after its last KO, from the round flow state): the win
// screens that follow take their input from each game's own pads.
bool MatchDecided();
// While held (a netplay match is over and the players have not agreed on
// what follows), the battle's results screen (flow event 13, where the
// game's own Rematch / Character select menu opens) does not open: the event
// is dropped, and ResultsHeld() tells the session to stop ticking the battle.
void SetHoldResults(bool hold);
bool ResultsHeld();
constexpr size_t kRoundFlowStateSize = 176;
// A new battle starts in its intro.
void ResetRoundFlow();
// Diagnostics: logs the world context's pending map change fields.
void LogWorldContext();
// The scene state changes seen so far, oldest first.
// Diagnostics: logs the battle tasks run by the next `calls` task calls.
void TaskCensus(int calls);
// Research (`phasecost`): the scheduler phase now running (0: none), and
// per-task timing by phase, logged when turned off (over `ticks` driven ticks).
void SetSchedulerPhase(int phase, bool replaying = false);
// Experiment (`resimtasks`): task functions (rvas) that replayed ticks leave
// out in scheduler phases 4 and 8.
void SetResimTaskSkip(std::vector<uintptr_t> rvas);
void SetTaskProfiling(bool on, uint64_t ticks = 0);
// Research: logs the battle's phase machine and its phases (`phases`), with
// the snapshot region each lies in (from the lookup SfvGame's driver sets).
void LogPhaseMachine();
void SetRegionLookup(std::function<std::string(const void*)> lookup);
// Research: director state and stop kind, phase machine index / requested, tick count.
std::string RoundEndState();
// Test (`emptystage`): the next jump finds its new loader's stage request
// empty, as a freshly started game's once did (43c9c9cb: "cannot set the
// stage (request '')", its battle objects after 3 ticks).
void TestEmptyStageRequest();
// Research: a character's costume arrays (`costumes <code>`).
void LogCharaCostumes(const std::string& code);
// Research (`variants`): every character's costumes with their names,
// variant counts and asset paths.
void LogAllVariants();
// Research (`presets [code]`): loads each costume's material preset and logs
// its entries (one character per frame, on the game thread).
void LogCostumePresets(const std::string& code);
// Research (`datatable <name> [text to match] [rows]`): a DataTable's rows,
// each property's value as text.
void LogDataTable(const std::string& name, const std::string& filter, int limit);
std::string FlowHistory();

} // namespace sfv::mod
