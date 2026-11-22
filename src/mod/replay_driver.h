#pragma once
// Plays a match recording (.sfvr) back in the game: the "replay <file>
// [stress <distance>]" command.
//
// The battle is started from the recording's match setup the way a netplay
// session starts it (a jump with the setup, neutral ticks until the battle's
// objects exist, frozen at frame 0), the state before frame 0 is compared
// with the recording's, then the recorded inputs are played and every
// frame's state is compared, region by region, with the hashes the recording
// holds:
// - straight (no stress): one frame per update, as it was played. A frame
//   that differs means this build does not reproduce the recording (state
//   outside the snapshot that the recording machine had, a different build,
//   or a rollback in the recorded session that did not restore everything);
// - stress <distance>: a GekkoNet stress session fed with the recorded
//   inputs: every frame is also loaded from <distance> frames back and
//   resimulated, and the resimulation must equal the first run. This finds
//   rollback bugs at exactly the recorded situation (characters, moves,
//   round ends), whether or not the recorded session rolled back there;
// - mispredict <delay> [remote 1|2]: one player's inputs arrive <delay>
//   frames late, as over a network: frames run on GekkoNet's guess (that
//   player's last input that arrived), and a guess that turns out wrong is
//   rolled back and resimulated. Stress replays only ever resimulate with the
//   right inputs; this finds state that a mispredicted frame leaves behind.
// The result goes to the log ("replay: PASS / FAIL ..."), with the first
// differing frames and regions.

#include <functional>
#include <string>
#include <vector>

