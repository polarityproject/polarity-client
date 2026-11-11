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
    double lowFps = 0;      // the 1% low
    double slowestMs = 0;
    int slowFrames = 0;     // frames over 20 ms (a frame late; 16-18 ms is pacing jitter)
    double rollbackMs = 0;  // per frame: the driver's ticks and rollbacks
    double gameMs = 0;      // the game's battle frame besides
    double drawMs = 0;      // the overlay and Present
    uint64_t when = 0;      // unix time
    std::string version;    // Polarity's
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
    bool stopped = false; // the player stopped it
};
void FinishBenchmark(const CapturedRun& run);

} // namespace sfv::mod
