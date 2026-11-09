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

bool InstallBattleDriver(const GameImage& image, SfvGame* game);
// The driver used from the next frame on; nullptr gives the frame loop back
// to the game. The caller keeps ownership.
void SetFrameDriver(IFrameDriver* driver);
// The driven tick about to run is a replay of a rolled-back frame (SfvGame::Advance).
void SetReplayingTick(bool replaying);
// Research (`phasecost 0|1`): logs the battle update's parts' cost per tick.
void SetPhaseTiming(bool on);
// Experiment (`resimskip 0|1`): replayed ticks leave out the presentation phases.
void SetResimPresentationSkip(bool on);
// SetFrameDriver(nullptr), then waits until the game thread has left the
// frame it was running with the driver (up to 5 s): after it, no OnFrame of
// the old driver runs.
void DetachFrameDriver();
IFrameDriver* CurrentFrameDriver();
// Research (`regions <size>`): logs the snapshot regions of that size with
// their addresses and owners; size 0: every owner with the sizes it holds.
void LogRegionsOfSize(size_t size);
// Research: the object a region lies in, as "vtable rva 0x... at -0x..." (the
// nearest vtable pointer at or before it), or "unknown".
std::string RegionOwner(const void* region);
// The match setup's seed, which the random draws and picks in driven ticks
// start from (the same on every machine of a match).
void SetMatchDrawSeed(uint64_t seed);
// Starts a driven tick's counts of random draws (per stream) and global
// picks (per call site), which key their seeds, and seeds the engine's
// rand() for the tick: called as each driven tick begins (game thread).
void SeedTickRandom();
// Research: logs the next `count` random stream draws made in driven ticks
// (tick, stream, caller).
void LogRandomDraws(int count);
// Research: logs every random draw (streams and global picks) in driven ticks
// from..to, rollbacks' resimulations included.
void LogRandomDrawsInTicks(uint32_t from, uint32_t to);
// Research: logs the game's own per-pad input values for the next `ticks`
// ticks, when they change.
void LogGameInputs(int ticks);
// Research: logs the next `count` fighter bone positions battle code asks for
// in driven ticks (rva 0x3919D0: tick, caller, result); `from` (non-zero):
// only calls returning to that rva.
void LogBonePositions(int count, uintptr_t from);
// Research: logs the next `count` CriWare cue plays (tick, resimulated or not, thread).
void LogSoundPlays(int count);
// The sounds the game asks for between BeginSoundCapture and
// StopCapturedSounds (a muted run: ui_sound.h MuteGameSounds) are stopped by
// the latter (what is left of them, which unmuting would make heard); it
// returns how many were asked for. A thread marked exempt (the mod's own
// menu sounds) is not captured.
void BeginSoundCapture();
int StopCapturedSounds();
void SetSoundCaptureExempt(bool exempt);
// How many are captured now; KeepCapturedFrom: those captured since the mark
// are left playing (a round call's first tick, run muted as a jump found it).
size_t SoundCaptureMark();
void KeepCapturedFrom(size_t mark);
// Logs the current battle's start probe (SfvGame::ArmStartProbe), once.
void LogStartProbe();

} // namespace sfv::mod
