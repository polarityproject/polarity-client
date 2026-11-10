#include "mod/replay_driver.h"

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
#include "mod/control.h"
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
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sfv::mod {

namespace {

std::atomic<bool> g_stop{false}; // `replay stop`: end the running replay at the next frame

// The viewer's controls (ReplayControl), from the UI or the control file;
// the driver takes them at its next frame.
std::mutex g_viewMutex;
std::vector<std::pair<ReplayControl, int>> g_viewCommands;
ReplayView g_view;                 // as the driver last published it
// The recording frame the tick being run plays (-1: none), and whether the
// state it starts from (after the frame before) is in a round's fight.
std::atomic<int> g_tickFrame{-1};
std::atomic<bool> g_tickFight{false};
uint32_t g_viewTaken = 0;          // commands the driver took from g_viewCommands (published later as g_view.handled)
std::function<void()> g_viewLeave; // what leaving a viewed replay does after the battle
// A viewer restarting (a jump back across a round's end): what the next
// driver takes on (its target, paused, fast forward).
struct RestartPending {
    int target = -1;
    bool paused = false;
    int fast = 0;
    // The rounds' starts, ends and calls seen so far (a file without its
    // rounds' action frames: the counter and the jumps keep them).
    std::vector<int> starts, ends, calls;
};
RestartPending g_restartPending;
// Round jumps back load the keyframe taken just before a round's call, also
// across round ends until the match is decided (on; `rview callkeys 0` for
// tests). Research (`rview crossrounds 1`): any key across round ends (a
// fight key from the match's end diverged, 60 frames, 2026-11-09).
std::atomic<bool> g_callKeys{true}, g_crossRounds{false}, g_anyKeys{false}, g_keysAnywhere{true};
// The viewer starts at round 1's call: the intro runs muted and lean behind
// black (as a round jump's run), not as it played. Not a Start skip: that
// would play a different battle than the recording's. `rview intro 1` shows it.
std::atomic<bool> g_skipIntro{true};
std::atomic<int> g_callPace{0}, g_callAfter{90}; // research: a run's pace through a round's call (ViewerFrame)
std::atomic<int> g_latch{-1};    // `latch tick|game`: the pad latch the next replay uses (-1: by recording)

class NoTransport : public ITransport {
public:
    void Send(uint64_t, const uint8_t*, size_t) override {}
    bool Receive(Packet&) override { return false; }
};

// Where a bare file name is looked for: SFVR\matches (files.h), then the
// reports folder earlier builds wrote to (next to the exe).
std::string FindRecording(const std::string& name)
{
    const std::string inMatches = Utf8Path(MatchesDir()) + "\\" + name;
    if (FILE* f = OpenFile(inMatches, "rb")) {
        std::fclose(f);
        return inMatches;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    return Utf8Path(dir) + "\\reports\\" + name;
}

class ReplayDriver : public IFrameDriver {
public:
    ReplayDriver(ReplayFile rf, std::string name, int stress, int watchFrom, int watchTo, std::string watchRegion)
        : rf_(std::move(rf)), name_(std::move(name)), stress_(stress), creationsAtJump_(BattleCreationCount()),
          watchFrom_(watchFrom), watchTo_(watchTo), watchRegion_(std::move(watchRegion))
    {
        last_ = rf_.finalThrough >= 0 ? std::min(rf_.finalThrough, rf_.FrameCount() - 1) : rf_.FrameCount() - 1;
    }

    // The Battle Log's playback: played as it was, with pause, frame steps
    // both ways, round jumps and fast forward (ReplayControl).
    void SetViewer(std::string title, std::string path)
    {
        viewer_ = true;
        title_ = std::move(title);
        path_ = std::move(path);
    }
    void SetBenchmark() { bench_ = true; }
    void StartAt(const RestartPending& r)
    {
        startAt_ = r.target;
        paused_ = r.paused;
        fast_ = r.fast;
        roundStarts_ = r.starts;
        roundEnds_ = r.ends;
        roundCalls_ = r.calls;
    }

    void Keep(int from, int to)
    {
        keepFrom_ = from;
        keepTo_ = to;
    }

    // `hold <frame> <seconds>`: the replay stands still after that frame for
    // a while (the game keeps drawing it), for a screenshot.
    void Hold(int frame, int seconds)
    {
        holdAt_ = frame;
        holdMs_ = seconds * 1000ull;
    }

    // `trap <frame> <region> <offset> <bytes>`: a write watch on those bytes
    // from that frame's tick on (debug registers, 8 bytes each, 4 in all);
    // the writers are logged when the replay ends.
    struct Trap {
        int frame;
        std::string region;
        long long offset; // may be negative: the owner's memory before the region
        int bytes;
    };
    void AddTrap(Trap t) { traps_.push_back(std::move(t)); }

    // `mispredict <delay> [remote 1|2]`: player `remote`'s inputs arrive
    // `delay` frames late (MispredictFrame).
    void Mispredict(int delay, int remote)
    {
        delay_ = delay;
        remote_ = remote >= 0 ? remote & 1 : rf_.localPlayer == 1 ? 0 : 1;
    }

    bool OnFrame(SfvGame& game) override
    {
        if (g_stop.exchange(false)) {
            Log("replay: stopped");
            Finish(game);
            return false;
        }
        // The battle we jumped from runs until the new one is created.
        if (!jumped_) {
            // A viewer restarted from inside a fight: its jump waits in the
            // game-thread queue, which nothing else runs there.
            if (viewer_ || bench_) PumpGameThread();
            if (BattleCreationCount() == creationsAtJump_) return false;
            jumped_ = true;
            game.ForgetBattleObjects();
            game.BeginWarmup();
            Log("replay: battle created (%s)", RoundEndState().c_str());
        }
        if (!ready_) return Warmup(game);
        if (holdUntil_) {
            if (GetTickCount64() < holdUntil_) return true;
            holdUntil_ = 0;
        }
        if (viewer_) return ViewerFrame(game);
        if (stress_ > 0) return StressFrame(game);
        if (delay_ > 0) return MispredictFrame(game);
        return StraightFrame(game);
    }

private:
    // ---- the viewer -------------------------------------------------------
    // Keyframes every kKeyEvery frames as the replay first gets there, and one
    // just after each round's FIGHT (the regions and what a load needs beside
    // them, flow.h OutsideState): a step or a jump back loads the nearest one
    // at or before the frame before the target, runs the frames between as a
    // rollback runs them (lean, muted), and the last one as it played, so the
    // picture is that frame's. A jump forward runs on from where the replay is.
    static constexpr int kKeyEvery = 60;
    struct Keyframe {
        int frame = 0;      // the state before this frame
        bool fight = false; // taken in a round's fight (after FIGHT, before its end)
        bool call = false;  // taken just before a round's call: a round jump back within the round loads it
        std::vector<uint8_t> regions;
        OutsideState outside;
    };

    // The keyframe a jump back to `target` loads (the last at or before
    // target - 1), or -1 when that jump restarts the battle instead. Only
    // keys taken in a round's fight load, and only within that round: one
    // from the battle's opening (its start events, the intro run once), from
    // a round's end on (KO to the next FIGHT: a load there runs the next
    // round's reset differently, frame 3851 of the step test) or from before
    // a round's end (the round over leaves things outside the snapshot)
    // came out different (seek and step tests, 2026-11-09). On the KO's own
    // frame (the director just stopped, as a netplay rollback undoing a
    // predicted KO sees it) a load back is fine. The key just before a
    // round's call (after the round's reset) loads too, within its round:
    // LB to the round's start is a load, the music and ambience untouched
    // (a restart starts the battle's music over), 0 frames differing.
    int LoadableKey(int target) const
    {
        auto it = std::upper_bound(keys_.begin(), keys_.end(), target - 1, [](int f, const Keyframe& k) { return f < k.frame; });
        if (it == keys_.begin()) return -1;
        const int k = static_cast<int>(it - keys_.begin()) - 1;
        const int keyFrame = keys_[k].frame;
        const bool sameRound =
            std::none_of(roundEnds_.begin(), roundEnds_.end(), [&](int e) { return e >= keyFrame && e < frame_ - 1; });
        // A round call's key loads from a fight, in its round or across round
        // ends (round 3's fight to round 1's call, played on to the match's
        // end: 0 frames differing), never from between fights: after a KO,
        // after the next round's reset, after the final KO, all diverged
        // (4100-4600 frames; cross-test3/4, 2026-11-09). From there a jump
        // runs on to the next fight first (RunToFight).
        const bool call = g_callKeys && keys_[k].call && wasFight_;
        // A takeover's rewind: a fight key from the next round's fight too
        // (crossFight_; from a fight, as a round call's key loads).
        const bool crossFight = crossFight_ && keys_[k].fight && wasFight_;
        if (g_anyKeys) return k; // research: every key loads
        // Since the round end wait's whole tail and the latest round's record
        // follow loads (flow.cpp), any key from the first FIGHT on loads, from
        // anywhere: across round ends, from between fights, from after the
        // match's last KO (seek tests on LAR vs NCL 364c0a17, 2026-11-10: 0
        // frames differing over every kind). Keys from the opening (before
        // the first FIGHT) still restart: loaded after later rounds were
        // played, the round's setup (flow event 2: the director's handler
        // 0x482010, fighter resets 0x35755F / 0x3581FC) filled both
        // fighters' vitality blocks from a source the key does not hold, for
        // one tick (frame 721), then reset them. Not the director's record
        // (+0x130..+0x110C4, tried). Sessions never load across a round's
        // setup from a later round. Research: `rview anykeys 1`, `watchname`.
        const bool opening = roundStarts_.empty() || keyFrame <= roundStarts_.front();
        if (!opening && g_keysAnywhere) return k;
        return (keys_[k].fight && sameRound) || call || crossFight || (g_crossRounds && (keys_[k].fight || keys_[k].call)) ? k : -1;
    }

    // The key a jump back to `target` would load if the state now were in a
    // fight: a round call's (else -1).
    int CallKeyFor(int target) const
    {
        auto it = std::upper_bound(keys_.begin(), keys_.end(), target - 1, [](int f, const Keyframe& k) { return f < k.frame; });
        if (it == keys_.begin() || !g_callKeys) return -1;
        return std::prev(it)->call ? static_cast<int>(it - keys_.begin()) - 1 : -1;
    }

    // A round's fight still to come (as known: the file's rounds, or those
    // seen; not known yet: unless the match is decided).
    bool FightAhead() const
    {
        if (MatchDecided()) return false;
        const int shown = frame_ - 1;
        for (const ActionRange& r : ActionRanges()) {
            if (r.first > shown) return true;
        }
        const bool fileHasRounds = std::any_of(rf_.summary.rounds.begin(), rf_.summary.rounds.end(), [](const RoundResult& r) { return r.fightFrame >= 0; });
        return !fileHasRounds; // the file's rounds all behind: none ahead
    }

    // Each round's action frames (RoundResult: the frame after "FIGHT!" to
    // the KO's): from the file's summary, else (and for what it lacks) as
    // this replay has seen them; last -1 while not known.
    struct ActionRange {
        int first = -1, last = -1;
    };
    std::vector<ActionRange> ActionRanges() const
    {
        std::vector<ActionRange> out;
        for (const RoundResult& r : rf_.summary.rounds) {
            if (r.fightFrame >= 0) out.push_back({r.fightFrame, r.endFrame});
        }
        for (size_t i = 0; i < roundStarts_.size(); i++) {
            const int first = roundStarts_[i] + 1;
            auto end = std::lower_bound(roundEnds_.begin(), roundEnds_.end(), first);
            const int last = end != roundEnds_.end() && (i + 1 == roundStarts_.size() || *end <= roundStarts_[i + 1]) ? *end : -1;
            if (i < out.size()) {
                if (out[i].last < 0) out[i].last = last;
            }
            else out.push_back({first, last});
        }
        return out;
    }

    // The earliest frame on screen a step back reaches (Publish): the first
    // loadable keyframe's frame, or the frame on screen when there is none.
    int BackLimit() const
    {
        for (const Keyframe& k : keys_) {
            // Steps stay in the action, and in this round (a step back never crosses a KO).
            const bool thisRound = std::none_of(roundEnds_.begin(), roundEnds_.end(), [&](int e) { return e >= k.frame && e < frame_ - 1; });
            if (k.fight && k.frame < frame_ && thisRound && LoadableKey(k.frame + 1) >= 0) return k.frame;
        }
        return std::max(0, frame_ - 1);
    }

    bool ViewerFrame(SfvGame& game)
    {
        if (!Valid(game)) return false;
        if (frame_ == 0 && keys_.empty()) {
            roundEndsSeen_ = RoundEndEvents();
            RecordBattleSounds(true); // for a restart to stop (Finish)
            Log("replay: viewing %s, %d frames%s", title_.c_str(), last_ + 1, startAt_ > 0 ? " (restarted)" : "");
            if (startAt_ > 0) {
                MuteGameSounds(true);
                target_ = startAt_;
                startAt_ = -1;
            }
            else if (g_skipIntro) {
                // On to round 1's call (findRound_'s run, as RB's), its banner
                // and "FIGHT!" then played as they were.
                Log("replay: on to round 1's call, past the intro");
                findRound_ = true;
                MuteGameSounds(true);
            }
            // Still muted from before a restart (MuteGameSounds(true) then does
            // nothing): the capture, ended with the old battle (Finish:
            // StopBattleSounds), begins again on this one's sounds.
            if (GameSoundsMuted()) BeginSoundCapture();
        }
        std::vector<std::pair<ReplayControl, int>> commands;
        {
            std::lock_guard lock(g_viewMutex);
            commands.swap(g_viewCommands);
            g_viewTaken += static_cast<uint32_t>(commands.size());
        }
        handledNow_ += static_cast<int>(commands.size()); // published with their effect (Publish, Finish)
        for (const auto& [c, arg] : commands) {
            if (c == ReplayControl::Leave) {
                Log("replay: left the replay at frame %d", frame_ - 1);
                Finish(game);
                return false;
            }
            if (c == ReplayControl::Takeover || c == ReplayControl::TakeoverStop) {
                TakeoverControl(game, c, arg);
                continue;
            }
            if (tk_.phase != TakeoverPhase::None) continue; // the replay's own controls wait
            Control(game, c, arg);
            if (restarting_) break;
        }
        if (tk_.phase != TakeoverPhase::None) {
            TakeoverFrame(game);
            if (restarting_) {
                Finish(game);
                return false;
            }
            Publish();
            PumpGameThread();
            return true;
        }
        if (restarting_) {
            Finish(game);
            return false;
        }
        // Research (`rview callpace <k>`, `rview callafter <n>`): through a
        // round's call and the first n frames of its fight, at most k ticks a
        // rendered frame (the HUD's banners play per rendered frame).
        const int runFrom = frame_;
        const auto koScreenOver = [&] { // (Simulate)
            if (GetTickCount64() < koHoldUntil_) return false;
            const int pace = g_callPace;
            if (pace <= 0 || frame_ - runFrom < pace) return true;
            const bool banner = RoundCallRunning() || (wasFight_ && !roundStarts_.empty() && frame_ - roundStarts_.back() <= g_callAfter);
            return !banner;
        };
        if (target_ >= 0) {
            // Seeking: the frames before the target lean and muted, the last as
            // played; ~40 ms of them a rendered frame, so the screen goes on.
            const ULONGLONG until = GetTickCount64() + 40;
            // (The last warm_ before the target in full, not lean: the camera
            // follows the fight as it played; a takeover's rewind.)
            while (frame_ < target_ - 1 && frame_ <= last_ && GetTickCount64() < until && koScreenOver()) {
                Simulate(game, frame_ < target_ - 1 - warm_);
            }
            if ((frame_ >= target_ - 1 || frame_ > last_) && koScreenOver()) {
                if (!quiet_) MuteGameSounds(false);
                if (frame_ == target_ - 1 && frame_ <= last_) Simulate(game, false);
                Log("replay: at frame %d (seek done)", frame_ - 1);
                target_ = -1;
                warm_ = 0;
            }
        }
        else if (runToFight_ >= 0) {
            // A jump back from between fights (Seek): on to the next fight
            // (waiting out a KO screen on the way), then the load.
            const ULONGLONG until = GetTickCount64() + 40;
            while (!wasFight_ && frame_ <= last_ && GetTickCount64() < until && koScreenOver() && !MatchDecided()) Simulate(game, true);
            if (wasFight_ || frame_ > last_ || MatchDecided()) {
                const int target = std::exchange(runToFight_, -1);
                if (wasFight_) Seek(game, target);
                else RestartAt(game, target); // no fight came: the battle again
                if (restarting_) {
                    Finish(game);
                    return false;
                }
            }
        }
        else if (findRound_) {
            // The next round's call, not reached yet: on until it is.
            const ULONGLONG until = GetTickCount64() + 40;
            while (findRound_ && frame_ <= last_ && GetTickCount64() < until && koScreenOver() && !holdUntil_) {
                const size_t known = roundCalls_.size();
                const size_t soundMark = SoundCaptureMark();
                Simulate(game, true);
                if (roundCalls_.size() > known) {
                    findRound_ = false;
                    // Found as it ran (here already, the usual case: Seek
                    // returns early then, sounds still muted from the run).
                    // The call's own tick's sounds play on; the run's before
                    // it are stopped as the volumes come back.
                    if (roundCalls_.back() + 1 == frame_) {
                        KeepCapturedFrom(soundMark);
                        MuteGameSounds(false);
                    }
                    else Seek(game, roundCalls_.back() + 1);
                    if (restarting_) {
                        Finish(game);
                        return false;
                    }
                }
            }
            if (findRound_ && frame_ > last_) {
                findRound_ = false;
                MuteGameSounds(false);
            }
        }
        else if (!paused_ && frame_ <= last_) {
            // Fast forward only through action frames: the presentation
            // (intro, round call, KO, win poses) plays at normal speed.
            for (int k = 0; k < std::max(1, fast_) && frame_ <= last_ && (k == 0 || wasFight_); k++) Simulate(game, false);
        }
        if (frame_ > last_ && target_ < 0 && !findRound_ && runToFight_ < 0) paused_ = true; // the end: it stays there
        Publish();
        // The game-thread queue does not run in a driven fight (its pumps are
        // the scene tick and the pad poll): tasks asked for meanwhile run here.
        PumpGameThread();
        return true;
    }

    // ---- Replay Takeover ----------------------------------------------------
    // From a frame in a round's fight: its state kept (as a keyframe is), a
    // 3 s countdown, then ticks with one side's input from the local pad
    // (MappedLocalInput: pad 0 and the keyboard, with this player's Button
    // Settings) and the other side's as recorded, until the fight ends (KO
    // or time); the KO screen gets kKoScreenMs, then the kept state is
    // loaded (a fight key in the same round, from the KO's own frame: loads
    // that came out right in the step tests) and the replay waits there,
    // paused. Nothing of the takeover is checked against the recording or
    // kept as keyframes; its sounds are stopped at the rewind.
    // After the fight: the KO screen played (KoScreen), the picture faded to
    // black (FadeOut), on to the next fight behind it if needed (ToFight),
    // then the start point loaded and the picture back (the stripe's fade).
    enum class TakeoverPhase { None, Countdown, Live, KoScreen, FadeOut, ToFight };
    static constexpr ULONGLONG kTakeoverCountdownMs = 3000;
    static constexpr ULONGLONG kTakeoverKoScreenMs = 2500; // the KO's slow motion and screen, as the game plays them
    static constexpr ULONGLONG kTakeoverFadeMs = 350;      // the stripe's fade to black (kFadeOut) and a little
    static constexpr int kTakeoverRunMax = 60 * 60;        // ticks behind the black looking for a fight
    static constexpr int kTakeoverWarm = 60;               // frames run in full before the start point (the camera)
    // The KO screen's ticks: within the per-tick copies a rollback restores
    // from (flow.cpp kPhaseRing, 64): past them a load back diverged (997
    // frames after a run on to the next round, 2026-11-09).
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

    void TakeoverControl(SfvGame& game, ReplayControl c, int arg)
    {
        if (c == ReplayControl::TakeoverStop) {
            if (tk_.phase == TakeoverPhase::Countdown) {
                Log("replay: takeover called off in its countdown");
                tk_ = Takeover{};
            }
            else if (tk_.phase == TakeoverPhase::Live || tk_.phase == TakeoverPhase::KoScreen) {
                Log("replay: takeover stopped at frame %d", frame_ - 1);
                TakeoverFadeOut(); // the fade, then back as after a KO
            }
            (void)game;
            return;
        }
        if (tk_.phase != TakeoverPhase::None) return;
        // Only in a round's fight, the replay at rest there (a fight key loads
        // back from its KO; a start in the presentation would not).
        if (!wasFight_ || target_ >= 0 || findRound_ || runToFight_ >= 0 || frame_ > last_) {
            Log("replay: no takeover here (frame %d, %s)", frame_ - 1, wasFight_ ? "the replay is moving" : "not in a fight");
            return;
        }
        EndQuiet();
        Takeover t;
        t.phase = TakeoverPhase::Countdown;
        t.side = arg & 1;
        t.until = GetTickCount64() + kTakeoverCountdownMs;
        t.start.frame = frame_;
        t.start.fight = true;
        t.start.regions.resize(StateSize(game.Regions()));
        SaveRegions(game.Regions(), t.start.regions.data());
        SaveOutsideState(&t.start.outside, keys_.empty() ? nullptr : &keys_.back().outside);
        tk_ = std::move(t);
        paused_ = true;
        Log("replay: takeover of P%d from frame %d", tk_.side + 1, frame_ - 1);
    }

    void TakeoverFrame(SfvGame& game)
    {
        const ULONGLONG now = GetTickCount64();
        switch (tk_.phase) {
        case TakeoverPhase::Countdown:
            if (now >= tk_.until) {
                tk_.phase = TakeoverPhase::Live;
                BeginSoundCapture(); // the takeover's sounds, stopped at the rewind
                Log("replay: takeover of P%d playing", tk_.side + 1);
            }
            break;
        case TakeoverPhase::Live: {
            FrameInputs in = frame_ <= last_ ? rf_.inputs[frame_] : FrameInputs{};
            in.p[tk_.side] = MappedLocalInput(0) & ~static_cast<PackedInput>(kStart);
            if (in.p[tk_.side] != tk_.lastInput && tk_.inputLogs++ < 12) {
                Log("replay: takeover input %08X at frame %d", static_cast<unsigned>(in.p[tk_.side]), frame_);
            }
            tk_.lastInput = in.p[tk_.side];
            g_tickFrame = frame_;
            g_tickFight = wasFight_;
            game.Advance(in, false);
            frame_++;
            wasFight_ = RoundInFight();
            if (!wasFight_) {
                // The KO's state kept, as netplay saves before a tick: back to
                // it after the KO screen as a rollback goes (the round's end
                // undone from the per-tick copies), then the start from there.
                tk_.koFrame = frame_;
                tk_.ko.resize(StateSize(game.Regions()));
                SaveRegions(game.Regions(), tk_.ko.data());
                tk_.phase = TakeoverPhase::KoScreen;
                tk_.until = now + kTakeoverKoScreenMs;
                tk_.ticks = 0;
                Log("replay: takeover's fight over at frame %d", frame_ - 1);
            }
            break;
        }
        case TakeoverPhase::KoScreen:
            // The round's end as the game plays it (the KO's slow motion, its
            // screen, the voice), with no one's input, then the fade: as long
            // as a rollback can still undo it (kTakeoverKoTicks).
            if (now >= tk_.until || tk_.ticks >= kTakeoverKoTicks) TakeoverFadeOut();
            else TakeoverTick(game);
            break;
        case TakeoverPhase::FadeOut:
            if (now >= tk_.until) TakeoverRewind(game);
            break;
        case TakeoverPhase::ToFight: {
            // Behind the black, on to the next fight (no keyframe loads the
            // same from between fights), then the load; with no fight to
            // come (the takeover decided the match), the battle again.
            const ULONGLONG until = now + 40;
            while (!wasFight_ && !MatchDecided() && tk_.ticks < kTakeoverRunMax && GetTickCount64() < until) TakeoverTick(game);
            if (wasFight_) TakeoverLoad(game);
            else if (MatchDecided() || tk_.ticks >= kTakeoverRunMax) {
                const int start = tk_.start.frame;
                Log("replay: no fight after the takeover's round (%s): the battle again to frame %d", MatchDecided() ? "the match decided" : "none came", start - 1);
                tk_ = Takeover{};
                paused_ = true;
                RestartAt(game, start);
            }
            break;
        }
        default: break;
        }
    }

    // A tick past the takeover's fight: nobody's input (a recorded Start
    // there would skip the game's screens), not checked, not kept.
    void TakeoverTick(SfvGame& game)
    {
        g_tickFrame = frame_;
        g_tickFight = wasFight_;
        game.Advance(FrameInputs{}, false);
        frame_++;
        tk_.ticks++;
        wasFight_ = RoundInFight();
    }

    void TakeoverFadeOut()
    {
        tk_.phase = TakeoverPhase::FadeOut;
        tk_.until = GetTickCount64() + kTakeoverFadeMs;
    }

    // Behind the black: the takeover's sounds stopped, the game's muted for
    // what runs now; then the start point (TakeoverLoad), from a fight.
    void TakeoverRewind(SfvGame& game)
    {
        const int stopped = StopCapturedSounds();
        MuteGameSounds(true);
        Log("replay: rewinding the takeover from frame %d (%d of its sounds stopped)", frame_ - 1, stopped);
        if (!wasFight_ && !tk_.ko.empty() && tk_.ticks <= kTakeoverKoTicks) {
            // Back to the KO's frame as a rollback loads (OnStateLoaded: the
            // director, the phase machine from its per-tick copies, the
            // round's end undone from its journal), within their reach.
            LoadRegions(game.Regions(), tk_.ko.data());
            game.OnStateLoaded(tk_.koFrame - 1);
            frame_ = tk_.koFrame;
            wasFight_ = RoundInFight();
            Log("replay: back at the takeover's KO frame %d (%d ticks after it undone)", frame_ - 1, tk_.ticks);
        }
        tk_.ticks = 0;
        if (wasFight_ || frame_ == tk_.koFrame) TakeoverLoad(game);
        else tk_.phase = TakeoverPhase::ToFight;
    }

    // Back to where the takeover began as a step back gets there: the round's
    // last keyframe before it loaded and the recording run on to it, the last
    // kTakeoverWarm frames in full (the camera settles on the start point as
    // it played there), the last as played. The load is from a fight, also
    // across the takeover's round end. Without such a keyframe: the kept
    // state itself.
    void TakeoverLoad(SfvGame& game)
    {
        const int start = tk_.start.frame;
        crossFight_ = true;
        const int k = LoadableKey(start);
        if (k >= 0) {
            tk_ = Takeover{};
            paused_ = true;
            warm_ = kTakeoverWarm;
            Seek(game, start); // muted on the way; the last frame as played
        }
        else {
            const Keyframe& kept = tk_.start;
            LoadRegions(game.Regions(), kept.regions.data());
            RestoreOutsideState(kept.outside);
            game.OnStateLoaded(kept.frame - 1);
            frame_ = kept.frame;
            wasFight_ = RoundInFight();
            paused_ = true;
            tk_ = Takeover{};
            MuteGameSounds(false);
        }
        crossFight_ = false;
        Log("replay: back to frame %d after the takeover (%s, from frame %d)", start - 1, k >= 0 ? "from a keyframe" : "the kept state",
            frame_ - 1);
    }

    // One frame of the recording, a keyframe first where one is due.
    void Simulate(SfvGame& game, bool lean)
    {
        // (roundStarts_ may hold later rounds already: a restart keeps them.)
        const bool due = frame_ % kKeyEvery == 0 || std::binary_search(roundStarts_.begin(), roundStarts_.end(), frame_ - 1);
        if (due && (keys_.empty() || keys_.back().frame < frame_)) {
            Keyframe k;
            k.frame = frame_;
            k.fight = wasFight_;
            k.regions.resize(StateSize(game.Regions()));
            SaveRegions(game.Regions(), k.regions.data());
            if (!SaveOutsideState(&k.outside, keys_.empty() ? nullptr : &keys_.back().outside) && keyWarnings_++ < 3) {
                Log("replay: keyframe at frame %d without the phase machine", frame_);
            }
            keys_.push_back(std::move(k));
        }
        // Outside a fight, the state before this tick, in case it turns out
        // to be a round's call (a key of its own: round jumps load it).
        const bool firstPass = keys_.empty() || keys_.back().frame <= frame_;
        if (!wasFight_ && firstPass) {
            scratch_.regions.resize(StateSize(game.Regions()));
            SaveRegions(game.Regions(), scratch_.regions.data());
            SaveOutsideState(&scratch_.outside, keys_.empty() ? nullptr : &keys_.back().outside);
            scratch_.frame = frame_;
        }
        // Lean only in a round's fight, as rollbacks run: the intro, the round
        // call and the KO count presentation work a lean tick leaves out (a
        // seek into the intro ran its round call a frame late without it).
        g_tickFrame = frame_;
        g_tickFight = wasFight_;
        game.Advance(rf_.inputs[frame_], lean && wasFight_);
        if (postLoadLogs_ > 0) {
            postLoadLogs_--;
            Log("replay: frame %d after the load: %s, battle frame stop flags %08X %08X", frame_, RoundEndState().c_str(),
                game.BattleFrameObject() ? *reinterpret_cast<const uint32_t*>(game.BattleFrameObject() + 0x190) : 0u,
                game.BattleFrameObject() ? *reinterpret_cast<const uint32_t*>(game.BattleFrameObject() + 0x194) : 0u);
        }
        CheckFrame(game, frame_);
        const bool fight = RoundInFight();
        if (fight && !wasFight_ && (roundStarts_.empty() || roundStarts_.back() < frame_)) {
            roundStarts_.push_back(frame_);
        }
        const bool call = RoundCallRunning();
        if (call && !wasCall_ && (roundCalls_.empty() || roundCalls_.back() < frame_)) roundCalls_.push_back(frame_);
        if (call && !wasCall_ && scratch_.frame == frame_) {
            // The state before the call's tick: a keyframe (a round jump loads
            // it and plays the call from its start).
            if (!keys_.empty() && keys_.back().frame == frame_) keys_.back().call = true;
            else if (keys_.empty() || keys_.back().frame < frame_) {
                scratch_.call = true;
                scratch_.fight = false;
                keys_.push_back(std::move(scratch_));
            }
            Log("replay: round call keyframe at frame %d", frame_);
            scratch_ = Keyframe{};
            scratch_.frame = -1;
        }
        wasCall_ = call;
        // A round's end: the fight over (KO or time). Keyframes from before
        // one are not loaded past it (Seek).
        if (!fight && wasFight_ && (roundEnds_.empty() || roundEnds_.back() < frame_)) roundEnds_.push_back(frame_);
        // A run (a jump behind the black) that passes a round's end waits
        // there until the KO screen, which plays in real time, is over:
        // else it was still on screen over the next round's call.
        if (!fight && wasFight_) {
            koMs_ = GetTickCount64();
            if (lean) Log("replay: a run passed the KO at frame %d", frame_);
        }
        if (const uint32_t ends = RoundEndEvents(); ends != roundEndsSeen_) {
            roundEndsSeen_ = ends;
            if (lean && koMs_) koHoldUntil_ = koMs_ + kKoScreenMs;
        }
        wasFight_ = fight;
        if (viewer_) MaybeHold(frame_); // tests' screenshots (`replay view <file> hold`)
        frame_++;
    }

    // Shows the state after frame target - 1 (target 0: before frame 0).
    // `quiet` (a step back): the target frame plays muted too, and the game's
    // sounds stay muted until the replay plays or moves on (EndQuiet).
    void Seek(SfvGame& game, int target, bool quiet = false)
    {
        target = std::clamp(target, 1, last_ + 1);
        if (target == frame_ || keys_.empty()) return;
        MuteGameSounds(true);
        quiet_ = quiet;
        if (target < frame_) {
            // The keyframe at or before the frame before the target, where
            // one loads (LoadableKey); other jumps start the battle again and
            // run to the target instead.
            const int k = LoadableKey(target);
            if (k < 0 && !wasFight_ && CallKeyFor(target) >= 0 && FightAhead()) {
                // A round call's key, but between fights now (it would not
                // load the same): on to the next fight first, muted, then it
                // loads (the music plays on; a restart starts it over).
                Log("replay: on to the next fight before loading frame %d's round call", target - 1);
                runToFight_ = target;
                return;
            }
            if (k < 0) {
                RestartAt(game, target);
                return;
            }
            const Keyframe& key = keys_[k];
            LoadRegions(game.Regions(), key.regions.data());
            RestoreOutsideState(key.outside);
            game.OnStateLoaded(key.frame - 1);
            frame_ = key.frame;
            wasFight_ = RoundInFight();
            seeks_++;
            logged_ = 0; // each load logs its own first differing frames
            Log("replay: loaded the keyframe of frame %d (%s, %s)", key.frame, key.fight ? "fight" : key.call ? "round call" : "between fights",
                RoundEndState().c_str());
            postLoadLogs_ = 4;
        }
        target_ = target;
        if (target_ == frame_) {
            if (!quiet_) MuteGameSounds(false);
            target_ = -1;
        }
    }
    void EndQuiet()
    {
        if (!quiet_) return;
        quiet_ = false;
        MuteGameSounds(false);
    }

    // The battle again from the recording's start, then on to `target`
    // (muted), paused or not as now: a new driver, the same view.
    void RestartAt(SfvGame& game, int target)
    {
        Log("replay: restarting the battle to reach frame %d (from %d)", target - 1, frame_ - 1);
        startAt_ = target;
        restarting_ = true; // ViewerFrame finishes this driver once its frame's work is done
        (void)game;
    }

    void Control(SfvGame& game, ReplayControl c, int arg)
    {
        const int shown = frame_ - 1; // the frame on screen
        switch (c) {
        case ReplayControl::Pause: paused_ = true; break;
        case ReplayControl::Play:
            paused_ = false;
            EndQuiet();
            if (frame_ > last_) Seek(game, StartFrame()); // from the end: again from the start
            break;
        case ReplayControl::Toggle: Control(game, paused_ ? ReplayControl::Play : ReplayControl::Pause, 0); break;
        case ReplayControl::Step:
            paused_ = true;
            if (arg < 0) {
                // Back: silent, and only as far as a keyframe of this round
                // loads (a restart is for round jumps, not steps).
                if (target_ >= 0 || findRound_ || runToFight_ >= 0) break;
                const int to = std::max(frame_ + arg, BackLimit() + 1);
                if (to < frame_) Seek(game, to, true);
                break;
            }
            // Forward: only through a round's action frames, never past its
            // KO (the KO's own frame is the last): the round's end, its win
            // pose and the next round's call play as they did, or a round
            // jump skips them.
            if (target_ >= 0 || findRound_ || runToFight_ >= 0 || !CanStepForward()) break;
            {
                int to = frame_ + std::max(1, arg);
                for (const ActionRange& r : ActionRanges()) {
                    if (r.first <= shown + 1 && r.last >= shown + 1) to = std::min(to, r.last + 1);
                }
                EndQuiet();
                Seek(game, to);
            }
            break;
        case ReplayControl::Seek: EndQuiet(); Seek(game, arg); break;
        case ReplayControl::Round:
            EndQuiet();
            Log("replay: round jump %+d from frame %d", arg, shown);
            // A round starts at its call ("ROUND 2"), which then plays as it
            // did: a seek run through the call in a frame or two left its
            // banner (then "FIGHT!") over the fight (UI checks, 2026-11-09).
            // Next: the first round call after the frame on screen, none in the
            // last round (it ran to the recording's end, which stops inside the
            // final KO); previous: PreviousRoundTarget.
            if (arg > 0 && !CanRoundForward()) Log("replay: no round after this one");
            else if (arg > 0) {
                auto next = std::upper_bound(roundCalls_.begin(), roundCalls_.end(), shown);
                if (next != roundCalls_.end()) Seek(game, *next + 1);
                else if (frame_ <= last_) {
                    findRound_ = true;
                    MuteGameSounds(true);
                }
            }
            else Seek(game, PreviousRoundTarget());
            break;
        case ReplayControl::Fast: fast_ = std::clamp(arg, 0, 4); break;
        case ReplayControl::OutOfSync:
            if (firstMismatch_ < 0) firstMismatch_ = std::max(0, frame_ - 1);
            mismatchFrames_++;
            break;
        default: break;
        }
    }

    void Publish()
    {
        std::lock_guard lock(g_viewMutex);
        g_view.active = true;
        g_view.title = title_;
        g_view.paused = paused_ && target_ < 0 && !findRound_ && runToFight_ < 0;
        g_view.seeking = target_ >= 0 || findRound_ || runToFight_ >= 0 || tk_.phase == TakeoverPhase::FadeOut || tk_.phase == TakeoverPhase::ToFight;
        g_view.opening = InOpeningRun();
        g_view.handled += static_cast<uint32_t>(handledNow_);
        handledNow_ = 0;
        g_view.takeover = tk_.phase == TakeoverPhase::None ? -1 : tk_.side;
        g_view.takeoverCountdown =
            tk_.phase == TakeoverPhase::Countdown ? std::max(0.0f, static_cast<float>(tk_.until - std::min(tk_.until, GetTickCount64())) / 1000.0f) : 0.0f;
        g_view.takeoverEnding = tk_.phase == TakeoverPhase::KoScreen || tk_.phase == TakeoverPhase::FadeOut || tk_.phase == TakeoverPhase::ToFight;
        g_view.takeoverFading = tk_.phase == TakeoverPhase::FadeOut || tk_.phase == TakeoverPhase::ToFight;
        g_view.frame = std::max(0, frame_ - 1);
        g_view.frames = last_ + 1;
        g_view.fast = fast_;
        g_view.backLimit = BackLimit();
        g_view.canStepForward = CanStepForward() && target_ < 0 && !findRound_ && runToFight_ < 0;
        g_view.canRoundForward = CanRoundForward();
        g_view.round = static_cast<int>(std::upper_bound(roundStarts_.begin(), roundStarts_.end(), frame_ - 1) - roundStarts_.begin());
        g_view.mismatches = mismatchFrames_;
        g_view.outOfSync = firstMismatch_ < 0 ? -1
                                              : static_cast<int>(std::upper_bound(roundStarts_.begin(), roundStarts_.end(), firstMismatch_) - roundStarts_.begin());
        g_view.seeks = seeks_;
        // The per-round counter and the stripe: 0 on a round's first action
        // frame, its last on the KO's; empty before a round's action (and
        // from the next round's call on), full after its KO. A round whose
        // end is not known yet (a file without the ranges) runs against the
        // round timer's length.
        const int shown = frame_ - 1;
        g_view.roundFrame = -1;
        g_view.roundFrames = 0;
        g_view.roundProgress = 0;
        for (const ActionRange& r : ActionRanges()) {
            if (shown < r.first) break;
            const int frames = r.last >= r.first ? r.last - r.first + 1 : 0;
            if (r.last < 0 || shown <= r.last) {
                g_view.roundFrame = shown - r.first;
                g_view.roundFrames = frames;
                const int span = frames > 1 ? frames - 1 : std::max(1, int(rf_.setup.timerSeconds) * 60);
                g_view.roundProgress = std::clamp(static_cast<float>(g_view.roundFrame) / static_cast<float>(span), 0.0f, 1.0f);
            }
            else {
                const bool nextCall = std::any_of(roundCalls_.begin(), roundCalls_.end(), [&](int c) { return c > r.last && c <= shown; });
                g_view.roundProgress = nextCall ? 0.0f : 1.0f;
            }
        }
    }

    // The replay's start for Play at the end and LB in round 1: round 1's call
    // (g_skipIntro), as a round jump lands on it, or the recording's first frame.
    int StartFrame() const { return g_skipIntro && !roundCalls_.empty() ? roundCalls_.front() + 1 : 0; }
    // Running to round 1's call from before it (the intro behind black).
    bool InOpeningRun() const
    {
        if (!g_skipIntro || (target_ < 0 && !findRound_)) return false;
        return roundCalls_.empty() ? findRound_ : frame_ <= roundCalls_.front() && (findRound_ || target_ == roundCalls_.front() + 1);
    }

    // RB has a round to go to: the round on screen (counted by calls, 0 in the
    // intro) is not the file's last. A file without its rounds: until the end.
    bool CanRoundForward() const
    {
        const int shown = frame_ - 1;
        const auto next = std::upper_bound(roundCalls_.begin(), roundCalls_.end(), shown);
        if (next != roundCalls_.end()) return true; // seen already
        const int round = static_cast<int>(next - roundCalls_.begin());
        const int rounds = static_cast<int>(std::count_if(rf_.summary.rounds.begin(), rf_.summary.rounds.end(), [](const RoundResult& r) { return r.fightFrame >= 0; }));
        if (rounds > 0) return round < rounds;
        return frame_ <= last_ && !MatchDecided();
    }

    // LB: the previous round's call while this round's has just begun (its
    // call, or the first second of its fight), else this round's call; before
    // round 2 the replay's start.
    int PreviousRoundTarget() const
    {
        const int shown = frame_ - 1;
        auto it = std::upper_bound(roundCalls_.begin(), roundCalls_.end(), shown); // past this round's call
        if (it == roundCalls_.begin()) return StartFrame();                       // before any call
        const int call = *std::prev(it);
        auto fight = std::upper_bound(roundStarts_.begin(), roundStarts_.end(), call - 1);
        const bool justBegun = fight == roundStarts_.end() || *fight >= shown || shown - *fight <= kKeyEvery;
        if (!justBegun) return call + 1;
        if (std::prev(it) == roundCalls_.begin()) return StartFrame();
        return *std::prev(it, 2) + 1;
    }

    // A frame step forward runs an action frame: the state on screen has a
    // round's fight on (from "FIGHT!" to the frame before its KO).
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
    Keyframe scratch_{-1};       // the state before the last tick outside a fight (a call key in waiting)
    bool wasFight_ = false;
    bool paused_ = false;
    bool findRound_ = false;
    bool quiet_ = false; // a step back's mute, kept while paused there (Seek)
    int runToFight_ = -1; // a jump back waiting for a fight to load from (Seek)
    Takeover tk_;         // Replay Takeover (TakeoverControl)
    bool crossFight_ = false; // LoadableKey: a takeover's rewind (fight keys across its round end)
    int warm_ = 0;            // a seek's last frames run in full (Seek's run, TakeoverLoad)
    int handledNow_ = 0; // controls taken since the last Publish (ReplayView::handled)
    // A jump's run waits for the KO screen (Simulate): it plays in real time
    // from the KO, about this long (UI checks, 2026-11-09).
    static constexpr ULONGLONG kKoScreenMs = 1500;
    ULONGLONG koMs_ = 0;          // when the last KO was simulated
    ULONGLONG koHoldUntil_ = 0;   // a run waits until then
    uint32_t roundEndsSeen_ = 0;  // RoundEndEvents as of the last tick
    int target_ = -1; // seeking: the frame to simulate next once there
    int fast_ = 0;    // frames a rendered frame while playing (0, 1: as played)
    int keyWarnings_ = 0;
    int seeks_ = 0;   // loads of a keyframe (testing)
    std::vector<int> roundEnds_; // frames whose tick ended a round's fight
    std::string path_;           // the file, for a restart
    int startAt_ = -1;           // a restart's target (RestartAt)
    bool restarting_ = false;

    // Neutral ticks until the battle's objects exist, then frozen at frame 0
    // as a netplay session is (SfvGame::WarmupStep).
    bool Warmup(SfvGame& game)
    {
        if (game.WarmupStep()) {
            if (game.WarmupTicks() % 300 == 0) Log("replay: still no battle objects after %d ticks", game.WarmupTicks());
            return true;
        }
        ready_ = true;
        SetPadIsolation(true);
        game.exclusiveTicks = true;
        // Recordings from before adapter-9 were made with the game's own
        // pad latch (once per rendered frame): an intro skip lands a tick
        // later with it. A straight run keeps it to follow them; runs that
        // roll back need the mod's (a replayed tick saw a newer frame's).
        game.latchEachTick = stress_ > 0 || delay_ > 0 || BuildAdapterVersion(rf_.gameBuild) >= 9;
        if (const int latch = g_latch.exchange(-1); latch >= 0) game.latchEachTick = latch != 0;
        if (!game.latchEachTick) Log("replay: %s predates adapter-9: the game's own pad latch", rf_.gameBuild.c_str());
        SetRoundDirectorPerTick(true, true);
        SetMatchDrawSeed(rf_.setup.seed);
        game.Recanonicalize();
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(CheckRegions(game), hashes.data());
        // The neutral ticks before the objects exist depend on loading (16 in
        // a session, 9 here), and so does the frozen state: take the
        // recording's, as a netplay joiner takes the host's.
        if (Compare(game, hashes.data(), rf_.initialHashes.data(), -2)) {
            TakeRecordedStart(game, hashes);
            game.Recanonicalize();
            hashes.assign(game.Regions().size(), 0);
            HashRegions(CheckRegions(game), hashes.data());
        }
        layout_ = game.LayoutVersion(); // after the copy and its new canonical order
        game.ArmStartProbe();
        if (stress_ == 0 && keepFrom_ >= 0) {
            // `keep`: this run as a full recording, its start state first.
            kept_ = ReplayFile{};
            kept_.modVersion = rf_.modVersion;
            kept_.gameBuild = game.BuildId();
            kept_.matchId = rf_.matchId;
            kept_.localPlayer = rf_.localPlayer;
            kept_.setup = rf_.setup;
            kept_.regions = DescribeRegions(game.Regions());
            kept_.initialHashes.assign(game.Regions().size(), 0);
            HashRegions(game.Regions(), kept_.initialHashes.data()); // this build's masks, as its region table
            std::vector<uint8_t> state(StateSize(game.Regions()));
            SaveRegions(game.Regions(), state.data());
            kept_.dumps.push_back({-1, "initial", std::move(state)});
        }
        const int differ = Compare(game, hashes.data(), rf_.initialHashes.data(), -1);
        Log("replay: battle objects ready after %d neutral ticks (%s); state before frame 0 %s", game.ObjectsAt(),
            RoundEndState().c_str(),
            differ ? ("differs in " + std::to_string(differ) + " regions").c_str() : "equal");
        if (stress_ > 0) {
            NetplayConfig nc;
            nc.stressCheckDistance = static_cast<unsigned char>(stress_);
            nc.predictionWindow = static_cast<unsigned char>(stress_ + 1);
            nc.historyDepth = static_cast<unsigned int>(stress_ + 2);
            nc.inputDelay = 0;
            nc.modVersion = "replay";
            nc.log = [](const std::string& m) { Log("replay: %s", m.c_str()); };
            // Both players' inputs of the next frame from the recording.
            game.inputOverride = [this](int player) {
                const int f = std::min(fed_, rf_.FrameCount() - 1);
                const PackedInput in = rf_.inputs[f].p[player & 1];
                if (player == 1) fed_++;
                return in;
            };
            session_ = std::make_unique<NetplaySession>(game, transport_, nc);
            // Resimulated words, by region; the presentation clocks apart.
            session_->onStressDiff = [this, &game](int frame, size_t region, size_t offset) {
                if (region >= game.Regions().size()) return;
                const std::string& name = game.Regions()[region].name;
                if (name == "viewtarget.clock" || name == "pawn.clock") return;
                if (stressFrames_.insert(frame).second) {
                    stressRegionFrames_[name]++;
                    if (stressLogged_++ < 20) {
                        Log("replay: frame %d resimulated differently in %s (+0x%zx)", frame, name.c_str(), offset);
                    }
                }
            };
            std::string error;
            if (!session_->Start(&error)) {
                Log("replay: cannot start the stress session: %s", error.c_str());
                Finish(game);
                return false;
            }
        }
        return true;
    }

    // Copies the recording's state before frame 0 (its dump) into the regions
    // that differ, as ApplyHostState in netplay_driver.cpp does: each recorded
    // region without an equal here pairs with the most similar region of the
    // same size; masked words and pointer-like values stay.
    void TakeRecordedStart(SfvGame& game, const std::vector<uint64_t>& hashes)
    {
        const StateDump* dump = rf_.FindDump(-1);
        if (!dump) {
            Log("replay: the recording has no state before frame 0 to start from");
            return;
        }
        const auto& regions = game.Regions();
        std::multiset<uint64_t> mine(hashes.begin(), hashes.end());
        std::multiset<uint64_t> recorded(rf_.initialHashes.begin(), rf_.initialHashes.end());
        std::vector<bool> taken(regions.size(), false);
        for (size_t i = 0; i < regions.size(); i++) {
            auto it = recorded.find(hashes[i]);
            if (it == recorded.end()) continue;
            recorded.erase(it);
            taken[i] = true;
        }
        size_t at = 0, changed = 0, words = 0, pointers = 0;
        for (size_t j = 0; j < rf_.regions.size(); j++) {
            const size_t size = static_cast<size_t>(rf_.regions[j].size);
            const uint8_t* bytes = dump->bytes.data() + at;
            at += size;
            if (at > dump->bytes.size()) break;
            auto it = mine.find(rf_.initialHashes[j]);
            if (it != mine.end()) {
                mine.erase(it);
                continue;
            }
            size_t best = SIZE_MAX, bestDiff = SIZE_MAX;
            for (size_t i = 0; i < regions.size(); i++) {
                if (taken[i] || regions[i].size != size) continue;
                size_t diff = 0;
                for (size_t w = 0; w + 8 <= size; w += 8) diff += std::memcmp(regions[i].ptr + w, bytes + w, 8) != 0;
                if (diff < bestDiff) {
                    best = i;
                    bestDiff = diff;
                }
            }
            if (best == SIZE_MAX) continue;
            taken[best] = true;
            const StateRegion& r = regions[best];
            size_t copied = 0;
            for (size_t w = 0; w + 8 <= size; w += 8) {
                const size_t word = w / 8;
                if (word / 8 < r.ignore.size() && (r.ignore[word / 8] >> (word % 8) & 1)) continue;
                uint64_t here, there;
                std::memcpy(&here, r.ptr + w, 8);
                std::memcpy(&there, bytes + w, 8);
                if (here == there) continue;
                const auto pointerLike = [](uint64_t v) { return v >= 0x10000000000ull && v < 0x800000000000ull; };
                if (pointerLike(here) || pointerLike(there)) {
                    if (pointers++ < 8) {
                        Log("replay:   %s +0x%zx kept: %016llx here, %016llx recorded", r.name.c_str(), w,
                            static_cast<unsigned long long>(here), static_cast<unsigned long long>(there));
                    }
                    continue;
                }
                std::memcpy(r.ptr + w, &there, 8);
                copied++;
            }
            if (copied) {
                changed++;
                words += copied;
                Log("replay:   %s (%zu bytes): %zu words from the recording", r.name.c_str(), r.size, copied);
            }
        }
        Log("replay: took the recording's start: %zu words in %zu regions (%zu pointer-like words kept)", words, changed,
            pointers);
    }

    bool StraightFrame(SfvGame& game)
    {
        if (!Valid(game)) return false;
        if (frame_ > last_) {
            Finish(game);
            return false;
        }
        if (frame_ == 0) Log("replay: frame 0 is driven tick %u", DrivenTicks() + 1);
        game.Advance(rf_.inputs[frame_], false);
        CheckFrame(game, frame_);
        WatchFrame(game, frame_);
        MaybeHold(frame_);
        if (keepFrom_ >= 0) {
            std::vector<uint64_t> hashes(game.Regions().size());
            HashRegions(game.Regions(), hashes.data());
            kept_.SetFrame(frame_, rf_.inputs[frame_], hashes.data());
            if (frame_ >= keepFrom_ && frame_ <= keepTo_) {
                std::vector<uint8_t> state(StateSize(game.Regions()));
                SaveRegions(game.Regions(), state.data());
                kept_.dumps.push_back({frame_, "kept", std::move(state)});
            }
        }
        frame_++;
        return true;
    }

    bool StressFrame(SfvGame& game)
    {
        if (!Valid(game)) return false;
        if (bench_) {
            // B stops it; the fight (FIGHT to the end) is measured.
            bool stop = false;
            {
                std::lock_guard lock(g_viewMutex);
                for (const auto& [c, arg] : g_viewCommands) stop = stop || c == ReplayControl::Leave;
                g_viewCommands.clear();
            }
            if (stop) {
                benchStopped_ = true;
                Finish(game);
                return false;
            }
            const bool fight = RoundInFight();
            if (fight && !benchCapturing_ && !benchMeasured_) {
                hitch::BeginCapture();
                benchCapturing_ = true;
                Log("replay: benchmark measuring from frame %d", session_->NextFrame());
            }
            if (benchCapturing_ && !fight) {
                benchCaptured_ = hitch::EndCapture();
                benchCapturing_ = false;
                benchMeasured_ = true;
                Log("replay: benchmark measured to frame %d (the fight ended)", session_->NextFrame());
                Finish(game);
                return false;
            }
            std::lock_guard lock(g_viewMutex);
            g_view.active = true;
            g_view.benchmark = true;
            g_view.frame = std::max(0, session_->NextFrame() - 1);
            g_view.frames = last_ + 1;
            g_view.title = "Benchmark";
        }
        if (session_->NextFrame() > last_) {
            Finish(game);
            return false;
        }
        if (session_->NextFrame() == 0) Log("replay: frame 0 is driven tick %u", DrivenTicks() + 1);
        session_->Update();
        // The state now is the end of the newest frame (after its
        // resimulation): it must equal the recording too.
        const int f = session_->NextFrame() - 1;
        WatchFrame(game, f);
        MaybeHold(f);
        if (f >= 0 && f != checked_) {
            CheckFrame(game, f);
            checked_ = f;
        }
        return true;
    }

    // Frame f runs on the remote player's input as GekkoNet predicts it: the
    // last one that arrived (frame f - delay's), neutral before any. That
    // frame's input arrives now; if its guess was wrong, the state before it
    // is loaded (and the game's post-load fix-ups run, as in a session) and
    // the frames since are resimulated with what is known now. A frame is
    // compared with the recording once its input has arrived: live after a
    // rollback, else by the checksum it got when it ran on the right input.
    bool MispredictFrame(SfvGame& game)
    {
        if (!Valid(game)) return false;
        const int f = frame_;
        if (f > last_ + delay_) {
            Finish(game);
            return false;
        }
        if (f == 0) {
            Log("replay: frame 0 is driven tick %u; P%d's inputs arrive %d frames late", DrivenTicks() + 1, remote_ + 1,
                delay_);
            ring_.assign(delay_ + 2, std::vector<uint8_t>(StateSize(game.Regions())));
            used_.assign(last_ + 1, FrameInputs{});
            sums_.assign(last_ + 1, 0);
        }
        const int arrived = f - delay_;
        if (arrived >= 0 && arrived <= last_) {
            if (used_[arrived].p[remote_] != rf_.inputs[arrived].p[remote_]) {
                Resimulate(game, arrived, std::min(f, last_ + 1));
            }
            else {
                ConfirmBySum(arrived);
            }
        }
        if (f <= last_) RunPredicted(game, f, arrived, false);
        WatchFrame(game, f);
        MaybeHold(f);
        frame_++;
        return true;
    }

    PackedInput Guess(int k, int known) const
    {
        if (k <= known) return rf_.inputs[k].p[remote_];
        return known >= 0 ? rf_.inputs[known].p[remote_] : PackedInput{};
    }

    void RunPredicted(SfvGame& game, int k, int known, bool rollingBack)
    {
        SaveRegions(game.Regions(), ring_[k % ring_.size()].data());
        FrameInputs in = rf_.inputs[k];
        in.p[remote_] = Guess(k, known);
        game.Advance(in, rollingBack);
        used_[k] = in;
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(CheckRegions(game), hashes.data());
        sums_[k] = FoldChecksum(hashes.data(), hashes.size());
    }

    void Resimulate(SfvGame& game, int from, int to)
    {
        LoadRegions(game.Regions(), ring_[from % ring_.size()].data());
        game.OnStateLoaded(from - 1);
        rollbacks_++;
        resimulated_ += to - from;
        for (int k = from; k < to; k++) {
            RunPredicted(game, k, from, true);
            if (k == from) CheckFrame(game, k);
        }
    }

    void ConfirmBySum(int k)
    {
        if (k >= rf_.FrameCount() || rf_.hashes.empty()) return;
        framesChecked_++;
        if (sums_[k] == rf_.FrameChecksum(k)) return;
        if (firstMismatch_ < 0) firstMismatch_ = k;
        mismatchFrames_++;
        if (logged_++ < 12) Log("replay: frame %d's checksum differs from the recording (ran on the right input)", k);
    }

    void MaybeHold(int f)
    {
        if (f != holdAt_ || holdMs_ == 0) return;
        holdUntil_ = GetTickCount64() + holdMs_;
        holdMs_ = 0;
        Log("replay: holding at frame %d", f);
    }

    // ArmWatch sets every thread's debug registers but the caller's, and the
    // ticks run on this (the game) thread: arm from another and wait.
    static void ArmFromHelper(int slot, const void* address, int length, uintptr_t base)
    {
        std::thread([=] {
            ThreadGuard running;
            ArmWatch(slot, address, length, base);
        }).join();
    }

    // `watch <from> <to> [region]`: per frame, the region's first 16 bytes
    // (by name) and the round's end state.
    void WatchFrame(SfvGame& game, int f)
    {
        for (const Trap& t : traps_) {
            if (f != t.frame - 1) continue;
            if (t.region == "director") {
                // The round director itself (not a region: flow.round copies
                // parts of it), e.g. its counts at +0x11128.
                uint8_t* d = RoundDirector();
                if (!d) {
                    Log("replay: trap: no round director");
                    continue;
                }
                const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                for (int done = 0; done < t.bytes && trapSlots_ < 4; done += 8) {
                    ArmFromHelper(trapSlots_++, d + t.offset + done, std::min(8, t.bytes - done), base);
                }
                Log("replay: trap armed on the round director +0x%llx (%d bytes) at %p before frame %d",
                    static_cast<unsigned long long>(t.offset), t.bytes, static_cast<void*>(d + t.offset), t.frame);
                continue;
            }
            const auto r = std::find_if(game.Regions().begin(), game.Regions().end(),
                                        [&](const StateRegion& x) { return x.name == t.region; });
            // An offset outside the region (up to 0x1000 before its start or past
            // its end) watches the owner's memory there: a field no region
            // covers, e.g. one read by a per-tick writer.
            const long long size = r == game.Regions().end() ? 0 : static_cast<long long>(r->size);
            const char* sign = t.offset < 0 ? "-" : "+";
            const unsigned long long at = static_cast<unsigned long long>(t.offset < 0 ? -t.offset : t.offset);
            if (r == game.Regions().end() || t.offset < -0x1000 || t.offset >= size + 0x1000) {
                Log("replay: trap: no region %s %s0x%llx", t.region.c_str(), sign, at);
                continue;
            }
            if (t.offset < 0 || t.offset >= size) Log("replay: trap on %s %s0x%llx is outside the region", t.region.c_str(), sign, at);
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            for (int done = 0; done < t.bytes && trapSlots_ < 4; done += 8) {
                ArmFromHelper(trapSlots_++, r->ptr + t.offset + done, std::min(8, t.bytes - done), base);
            }
            Log("replay: trap armed on %s %s0x%llx (%d bytes) at %p before frame %d", t.region.c_str(), sign, at, t.bytes,
                static_cast<void*>(r->ptr + t.offset), t.frame);
        }
        if (f < watchFrom_ || f > watchTo_) return;
        if (watchRegion_ == "trails") {
            LogTrails(game, f);
            return;
        }
        std::string bytes = "-";
        // "<region>+<hex offset>": the 16 bytes from there.
        const size_t plus = watchRegion_.find('+');
        const std::string watchName = watchRegion_.substr(0, plus);
        const size_t at = plus == std::string::npos ? 0 : std::stoull(watchRegion_.substr(plus + 1), nullptr, 16);
        for (const StateRegion& r : game.Regions()) {
            if (r.name != watchName || at >= r.size) continue;
            char b[64];
            uint64_t w[2] = {};
            std::memcpy(w, r.ptr + at, std::min<size_t>(16, r.size - at));
            std::snprintf(b, sizeof(b), "%016llx %016llx", static_cast<unsigned long long>(w[0]),
                          static_cast<unsigned long long>(w[1]));
            bytes = b;
        }
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(game.Regions(), hashes.data());
        Log("replay: watch frame %d: %s %s | %s | state %08x | camera %s", f, watchRegion_.c_str(), bytes.c_str(),
            RoundEndState().c_str(), FoldChecksum(hashes.data(), hashes.size()), CameraText().c_str());
    }

    // `watch <from> <to> trails` (research, finding 33): the motion trail
    // objects (vtable rva 0x2C6FB88, a pool; only +0xC0..+0xEB is in the
    // snapshot, the 44-byte regions) and their KWTrailManager (+0x190), word
    // by word, snapshotted or not: what changed since the last logged frame.
    static constexpr uint32_t kTrailVtable = 0x2C6FB88;
    static constexpr size_t kTrailBytes = 0x200, kManagerBytes = 0xB00;
    std::map<const uint8_t*, std::vector<uint64_t>> trailSeen_;

    void LogTrails(SfvGame& game, int f)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::vector<const uint8_t*> objects;
        for (const StateRegion& r : game.Regions()) {
            if (r.size != 44) continue;
            const uint8_t* obj = r.ptr - 0xC0;
            if (!IsReadable(obj, kTrailBytes) || *reinterpret_cast<const uintptr_t*>(obj) - base != kTrailVtable) continue;
            objects.push_back(obj);
        }
        std::sort(objects.begin(), objects.end());
        const uint8_t* manager = objects.empty() ? nullptr : *reinterpret_cast<const uint8_t* const*>(objects[0] + 0x190);
        auto diff = [&](const char* what, int index, const uint8_t* p, size_t size) {
            if (!p || !IsReadable(p, size)) return;
            std::vector<uint64_t> now(size / 8);
            std::memcpy(now.data(), p, size);
            auto& seen = trailSeen_[p];
            if (seen.size() == now.size()) {
                std::string changes;
                for (size_t w = 0; w < now.size(); w++) {
                    if (now[w] == seen[w]) continue;
                    char b[80];
                    std::snprintf(b, sizeof(b), " +0x%zx %llx>%llx", w * 8, static_cast<unsigned long long>(seen[w]),
                                  static_cast<unsigned long long>(now[w]));
                    changes += b;
                }
                if (!changes.empty()) Log("replay: trails frame %d %s %d%s", f, what, index, changes.c_str());
            }
            seen = std::move(now);
        };
        // Before the last logged frame: write watches on the state (+0xE8) of
        // the first unused objects, to find what hands one out.
        if (f == watchTo_ - 9 && trapSlots_ == 0) {
            auto unused = [](const uint8_t* obj) {
                for (size_t w = 0xC0; w < 0xEC; w += 4) {
                    if (*reinterpret_cast<const uint32_t*>(obj + w) != 0) return false;
                }
                return true;
            };
            size_t after = 0; // past the last object in use
            for (size_t i = 0; i < objects.size(); i++) {
                if (!unused(objects[i])) after = i + 1;
            }
            for (size_t i = after; i < objects.size(); i++) {
                const uint8_t* obj = objects[i];
                if (trapSlots_ >= 4) break;
                ArmFromHelper(trapSlots_++, obj + 0xE8, 4, base);
                Log("replay: trails: watching %p +0xe8 from frame %d", static_cast<const void*>(obj), f + 1);
            }
        }
        for (size_t i = 0; i < objects.size(); i++) diff("trail", static_cast<int>(i), objects[i], kTrailBytes);
        diff("manager", 0, manager, kManagerBytes);
        if (trailSeen_.size() == objects.size() + 1 && f == watchFrom_) {
            Log("replay: trails frame %d: %zu trail objects, manager %p", f, objects.size(), static_cast<const void*>(manager));
        }
    }

    // The battle camera as last drawn: KWBattleCameraManager's CameraCache.POV
    // (+0x368 + 0x8): location, rotation (pitch, yaw, roll), FOV. Outside the
    // snapshot; compared between straight and stress replays by hand.
    std::string CameraText()
    {
        constexpr size_t kPov = 0x370;
        // The one drawn: the PlayerCameraManager (+0x390) of the player
        // controller that has a local player (Player, +0x360); the other
        // managers follow pawns. A walk over every object is slow: one try
        // per 120 frames until found.
        constexpr size_t kPlayer = 0x360, kCameraManager = 0x390;
        if (!camera_ && cameraTries_++ % 120 == 0 && ue4::EnsureObjects(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) {
            ue4::ForEachObject([&](const void* object) {
                if (camera_ || ue4::ObjectName(ue4::ClassOf(object)).find("PlayerController") == std::string::npos) return;
                const auto* p = static_cast<const uint8_t*>(object);
                if (!IsReadable(p + kCameraManager, 8) || !*reinterpret_cast<const void* const*>(p + kPlayer)) return;
                const auto* manager = *reinterpret_cast<const uint8_t* const*>(p + kCameraManager);
                if (manager && ue4::ObjectName(ue4::ClassOf(manager)).find("CameraManager") != std::string::npos) camera_ = manager;
            });
            if (camera_) Log("replay: camera manager %s (%p)", ue4::Describe(camera_).c_str(), static_cast<const void*>(camera_));
        }
        if (!camera_ || !IsReadable(camera_ + kPov, 0x1C)) return "-";
        float v[7];
        std::memcpy(v, camera_ + kPov, sizeof(v));
        char b[128];
        std::snprintf(b, sizeof(b), "%.1f %.1f %.1f r %.2f %.2f %.2f fov %.2f", v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
        return b;
    }

    bool Valid(SfvGame& game)
    {
        game.RefreshRegions();
        if (game.LayoutVersion() == layout_ && !game.Regions().empty()) return true;
        Log("replay: the battle ended or changed under the replay at frame %d", stress_ > 0 ? session_->NextFrame() : frame_);
        Finish(game);
        return false;
    }

    // The regions the recording is checked against: this build's, with the
    // recording's checksum masks where they differ (a later build may leave
    // more or fewer words out of the checksum; what the battle computes is
    // the same). A region takes the mask the recording has for its size
    // when there is one; with several, the one at its place when the two
    // tables have the same sizes in the same order.
    const std::vector<StateRegion>& CheckRegions(SfvGame& game)
    {
        const std::vector<StateRegion>& live = game.Regions();
        if (game.LayoutVersion() == checkLayout_ && live.size() == checkCount_) return checkRegions_.empty() ? live : checkRegions_;
        checkLayout_ = game.LayoutVersion();
        checkCount_ = live.size();
        checkRegions_.clear();
        const auto masksOf = [](const std::string& id) {
            const size_t at = id.find("/map-");
            return at == std::string::npos ? std::string() : id.substr(at);
        };
        if (masksOf(rf_.gameBuild) == masksOf(game.BuildId())) return live;
        std::map<uint64_t, std::vector<const std::vector<uint8_t>*>> bySize;
        for (const RegionInfo& r : rf_.regions) {
            auto& masks = bySize[r.size];
            if (std::none_of(masks.begin(), masks.end(), [&](const std::vector<uint8_t>* m) { return *m == r.ignore; })) masks.push_back(&r.ignore);
        }
        bool sameOrder = rf_.regions.size() == live.size();
        for (size_t i = 0; sameOrder && i < live.size(); i++) sameOrder = rf_.regions[i].size == live[i].size;
        checkRegions_ = live;
        int changed = 0;
        for (size_t i = 0; i < checkRegions_.size(); i++) {
            const auto& masks = bySize[checkRegions_[i].size];
            const std::vector<uint8_t>* mask = masks.size() == 1 ? masks[0] : sameOrder ? &rf_.regions[i].ignore : nullptr;
            if (mask && *mask != checkRegions_[i].ignore) {
                checkRegions_[i].ignore = *mask;
                changed++;
            }
        }
        Log("replay: checked with the recording's checksum masks (%d of %zu regions masked differently on this build)", changed,
            checkRegions_.size());
        if (!changed) checkRegions_.clear();
        return checkRegions_.empty() ? live : checkRegions_;
    }

    void CheckFrame(SfvGame& game, int f)
    {
        if (f >= rf_.FrameCount() || rf_.hashes.empty()) return;
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(CheckRegions(game), hashes.data());
        framesChecked_++;
        // A state dump of the frame (a compact recording's every 600 frames, a
        // kept run's chosen frames) is compared word by word.
        if (const StateDump* dump = rf_.FindDump(f)) CompareDump(game, *dump, f);
        if (rf_.HasRegionHashes(f)) {
            Compare(game, hashes.data(), rf_.FrameHashes(f), f);
            return;
        }
        // A compact recording: the frame's checksum only (a mismatch names no
        // region).
        const bool equal = FoldChecksum(hashes.data(), hashes.size()) == rf_.FrameChecksum(f);
        if (equal) return;
        if (firstMismatch_ < 0) firstMismatch_ = f;
        mismatchFrames_++;
        if (logged_++ < 12) Log("replay: frame %d's checksum differs from the recording (no region hashes kept)", f);
    }

    // The state against the recording's dump of the same frame: each region
    // here pairs with the same-size dump region it differs least from
    // (masked words left out); the differing words are logged.
    void CompareDump(SfvGame& game, const StateDump& dump, int f)
    {
        struct Recorded {
            const uint8_t* bytes;
            size_t size;
            bool taken;
        };
        std::vector<Recorded> recorded;
        size_t at = 0;
        for (const auto& r : rf_.regions) {
            const size_t size = static_cast<size_t>(r.size);
            if (at + size > dump.bytes.size()) break;
            recorded.push_back({dump.bytes.data() + at, size, false});
            at += size;
        }
        int differing = 0, shown = 0;
        // Regions wholly out of the checksum (pooled effects) equal any dump
        // region of their size: paired last, they cannot take another's.
        std::vector<const StateRegion*> order;
        for (const StateRegion& r : game.Regions()) order.push_back(&r);
        const auto wholly = [](const StateRegion* r) {
            return !r->ignore.empty() && std::all_of(r->ignore.begin(), r->ignore.end(), [](uint8_t b) { return b == 0xFF; });
        };
        std::stable_partition(order.begin(), order.end(), [&](const StateRegion* r) { return !wholly(r); });
        for (const StateRegion* rp : order) {
            const StateRegion& r = *rp;
            auto masked = [&r](size_t word) { return word / 8 < r.ignore.size() && (r.ignore[word / 8] >> (word % 8) & 1); };
            auto diff = [&](const Recorded& d) {
                size_t n = 0;
                for (size_t w = 0; w + 8 <= r.size; w += 8) n += !masked(w / 8) && std::memcmp(r.ptr + w, d.bytes + w, 8) != 0;
                return n;
            };
            Recorded* best = nullptr;
            size_t bestDiff = SIZE_MAX;
            for (auto& d : recorded) {
                if (d.taken || d.size != r.size) continue;
                const size_t n = diff(d);
                if (n < bestDiff) {
                    best = &d;
                    bestDiff = n;
                    if (n == 0) break;
                }
            }
            if (!best) continue;
            best->taken = true;
            if (bestDiff == 0) continue;
            differing++;
            if (shown < 96) Log("replay: frame %d dump: %s owner %s", f, r.name.c_str(), RegionOwner(r.ptr).c_str());
            for (size_t w = 0; w + 8 <= r.size && shown < 96; w += 8) {
                if (masked(w / 8) || std::memcmp(r.ptr + w, best->bytes + w, 8) == 0) continue;
                uint64_t mine, theirs;
                std::memcpy(&mine, r.ptr + w, 8);
                std::memcpy(&theirs, best->bytes + w, 8);
                Log("replay: frame %d dump: %s +0x%zx here %016llx, recorded %016llx", f, r.name.c_str(), w,
                    static_cast<unsigned long long>(mine), static_cast<unsigned long long>(theirs));
                shown++;
            }
        }
        Log("replay: frame %d against the recording's dump: %d regions differ", f, differing);
    }

    // Region by region, by content: each region here must have an equal
    // among the recording's regions of its size. (Names do not pair them: the
    // canonical order numbers same-size regions by content, and the battle
    // can bring a region more or less, which shifts every name after it.)
    int Compare(SfvGame& game, const uint64_t* ours, const uint64_t* recorded, int f)
    {
        if (!recorded) return 0;
        std::map<uint64_t, std::multiset<uint64_t>> bySize;
        for (size_t j = 0; j < rf_.regions.size(); j++) bySize[rf_.regions[j].size].insert(recorded[j]);
        int differ = 0;
        std::string names;
        for (size_t i = 0; i < game.Regions().size(); i++) {
            auto& pool = bySize[game.Regions()[i].size];
            const auto it = pool.find(ours[i]);
            if (it != pool.end()) {
                pool.erase(it);
                continue;
            }
            // The camera's and the pawns' animation clocks follow how ticks
            // fall into rendered frames: presentation, counted apart.
            const std::string& name = game.Regions()[i].name;
            if (f >= 0 && (name == "viewtarget.clock" || name == "pawn.clock")) {
                presentationFrames_++;
                continue;
            }
            differ++;
            if (f >= 0) regionFrames_[name]++;
            if (names.size() < 160) names += (names.empty() ? "" : ", ") + name;
        }
        if (differ && f >= 0) {
            if (firstMismatch_ < 0) firstMismatch_ = f;
            mismatchFrames_++;
            if (logged_++ < 12) Log("replay: frame %d differs from the recording in %d regions: %s", f, differ, names.c_str());
        }
        if (differ && f == -1) Log("replay: state before frame 0 differs in %d regions: %s", differ, names.c_str());
        return differ;
    }

    void Finish(SfvGame& game)
    {
        if (trapSlots_) {
            WatchReport();
            DisarmWatches();
            trapSlots_ = 0;
        }
        if (stress_ == 0 && keepFrom_ >= 0 && kept_.FrameCount() > 0) {
            kept_.finalThrough = kept_.FrameCount() - 1;
            const std::string stem = name_.substr(0, name_.rfind('.'));
            const std::string path = Utf8Path(MatchesDir()) + "\\" + stem + "_kept.sfvr";
            std::string error;
            if (SaveReplay(kept_, path, &error, ReplayDetail::Full)) Log("replay: kept this run as %s", path.c_str());
            else Log("replay: cannot keep this run: %s", error.c_str());
        }
        game.inputOverride = nullptr;
        game.exclusiveTicks = false;
        game.latchEachTick = true;
        if (bench_) {
            CapturedRun run;
            if (benchCapturing_) benchCaptured_ = hitch::EndCapture();
            for (const auto& c : benchCaptured_) {
                run.micros.push_back(c.micros);
                run.net.push_back(c.net);
                run.battle.push_back(c.battle);
                run.draw.push_back(c.draw);
            }
            run.matched = firstMismatch_ < 0 && stressFrames_.empty();
            run.firstDiffer = firstMismatch_ >= 0 ? firstMismatch_ : stressFrames_.empty() ? -1 : *stressFrames_.begin();
            run.stopped = benchStopped_;
            FinishBenchmark(run);
            std::lock_guard lock(g_viewMutex);
            g_view = ReplayView{};
            g_viewCommands.clear();
            g_viewTaken = 0;
        }
        std::function<void()> leave;
        if (viewer_ && !restarting_) {
            MuteGameSounds(false);
            StopCapturedSounds(); // a takeover's, unmuted (and the capture ends with the battle)
            RecordBattleSounds(false);
            std::lock_guard lock(g_viewMutex);
            g_view = ReplayView{};
            g_viewCommands.clear();
            g_viewTaken = 0;
            leave = std::move(g_viewLeave);
        }
        else if (viewer_) {
            // Started again (a jump back): everything this battle still has
            // playing (its loops, its ambience: never ended by the game here)
            // is stopped before it goes, or it plays on over the new one.
            const int stopped = StopBattleSounds();
            Log("replay: the battle's %d sounds stopped for the restart", stopped);
            std::lock_guard lock(g_viewMutex);
            g_view.seeking = true;
            g_view.paused = false;
            g_view.handled += static_cast<uint32_t>(handledNow_);
            handledNow_ = 0;
        }
        const std::string restartPath = path_, restartTitle = title_;
        const RestartPending restartWith{startAt_, paused_, fast_, roundStarts_, roundEnds_, roundCalls_};
        const bool restart = viewer_ && restarting_;
        SetRoundDirectorPerTick(false, false);
        SetPadIsolation(false);
        // Stress: frames whose resimulation differed outside the presentation clocks.
        const int stressDesyncs = static_cast<int>(stressFrames_.size());
        const bool pass = ready_ && firstMismatch_ < 0 && stressDesyncs == 0 && framesChecked_ > last_;
        const std::string mode = stress_ > 0 ? ", stress distance " + std::to_string(stress_)
                                 : delay_ > 0 ? ", P" + std::to_string(remote_ + 1) + " " + std::to_string(delay_) +
                                                    " frames late (" + std::to_string(rollbacks_) + " rollbacks, " +
                                                    std::to_string(resimulated_) + " frames resimulated)"
                                              : "";
        Log("replay: %s %s, %d of %d frames checked%s, %d frames differ from the recording (first %d), "
            "%d presentation clock differences%s", name_.c_str(),
            pass ? "PASS" : "FAIL", framesChecked_, last_ + 1, mode.c_str(),
            mismatchFrames_, firstMismatch_, presentationFrames_,
            stress_ > 0 ? (", " + std::to_string(stressDesyncs) + " frames resimulated differently").c_str() : "");
        // Which regions, on how many frames: one or two small regions on many
        // frames point at a presentation or loading-timing word, many regions
        // at a real divergence.
        std::vector<std::pair<int, std::string>> byCount;
        for (const auto& [name, frames] : regionFrames_) byCount.push_back({frames, name});
        std::sort(byCount.rbegin(), byCount.rend());
        std::string top;
        for (size_t i = 0; i < byCount.size() && i < 12; i++) {
            top += (top.empty() ? "" : ", ") + byCount[i].second + " " + std::to_string(byCount[i].first);
        }
        if (!top.empty()) Log("replay:   regions (frames): %s", top.c_str());
        top.clear();
        byCount.clear();
        for (const auto& [name, frames] : stressRegionFrames_) byCount.push_back({frames, name});
        std::sort(byCount.rbegin(), byCount.rend());
        for (size_t i = 0; i < byCount.size() && i < 12; i++) {
            top += (top.empty() ? "" : ", ") + byCount[i].second + " " + std::to_string(byCount[i].first);
        }
        if (!top.empty()) Log("replay:   resimulated differently (frames): %s", top.c_str());
        const bool viewer = viewer_ || bench_;
        SetFrameDriver(nullptr);
        delete this;
        if (restart) {
            g_restartPending = restartWith;
            RequestReplay(restartPath, 0, -1, -1, "", -1, -1, false, "", -1, 0, 0, -1, restartTitle, true);
        }
        else if (viewer) {
            // Straight to the menus (this is the game thread; the queue a
            // later return goes through does not run mid-fight).
            ChangeScene("MainMenuState");
            if (leave) leave();
        }
    }

    ReplayFile rf_;
    std::string name_;
    // `keep <from> <to>` (straight runs): this run saved as a full recording
    // (<name>_kept.sfvr), with state dumps of those frames: a stress replay of
    // it names the words that differ there (CompareDump).
    int keepFrom_ = -1, keepTo_ = -1;
    int holdAt_ = -1;
    uint64_t holdMs_ = 0, holdUntil_ = 0;
    std::vector<Trap> traps_;
    int trapSlots_ = 0;
    ReplayFile kept_;
    int stress_;
    // `mispredict`: the late player, how late, and per frame the inputs it
    // ran on and its checksum; states before the last delay + 2 frames.
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
    int postLoadLogs_ = 0; // research: the round's state on the ticks after a keyframe load
    std::vector<StateRegion> checkRegions_; // CheckRegions
    int checkLayout_ = -1;
    size_t checkCount_ = 0;
    int frame_ = 0;
    int fed_ = 0;
    int checked_ = -1;
    int framesChecked_ = 0;
    int firstMismatch_ = -1;
    int mismatchFrames_ = 0;
    int presentationFrames_ = 0; // camera / pawn clock differences (not a failure)
    std::map<std::string, int> regionFrames_; // frames each region differed on
    std::set<int> stressFrames_;                    // frames resimulated differently (not the clocks)
    std::map<std::string, int> stressRegionFrames_;
    int stressLogged_ = 0;
    int logged_ = 0;
    NoTransport transport_;
    std::unique_ptr<NetplaySession> session_;
};

} // namespace

void SetReplayLatch(int latch) { g_latch = latch; }

void RequestReplayView(const std::string& path, const std::string& title, std::function<void()> onLeave, int holdAt, int holdSeconds)
{
    {
        std::lock_guard lock(g_viewMutex);
        g_viewCommands.clear();
        g_viewTaken = g_view.handled;
        g_viewLeave = std::move(onLeave);
    }
    RequestReplay(path, 0, -1, -1, "", -1, -1, true, "", holdAt, holdSeconds, 0, -1, title.empty() ? path : title);
}

bool RequestBenchmark(const std::string& path)
{
    if (CurrentFrameDriver()) {
        Log("benchmark: another driver is active");
        return false;
    }
    {
        std::lock_guard lock(g_viewMutex);
        g_viewCommands.clear();
        g_viewLeave = nullptr;
        g_view = ReplayView{};
        g_viewTaken = 0;
        g_view.active = true;
        g_view.benchmark = true;
        g_view.title = "Benchmark";
    }
    RequestReplay(path, 8, -1, -1, "", -1, -1, true, "", -1, 0, 0, -1, "", false, true);
    return CurrentFrameDriver() != nullptr;
}

void SetReplayExperimentValue(const std::string& what, int value)
{
    if (what == "callpace") g_callPace = value;
    else if (what == "callafter") g_callAfter = value;
    Log("rview: %s %d", what.c_str(), value);
}

void SetReplayExperiment(const std::string& what, bool on)
{
    if (what == "callkeys") g_callKeys = on;
    else if (what == "crossrounds") g_crossRounds = on;
    else if (what == "anykeys") g_anyKeys = on;
    else if (what == "keysanywhere") g_keysAnywhere = on; // off: the old rule (fight keys within a round, call keys)
    else if (what == "intro") g_skipIntro = !on;            // on: the viewer plays the intro
    else return Log("rview: no experiment %s", what.c_str());
    Log("rview: %s %s", what.c_str(), on ? "on" : "off");
}

uint32_t ControlReplay(ReplayControl c, int arg)
{
    std::lock_guard lock(g_viewMutex);
    if (!g_view.active && c != ReplayControl::Leave) return 0;
    g_viewCommands.push_back({c, arg});
    // Taken in order: this one is in once `handled` (as published) reaches
    // the count of those taken plus those waiting.
    return g_viewTaken + static_cast<uint32_t>(g_viewCommands.size());
}

int ReplayTickFrame() { return g_tickFrame; }
bool ReplayTickInFight() { return g_tickFight; }

ReplayView ReplayViewNow()
{
    std::lock_guard lock(g_viewMutex);
    return g_view;
}

void RequestReplay(const std::string& path, int stressDistance, int watchFrom, int watchTo, const std::string& watchRegion,
                   int keepFrom, int keepTo, bool preload, const std::string& traps, int holdAt, int holdSeconds,
                   int mispredictDelay, int remote, const std::string& viewTitle, bool jumpNow, bool benchmark)
{
    if (path == "stop") {
        g_stop = CurrentFrameDriver() != nullptr;
        return;
    }
    if (CurrentFrameDriver()) {
        Log("replay: another driver is active (leave the session first)");
        return;
    }
    const bool relative = path.size() < 2 || (path[1] != ':' && path[0] != '\\' && path[0] != '/');
    const std::string full = relative ? FindRecording(path) : path;
    ReplayFile rf;
    std::string error;
    if (!LoadReplay(full, &rf, &error)) {
        Log("replay: %s: %s", full.c_str(), error.c_str());
        return;
    }
    stressDistance = std::clamp(stressDistance, 0, 30);
    const std::string name = full.substr(full.find_last_of("\\/") + 1);
    Log("replay: %s, %d frames (confirmed through %d), build %s, %s%s", name.c_str(), rf.FrameCount(), rf.finalThrough,
        rf.gameBuild.c_str(), DescribeSetup(rf.setup).c_str(),
        stressDistance ? (", stress distance " + std::to_string(stressDistance)).c_str() : "");
    // Both characters loaded first, as a netplay lobby has them (assets that
    // load while the battle runs can put effects out of step, finding 23).
    // The jump follows.
    std::vector<PreloadPick> picks;
    for (const PlayerSetup& p : rf.setup.players) {
        if (p.character) picks.push_back({UnpackCode(p.character), static_cast<int>(p.costume), p.version});
    }
    // The recorded setup and stage for the battle the jump starts (without
    // the stage the battle loads the game's default request, Training).
    // As the PC that recorded it showed it (two players on one look: the
    // other player recolored, core/look.h), so its frozen state hashes the
    // same; the host's view for a recording made outside a session.
    SetPendingSetup(ShownSetup(rf.setup, rf.localPlayer == 1 ? 1 : 0, GameLookRules()));
    SetPendingStage(rf.setup.stage == kKeepStage ? "" : StageCode(rf.setup.stage));
    Log("replay: stage %s", rf.setup.stage == kKeepStage ? "as prepared" : StageCode(rf.setup.stage).c_str());
    auto* driver = new ReplayDriver(std::move(rf), name, stressDistance, watchFrom, watchTo, watchRegion);
    if (benchmark) driver->SetBenchmark();
    if (!viewTitle.empty()) {
        driver->SetViewer(viewTitle, full);
        if (g_restartPending.target > 0) {
            driver->StartAt(g_restartPending);
            g_restartPending = {};
        }
    }
    driver->Keep(keepFrom, keepTo);
    driver->Hold(holdAt, holdSeconds);
    if (mispredictDelay > 0 && stressDistance == 0) driver->Mispredict(std::clamp(mispredictDelay, 1, 8), remote);
    std::istringstream ts(traps);
    ReplayDriver::Trap t;
    while (ts >> t.frame >> t.region >> std::hex >> t.offset >> std::dec >> t.bytes) driver->AddTrap(t);
    SetFrameDriver(driver);
    if (jumpNow) {
        // A viewer's restart, on the game thread from inside the battle (where
        // the game-thread queue does not run): the characters are loaded.
        JumpToBattle();
        return;
    }
    if (!preload) picks.clear();
    PreloadCharacters(std::move(picks), [] { JumpToBattle(); });
}

} // namespace sfv::mod
