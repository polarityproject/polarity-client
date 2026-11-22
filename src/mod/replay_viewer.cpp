#include "mod/replay_internal.h"

namespace sfv::mod {

using namespace replay;

namespace {

// The battle frame's two stop flag words.
constexpr uint32_t kStopFlagsOff = 0x190;

} // namespace

namespace replay {

int ReplayDriver::LoadableKey(int target) const {
    auto it = std::upper_bound(keys_.begin(), keys_.end(), target - 1, [](int f, const Keyframe& k) { return f < k.frame; });
    if (it == keys_.begin()) return -1;
    const int k = static_cast<int>(it - keys_.begin()) - 1;
    const int keyFrame = keys_[k].frame;
    const bool sameRound =
        std::none_of(roundEnds_.begin(), roundEnds_.end(), [&](int e) { return e >= keyFrame && e < frame_ - 1; });
    const bool call = g_callKeys && keys_[k].call && wasFight_;
    const bool crossFight = crossFight_ && keys_[k].fight && wasFight_;
    if (g_anyKeys) return k;
    const bool opening = roundStarts_.empty() || keyFrame <= roundStarts_.front();
    if (!opening && g_keysAnywhere) return k;
    return (keys_[k].fight && sameRound) || call || crossFight || (g_crossRounds && (keys_[k].fight || keys_[k].call)) ? k : -1;
}

int ReplayDriver::CallKeyFor(int target) const {
    auto it = std::upper_bound(keys_.begin(), keys_.end(), target - 1, [](int f, const Keyframe& k) { return f < k.frame; });
    if (it == keys_.begin() || !g_callKeys) return -1;
    return std::prev(it)->call ? static_cast<int>(it - keys_.begin()) - 1 : -1;
}

bool ReplayDriver::FightAhead() const {
    if (FlowMatchDecided()) return false;
    const int shown = frame_ - 1;
    for (const ActionRange& r : ActionRanges()) {
        if (r.first > shown) return true;
    }
    const bool fileHasRounds = std::any_of(rf_.summary.rounds.begin(), rf_.summary.rounds.end(), [](const RoundResult& r) { return r.fightFrame >= 0; });
    return !fileHasRounds;
}

std::vector<ReplayDriver::ActionRange> ReplayDriver::ActionRanges() const {
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
        } else out.push_back({first, last});
    }
    return out;
}

int ReplayDriver::BackLimit() const {
    for (const Keyframe& k : keys_) {
        const bool thisRound = std::none_of(roundEnds_.begin(), roundEnds_.end(), [&](int e) { return e >= k.frame && e < frame_ - 1; });
        if (k.fight && k.frame < frame_ && thisRound && LoadableKey(k.frame + 1) >= 0) return k.frame;
    }
    return std::max(0, frame_ - 1);
}

