#pragma once

#include "mod/replay_driver.h"

#include "mod/battle_data.h"
#include "core/build_id.h"
#include "core/netplay.h"
#include "core/file_open.h"
#include "core/replay_file.h"
#include "core/state.h"
#include "core/transport.h"
#include "mod/battle_driver.h"
#include "mod/battle_setup.h"
#include "mod/benchmark.h"
#include "mod/hitch.h"
#include "mod/hud_step.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/files.h"
#include "mod/flow.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/sfv_game.h"
#include "mod/ui_sound.h"
#include "mod/ue4.h"
#include "mod/watch.h"

#include <windows.h>
#include <algorithm>
#include <ctime>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sfv::mod {

} // namespace sfv::mod

namespace sfv::mod::replay {

inline std::mutex g_viewMutex;

inline std::vector<std::pair<ReplayControl, int>> g_viewCommands;

inline ReplayView g_view; // as the driver last published it

inline std::atomic<int> g_tickFrame{-1};

inline std::atomic<bool> g_tickFight{false};

inline std::atomic<int> g_tickRound{0}; // round calls up to the tick's starting frame (ReplayTickRound)

inline uint32_t g_viewTaken = 0; // commands the driver took from g_viewCommands (published later as g_view.handled)

inline std::function<void()> g_viewLeave;

struct RestartPending {
    int target = -1;
    bool paused = false;
    int fast = 0;
    std::vector<int> starts, ends, calls;
};

inline RestartPending g_restartPending;

inline std::atomic<bool> g_callKeys{true}, g_crossRounds{false}, g_anyKeys{false}, g_keysAnywhere{true};

inline std::atomic<bool> g_skipIntro{true};

// A run behind the black takes at most this many ticks per rendered frame (docs/replays.md); 0: no cap.
constexpr int kRunTicksPerFrame = 16;
inline std::atomic<int> g_callPace{0}, g_callAfter{90}, g_runTicks{kRunTicksPerFrame};

inline std::atomic<int> g_latch{-1}; // `latch tick|game`: the pad latch the next replay uses (-1: by recording)

inline std::atomic<uint32_t> g_forcedGuessXor{0}; // the forced frames first run on the remote input XOR this (a wrong guess)

bool ForcedRollback(int frame);

class NoTransport : public ITransport {
public:
    void Send(uint64_t, const uint8_t*, size_t) override {}

    bool Receive(Packet&) override { return false; }
};

class ReplayDriver : public IFrameDriver {
public:
    ReplayDriver(ReplayFile rf, std::string name, int stress, int watchFrom, int watchTo, std::string watchRegion);

    void SetViewer(std::string title, std::string path);

    void SetBenchmark() { bench_ = true; }

    void StartAt(const RestartPending& r);

    void Keep(int from, int to);

    void Hold(int frame, int seconds);

    struct Trap {
        int frame;
        std::string region;
        long long offset; // may be negative: the owner's memory before the region
        int bytes;
    };

    void AddTrap(Trap t) { traps_.push_back(std::move(t)); }

    void Mispredict(int delay, int remote);

    bool OnFrame(SfvGame& game) override;

private:
    static constexpr int kKeyEvery = 60;

    struct Keyframe {
        int frame = 0;      // the state before this frame
        bool fight = false; // taken in a round's fight (after FIGHT, before its end)
        bool call = false;  // taken just before a round's call: a round jump back within the round loads it
        std::vector<uint8_t> regions;
        OutsideState outside;
    };

    int LoadableKey(int target) const;

    int CallKeyFor(int target) const;

    bool FightAhead() const;

    struct ActionRange {
        int first = -1, last = -1;
    };

    std::vector<ActionRange> ActionRanges() const;

    int BackLimit() const;

    bool ViewerFrame(SfvGame& game);

