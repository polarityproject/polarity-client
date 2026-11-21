#pragma once
// The game's scene flow: which scene state runs (title, menus, stage and
// character select, battle, results) and how it moves on.
//
// SceneManager::ChangeSceneState(stateClass, param) (rva 0x584B70) switches
// to a scene state class (a UClass); LaunchNextGameScene (rva 0x56C100) moves
// to the next game scene. Both are logged with the class names.

#include "core/replay_file.h"
#include "mod/image.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfv::mod {

// When the title asks for InitialState (which logs in to the game's servers,
// then shows the login result dialogs), switch to the main menu instead:
// always, so the game never tries Capcom's servers with the mod in (the
// [boot] offline setting is gone).
bool FlowInstallProbes(const GameImage& image);

// Runs `task` on the game thread at its next frame: FlowPumpGameThread runs the
// queue from the scene manager's tick and from the pad poll (neither runs
// every frame in every screen), at most once per 10 ms.
void FlowRunOnGameThread(std::function<void()> task);
// Loads one asset by path ("/Game/UI/Foo/T_Foo.T_Foo") as an object of the
// named class through the game's StaticLoadObject. Game thread; null on failure.
void* FlowLoadAsset(const std::string& className, const std::string& path);
void FlowPumpGameThread();
std::string FlowPumpStats(); // diagnostics: how often each source ran it
void FlowRunOnGameThreadAfter(int frames, std::function<void()> task);

// Switches the scene flow straight to the Versus battle scene
// (ChangeSceneState(VersusBattleState)), skipping stage and character select.
// The battle uses the battle settings as they are (and a pending match setup).
void FlowJumpToBattle();
// Calls off the jump under way, whichever step it waits in (a preload, a
// scene settling, its stage loading): its battle never comes. A jump that
// already switched to the battle scene goes on.
void FlowCancelBattleJump();
// Back to the main menu after `delayFrames`, unless a jump to a battle starts
// in the meantime.
void FlowReturnToMainMenu(int delayFrames);
// Switches the scene flow to a scene state class by name ("MainMenuState"),
// with an init message object by name (none when empty).
void FlowChangeScene(const std::string& stateClassName, const std::string& paramName = "");
// The scene state class the game last switched to ("MainMenuState",
// "VersusBattleState"); empty before the first switch.
std::string FlowCurrentScene();
// How many jumps switched to the battle scene so far: the screen goes black
// for the battle's loading right after (the menus fade out just before).
uint64_t FlowBattleSceneSwitches();
// The stage the next stage request loads, by code from the stage table
// ("S25", "LVS"; empty: what the game asks for).
void FlowSetPendingStage(const std::string& code);
std::string FlowPendingStage();
// The stage code the last jump to battle loads (its request: the pending
// stage or the one already prepared), empty before any jump or when unknown.
std::string FlowJumpStageCode();
// Loads a character (its latest version) the way battle setup does and puts it
// in the game's character cache (4 slots, least recently used dropped), so a
// battle with it later finds its assets loaded. Takes the asset queue for a
// few seconds (the game thread is busy meanwhile); a jump waits for it.
void FlowPreloadCharacter(const std::string& code, int costume);

// What to keep preloaded: polled at the main menu, preloaded once unchanged
// for a second (the local pick, from the netplay driver).
struct PreloadPick {
    std::string code;
    int costume = 0;
    int version = -1; // the match setup's; -1: the final game's if this game has it, else its latest
};

