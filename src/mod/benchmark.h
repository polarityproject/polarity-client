#pragma once
// The benchmark (docs/benchmark-plan.md; mock-up canvas "Polarity Benchmark"):
// one recorded round, built into the DLL, played as a stress replay with an
// 8-frame rollback on every frame (the most a match can have), measured from
// its FIGHT to its end. It passes with an average of 59 fps or more, and only
// when the round played exactly as recorded (else this PC would desync).

#include <cstdint>
#include <string>
#include <vector>

namespace sfv::mod {

struct BenchmarkResult {
    bool valid = false;   // a run finished
    bool matched = true;  // the round played as recorded
    int firstDiffer = -1; // the first frame that did not
    int frames = 0;       // presented frames measured
    double seconds = 0;
    double averageFps = 0;
    double lowFps = 0; // the 1% low
    double slowestMs = 0;
    int slowFrames = 0;    // frames over 20 ms (a frame late; 16-18 ms is pacing jitter)
    double rollbackMs = 0; // per frame: the driver's ticks and rollbacks
    double gameMs = 0;     // the game's battle frame besides
    double drawMs = 0;     // the overlay and Present
    uint64_t when = 0;     // unix time
    std::string version;   // Polarity's
    // The install's battle data (battle_data.h), when the run ended:
    // mods: "none", or "<paks>[,<n>-battle][:<characters>]" (ModScan::Short);
    // data: "stock", "changed:<characters>", "unknown" (this build has no
    // table of the game's own, or the check had not finished), "" (an older run).
    std::string mods, data;
    // How it went wrong, to tell a cause apart without the player's log:
    // differ "straight" (the round played forward came out different from
    // the recording), "resim" (only a rollback's replay of a frame did),
    // "both", or "" (it matched); the frames of each; the first resimulated
    // frame's differing regions ("s300.0,flow.round"); the session before
    // the run ("up12m,battles3,match2,replay0,training1,bench0,desync0");
    // the state dump of the first differing frame (Polarity\logs, "" none).
    std::string differ;
    int straightFrames = 0, resimFrames = 0;
    std::string regions, session, dump;

    bool DataChanged() const { return data.rfind("changed:", 0) == 0; }

    bool ModsChangeBattle() const { return mods.find("-battle") != std::string::npos; }

    bool Pass() const { return valid && matched && averageFps >= 59.0; }
};

// Starts a run from the menus (the built-in round written to Polarity\benchmark.sfvr
// first). False when it cannot (no round in this build, a driver busy).
bool StartBenchmark(const std::string& modVersion);
// The run's progress, for the stripe (frame, of frames; false when none).
bool BenchmarkRunning(int* frame = nullptr, int* frames = nullptr);
// The last result (this session's, else the last one logged in
// Polarity\logs\benchmark.txt); valid false when there is none.
BenchmarkResult LastBenchmark();
// A run just ended: its result once (then false).
bool TakeBenchmarkResult(BenchmarkResult* out);

// From the replay driver: the measured frames, how the round played.
struct CapturedRun {
    std::vector<int64_t> micros, net, battle, draw;
    bool matched = true;
    int firstDiffer = -1;
    bool stopped = false;                    // the player stopped it
    int straightFirst = -1, resimFirst = -1; // the first frame of each kind of difference
    int straightFrames = 0, resimFrames = 0;
    std::string regions; // the first resimulated frame's differing regions, comma-separated
    std::string session; // SessionActivityText() as the run began
    std::string dump;    // the state dump's file name, "" none
};

// What this game session did before (the benchmark's result says): counts
// of each kind ("match", "replay", "training", "benchmark", "desync") and
// the battles, with the time since the game started.
void NoteSessionActivity(const char* kind);
std::string SessionActivityText();
// Research (`benchmark dump <frame>`): the next runs save that frame's state
// as a failing run saves its first differing frame's (a reference to compare
// a player's dump with); -1 off.
void SetBenchmarkDumpFrame(int frame);
int BenchmarkDumpFrame();
void FinishBenchmark(const CapturedRun& run);

} // namespace sfv::mod