    enum class TakeoverPhase { None,
                               Countdown,
                               Live,
                               KoScreen,
                               FadeOut,
                               ToFight };
    static constexpr ULONGLONG kTakeoverCountdownMs = 3000;
    static constexpr ULONGLONG kTakeoverKoScreenMs = 2500; // the KO's slow motion and screen, as the game plays them
    static constexpr ULONGLONG kTakeoverFadeMs = 350;      // the stripe's fade to black (kFadeOut) and a little
    static constexpr int kTakeoverRunMax = 60 * 60;        // ticks behind the black looking for a fight
    static constexpr int kTakeoverWarm = 60;               // frames run in full before the start point (the camera)
    static constexpr int kTakeoverKoTicks = 56;

    struct Takeover {
        TakeoverPhase phase = TakeoverPhase::None;
        int side = 0;
        ULONGLONG until = 0;
        Keyframe start;
        PackedInput lastInput = 0;
        int inputLogs = 0;
        int ticks = 0;
        int koFrame = -1;
        std::vector<uint8_t> ko; // the state on the KO's frame (TakeoverRewind)
    };

    void TakeoverControl(SfvGame& game, ReplayControl c, int arg);

    void TakeoverFrame(SfvGame& game);

    void TakeoverTick(SfvGame& game);

    void TakeoverFadeOut();

    void TakeoverRewind(SfvGame& game);

    void TakeoverLoad(SfvGame& game);

    void Simulate(SfvGame& game, bool lean);

    void Seek(SfvGame& game, int target, bool quiet = false);

    void EndQuiet();

    void RestartAt(SfvGame& game, int target);

    void Control(SfvGame& game, ReplayControl c, int arg);

    void Publish();

    int CallsThrough(int f) const { return static_cast<int>(std::upper_bound(roundCalls_.begin(), roundCalls_.end(), f) - roundCalls_.begin()); }

    int StartFrame() const { return g_skipIntro && !roundCalls_.empty() ? roundCalls_.front() + 1 : 0; }

    bool InOpeningRun() const;

    bool CanRoundForward() const;

    int PreviousRoundTarget() const;

    bool CanStepForward() const { return wasFight_ && frame_ <= last_; }

    bool viewer_ = false;
    bool bench_ = false;
    bool benchCapturing_ = false, benchMeasured_ = false, benchStopped_ = false;
    std::vector<hitch::CapturedFrame> benchCaptured_;
    std::string title_;
    std::vector<Keyframe> keys_;
    std::vector<int> roundStarts_; // frames whose tick began a round's fight
    std::vector<int> roundCalls_;  // frames whose tick began a round's call (Round jumps land there)
    bool wasCall_ = false;
    Keyframe scratch_{-1}; // the state before the last tick outside a fight (a call key in waiting)
    bool wasFight_ = false;
    bool paused_ = false;
    bool findRound_ = false;
    bool quiet_ = false;                        // a step back's mute, kept while paused there (Seek)
    int runToFight_ = -1;                       // a jump back waiting for a fight to load from (Seek)
    Takeover tk_;                               // Replay Takeover (TakeoverControl)
    bool crossFight_ = false;                   // LoadableKey: a takeover's rewind (fight keys across its round end)
    int warm_ = 0;                              // a seek's last frames run in full (Seek's run, TakeoverLoad)
    int handledNow_ = 0;                        // controls taken since the last Publish (ReplayView::handled)
    int hudTicks_ = 0;                          // ticks simulated this rendered frame (Simulate: the HUD's step)
    static constexpr int kHudFightFrames = 120; // "FIGHT!" plays out within them (90 measured clean)
    static constexpr ULONGLONG kKoScreenMs = 1500;
    ULONGLONG koMs_ = 0;         // when the last KO was simulated
    ULONGLONG koHoldUntil_ = 0;  // a run waits until then
    uint32_t roundEndsSeen_ = 0; // FlowRoundEndEvents as of the last tick
    int target_ = -1;            // seeking: the frame to simulate next once there
    int fast_ = 0;               // frames a rendered frame while playing (0, 1: as played)
    int keyWarnings_ = 0;
    int seeks_ = 0;              // loads of a keyframe (testing)
    std::vector<int> roundEnds_; // frames whose tick ended a round's fight
    std::string path_;           // the file, for a restart
    int startAt_ = -1;           // a restart's target (RestartAt)
    bool restarting_ = false;