bool ReplayDriver::ViewerFrame(SfvGame& game) {
    if (!Valid(game)) return false;
    hudTicks_ = 0;
    if (frame_ == 0 && keys_.empty()) {
        roundEndsSeen_ = FlowRoundEndEvents();
        BattleRecordSounds(true);
        Log("replay: viewing %s, %d frames%s", title_.c_str(), last_ + 1, startAt_ > 0 ? " (restarted)" : "");
        if (startAt_ > 0) {
            UiSoundMuteGame(true);
            target_ = startAt_;
            startAt_ = -1;
        } else if (g_skipIntro) {
            Log("replay: on to round 1's call, past the intro");
            findRound_ = true;
            UiSoundMuteGame(true);
        }
        if (UiSoundGameMuted()) BattleBeginSoundCapture();
    }
    std::vector<std::pair<ReplayControl, int>> commands;
    {
        std::lock_guard lock(g_viewMutex);
        commands.swap(g_viewCommands);
        g_viewTaken += static_cast<uint32_t>(commands.size());
    }
    handledNow_ += static_cast<int>(commands.size());
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
        if (tk_.phase != TakeoverPhase::None) continue;
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
        FlowPumpGameThread();
        return true;
    }
    if (restarting_) {
        Finish(game);
        return false;
    }
    const int runFrom = frame_;
    int ran = 0;
    const auto room = [&] { return g_runTicks <= 0 || ran++ < g_runTicks; };
    const auto koScreenOver = [&] {
        if (GetTickCount64() < koHoldUntil_) return false;
        const int pace = g_callPace;
        if (pace <= 0 || frame_ - runFrom < pace) return true;
        const bool banner = FlowRoundCallRunning() || (wasFight_ && !roundStarts_.empty() && frame_ - roundStarts_.back() <= g_callAfter);
        return !banner;
    };
    if (target_ >= 0) {
        const ULONGLONG until = GetTickCount64() + 40;
        while (frame_ < target_ - 1 && frame_ <= last_ && GetTickCount64() < until && koScreenOver() && room()) {
            Simulate(game, frame_ < target_ - 1 - warm_);
        }
        if ((frame_ >= target_ - 1 || frame_ > last_) && koScreenOver()) {
            if (!quiet_) UiSoundMuteGame(false);
            if (frame_ == target_ - 1 && frame_ <= last_) Simulate(game, false);
            Log("replay: at frame %d (seek done), %s", frame_ - 1, HudStepReport().c_str());
            target_ = -1;
            warm_ = 0;
        }
    } else if (runToFight_ >= 0) {
        const ULONGLONG until = GetTickCount64() + 40;
        while (!wasFight_ && frame_ <= last_ && GetTickCount64() < until && koScreenOver() && !FlowMatchDecided() && room()) Simulate(game, true);
        if (wasFight_ || frame_ > last_ || FlowMatchDecided()) {
            const int target = std::exchange(runToFight_, -1);
            if (wasFight_) Seek(game, target);
            else RestartAt(game, target);
            if (restarting_) {
                Finish(game);
                return false;
            }
        }
    } else if (findRound_) {
        const ULONGLONG until = GetTickCount64() + 40;
        while (findRound_ && frame_ <= last_ && GetTickCount64() < until && koScreenOver() && !holdUntil_ && room()) {
            const size_t known = roundCalls_.size();
            const size_t soundMark = BattleSoundCaptureMark();
            Simulate(game, true);
            if (roundCalls_.size() > known) {
                findRound_ = false;
                Log("replay: on at round call %d, frame %d (%s)", static_cast<int>(roundCalls_.size()), roundCalls_.back(), HudStepReport().c_str());
                if (roundCalls_.back() + 1 == frame_) {
                    BattleKeepCapturedFrom(soundMark);
                    UiSoundMuteGame(false);
                } else Seek(game, roundCalls_.back() + 1);
                if (restarting_) {
                    Finish(game);
                    return false;
                }
            }
        }
        if (findRound_ && frame_ > last_) {
            findRound_ = false;
            UiSoundMuteGame(false);
        }
    } else if (!paused_ && frame_ <= last_) {
        for (int k = 0; k < std::max(1, fast_) && frame_ <= last_ && (k == 0 || wasFight_); k++) Simulate(game, false);
    }
    if (frame_ > last_ && target_ < 0 && !findRound_ && runToFight_ < 0) paused_ = true;
    Publish();
    FlowPumpGameThread();
    return true;
}

