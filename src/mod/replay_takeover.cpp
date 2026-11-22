#include "mod/replay_internal.h"

namespace sfv::mod {

using namespace replay;

namespace replay {

void ReplayDriver::TakeoverControl(SfvGame& game, ReplayControl c, int arg) {
    if (c == ReplayControl::TakeoverStop) {
        if (takeover_.phase == TakeoverPhase::Countdown) {
            Log("replay: takeover called off in its countdown");
            takeover_ = Takeover{};
        } else if (takeover_.phase == TakeoverPhase::Live || takeover_.phase == TakeoverPhase::KoScreen) {
            Log("replay: takeover stopped at frame %d", frame_ - 1);
            TakeoverFadeOut();
        }
        (void)game;
        return;
    }
    if (takeover_.phase != TakeoverPhase::None) return;
    if (!wasFight_ || seekTarget_ >= 0 || findRound_ || runToFight_ >= 0 || frame_ > last_) {
        Log("replay: no takeover here (frame %d, %s)", frame_ - 1, wasFight_ ? "the replay is moving" : "not in a fight");
        return;
    }
    EndQuiet();
    Takeover t;
    t.phase = TakeoverPhase::Countdown;
    t.side = arg & 1;
    t.until = GetTickCount64() + kTakeoverCountdownMs;
    t.start.frame = frame_;
    t.start.inFight = true;
    t.start.regions.resize(StateSize(game.Regions()));
    SaveRegions(game.Regions(), t.start.regions.data());
    FlowSaveOutsideState(&t.start.outside, keys_.empty() ? nullptr : &keys_.back().outside);
    takeover_ = std::move(t);
    paused_ = true;
    Log("replay: takeover of P%d from frame %d", takeover_.side + 1, frame_ - 1);
}

void ReplayDriver::TakeoverFrame(SfvGame& game) {
    const ULONGLONG now = GetTickCount64();
    switch (takeover_.phase) {
        case TakeoverPhase::Countdown:
            if (now >= takeover_.until) {
                takeover_.phase = TakeoverPhase::Live;
                BattleBeginSoundCapture();
                Log("replay: takeover of P%d playing", takeover_.side + 1);
            }
            break;
        case TakeoverPhase::Live: {
            FrameInputs in = frame_ <= last_ ? rf_.inputs[frame_] : FrameInputs{};
            in.p[takeover_.side] = InputMappedLocal(0) & ~static_cast<PackedInput>(kStart);
            if (in.p[takeover_.side] != takeover_.lastInput && takeover_.inputLogs++ < 12) {
                Log("replay: takeover input %08X at frame %d", static_cast<unsigned>(in.p[takeover_.side]), frame_);
            }
            takeover_.lastInput = in.p[takeover_.side];
            g_tickFrame = frame_;
            g_tickFight = wasFight_;
            g_tickRound = CallsThrough(frame_ - 1);
            game.Advance(in, false);
            frame_++;
            wasFight_ = FlowRoundInFight();
            if (!wasFight_) {
                takeover_.koFrame = frame_;
                takeover_.ko.resize(StateSize(game.Regions()));
                SaveRegions(game.Regions(), takeover_.ko.data());
                takeover_.phase = TakeoverPhase::KoScreen;
                takeover_.until = now + kTakeoverKoScreenMs;
                takeover_.ticks = 0;
                Log("replay: takeover's fight over at frame %d", frame_ - 1);
            }
            break;
        }
        case TakeoverPhase::KoScreen:
            if (now >= takeover_.until || takeover_.ticks >= kTakeoverKoTicks) TakeoverFadeOut();
            else TakeoverTick(game);
            break;
        case TakeoverPhase::FadeOut:
            if (now >= takeover_.until) TakeoverRewind(game);
            break;
        case TakeoverPhase::ToFight: {
            const ULONGLONG until = now + 40;
            while (!wasFight_ && !FlowMatchDecided() && takeover_.ticks < kTakeoverRunMax && GetTickCount64() < until) TakeoverTick(game);
            if (wasFight_) TakeoverLoad(game);
            else if (FlowMatchDecided() || takeover_.ticks >= kTakeoverRunMax) {
                const int start = takeover_.start.frame;
                Log("replay: no fight after the takeover's round (%s): the battle again to frame %d", FlowMatchDecided() ? "the match decided" : "none came", start - 1);
                takeover_ = Takeover{};
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
    takeover_.ticks++;
    wasFight_ = FlowRoundInFight();
}

void ReplayDriver::TakeoverFadeOut() {
    takeover_.phase = TakeoverPhase::FadeOut;
    takeover_.until = GetTickCount64() + kTakeoverFadeMs;
}

void ReplayDriver::TakeoverRewind(SfvGame& game) {
    const int stopped = BattleStopCapturedSounds();
    UiSoundMuteGame(true);
    Log("replay: rewinding the takeover from frame %d (%d of its sounds stopped)", frame_ - 1, stopped);
    if (!wasFight_ && !takeover_.ko.empty() && takeover_.ticks <= kTakeoverKoTicks) {
        LoadRegions(game.Regions(), takeover_.ko.data());
        game.OnStateLoaded(takeover_.koFrame - 1);
        frame_ = takeover_.koFrame;
        wasFight_ = FlowRoundInFight();
        Log("replay: back at the takeover's KO frame %d (%d ticks after it undone)", frame_ - 1, takeover_.ticks);
    }
    takeover_.ticks = 0;
    if (wasFight_ || frame_ == takeover_.koFrame) TakeoverLoad(game);
    else takeover_.phase = TakeoverPhase::ToFight;
}

void ReplayDriver::TakeoverLoad(SfvGame& game) {
    const int start = takeover_.start.frame;
    crossFight_ = true;
    const int k = LoadableKey(start);
    if (k >= 0) {
        takeover_ = Takeover{};
        paused_ = true;
        warmFrames_ = kTakeoverWarm;
        Seek(game, start);
    } else {
        const Keyframe& kept = takeover_.start;
        LoadRegions(game.Regions(), kept.regions.data());
        FlowRestoreOutsideState(kept.outside);
        game.OnStateLoaded(kept.frame - 1);
        frame_ = kept.frame;
        wasFight_ = FlowRoundInFight();
        paused_ = true;
        takeover_ = Takeover{};
        UiSoundMuteGame(false);
    }
    crossFight_ = false;
    Log("replay: back to frame %d after the takeover (%s, from frame %d)", start - 1, k >= 0 ? "from a keyframe" : "the kept state",
        frame_ - 1);
}

} // namespace replay

} // namespace sfv::mod