// Preloads each in turn, then runs `then` on the game thread (a replay's jump).
void FlowPreloadCharacters(std::vector<PreloadPick> picks, std::function<void()> then);
void FlowSetPreloadSource(std::function<std::vector<PreloadPick>()> source);
// Every pick of the source is in the character cache (or would not stay
// there), as of the last check (about twice a second).
bool FlowPreloadsDone();
// FlowPreloadsDone, and the check was made for these picks as the source gives
// them ("RYU/0 KEN/0 "): false until it has seen a change of picks.
bool FlowPreloadsDoneFor(const std::string& picks);
// One of the source's picks at the menus (My character's card): 2 in the
// character cache (as of the last check), 1 loading now (`seconds` since it
// started), 0 waiting for its turn, -1 not a pick of the source, or it does
// not stay cached (tried enough: the match loads it).
int FlowPreloadStatus(const std::string& code, int costume, double* seconds);
// The version a battle resolves this character's to when asked for its latest.
int FlowLatestCharaVersion(const std::string& code);
// The final game's balance version of a playable character (60, 61 or 62),
// -1 for others: what a session's match setup plays.
int FlowFinalCharaVersion(const std::string& code);
// Whether this game has that version of the character (any thread; true
// until its version lists are read, at the main menu, and for -1).
bool FlowHasCharaVersion(const std::string& code, int version);
// The last entry of this game's version list for the character, -1 if not known.
int FlowNewestLocalVersion(const std::string& code);
// This game's version lists against the final game's, for logs and reports.
std::string FlowCharaVersionsText();
// The language this game's player set this character to speak (Options,
// Sound: 0 Japanese, 1 English), -1 if not known; game thread. A voice is
// this player's own choice: each side sets its own. The override (testing,
// -1 off) answers for every character.
int FlowVoiceLanguageFor(const std::string& code);
void FlowSetVoiceLanguageOverride(int language);
// Diagnostics: logs every character's versions.
void FlowLogCharaVersions();
// Diagnostics: logs the character cache's slots.
void FlowLogCharaCache();
// The stage table (singleton +0x68, KWStageEntryDataAsset): code and level paths.
void FlowLogStageTable();
// The stage table: entries, the code of entry `index` ("" if none), and whether
// it is a complete stage (a code and both level paths), for random picks.
int FlowStageCount();
std::string FlowStageCode(int index);
bool FlowIsPlayableStage(int index);
// The stages the game's stage select lists (by stage table index), each
// with its settings (other stage indices: The Grid's The Grid Alternative),
// as the game's DA_StageArrange has them. FlowIsSelectableStage: a menu stage or
// one of their settings (for random picks). The first call walks every object.
bool FlowIsMenuStage(int index);
// Stage names come from each stage's personal data, which the game leaves
// unloaded for some (Ring of Arcade, S24): the first call loads those on the
// game thread (once, a short hitch at the menus). True once done; until then
// such a stage has no name, and is no menu stage. Any thread.
bool FlowStageNamesReady();
std::vector<int> FlowStageSettings(int base);
int FlowStageSettingBase(int index); // the menu stage a setting belongs to, or -1
bool FlowIsSelectableStage(int index);
// The mod's random stage for `roll`: a selectable stage (only those the
// profile owns with `ownedOnly`), never one of core/random_stage.h's
// never-random stages, nor one `off` leaves out (the host's Random stages
// list); -1 when none is left.
int FlowRandomStage(uint64_t roll, bool ownedOnly, const std::vector<std::string>& off = {});
// The character table (KWCharaEntryDataAsset): entries, the code of entry
// `index` ("RYU"; "" if none) and how many costumes it has.
int FlowCharaCount();
std::string FlowCharaCode(int index);
int FlowCharaCostumeCount(int index);
// How many colours the game offers for costume `costume` (an index into the
// character's costume list) of character `index`; 0 for a costume the menus do
// not offer (no colour preview: 0C, which random select's Eleven wears, and
// 0E). The first call walks every object.
int FlowCostumeColorCount(int index, int costume);
// A costume's variants, as PlayerSetup::variation bits: kVariantDress, its
// dress state 1 (a dress state asset, asked of the paks, or its material
// preset's variant entry), and kVariantSpecial, its character's second look
// (Urien's thong, Akira's helmet off). Not for the render thread.
constexpr uint8_t kVariantDress = 1;
constexpr uint8_t kVariantSpecial = 2;
uint8_t FlowCostumeVariants(int index, int costume);
// The same without waiting: nullopt until a background thread has asked the
// paks (the first call starts it).
std::optional<uint8_t> FlowCostumeVariantsKnown(int index, int costume);
// The character's special variant ("Thong", "Helmet off"), or nullptr.
const char* FlowSpecialVariantName(const std::string& code);
// A variation's name for the panel and logs: "Normal", "Variant", "Thong",
// "Variant + Thong".
std::string FlowVariantLabel(const std::string& code, uint8_t variation);

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