void ReplayDriver::Simulate(SfvGame& game, bool lean) {
    if (viewer_ && hudTicks_++ > 0 &&
        (FlowRoundCallRunning() || (wasFight_ && !roundStarts_.empty() && frame_ - roundStarts_.back() <= kHudFightFrames))) {
        StepHudMovies(1.0f / 60.0f);
    }
    const bool due = frame_ % kKeyEvery == 0 || std::binary_search(roundStarts_.begin(), roundStarts_.end(), frame_ - 1);
    if (due && (keys_.empty() || keys_.back().frame < frame_)) {
        Keyframe k;
        k.frame = frame_;
        k.fight = wasFight_;
        k.regions.resize(StateSize(game.Regions()));
        SaveRegions(game.Regions(), k.regions.data());
        if (!FlowSaveOutsideState(&k.outside, keys_.empty() ? nullptr : &keys_.back().outside) && keyWarnings_++ < 3) {
            Log("replay: keyframe at frame %d without the phase machine", frame_);
        }
        keys_.push_back(std::move(k));
    }
    const bool firstPass = keys_.empty() || keys_.back().frame <= frame_;
    if (!wasFight_ && firstPass) {
        scratch_.regions.resize(StateSize(game.Regions()));
        SaveRegions(game.Regions(), scratch_.regions.data());
        FlowSaveOutsideState(&scratch_.outside, keys_.empty() ? nullptr : &keys_.back().outside);
        scratch_.frame = frame_;
    }
    g_tickFrame = frame_;
    g_tickFight = wasFight_;
    g_tickRound = CallsThrough(frame_ - 1);
    game.Advance(rf_.inputs[frame_], lean && wasFight_);
    if (postLoadLogs_ > 0) {
        postLoadLogs_--;
        Log("replay: frame %d after the load: %s, battle frame stop flags %08X %08X", frame_, FlowRoundEndState().c_str(),
            game.BattleFrameObject() ? *reinterpret_cast<const uint32_t*>(game.BattleFrameObject() + kStopFlagsOff) : 0u,
            game.BattleFrameObject() ? *reinterpret_cast<const uint32_t*>(game.BattleFrameObject() + kStopFlagsOff + 4) : 0u);
    }
    CheckFrame(game, frame_);
    const bool fight = FlowRoundInFight();
    if (fight && !wasFight_ && (roundStarts_.empty() || roundStarts_.back() < frame_)) {
        roundStarts_.push_back(frame_);
    }
    const bool call = FlowRoundCallRunning();
    if (call && !wasCall_ && (roundCalls_.empty() || roundCalls_.back() < frame_)) roundCalls_.push_back(frame_);
    if (call && !wasCall_ && scratch_.frame == frame_) {
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
    if (!fight && wasFight_ && (roundEnds_.empty() || roundEnds_.back() < frame_)) roundEnds_.push_back(frame_);
    if (!fight && wasFight_) {
        koMs_ = GetTickCount64();
        if (lean) Log("replay: a run passed the KO at frame %d", frame_);
    }
    if (const uint32_t ends = FlowRoundEndEvents(); ends != roundEndsSeen_) {
        roundEndsSeen_ = ends;
        if (lean && koMs_) koHoldUntil_ = koMs_ + kKoScreenMs;
    }
    wasFight_ = fight;
    if (viewer_) MaybeHold(frame_);
    frame_++;
}

void ReplayDriver::Seek(SfvGame& game, int target, bool quiet) {
    target = std::clamp(target, 1, last_ + 1);
    if (target == frame_ || keys_.empty()) return;
    UiSoundMuteGame(true);
    quiet_ = quiet;
    if (target < frame_) {
        const int shown = frame_ - 1;
        const auto end = std::upper_bound(roundEnds_.begin(), roundEnds_.end(), shown);
        const bool afterKo = end != roundEnds_.begin() && std::none_of(roundStarts_.begin(), roundStarts_.end(), [&](int s) {
                                 return s > *std::prev(end) && s <= shown;
                             });
        const bool hudAway = !wasFight_ && !crossFight_ && afterKo;
        if (hudAway) {
            if (FightAhead()) {
                Log("replay: on to the next fight before going back to frame %d (the HUD is away)", target - 1);
                runToFight_ = target;
            } else RestartAt(game, target);
            return;
        }
        const int k = LoadableKey(target);
        if (k < 0 && !wasFight_ && CallKeyFor(target) >= 0 && FightAhead()) {
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
        FlowRestoreOutsideState(key.outside);
        game.OnStateLoaded(key.frame - 1);
        frame_ = key.frame;
        wasFight_ = FlowRoundInFight();
        seeks_++;
        logged_ = 0;
        Log("replay: loaded the keyframe of frame %d (%s, %s)", key.frame, key.fight ? "fight" : key.call ? "round call"
                                                                                                          : "between fights",
            FlowRoundEndState().c_str());
        postLoadLogs_ = 4;
    }
    target_ = target;
    if (target_ == frame_) {
        if (!quiet_) UiSoundMuteGame(false);
        target_ = -1;
    }
}

void ReplayDriver::EndQuiet() {
    if (!quiet_) return;
    quiet_ = false;
    UiSoundMuteGame(false);
}

void ReplayDriver::RestartAt(SfvGame& game, int target) {
    Log("replay: restarting the battle to reach frame %d (from %d)", target - 1, frame_ - 1);
    startAt_ = target;
    restarting_ = true;
    (void)game;
}

void ReplayDriver::Control(SfvGame& game, ReplayControl c, int arg) {
    const int shown = frame_ - 1;
    switch (c) {
        case ReplayControl::Pause: paused_ = true; break;
        case ReplayControl::Play:
            paused_ = false;
            EndQuiet();
            if (frame_ > last_) Seek(game, StartFrame());
            break;
        case ReplayControl::Toggle: Control(game, paused_ ? ReplayControl::Play : ReplayControl::Pause, 0); break;
        case ReplayControl::Step:
            paused_ = true;
            if (arg < 0) {
                if (target_ >= 0 || findRound_ || runToFight_ >= 0) break;
                const int to = std::max(frame_ + arg, BackLimit() + 1);
                if (to < frame_) Seek(game, to, true);
                break;
            }
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
        case ReplayControl::Seek:
            EndQuiet();
            Seek(game, arg);
            break;
        case ReplayControl::Round:
            EndQuiet();
            Log("replay: round jump %+d from frame %d", arg, shown);
            if (arg > 0 && !CanRoundForward()) Log("replay: no round after this one");
            else if (arg > 0) {
                auto next = std::upper_bound(roundCalls_.begin(), roundCalls_.end(), shown);
                if (next != roundCalls_.end()) Seek(game, *next + 1);
                else if (frame_ <= last_) {
                    findRound_ = true;
                    UiSoundMuteGame(true);
                }
            } else Seek(game, PreviousRoundTarget());
            break;
        case ReplayControl::Fast: fast_ = std::clamp(arg, 0, 4); break;
        case ReplayControl::HoldAt: Hold(arg, 5); break;
        case ReplayControl::OutOfSync:
            if (firstMismatch_ < 0) firstMismatch_ = std::max(0, frame_ - 1);
            mismatchFrames_++;
            break;
        default: break;
    }
}

void ReplayDriver::Publish() {
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
    g_view.callRound = CallsThrough(frame_ - 1);
    g_view.mismatches = mismatchFrames_;
    g_view.outOfSync = firstMismatch_ < 0 ? -1
                                          : static_cast<int>(std::upper_bound(roundStarts_.begin(), roundStarts_.end(), firstMismatch_) - roundStarts_.begin());
    g_view.seeks = seeks_;
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
        } else {
            const bool nextCall = std::any_of(roundCalls_.begin(), roundCalls_.end(), [&](int c) { return c > r.last && c <= shown; });
            g_view.roundProgress = nextCall ? 0.0f : 1.0f;
        }
    }
}

