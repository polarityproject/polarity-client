#include "mod/replay_internal.h"

namespace sfv::mod {

using namespace replay;

namespace replay {

bool ReplayDriver::Warmup(SfvGame& game) {
    if (game.WarmupStep()) {
        if (game.WarmupTicks() % 300 == 0) Log("replay: still no battle objects after %d ticks", game.WarmupTicks());
        return true;
    }
    ready_ = true;
    InputIsolatePads(true);
    game.exclusiveTicks = true;
    game.latchEachTick = stress_ > 0 || delay_ > 0 || BuildAdapterVersion(rf_.gameBuild) >= 9;
    if (const int latch = g_latch.exchange(-1); latch >= 0) game.latchEachTick = latch != 0;
    if (!game.latchEachTick) Log("replay: %s predates adapter-9: the game's own pad latch", rf_.gameBuild.c_str());
    FlowSetRoundDirectorPerTick(true, true);
    BattleSetMatchDrawSeed(rf_.setup.seed);
    game.Recanonicalize();
    std::vector<uint64_t> hashes(CheckRegions(game).size());
    HashRegions(CheckRegions(game), hashes.data());
    layout_ = game.LayoutVersion();
    game.ArmStartProbe();
    if (stress_ == 0 && keepFrom_ >= 0) {
        kept_ = ReplayFile{};
        kept_.modVersion = rf_.modVersion;
        kept_.gameBuild = game.BuildId();
        kept_.matchId = rf_.matchId;
        kept_.localPlayer = rf_.localPlayer;
        kept_.setup = rf_.setup;
        kept_.regions = DescribeRegions(game.Regions());
        kept_.initialHashes.assign(game.Regions().size(), 0);
        HashRegions(game.Regions(), kept_.initialHashes.data());
        std::vector<uint8_t> state(StateSize(game.Regions()));
        SaveRegions(game.Regions(), state.data());
        kept_.dumps.push_back({-1, "initial", std::move(state)});
    }
    const int differ = Compare(game, hashes.data(), rf_.initialHashes.data(), -1);
    Log("replay: battle objects ready after %d neutral ticks (%s); state before frame 0 %s", game.ObjectsAt(),
        FlowRoundEndState().c_str(),
        differ ? ("differs in " + std::to_string(differ) + " regions").c_str() : "equal");
    if (viewer_) HudStepFindMovies();
    BattleDataCheckSoon(bench_ ? "benchmark" : "replay");
    if (bench_) benchSession_ = BenchmarkActivityText();
    BenchmarkNoteActivity(bench_ ? "benchmark" : "replay");
    if (stress_ > 0) {
        NetplayConfig nc;
        nc.stressCheckDistance = static_cast<unsigned char>(stress_);
        nc.predictionWindow = static_cast<unsigned char>(stress_ + 1);
        nc.historyDepth = static_cast<unsigned int>(stress_ + 2);
        nc.inputDelay = 0;
        nc.modVersion = "replay";
        nc.log = [](const std::string& m) { Log("replay: %s", m.c_str()); };
        game.inputOverride = [this](int player) {
            const int f = std::min(fed_, rf_.FrameCount() - 1);
            const PackedInput in = rf_.inputs[f].p[player & 1];
            if (player == 1) fed_++;
            return in;
        };
        session_ = std::make_unique<NetplaySession>(game, transport_, nc);
        session_->onStressDiff = [this, &game](int frame, size_t region, size_t offset) {
            if (region >= game.Regions().size()) return;
            const std::string& name = game.Regions()[region].name;
            if (name == "viewtarget.clock" || name == "pawn.clock") return;
            if (resimFirst_ < 0) resimFirst_ = frame;
            if (frame == resimFirst_ && resimRegions_.size() < 4 &&
                std::find(resimRegions_.begin(), resimRegions_.end(), name) == resimRegions_.end()) {
                resimRegions_.push_back(name);
            }
            if (bench_ && failDump_.empty()) TakeFailDump(game, frame, "resim");
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

bool ReplayDriver::StraightFrame(SfvGame& game) {
    if (!Valid(game)) return false;
    if (frame_ > last_) {
        Finish(game);
        return false;
    }
    if (frame_ == 0) Log("replay: frame 0 is driven tick %u", FlowDrivenTicks() + 1);
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

bool ReplayDriver::StressFrame(SfvGame& game) {
    if (!Valid(game)) return false;
    if (bench_) {
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
        const bool fight = FlowRoundInFight();
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
    if (session_->NextFrame() == 0) Log("replay: frame 0 is driven tick %u", FlowDrivenTicks() + 1);
    session_->Update();
    const int f = session_->NextFrame() - 1;
    WatchFrame(game, f);
    MaybeHold(f);
    if (f >= 0 && f != checked_) {
        CheckFrame(game, f);
        checked_ = f;
    }
    return true;
}

bool ReplayDriver::MispredictFrame(SfvGame& game) {
    if (!Valid(game)) return false;
    const int f = frame_;
    if (f > last_ + delay_) {
        Finish(game);
        return false;
    }
    if (f == 0) {
        Log("replay: frame 0 is driven tick %u; P%d's inputs arrive %d frames late", FlowDrivenTicks() + 1, remote_ + 1,
            delay_);
        ring_.assign(delay_ + 2, std::vector<uint8_t>(StateSize(game.Regions())));
        used_.assign(last_ + 1, FrameInputs{});
        sums_.assign(last_ + 1, 0);
    }
    const int arrived = f - delay_;
    if (arrived >= 0 && arrived <= last_) {
        const bool forced = ForcedRollback(arrived);
        if (forced) Log("replay: rollback forced at frame %d (to the state before it)", arrived);
        if (forced || used_[arrived].p[remote_] != rf_.inputs[arrived].p[remote_]) {
            Resimulate(game, arrived, std::min(f, last_ + 1));
        } else {
            ConfirmBySum(arrived);
        }
    }
    if (f <= last_) RunPredicted(game, f, arrived, false);
    WatchFrame(game, f);
    MaybeHold(f);
    frame_++;
    return true;
}

PackedInput ReplayDriver::Guess(int k, int known) const {
    if (k <= known) return rf_.inputs[k].p[remote_];
    return known >= 0 ? rf_.inputs[known].p[remote_] : PackedInput{};
}

void ReplayDriver::RunPredicted(SfvGame& game, int k, int known, bool rollingBack) {
    SaveRegions(game.Regions(), ring_[k % ring_.size()].data());
    FrameInputs in = rf_.inputs[k];
    in.p[remote_] = Guess(k, known);
    if (!rollingBack && g_forcedGuessXor && ForcedRollback(k)) in.p[remote_] ^= g_forcedGuessXor;
    game.Advance(in, rollingBack);
    used_[k] = in;
    std::vector<uint64_t> hashes(CheckRegions(game).size());
    HashRegions(CheckRegions(game), hashes.data());
    sums_[k] = FoldChecksum(hashes.data(), hashes.size());
}

void ReplayDriver::Resimulate(SfvGame& game, int from, int to) {
    LoadRegions(game.Regions(), ring_[from % ring_.size()].data());
    game.OnStateLoaded(from - 1);
    rollbacks_++;
    resimulated_ += to - from;
    for (int k = from; k < to; k++) {
        RunPredicted(game, k, from, true);
        if (k == from) CheckFrame(game, k);
    }
}

void ReplayDriver::ConfirmBySum(int k) {
    if (k >= rf_.FrameCount() || rf_.hashes.empty()) return;
    framesChecked_++;
    if (sums_[k] == rf_.FrameChecksum(k)) return;
    if (firstMismatch_ < 0) firstMismatch_ = k;
    mismatchFrames_++;
    if (logged_++ < 12) Log("replay: frame %d's checksum differs from the recording (ran on the right input)", k);
}

void ReplayDriver::MaybeHold(int f) {
    for (size_t i = 0; i < holds_.size(); i++) {
        if (holds_[i].first != f) continue;
        holdUntil_ = GetTickCount64() + holds_[i].second;
        holds_.erase(holds_.begin() + static_cast<std::ptrdiff_t>(i));
        Log("replay: holding at frame %d", f);
        return;
    }
}

} // namespace replay

} // namespace sfv::mod