std::map<int, CostumeSpec> FlowCharaCostumeSpecs(int index);
// A stage's entitlement code (StageSpec RefAvailableCode, by its index in the
// stage list; "" when it has none or the table is not loaded).
std::string FlowStageAvailableCode(int index);
// The character's English display name ("" if its data is not loaded).
std::string FlowCharaName(int index);
// The game's Battle Lounge comment id (FixedPhraseCommentSpec) in the game's
// language, {common1}/{common2} where its arguments go; "" until the table is
// loaded or for an unknown Id. The rooms' titles (keeper_protocol.h RoomTitle).
std::string FlowLoungeCommentText(int id);
// A stage's display name ("Training Stage"; "" if unknown). The first call
// walks every object.
std::string FlowStageName(int index);
// A character's V-Skill (or V-Trigger) names in the game's order ("" for one
// without a name; empty when unknown).
std::vector<std::string> FlowSkillNames(const std::string& charaCode, bool vtrigger);
// How many: FlowSkillNames' count once its asset has been found, without waiting
// (0 until then; the search runs on a thread of its own, at most every 5 s).
size_t FlowSkillCountKnown(const std::string& charaCode, bool vtrigger);
// A character's V-Comments as the menus list them after Auto (the battle
// setup's comment n is the n-th; empty when unknown). Loads the character's
// subtitle asset (DA_<code>_Subtitle) on the game thread when needed and
// waits for it, so not from the game thread.
std::vector<std::string> FlowVCommentNames(const std::string& charaCode);
// How many V-Comments the character has, without waiting: nullopt while its
// list loads (the first call starts the load on the game thread); 0 when the
// game has none for it.
std::optional<size_t> FlowVCommentCount(const std::string& charaCode);

// The colour preview's two swatch colours (0xAARRGGBB) per colour of a costume:
// its colours, then its EX colours (ids 100..). Empty without a preview.
struct CostumeSwatches {
    std::vector<std::pair<uint32_t, uint32_t>> colors, ex;
};

CostumeSwatches FlowCostumeSwatchColors(int index, int costume);
// The index of character `index`'s costume in SkelMesh folder `folder` ("0C",
// the silver model Eleven wears), or -1.
int FlowCostumeInFolder(int index, const std::string& folder);
// Index of a character code in the character table, or -1.
int FlowCharaIndex(const std::string& code);
// Eleven's stand-in: the `roll`-th (modulo) of the fighters that have a 0C
// costume, and that costume's index. False when there is none.
bool FlowElevenStandIn(uint32_t roll, std::string* code, int* costume);
// Index of a stage code, or -1.
int FlowStageIndex(const std::string& code);
constexpr uint16_t kKeepStage = 0xFFFF; // MatchSetup::stage: whatever the game has prepared
// The round's flow, from the battle's flow events: whether the fight runs
// (between "FIGHT!" and the round's end) or a presentation phase (intro, round
// call, KO and win poses). Inputs can skip parts of the presentation. Its
// objects are not in the snapshot, but what the round's stages keep outside
// it follows loads (the round director, the phase machine, the round end
// wait, the round over's writes), so sessions predict and roll back there
// too ([netplay] lockstep_round_stages=1: lockstep, as before).
// The state lives in a snapshot region of its own, so a rollback restores it.
bool FlowRoundInFight();