bool ReplayDriver::InOpeningRun() const {
    if (!g_skipIntro || (target_ < 0 && !findRound_)) return false;
    return roundCalls_.empty() ? findRound_ : frame_ <= roundCalls_.front() && (findRound_ || target_ == roundCalls_.front() + 1);
}

bool ReplayDriver::CanRoundForward() const {
    const int shown = frame_ - 1;
    const auto next = std::upper_bound(roundCalls_.begin(), roundCalls_.end(), shown);
    if (next != roundCalls_.end()) return true;
    const int round = static_cast<int>(next - roundCalls_.begin());
    const int rounds = static_cast<int>(std::count_if(rf_.summary.rounds.begin(), rf_.summary.rounds.end(), [](const RoundResult& r) { return r.fightFrame >= 0; }));
    if (rounds > 0) return round < rounds;
    return frame_ <= last_ && !FlowMatchDecided();
}

int ReplayDriver::PreviousRoundTarget() const {
    const int shown = frame_ - 1;
    auto it = std::upper_bound(roundCalls_.begin(), roundCalls_.end(), shown);
    if (it == roundCalls_.begin()) return StartFrame();
    const int call = *std::prev(it);
    auto fight = std::upper_bound(roundStarts_.begin(), roundStarts_.end(), call - 1);
    const bool justBegun = fight == roundStarts_.end() || *fight >= shown || shown - *fight <= kKeyEvery;
    if (!justBegun) return call + 1;
    if (std::prev(it) == roundCalls_.begin()) return StartFrame();
    return *std::prev(it, 2) + 1;
}

} // namespace replay

} // namespace sfv::mod
