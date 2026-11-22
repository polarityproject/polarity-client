#pragma once
// Takes over the battle's frame loop.
//
// BattleFrame::Update runs once per rendered frame on the game thread and
// performs that frame's battle ticks. With a frame driver installed, the hook
// calls the driver instead; the driver runs ticks itself through
// SfvGame::Advance (one tick with given inputs each), loads and saves state
// in between, or runs none to stall. InputUnit::RecordFrame is hooked so each
// tick records exactly the inputs the driver chose.

#include "mod/sfv_game.h"

namespace sfv::mod {

class IFrameDriver {
public:
    virtual ~IFrameDriver() = default;
    // Once per rendered battle frame, on the game thread, between ticks.
    // Return false to let the game run the frame normally.
    virtual bool OnFrame(SfvGame& game) = 0;
};

bool BattleInstallDriver(const GameImage& image, SfvGame* game);
// The driver used from the next frame on; nullptr gives the frame loop back
// to the game. The caller keeps ownership.
void BattleSetFrameDriver(IFrameDriver* driver);
// The driven tick about to run is a replay of a rolled-back frame (SfvGame::Advance).
void BattleSetReplayingTick(bool replaying);
// Whether the tick being run is a resimulation (inside the frame driver).
bool BattleInReplayedTick();
// Research (`phasecost 0|1`): logs the battle update's parts' cost per tick.
void BattleSetPhaseTiming(bool on);
// Experiment (`resimskip 0|1`): replayed ticks leave out the presentation phases.
void BattleSetResimPresentationSkip(bool on);
// Driven ticks run the battle's jobs one after another on the game thread, in
// the scheduler's order (on by default; `serialjobs 0|1`); off, the game's
// worker threads take them as they come, in an order that can differ by PC.
void BattleSetSerialJobs(bool on);
bool BattleSerialJobs();
// Research (`joborder [n]`): the jobs of the next n driven job runs, in order.
void BattleLogJobOrderNext(int calls);
// BattleSetFrameDriver(nullptr), then waits until the game thread has left the
// frame it was running with the driver (up to 5 s): after it, no OnFrame of
// the old driver runs.
void BattleDetachFrameDriver();
IFrameDriver* BattleCurrentFrameDriver();
// Research (`regions <size>`): logs the snapshot regions of that size with
// their addresses and owners; size 0: every owner with the sizes it holds.
void BattleLogRegionsOfSize(size_t size);
// Research (`rfind f <float>` | `i <int>` | `bac <KWBAC name>`): where the
// snapshot regions hold that float, int32, or a pointer into that move file.
void BattleFindInRegions(const std::string& kind, const std::string& a, const std::string& b);
// Research (`dumpbac <name>`): a loaded move file's bytes to Polarity\research\<name>.bin.
void BattleDumpMoveFile(const std::string& name);
// Research (`actorlog <ticks>`): per tick, each script runner's move file,
// script offset and frame, and the position regions' floats.
void BattleLogActors(int ticks);
void BattleActorLogTick();
// Research (`cinelog 2|1|0`): the fighters' cinematic flags and scripts per
// tick (replayed ticks marked), the drawn camera per frame while one plays.
void BattleLogCinematics(int mode);
void BattleCineLogTick();
void BattleCineLogFrame();
// Research (`camtrace <from> <to> [class text]`): camera objects' bytes per
// tick and frame to Polarity\research\camtrace.bin.
void BattleTraceCameraObjects(uint32_t from, uint32_t to, const std::string& text);
// Records the traced objects at a tick's end (sfv_game.cpp) or a rendered frame.
enum class TraceAt { TickEnd,
                     Frame };
void BattleCamTraceRecord(TraceAt at);
// Research (`rsnap`, `rdiff side|all [prefix]`): copies every snapshot region,
// then logs the words that changed since (side: flips between 1.0/-1.0 or 0/1).
void BattleSnapRegions();
void BattleDiffRegions(const std::string& mode, const std::string& prefix = "");
// Research (`ftrace <ticks>`): every changed snapshot word per tick, with
// the fighters' script and frame, to Polarity\research\ftrace.bin.
void BattleTraceRegions(int ticks);
void BattleFtraceTick();
// Research (`freeze 1|0`): the battle runs no ticks (held on its frame) / runs again.
void BattleSetFreeze(bool on);
// Research (`rpeek <region> <hex offset, may be negative> [qwords]`).
void BattlePeekRegion(const std::string& name, long long offset, int qwords);
// Research (`watchname <region> <hex offset> <len>`): a write watch on a region by name (`watchreport`).
void BattleWatchRegionNamed(const std::string& name, size_t offset, int length);
// Research (`rdump <region> <tag>`): its bytes, from 0x40 before it, to Polarity\research\<tag>.bin.
void BattleDumpRegion(const std::string& name, const std::string& tag);
// Research (`rcmp <a> <b> <hex from> <hex to>`): small u32 values that differ.
void BattleCompareRegions(const std::string& a, const std::string& b, long long from, long long to);
// Research (`ptrscan <from> <before> <after> <to>`): pointers to region `to`'s
// owner in the memory around region `from`.
void BattleScanForPointers(const std::string& from, size_t before, size_t after, const std::string& to);
// Research: the object a region lies in, as "vtable rva 0x... at -0x..." (the
// nearest vtable pointer at or before it), or "unknown".
std::string BattleRegionOwner(const void* region);
// The match setup's seed, which the random draws and picks in driven ticks
// start from (the same on every machine of a match).
void BattleSetMatchDrawSeed(uint64_t seed);
// Starts a driven tick's counts of random draws (per stream) and global
// picks (per call site), which key their seeds, and seeds the engine's
// rand() for the tick: called as each driven tick begins (game thread).
void BattleSeedTickRandom();
// Research: logs the next `count` random stream draws made in driven ticks
// (tick, stream, caller).
void BattleLogRandomDraws(int count);
// Research: logs every random draw (streams and global picks) in driven ticks
// from..to, rollbacks' resimulations included.
void BattleLogRandomDrawsInTicks(uint32_t from, uint32_t to);
// Research: logs the game's own per-pad input values for the next `ticks`
// ticks, when they change.
void BattleLogGameInputs(int ticks);
// Research: logs the next `count` fighter bone positions battle code asks for
// in driven ticks (rva 0x3919D0: tick, caller, result); `from` (non-zero):
// only calls returning to that rva.
void BattleLogBonePositions(int count, uintptr_t from);
// Research: logs the next `count` CriWare cue plays (tick, resimulated or not, thread).
void BattleLogSoundPlays(int count);
// The sounds the game asks for between BattleBeginSoundCapture and
// BattleStopCapturedSounds (a muted run: ui_sound.h UiSoundMuteGame) are stopped by
// the latter (what is left of them, which unmuting would make heard); it
// returns how many were asked for. A thread marked exempt (the mod's own
// menu sounds) is not captured.
void BattleBeginSoundCapture();
int BattleStopCapturedSounds();
void BattleSetSoundCaptureExempt(bool exempt);
// How many are captured now; BattleKeepCapturedFrom: those captured since the mark
// are left playing (a round call's first tick, run muted as a jump found it).
size_t BattleSoundCaptureMark();
void BattleKeepCapturedFrom(size_t mark);
// A viewed replay's battle: its sounds are kept track of (on), so that
// BattleStopSounds can stop every one still playing (loops, ambience) before
// the battle is started again; returns how many it had. Off forgets them.
// BattleStopSounds also ends the capture: the old battle's players go with it.
void BattleRecordSounds(bool on);
int BattleStopSounds();
// Logs the current battle's start probe (SfvGame::ArmStartProbe), once.
void BattleLogStartProbe();

} // namespace sfv::mod