namespace sfv::mod {

// From any thread; starts with the next battle frame or jump. A relative path
// is taken from Polarity\matches, else the reports folder earlier builds used.
// "stop" ends the running replay.
// watchFrom..watchTo (-1: none): logs watchRegion's first 16 bytes and the
// round's end state for each of those frames (research).
// keepFrom..keepTo (>= 0, straight runs): the run saved as a full recording
// next to the file (<name>_kept.sfvr), with state dumps of those frames.
// preload false: the jump without loading the characters first.
// traps: "<frame> <region> <hex offset> <bytes>" groups, write watches armed
// before that frame's tick, their writers logged at the end (research).
// holdAt (>= 0): the replay stands still after that frame for holdSeconds.
// mispredictDelay (> 0, no stress): player `remote`'s (0/1; -1: the one the
// recording machine played against) inputs arrive that many frames late.
void ReplayRequest(const std::string& path, int stressDistance, int watchFrom = -1, int watchTo = -1,
                   const std::string& watchRegion = "", int keepFrom = -1, int keepTo = -1, bool preload = true,
                   const std::string& traps = "", int holdAt = -1, int holdSeconds = 0, int mispredictDelay = 0,
                   int remote = -1, const std::string& viewTitle = "", bool jumpNow = false, bool benchmark = false);

// The Battle Log's playback of a recording: as played, with the controls
// below; the game goes back to the menus when it is left, then onLeave runs
// (on the game thread). holdAt/holdSeconds: tests' screenshots (the `hold` of ReplayRequest).
void ReplayRequestView(const std::string& path, const std::string& title, std::function<void()> onLeave, int holdAt = -1,
                       int holdSeconds = 0);
enum class ReplayControl {
    Pause,
    Play,
    Toggle,
    Step,  // arg frames (negative: back, silent, never past backLimit; forward only through action frames), paused after
    Seek,  // to frame arg (the state after frame arg - 1)
    Round, // arg > 0 the next round's start, else this round's (or the one before)
    Fast,  // arg frames per rendered frame (0 or 1: as played) through action frames
    Leave,
    // Replay Takeover: in a round's fight, side arg (0 P1, 1 P2) is played
    // with the local pad (InputMappedLocal) after a 3 s countdown, until the
    // round's fight ends (KO or time); then back to where it began, paused.
    Takeover,
    TakeoverStop, // early: back to where it began (or the countdown called off)
    OutOfSync,    // testing: the frame on screen counted as differing from the recording
    HoldAt,       // testing: stand still 5 s after frame arg (a screenshot)
};
// Any thread; taken at the replay's next frame. Returns the command's number:
// ReplayView::handled reaches it once the replay has taken it (0: no replay).
uint32_t ReplayApplyControl(ReplayControl c, int arg = 0);
// Research (`rview callkeys|crossrounds 0|1`): round jumps load the keyframe
// before a round's call; keyframe loads across round ends.
void ReplaySetExperiment(const std::string& what, bool on);
void ReplaySetExperimentValue(const std::string& what, int value);

struct ReplayView {
    bool active = false;
    bool paused = false;
    bool seeking = false;
    bool opening = false; // running the intro to round 1's call: black until the call
    int frame = 0;        // on screen
    int frames = 0;       // in the recording
    int round = 0;        // 1 from the first round's FIGHT; 0 before it
    int callRound = 0;    // 1 from the first round's call ("ROUND 1"); 0 before it
    int fast = 0;
    // The earliest frame a step back reaches: one a keyframe of this round
    // loads (going back further restarts the battle, which steps never do).
    int backLimit = 0;
    // Steps forward and fast forward run only through a round's action
    // frames (after "FIGHT!" to its KO); the rest plays at normal speed.
    bool canStepForward = false;
    // RB has a round to go to: one starts after the round on screen (the
    // file's rounds; a file without them: until its end).
    bool canRoundForward = true;
    // The frame on screen within its round's action frames (0 the first, the
    // KO's the last), -1 outside them; how many there are (0: not known);
    // the stripe's fill (0..1).
    int roundFrame = -1;
    int roundFrames = 0;
    float roundProgress = 0;
    // Replay Takeover: the side played with the local pad (-1: none), the
    // countdown's seconds left (0 once it plays), and whether the fight has
    // ended and the replay is about to go back to where it began.
    int takeover = -1;
    float takeoverCountdown = 0;
    bool takeoverEnding = false;
    bool takeoverFading = false; // the KO screen over: to black, back to the start behind it
    // Controls the replay has taken so far, published with their effect (a
    // seek they started shows as seeking from then on): the UI's fades wait on it.
    uint32_t handled = 0;
    int mismatches = 0; // frames that differed from the recording (testing)
    // The round (1..) in which the replay stopped matching the recording
    // (0: before the first; -1: it matches so far). A recording from a build
    // that computes battles differently (a missed sim version bump).
    int outOfSync = -1;
    int seeks = 0;          // keyframe loads (testing)
    bool benchmark = false; // the benchmark's run (benchmark.h), not a viewed replay
    std::string title;
};

ReplayView ReplayViewNow();
// Game thread, inside a viewed replay's tick: the recording frame the tick
// plays, and whether the state it starts from (after the frame before) is in
// a round's fight (the frame meter's per-frame record, frame_meter.cpp).
int ReplayTickFrame();
bool ReplayTickInFight();
// The round (by its call: 1 from "ROUND 1") of the state the tick starts from.
int ReplayTickRound();
// The benchmark's run of its round (benchmark.h): a stress replay with an
// 8-frame rollback every frame, its FIGHT to its end measured; back to the
// menus after, the result to BenchmarkFinish. B (ReplayControl::Leave) stops.
bool ReplayRequestBenchmark(const std::string& path);
// The pad latch of the next replay (research): after every tick
// (SfvGame::latchEachTick), the game's once-per-rendered-frame latch, or (the
// default) picked by the recording's build and the replay's mode.
enum class PadLatch { ByRecording,
                      EachTick,
                      PerRenderedFrame };
void ReplaySetLatch(PadLatch latch);
// Research: frames a mispredict replay rolls back to whatever the guess (empty: none).
void ReplaySetForcedRollbacks(std::vector<int> frames, uint32_t guessXor);

} // namespace sfv::mod