    bool Warmup(SfvGame& game);

    bool StraightFrame(SfvGame& game);

    bool StressFrame(SfvGame& game);

    bool MispredictFrame(SfvGame& game);

    PackedInput Guess(int k, int known) const;

    void RunPredicted(SfvGame& game, int k, int known, bool rollingBack);

    void Resimulate(SfvGame& game, int from, int to);

    void ConfirmBySum(int k);

    void MaybeHold(int f);

    static void ArmFromHelper(int slot, const void* address, int length, uintptr_t base);

    void WatchFrame(SfvGame& game, int f);

    static constexpr uint32_t kTrailVtable = 0x2C6FB88;
    static constexpr size_t kTrailBytes = 0x200, kManagerBytes = 0xB00;
    std::map<const uint8_t*, std::vector<uint64_t>> trailSeen_;

    void LogTrails(SfvGame& game, int f);

    std::string CameraText();

    bool Valid(SfvGame& game);

    const std::vector<StateRegion>& CheckRegions(SfvGame& game);

    void CheckFrame(SfvGame& game, int f);

    void TakeFailDump(SfvGame& game, int f, const char* label);

    void CheckFrameAgainstRecording(SfvGame& game, int f);

    void CompareDump(SfvGame& game, const StateDump& dump, int f);

    int Compare(SfvGame& game, const uint64_t* ours, const uint64_t* recorded, int f);

    void Finish(SfvGame& game);

    ReplayFile rf_;
    std::string name_;
    int keepFrom_ = -1, keepTo_ = -1;
    std::vector<std::pair<int, uint64_t>> holds_; // tests' holds: frame, ms (Hold, `rview holdat`)
    uint64_t holdUntil_ = 0;
    std::vector<Trap> traps_;
    int trapSlots_ = 0;
    ReplayFile kept_;
    int stress_;
    int delay_ = 0, remote_ = 1;
    std::vector<FrameInputs> used_;
    std::vector<uint32_t> sums_;
    std::vector<std::vector<uint8_t>> ring_;
    int rollbacks_ = 0, resimulated_ = 0;
    int creationsAtJump_;
    int watchFrom_, watchTo_;
    std::string watchRegion_;
    const uint8_t* camera_ = nullptr; // the battle's camera manager (CameraText)
    int cameraTries_ = 0;
    int last_ = -1;
    bool jumped_ = false;
    bool ready_ = false;
    int layout_ = 0;
    int postLoadLogs_ = 0;                  // research: the round's state on the ticks after a keyframe load
    std::vector<StateRegion> checkRegions_; // CheckRegions
    int checkLayout_ = -1;
    size_t checkCount_ = 0;
    int frame_ = 0;
    int fed_ = 0;
    int checked_ = -1;
    int framesChecked_ = 0;
    int firstMismatch_ = -1;
    int mismatchFrames_ = 0;
    int presentationFrames_ = 0;              // camera / pawn clock differences (not a failure)
    std::map<std::string, int> regionFrames_; // frames each region differed on
    std::set<int> stressFrames_;              // frames resimulated differently (not the clocks)
    int resimFirst_ = -1;                     // the first of them reported
    std::vector<std::string> resimRegions_;   // its differing regions (at most 4)
    std::vector<uint8_t> failDump_;           // the benchmark's first differing frame's state (TakeFailDump)
    int failDumpFrame_ = -1;
    std::string failDumpLabel_;
    std::vector<RegionInfo> failRegions_;
    std::vector<uint64_t> failHashes_;
    std::string benchSession_; // BenchmarkActivityText() as the benchmark began
    std::map<std::string, int> stressRegionFrames_;
    int stressLogged_ = 0;
    int logged_ = 0;
    NoTransport transport_;
    std::unique_ptr<NetplaySession> session_;
};

} // namespace sfv::mod::replay
