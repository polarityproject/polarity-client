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

#include <string>

namespace sfv::mod {

// From any thread; starts with the next battle frame or jump. A relative path
// is taken from SFVR\matches, else the reports folder earlier builds used.
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
void RequestReplay(const std::string& path, int stressDistance, int watchFrom = -1, int watchTo = -1,
                   const std::string& watchRegion = "", int keepFrom = -1, int keepTo = -1, bool preload = true,
                   const std::string& traps = "", int holdAt = -1, int holdSeconds = 0, int mispredictDelay = 0,
                   int remote = -1);

} // namespace sfv::mod
