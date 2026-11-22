#include "mod/replay_internal.h"

namespace sfv::mod {

using namespace replay;

namespace replay {

void ReplayDriver::TakeoverControl(SfvGame& game, ReplayControl c, int arg) {
    if (c == ReplayControl::TakeoverStop) {
        if (tk_.phase == TakeoverPhase::Countdown) {
            Log("replay: takeover called off in its countdown");
            tk_ = Takeover{};
        } else if (tk_.phase == TakeoverPhase::Live || tk_.phase == TakeoverPhase::KoScreen) {
            Log("replay: takeover stopped at frame %d", frame_ - 1);
            TakeoverFadeOut();
        }
        (void)game;
        return;
    }
    if (tk_.phase != TakeoverPhase::None) return;
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
    FlowSaveOutsideState(&t.start.outside, keys_.empty() ? nullptr : &keys_.back().outside);
    tk_ = std::move(t);
    paused_ = true;
    Log("replay: takeover of P%d from frame %d", tk_.side + 1, frame_ - 1);
}

void ReplayDriver::TakeoverFrame(SfvGame& game) {
    const ULONGLONG now = GetTickCount64();
    switch (tk_.phase) {
        case TakeoverPhase::Countdown:
            if (now >= tk_.until) {
                tk_.phase = TakeoverPhase::Live;
                BattleBeginSoundCapture();
                Log("replay: takeover of P%d playing", tk_.side + 1);
            }
            break;
        case TakeoverPhase::Live: {
            FrameInputs in = frame_ <= last_ ? rf_.inputs[frame_] : FrameInputs{};
            in.p[tk_.side] = InputMappedLocal(0) & ~static_cast<PackedInput>(kStart);
            if (in.p[tk_.side] != tk_.lastInput && tk_.inputLogs++ < 12) {
                Log("replay: takeover input %08X at frame %d", static_cast<unsigned>(in.p[tk_.side]), frame_);
            }
            tk_.lastInput = in.p[tk_.side];
            g_tickFrame = frame_;
            g_tickFight = wasFight_;
            g_tickRound = CallsThrough(frame_ - 1);
            game.Advance(in, false);
            frame_++;
            wasFight_ = FlowRoundInFight();
            if (!wasFight_) {
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
            if (now >= tk_.until || tk_.ticks >= kTakeoverKoTicks) TakeoverFadeOut();
            else TakeoverTick(game);
            break;
        case TakeoverPhase::FadeOut:
            if (now >= tk_.until) TakeoverRewind(game);
            break;
        case TakeoverPhase::ToFight: {
            const ULONGLONG until = now + 40;
            while (!wasFight_ && !FlowMatchDecided() && tk_.ticks < kTakeoverRunMax && GetTickCount64() < until) TakeoverTick(game);
            if (wasFight_) TakeoverLoad(game);
            else if (FlowMatchDecided() || tk_.ticks >= kTakeoverRunMax) {
                const int start = tk_.start.frame;
                Log("replay: no fight after the takeover's round (%s): the battle again to frame %d", FlowMatchDecided() ? "the match decided" : "none came", start - 1);
                tk_ = Takeover{};
                paused_ = true;
                RestartAt(game, start);
            }
            break;
        }
        default: break;
    }
}

void ReplayDriver::TakeoverTick(SfvGame& game) {
    g_tickFrame = frame_;
    g_tickFight = wasFight_;
    g_tickRound = CallsThrough(frame_ - 1);
    game.Advance(FrameInputs{}, false);
    frame_++;
    tk_.ticks++;
    wasFight_ = FlowRoundInFight();
}

void ReplayDriver::TakeoverFadeOut() {
    tk_.phase = TakeoverPhase::FadeOut;
    tk_.until = GetTickCount64() + kTakeoverFadeMs;
}

void ReplayDriver::TakeoverRewind(SfvGame& game) {
    const int stopped = BattleStopCapturedSounds();
    UiSoundMuteGame(true);
    Log("replay: rewinding the takeover from frame %d (%d of its sounds stopped)", frame_ - 1, stopped);
    if (!wasFight_ && !tk_.ko.empty() && tk_.ticks <= kTakeoverKoTicks) {
        LoadRegions(game.Regions(), tk_.ko.data());
        game.OnStateLoaded(tk_.koFrame - 1);
        frame_ = tk_.koFrame;
        wasFight_ = FlowRoundInFight();
        Log("replay: back at the takeover's KO frame %d (%d ticks after it undone)", frame_ - 1, tk_.ticks);
    }
    tk_.ticks = 0;
    if (wasFight_ || frame_ == tk_.koFrame) TakeoverLoad(game);
    else tk_.phase = TakeoverPhase::ToFight;
}

void ReplayDriver::TakeoverLoad(SfvGame& game) {
    const int start = tk_.start.frame;
    crossFight_ = true;
    const int k = LoadableKey(start);
    if (k >= 0) {
        tk_ = Takeover{};
        paused_ = true;
        warm_ = kTakeoverWarm;
        Seek(game, start);
    } else {
        const Keyframe& kept = tk_.start;
        LoadRegions(game.Regions(), kept.regions.data());
        FlowRestoreOutsideState(kept.outside);
        game.OnStateLoaded(kept.frame - 1);
        frame_ = kept.frame;
        wasFight_ = FlowRoundInFight();
        paused_ = true;
        tk_ = Takeover{};
        UiSoundMuteGame(false);
    }
    crossFight_ = false;
    Log("replay: back to frame %d after the takeover (%s, from frame %d)", start - 1, k >= 0 ? "from a keyframe" : "the kept state",
        frame_ - 1);
}

} // namespace replay

} // namespace sfv::mod
