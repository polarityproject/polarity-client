#pragma once
// .sfvr: a match recording that is also a desync report.
//
// Each peer writes its own file. It holds the confirmed inputs of both players,
// the state's checksum after every frame and, for some frames, one hash per
// state region taken right after the frame was simulated: every frame in a
// full recording, only the frames around each desync in a compact one (what
// the mod writes for players, a few hundred KB a match). Two files of the same
// match can be lined up frame by frame to find the first frame, and where
// region hashes were kept the regions, where the peers disagreed; one file can
// be played back offline against the game to check that the simulation
// reproduces its own checksums (see verifier.h).

#include "core/game.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sfv {

constexpr uint32_t kReplayFormatVersion = 3; // 2: 32-bit inputs; 3: checksums, region hashes in windows, compression

// Which frames' region hashes a saved file keeps (the checksums are always
// there, for every frame).
enum class ReplayDetail {
    Full,    // every frame that has them
    Compact, // only from kDesyncHashesBefore frames before each desync to kDesyncHashesAfter after it
};
constexpr int kDesyncHashesBefore = 120;
constexpr int kDesyncHashesAfter = 30;

enum class ReplayEventType : uint16_t {
    Rollback = 1,   // frame: frame loaded, a: frames resimulated
    Desync = 2,     // frame: desynced frame, a: local checksum, b: remote checksum
    Disconnect = 3, // frame: last frame simulated
    Note = 4,       // a, b: free use
};

struct ReplayEvent {
    int32_t frame = 0;
    ReplayEventType type = ReplayEventType::Note;
    uint32_t a = 0;
    uint32_t b = 0;
};

struct RegionInfo {
    std::string name;
    uint64_t size = 0;
    std::vector<uint8_t> ignore; // see StateRegion::ignore
    bool operator==(const RegionInfo&) const = default;
};

// Raw state, regions concatenated in table order.
struct StateDump {
    int32_t frame = 0;
    std::string label;
    std::vector<uint8_t> bytes;
};

// What the Battle Log lists (the INFO chunk, written uncompressed right after
// the setup, so a list reads only a file's first bytes: LoadReplayHeader).
// Files from before it have none (present false).
// A round's action frames: the frames whose tick ran with its fight on,
// from the one after "FIGHT!" to its KO (or time over), both included; -1
// when not known (files before the chunk's version 2, a match left mid-round).
struct RoundResult {
    int32_t endFrame = -1;   // the last action frame: the KO's
    int8_t winner = -1;      // 0, 1, or -1 (a draw)
    int32_t fightFrame = -1; // the first action frame
};
// The rounds' action frames from a session's per-frame flags
// (ReplayFile::frameFlags), in order; a round still on at the end has endFrame -1.
std::vector<RoundResult> RoundsFromFrameFlags(const std::vector<uint8_t>& flags);
struct MatchSummary {
    bool present = false;
    std::string names[kNumPlayers];      // as the players typed them
    uint8_t roundWins[kNumPlayers] = {}; // rounds each side won
    int8_t winner = -1;                  // the match's: 0, 1, or -1 (not decided: someone left)
    uint8_t ending = 0;                  // 0 played out, 1 left early
    std::vector<RoundResult> rounds;
    // The set: the match id of its first match (both players' files carry the
    // same), this match's number in it (1..), its rule ("First to 3", or
    // empty for a Direct Match session) and the score after this match.
    uint64_t setKey = 0;
    uint16_t setGame = 0;
    std::string setRule;
    uint8_t setWins[kNumPlayers] = {};
    uint32_t seconds = 0; // how long the match ran
};

struct ReplayFile {
    std::string modVersion;
    std::string gameBuild;
    uint64_t matchId = 0;      // chosen by the host, identical in both peers' files
    int32_t localPlayer = -1;  // -1 when not recorded in a live session
    uint64_t createdUnix = 0;
    uint32_t inputDelay = 0;
    uint32_t predictionWindow = 0;
    // Inputs and hashes up to and including this frame are confirmed. Later
    // frames may still hold predicted remote inputs (a session that ended early).
    int32_t finalThrough = -1;
    MatchSetup setup;
    std::vector<RegionInfo> regions;
    std::vector<uint64_t> initialHashes; // state before frame 0, one per region
    std::vector<FrameInputs> inputs;     // indexed by frame
    std::vector<uint64_t> hashes;        // [frame * regions.size() + region], state after the frame (0 where not kept)
    // Loaded from a file that kept region hashes for some frames only: 1 for
    // the frames it kept, and every frame's checksum. Empty when every frame
    // has its region hashes (a live recording, a full file).
    std::vector<uint8_t> hashesKept;
    std::vector<uint32_t> checksums;
    std::vector<ReplayEvent> events;
    std::vector<StateDump> dumps;
    MatchSummary summary;
    // A live session's: per frame, IGame::FrameFlags of its last simulation
    // (for the summary's rounds). Not saved.
    std::vector<uint8_t> frameFlags;

    int FrameCount() const { return static_cast<int>(inputs.size()); }
    size_t RegionCount() const { return regions.size(); }
    // Whether FrameHashes(frame) holds that frame's region hashes (always for
    // frame -1, the state before frame 0).
    bool HasRegionHashes(int frame) const;
    const uint64_t* FrameHashes(int frame) const;
    uint32_t FrameChecksum(int frame) const;
    // Records (or, after a rollback, overwrites) one frame.
    void SetFrame(int frame, const FrameInputs& in, const uint64_t* regionHashes);
    const StateDump* FindDump(int frame) const;
};

std::vector<uint8_t> SerializeReplay(const ReplayFile& replay, ReplayDetail detail = ReplayDetail::Full);
bool DeserializeReplay(const uint8_t* data, size_t len, ReplayFile* out, std::string* error);

bool SaveReplay(const ReplayFile& replay, const std::string& path, std::string* error,
                ReplayDetail detail = ReplayDetail::Full);
bool LoadReplay(const std::string& path, ReplayFile* out, std::string* error);
// Only the file's start: META, the setup and the summary (no inputs, no
// checksum check). For lists.
bool LoadReplayHeader(const std::string& path, ReplayFile* out, std::string* error);
bool DeserializeReplayHeader(const uint8_t* data, size_t len, ReplayFile* out, std::string* error);

} // namespace sfv