// A replay's keyframe of what a load needs beside the snapshot regions: the
// battle phase machine and its phases, the round end wait, the input
// recorder's head (the per-tick copies a rollback restores from reach only
// 64 ticks back) and the battle context's round history and latest round's
// record (what the round over writes, ~550 KB, in 4 KB chunks each shared
// with the previous keyframe's while unchanged).
// Game thread, in a driven battle.
struct OutsideState {
    std::vector<uint8_t> director; // the round director's state, timers and counts
    std::vector<uint8_t> machine;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> rounds; // in chunks, shared with the previous keyframe's while unchanged
};

bool FlowSaveOutsideState(OutsideState* out, const OutsideState* previous);
// After the regions were loaded, before SfvGame::OnStateLoaded (whose fix-ups
// then see the director as it was).
void FlowRestoreOutsideState(const OutsideState& s);
// Research (`roundinfo`): the battle context's round state and round history.
void FlowLogRoundInfo();
// Research (`ufunc <name>`): every UFunction of that name, its owner and the
// code pointers in it; (`functrace <name> [owner] <offset hex>`): hooks the
// native at that UFunction offset and logs the calls (one at a time).
void FlowLogFunction(const std::string& name);
// Research (`uobj <name> [dump]`): every object of that name, its class and
// owner; dump: its first 0x100 bytes, and text where its words point.
void FlowLogObjectsNamed(const std::string& name, bool dump = false);
void FlowTraceFunction(const std::string& name, const std::string& owner, uint32_t offset);
// The match's rounds as the battle has them now (at its end): each round's
// winner, and the rounds each side won. False when they cannot be read.
bool FlowReadRoundResults(std::vector<RoundResult>* rounds, uint8_t wins[2]);
// The round call ("ROUND 1"): from its flow event (5) until "FIGHT!" (6).
bool FlowRoundCallRunning();
// Round end flow events (7: after a KO's stop, or time over) seen so far.
uint32_t FlowRoundEndEvents();
// After a snapshot load: makes the battle frame's stop bit (KO and other
// stops) agree with the round director, which is not in the snapshot.
void FlowMatchStopToDirector(uint8_t* battleFrame);
// The round director's check for a KO (or time over) runs once per rendered
// frame, outside the tick, and the director is not in the game's snapshot: a
// KO first reached in a resimulation was found only after it, frames late,
// and one a rollback took back left the director stopped (netplay-testing.md,
// finding 24). While a session owns the ticks (on), the check runs after each
// tick instead (FlowRunRoundDirector, which also copies the director into the
// round flow state), and a load writes it back (FlowRoundDirectorFromSnapshot).
// roundEndRollback: the round's end may happen on predicted frames (the
// phase machine copied per tick and the round over journaled, put back by a
// load) instead of being held until no rollback can reach it.
void FlowSetRoundDirectorPerTick(bool on, bool roundEndRollback);
// Before each driven tick: counts it (the count keys the round's end copies).
void FlowBeginDrivenTick();
// The driven ticks run so far (in the snapshot: restored with a load).
uint32_t FlowDrivenTicks();
void FlowRunRoundDirector();
void FlowRoundDirectorFromSnapshot();
uint8_t* FlowRoundFlowState();
// The battle's round director task object (research: replay traps), or nullptr.
uint8_t* FlowRoundDirector();
// Start during a netplay match, for the tick about to run (from both
// players' inputs): outside the fight (intro, round call, KO, win poses) once
// both players have pressed it, Start for a few ticks (for both players'
// values: the intro takes it from player 1) to skip the intro or the round's
// win pose; otherwise nothing. After the match's end it also presses Start
// on each game's own pad input: the win screens read it there. The
// presses are counted in the round flow state, so a rollback restores them.
uint32_t FlowPresentationStart(uint32_t p1, uint32_t p2);
// The match is over (after its last KO, from the round flow state): the win
// screens that follow take their input from each game's own pads.
bool FlowMatchDecided();
// While held (a netplay match is over and the players have not agreed on
// what follows), the battle's results screen (flow event 13, where the
// game's own Rematch / Character select menu opens) does not open: the event
// is dropped, and FlowResultsHeld() tells the session to stop ticking the battle.
void FlowSetHoldResults(bool hold);
bool FlowResultsHeld();
constexpr size_t kRoundFlowStateSize = 176;
// A new battle starts in its intro.
void FlowResetRoundFlow();
// Diagnostics: logs the world context's pending map change fields.
void FlowLogWorldContext();
// The scene state changes seen so far, oldest first.
// Diagnostics: logs the battle tasks run by the next `calls` task calls.
void FlowTaskCensus(int calls);
// Research (`phasecost`): the scheduler phase now running (0: none), and
// per-task timing by phase, logged when turned off (over `ticks` driven ticks).
void FlowSetSchedulerPhase(int phase, bool replaying = false);
// The scheduler phase running now (1, 2, 4, 8; 0 outside one).
int FlowSchedulerPhaseNow();
// Experiment (`resimtasks`): task functions (rvas) that replayed ticks leave
// out in scheduler phases 4 and 8.
void FlowSetResimTaskSkip(std::vector<uintptr_t> rvas);
// The phase-4 tasks replayed ticks leave out by default (flow.cpp kLeanResimTasks).
std::vector<uintptr_t> FlowDefaultLeanResimTasks();
// A lean resim task named by its object's class (the generic task's vtable
// rva) carries this bit.
constexpr uintptr_t kResimTaskByClass = 0x80000000u;
// The game's own rollback of sounds (flow.cpp kSoundReconcileTask): on by
// default; off, resimulated ticks play their sounds again (`soundrollback`).
void FlowSetSoundRollback(bool on);
// The round end wait's mode (flow.cpp kKoWaitState) put back by loads (on by default).
void FlowSetKoWaitRollback(bool on);
// Research (`burston <code> <dir> <frames> [scale] [count]`): a frame burst
// (overlay.h OverlayRequestShotBurst) on each of the next `count` flow events `code`.
void FlowSetBurstOnEvent(int code, const std::string& dir, int frames, int scale, int count);
// Research (`ptrtest old|new`): the phase restores' pointer test as before
// 2026-11-13 (any mapped, aligned word) or as now.
void FlowSetLegacyPointerTest(bool on);
bool FlowSoundRollbackOn();
void FlowSetTaskProfiling(bool on, uint64_t ticks = 0);
// Research: logs the battle's phase machine and its phases (`phases`), with
// the snapshot region each lies in (from the lookup SfvGame's driver sets).
void FlowLogPhaseMachine();
// The battle's phase machine object, or nullptr (research: `watchphase`).
uint8_t* FlowPhaseMachineObject();
void FlowSetRegionLookup(std::function<std::string(const void*)> lookup);
// Research: director state and stop kind, phase machine index / requested, tick count.
std::string FlowRoundEndState();
// Test (`emptystage`): the next jump finds its new loader's stage request
// empty, as a freshly started game's once did (43c9c9cb: "cannot set the
// stage (request '')", its battle objects after 3 ticks).
void FlowTestEmptyStageRequest();
// Research: a character's costume arrays (`costumes <code>`).
void FlowLogCharaCostumes(const std::string& code);
// Research (`variants`): every character's costumes with their names,
// variant counts and asset paths.
void FlowLogAllVariants();
// Research (`presets [code]`): loads each costume's material preset and logs
// its entries (one character per frame, on the game thread).
void FlowLogCostumePresets(const std::string& code);
// Research (`datatable <name> [text to match] [rows]`): a DataTable's rows,
// each property's value as text.
void FlowLogDataTable(const std::string& name, const std::string& filter, int limit);
std::string FlowHistory();

} // namespace sfv::mod
