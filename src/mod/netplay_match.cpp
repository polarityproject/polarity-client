#include "mod/netplay_internal.h"

namespace sfv::mod::netplay {

bool NetplayDriver::FrameWhileRunning(SfvGame& game) {
    const State s = GetState();
    game.RefreshRegions();
    if (game.LayoutVersion() != layoutAtStart_ || game.Regions().empty()) {
        FinishMatch(game);
        if (s == State::Running) EndSession("match over");
        return false;
    }
    if (s == State::Running && (peerLeft_ || session_->Phase() == NetplayPhase::Ended)) {
        FinishMatch(game);
        EndSession("the other player left");
        return true;
    }

    if (!options_.inputLoop.empty() && s == State::Running) PlayInputLoop(session_->NextFrame());
    else if (options_.mashSeed && s == State::Running) Mash(session_->NextFrame());
    UpdatePredictionWindow();

    const float ahead = session_->FramesAhead();
    PaceLead(ahead);
    framesRun_++;
    if (waitCooldown_ > 0) waitCooldown_--;
    if (waitFrames_ == 0 && waitCooldown_ == 0 && predictionWindow_ > 0 && ahead >= 8.0f) {
        waitFrames_ = 1;
        waitCooldown_ = 10;
    }
    const bool wait = waitFrames_ > 0;
    if (wait) waitFrames_--;
    LogPace(ahead, wait);
    if (!wait && !FlowResultsHeld()) session_->Update();
    sessionSyncing_ = session_->Phase() == NetplayPhase::Syncing;

    if (s == State::Running && FlowMatchDecided()) {
        uint32_t now[2] = {};
        const bool counted = winsKnown_ && game_.MatchWins(now[0], now[1]);
        const int winner = !counted ? -1 : now[0] > winsAtStart_[0] ? 0
                                       : now[1] > winsAtStart_[1]   ? 1
                                                                    : -1;
        {
            std::lock_guard lock(mutex_);
            if (host_) {
                played_++;
                if (winner >= 0) {
                    seats_[winner].wins++;
                    streaks_[winner]++;
                    streaks_[1 - winner] = 0;
                }
            }
        }
        matchWinner_ = winner;
        if (winner >= 0) setWins_[winner]++;
        FlowReadRoundResults(&matchRounds_, matchRoundWins_);
        if (winner >= 0) Log("netplay: P%d won the match", winner + 1);
        else Log("netplay: the match's winner is not known (win counts %s)", counted ? "unchanged" : "not found");
        if (host_) {
            std::lock_guard lock(mutex_);
            const auto k = kept_.find(liveMatch_);
            if (k != kept_.end()) {
                k->second.finished = true;
                k->second.winner = static_cast<int8_t>(winner);
            }
        }
        decidedAt_ = Clock::now();
        SetState(State::PostMatch);
        FlowSetHoldResults(true);
        if (options_.mashSeed || !options_.inputLoop.empty()) InputScriptedPad(0, true, 0);
        Log("netplay: the match is decided at frame %d", session_->NextFrame());
    }
    if (host_) Stream();
    Publish(ahead);
    return true;
}

void NetplayDriver::Stream() {
    const int through = session_->ConfirmedThrough();
    if (through < streamed_) return;
    std::lock_guard lock(mutex_);
    KeepMatchLocked(liveMatch_);
    std::vector<lobby::FrameRecord>& stream = streams_[liveMatch_];
    for (; streamed_ <= through; streamed_++) {
        lobby::FrameRecord f;
        if (!session_->ConfirmedFrame(streamed_, &f.inputs, &f.checksum)) break;
        stream.push_back(f);
    }
}

void NetplayDriver::KeepMatchLocked(uint64_t id) {
    if (std::find(streamOrder_.begin(), streamOrder_.end(), id) != streamOrder_.end()) return;
    streamOrder_.push_back(id);
    while (streamOrder_.size() > lobby::kKeptMatches) {
        streams_.erase(streamOrder_.front());
        kept_.erase(streamOrder_.front());
        streamOrder_.erase(streamOrder_.begin());
    }
}

lobby::Message NetplayDriver::MatchListLocked() const {
    lobby::Message m;
    m.type = lobby::Type::Matches;
    for (uint64_t id : streamOrder_) {
        const auto k = kept_.find(id);
        if (k == kept_.end()) continue;
        lobby::MatchEntry e;
        e.id = id;
        e.setup = k->second.setup;
        const auto s = streams_.find(id);
        e.frames = s == streams_.end() ? 0 : static_cast<uint32_t>(s->second.size());
        e.finished = k->second.finished;
        e.winner = k->second.winner;
        m.matches.push_back(e);
    }
    return m;
}

std::optional<lobby::Message> NetplayDriver::KeptLoaded(uint64_t id) {
    std::lock_guard lock(mutex_);
    const auto k = kept_.find(id);
    if (k == kept_.end()) return std::nullopt;
    lobby::Message loaded;
    loaded.type = lobby::Type::Loaded;
    loaded.matchId = id;
    loaded.setup = k->second.setup;
    loaded.stateChecksum = k->second.checksum;
    return loaded;
}

void NetplayDriver::WatchMatch(const lobby::MatchEntry& e) {
    {
        std::lock_guard lock(mutex_);
        setup_ = e.setup;
    }
    matchId_ = e.id;
    options_.predictionWindow = kPredictionWindow;
    Log("netplay: to watch match %s; %s", Hex64(e.id).c_str(), DescribeSetup(e.setup).c_str());
    next_ = Next::Jump;
}

bool NetplayDriver::WatchNext() {
    lobby::MatchEntry next;
    {
        std::lock_guard lock(mutex_);
        if (matchList_.empty()) return false;
        const uint64_t last = lastWatched_;
        const auto it = std::find_if(matchList_.begin(), matchList_.end(), [&](const lobby::MatchEntry& e) { return e.id == last; });
        if (last && it != matchList_.end()) {
            if (it + 1 == matchList_.end()) return false;
            next = *(it + 1);
        } else {
            next = matchList_.back();
        }
    }
    WatchMatch(next);
    return true;
}

void NetplayDriver::Publish(float ahead) {
    std::string status;
    std::unique_lock lock(mutex_);
    live_.syncing = sessionSyncing_;
    live_.frame = session_->NextFrame();
    live_.framesAhead = ahead;
    live_.desyncFrame = desyncFrame_;
    live_.inputDelay = options_.inputDelay;
    live_.predictionWindow = options_.predictionWindow;
    const auto rb = session_->RollbackStats();
    recentRollbacks_[framesRun_ % recentRollbacks_.size()] =
        rb.total_count != rollbacksSeen_ ? static_cast<uint8_t>(std::min(rb.last_frames, 255u)) : 0;
    rollbacksSeen_ = rb.total_count;
    if (framesRun_ % 10 == 0) {
        const auto net = session_->NetworkStats();
        live_.pingMs = net.last_ping;
        live_.avgPingMs = net.avg_ping;
        live_.jitterMs = net.jitter;
        live_.rollbacks = rb.total_count;
        live_.longestRollback = rb.max_frames;
        live_.recentRollback = *std::max_element(recentRollbacks_.begin(), recentRollbacks_.end());
    }
    if (framesRun_ % 30 == 0) {
        const auto net = session_->NetworkStats();
        char buf[200];
        std::snprintf(buf, sizeof(buf), "P%d | frame %d | ping %u ms | rollbacks %u (max %u) | ahead %.1f%s",
                      host_ ? 1 : 2, session_->NextFrame(), net.last_ping, rb.total_count, rb.max_frames, ahead,
                      desyncFrame_ >= 0 ? (" | DESYNC at " + std::to_string(desyncFrame_)).c_str() : "");
        status = sessionSyncing_ ? "connecting to the other player" : buf;
    }
    lock.unlock();
    if (!status.empty()) SetStatus(status);
}

bool NetplayDriver::FrameWhileWatching(SfvGame& game) {
    game.RefreshRegions();
    if (game.LayoutVersion() != layoutAtStart_ || game.Regions().empty()) {
        FinishMatch(game);
        return false;
    }
    std::vector<lobby::Message> in;
    {
        std::lock_guard lock(mutex_);
        in.swap(inbound_);
    }
    for (const lobby::Message& m : in) {
        std::vector<FrameInputs> inputs;
        std::vector<uint32_t> sums;
        for (const auto& f : m.frames) {
            inputs.push_back(f.inputs);
            sums.push_back(f.checksum);
        }
        spectate_->Add(static_cast<int>(m.first), inputs, sums);
    }
    if (!FlowResultsHeld()) spectate_->Update();
    specWanted_ = spectate_->NextWanted();
    specPlayed_ = spectate_->NextFrame();
    framesRun_++;
    if (spectate_->DesyncFrame() >= 0 && desyncFrame_ < 0) {
        desyncFrame_ = spectate_->DesyncFrame();
        const std::string path = SaveReport(spectate_->Recording(), "spectator-desync", true);
        std::lock_guard lock(mutex_);
        desyncReport_ = path;
    }
    if (GetState() == State::Running && FlowMatchDecided()) {
        SetState(State::PostMatch);
        FlowSetHoldResults(true);
        Log("netplay: the match is decided at frame %d", spectate_->NextFrame());
        if ((next_ == Next::None || next_ == Next::Lobby) && peerLeftRoom_) {
            const std::string over = SetOverText();
            EndSession(!over.empty() ? over : "the host left");
        } else if (next_ == Next::None || next_ == Next::Lobby) WatchNext();
    }
    {
        std::lock_guard lock(mutex_);
        live_.frame = spectate_->NextFrame();
        live_.behind = spectate_->Buffered();
        live_.desyncFrame = desyncFrame_;
    }
    if (framesRun_ % 30 == 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "watching | frame %d | %d frames in hand%s", spectate_->NextFrame(),
                      spectate_->Buffered(),
                      desyncFrame_ >= 0 ? (" | DESYNC at " + std::to_string(desyncFrame_)).c_str() : "");
        SetStatus(buf);
    }
    return true;
}

void NetplayDriver::UpdatePredictionWindow() {
    if (const bool inFight = FlowRoundInFight(); inFight != loggedInFight_) {
        loggedInFight_ = inFight;
        Log(inFight ? "netplay: fight from frame %d" : "netplay: fight over at frame %d", session_->NextFrame());
    }
    const bool inFight = FlowRoundInFight();
    if (inFight) sawFight_ = true;
    const bool transition = options_.lockstepTransitions && !inFight && (sawFight_ || FlowRoundCallRunning());
    const bool fight = (!options_.lockstepPresentation || inFight) && !transition;
    const uint8_t window = fight ? options_.predictionWindow : 0;
    const int frame = session_->NextFrame();
    if (FlowRoundCallRunning()) {
        if (roundCallFrame_ < 0) roundCallFrame_ = frame;
    } else if (roundCallFrame_ >= 0) {
        if (fight) Log("netplay: the round call took %d frames", frame - roundCallFrame_);
        roundCallFrame_ = -1;
        loweredEarly_ = false;
    }
    uint8_t delay = options_.inputDelay;
    if (!fight && options_.lockstepDelay) delay = std::max(window == predictionWindow_ ? inputDelay_ : delay, LockstepDelay());
    if (!fight && roundCallFrame_ >= 0 && delay > options_.inputDelay &&
        (loweredEarly_ || frame >= roundCallFrame_ + kRoundCallFrames - delay - kEarlyMargin)) {
        if (!loweredEarly_) {
            Log("netplay: input delay back to %u before FIGHT (round call frame %d, %d to go)", options_.inputDelay,
                frame - roundCallFrame_, roundCallFrame_ + kRoundCallFrames - frame);
        }
        loweredEarly_ = true;
        delay = options_.inputDelay;
    }
    if (delay != inputDelay_) {
        session_->SetInputDelay(delay);
        Log("netplay: input delay %u from frame %d", delay, session_->NextFrame());
        inputDelay_ = delay;
    }
    if (window == predictionWindow_) return;
    session_->SetPredictionWindow(window);
    Log("netplay: prediction window %u from frame %d (%s)", window, session_->NextFrame(),
        window ? "fight" : transition ? "round transition"
                                      : "presentation");
    predictionWindow_ = window;
}

void NetplayDriver::LogPace(float ahead, bool wait) {
    pace_.waits += wait;
    pace_.aheadSum += ahead;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(InputRealForegroundWindow(), &foregroundPid);
    pace_.focused += foregroundPid == GetCurrentProcessId();
    if (++pace_.rendered < kPaceLogFrames) return;
    const auto now = std::chrono::steady_clock::now();
    const int frame = session_->NextFrame();
    if (pace_.since.time_since_epoch().count() != 0) {
        const double s = std::chrono::duration<double>(now - pace_.since).count();
        const GekkoNetworkStats net = session_->NetworkStats();
        const PaceStats p = TakePaceStats();
        Log("netplay: pace %.1f frames/s (%d frames in %.1f s), %d of %d rendered frames waited, ahead %.2f, "
            "stretched %d frames (%.1f %% on average), %d late, %d resets, %.1f ms waited per frame, "
            "delay %u, window %u, ping %.0f ms, jitter %.0f ms, focused %d",
            (frame - pace_.frame) / s, frame - pace_.frame, s, pace_.waits, pace_.rendered,
            pace_.aheadSum / pace_.rendered, p.stretched, p.stretched ? p.stretchSum / p.stretched * 100 : 0.0, p.late,
            p.resets, p.paced ? p.waitedMs / p.paced : 0.0, inputDelay_, predictionWindow_, net.avg_ping, net.jitter,
            pace_.focused);
    }
    pace_ = {};
    pace_.since = now;
    pace_.frame = frame;
}

uint8_t NetplayDriver::LockstepDelay() {
    constexpr int kMaxLockstepDelay = 12;
    const GekkoNetworkStats net = session_->NetworkStats();
    const float rttMs = std::max(net.avg_ping, static_cast<float>(linkPingMs_.load()));
    const int frames = static_cast<int>(std::ceil((rttMs / 2 + net.jitter) * kFramesPerSecond / 1000)) + 1;
    return static_cast<uint8_t>(std::clamp(frames, static_cast<int>(options_.inputDelay), kMaxLockstepDelay));
}

void NetplayDriver::PlayInputLoop(int frame) {
    int period = 0;
    for (const auto& [b, n] : options_.inputLoop) period += n;
    if (period <= 0) return;
    const int at = frame - options_.mashFrom;
    uint16_t buttons = 0;
    if (at >= 0) {
        int pos = at % period;
        if (pos == 0 && frame != loopLoggedAt_ && loopsLogged_ < kLoopsLogged) {
            loopLoggedAt_ = frame;
            Log("netplay: input loop %d starts at frame %d", ++loopsLogged_, frame);
        }
        for (const auto& [b, n] : options_.inputLoop) {
            if (pos < n) {
                buttons = b;
                break;
            }
            pos -= n;
        }
    }
    InputScriptedPad(0, true, buttons, 0);
}

void NetplayDriver::Mash(int frame) {
    const uint64_t key[2] = {options_.mashSeed, static_cast<uint64_t>(frame / 5)};
    const uint64_t h = Hash64(key, sizeof(key));
    uint16_t b = 0;
    switch (h % 8) {
        case 0:
        case 1: b |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 2:
        case 3: b |= XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 4: b |= XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 5: b |= XINPUT_GAMEPAD_DPAD_UP; break;
        default: break;
    }
    static const uint16_t kAttack[] = {XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                       XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B};
    if ((h >> 8) % 3 == 0) b |= kAttack[(h >> 16) % 5];
    uint8_t rt = 0;
    switch ((h >> 24) % 16) {
        case 0: b = XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B; break;
        case 1:
            b = XINPUT_GAMEPAD_RIGHT_SHOULDER;
            rt = 255;
            break;
        default: break;
    }
    if (frame < options_.mashFrom) b = 0, rt = 0;
    InputScriptedPad(0, true, b, rt);
}

void NetplayDriver::FinishMatch(SfvGame& game, bool keepSetup) {
    InputBlockPause(false);
    InputIsolatePads(false);
    game.localInput = nullptr;
    game.exclusiveTicks = false;
    FlowSetRoundDirectorPerTick(false, false);
    InputScriptedPad(0, false);
    if (!keepSetup) ClearPendingSetup();
    if (session_) {
        const auto rf = WithSummary(session_->Recording(), false);
        const auto rb = session_->RollbackStats();
        const std::string path = SaveReport(rf, "match");
        SaveToBattleLog(rf);
        char summary[160];
        std::snprintf(summary, sizeof(summary), "%d frames, %u rollbacks (max %u), %d desync events",
                      session_->NextFrame(), rb.total_count, rb.max_frames, session_->DesyncCount());
        Log("netplay: match %s: %s; recording %s", Hex64(liveMatch_).substr(0, 8).c_str(), summary, path.c_str());
        WriteMatchLog(path, rf, summary);
        session_.reset();
    }
    if (spectate_) {
        const ReplayFile rf = spectate_->Recording();
        const std::string path = SaveReport(rf, "watched");
        char summary[160];
        std::snprintf(summary, sizeof(summary), "watched %d frames (%d stalls, %d frames held, longest wait for frames %d ms, %d desyncs)",
                      spectate_->NextFrame(), spectate_->Stalls(), spectate_->Held(), spectate_->LongestGapMs(), spectate_->DesyncCount());
        Log("netplay: %s; recording %s", summary, path.c_str());
        WriteMatchLog(path, rf, summary);
        spectate_.reset();
    }
    watchingMatch_ = 0;
    SetFrameDriver(nullptr);
    matchLive_ = false;
    SetMatchRunning(false);
}

ReplayFile NetplayDriver::WithSummary(ReplayFile rf, bool leftEarly) {
    if (matchWinner_ >= 0) leftEarly = false;
    MatchSummary& s = rf.summary;
    s.present = true;
    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < kNumPlayers; i++) s.names[i] = seats_[i].name;
    }
    s.winner = static_cast<int8_t>(leftEarly ? -1 : matchWinner_);
    s.ending = leftEarly || matchWinner_ < 0 ? 1 : 0;
    s.rounds = matchRounds_;
    const std::vector<RoundResult> fights = RoundsFromFrameFlags(rf.frameFlags);
    if (s.rounds.size() < fights.size()) s.rounds.resize(fights.size());
    for (size_t i = 0; i < fights.size(); i++) {
        s.rounds[i].fightFrame = fights[i].fightFrame;
        s.rounds[i].endFrame = fights[i].endFrame;
    }
    for (int i = 0; i < kNumPlayers; i++) {
        s.roundWins[i] = matchRoundWins_[i];
        s.setWins[i] = setWins_[i];
    }
    s.setKey = setKey_;
    s.setGame = setGame_;
    s.setRule = setRule_;
    s.seconds = static_cast<uint32_t>(std::max(0, rf.FrameCount()) / 60);
    return rf;
}

void NetplayDriver::SaveToBattleLog(const ReplayFile& rf) {
    if (role_ == Role::Spectator || rf.FrameCount() < 60) return;
    const std::wstring dir = ReplaysDir();
    EnsureDirectory(dir);
    const std::time_t t = static_cast<std::time_t>(rf.createdUnix ? rf.createdUnix : std::time(nullptr));
    std::tm tm;
    localtime_s(&tm, &t);
    char when[32];
    std::snprintf(when, sizeof(when), "%04d-%02d-%02d_%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    std::string name = when;
    for (int i = 0; i < kNumPlayers; i++) {
        name += (i == 0 ? "_" : "_vs_") + FileSafe(rf.summary.names[i], 20, i == 0 ? "P1" : "P2") + "-" +
                FileSafe(UnpackCode(rf.setup.players[i].character), 8, "unknown");
    }
    name += "_" + Hex64(rf.matchId).substr(0, 8) + ".sfvr";
    const std::string path = Utf8Path(dir) + "\\" + name;
    std::thread([path, file = rf] {
        ThreadGuard running;
        std::string error;
        if (SaveReplay(file, path, &error, ReplayDetail::Compact)) Log("netplay: battle log %s", path.c_str());
        else Log("netplay: cannot save the battle log copy %s: %s", path.c_str(), error.c_str());
    }).detach();
}

std::string NetplayDriver::SaveReport(const ReplayFile& rf, const char* kind, bool background) {
    EnsureDirectory(reportDir_);
    std::string names[kNumPlayers];
    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < kNumPlayers; i++) names[i] = seats_[i].name;
    }
    const std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);
    char when[32];
    std::snprintf(when, sizeof(when), "%04d-%02d-%02d_%02d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string name = when;
    for (int i = 0; i < kNumPlayers; i++) {
        const std::string player = FileSafe(names[i], 20, i == 0 ? "P1" : "P2");
        const std::string character = FileSafe(UnpackCode(rf.setup.players[i].character), 8, "unknown");
        name += (i == 0 ? "_" : "_vs_") + player + "-" + character;
    }
    name += "_" + Hex64(liveMatch_).substr(0, 8) + (role_ == Role::Spectator ? "_spec" : host_ ? "_p1"
                                                                                               : "_p2");
    if (std::strstr(kind, "desync")) name += "_desync";
    const std::string path = Utf8Path(reportDir_) + "\\" + name + ".sfvr";
    if (!ReportUploadsOn()) KeepReportLocal(reportDir_, name + ".sfvr");
    const ReplayDetail detail = options_.fullRecordings ? ReplayDetail::Full : ReplayDetail::Compact;
    auto save = [path, detail, dir = reportDir_](const ReplayFile& file) {
        std::string error;
        if (!SaveReplay(file, path, &error, detail)) Log("netplay: cannot save %s: %s", path.c_str(), error.c_str());
        else Log("netplay: saved %s", path.c_str());
        PruneOldest(dir, L"*.sfvr", kKeptRecordings);
    };
    if (background) {
        std::thread([save, file = rf] {
            ThreadGuard running;
            save(file);
        }).detach();
    } else {
        save(rf);
    }
    return path;
}

void NetplayDriver::NotePeerReports(bool reports) {
    if (peerReports_.exchange(reports ? 1 : 0) == (reports ? 1 : 0)) return;
    Log("netplay: the other side %s desync reports", reports ? "sends" : "does not send");
}

void NetplayDriver::WriteMatchLog(const std::string& recording, const ReplayFile& rf, const std::string& summary) {
    std::string names[kNumPlayers];
    std::string server;
    NetView::Path path;
    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < kNumPlayers; i++) names[i] = seats_[i].name;
        server = options_.udp ? "direct UDP" : options_.server;
        path = linkPath_;
    }
    const std::string logPath = recording.substr(0, recording.rfind('.')) + ".log";
    if (!ReportUploadsOn()) KeepReportLocal(reportDir_, logPath.substr(logPath.find_last_of('\\') + 1));
    FILE* f = OpenFile(logPath, "w");
    const std::vector<std::string> lines = LogCaptureTake();
    if (!f) {
        Log("netplay: cannot write %s", logPath.c_str());
        return;
    }
    const std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);
    std::fprintf(f, "Polarity %s match log\n", modVersion_.c_str());
    std::fprintf(f, "match %s, saved %04d-%02d-%02d %02d:%02d:%02d\n", Hex64(liveMatch_).substr(0, 8).c_str(),
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    for (int i = 0; i < kNumPlayers; i++) {
        const PlayerSetup& p = rf.setup.players[i];
        std::fprintf(f, "P%d %s: %s, costume %u, color %u, V-Skill %u, V-Trigger %u\n", i + 1,
                     names[i].empty() ? "(no name)" : names[i].c_str(), UnpackCode(p.character).c_str(), p.costume,
                     p.color + 1, p.vskill + 1, p.vtrigger + 1);
    }
    std::fprintf(f, "this side: %s\n", role_ == Role::Spectator ? "spectator" : host_ ? "P1 (host)"
                                                                                      : "P2");
    std::fprintf(f, "server %s, link %s, input delay %u\n", server.c_str(),
                 path == NetView::Path::Relayed ? "relayed (TURN)" : path == NetView::Path::Direct ? "direct"
                                                                                                   : "unknown",
                 options_.inputDelay);
    std::fprintf(f, "%s\n", summary.c_str());
    std::fprintf(f, "game files: %s\n", GameFilesText().c_str());
    std::fprintf(f, "mods: %s\n", ScanModPaks().Text().c_str());
    std::fprintf(f, "battle data: %s\n", LastDataCheck().Text().c_str());
    std::fprintf(f, "character versions: %s\n", FlowCharaVersionsText().c_str());
    const int peerReports = peerReports_;
    std::fprintf(f, "desync reports: %s from this side, %s from the other side\n", ReportUploadsOn() ? "sent" : "not sent",
                 peerReports > 0 ? "sent" : peerReports == 0 ? "not sent"
                                                             : "not known if sent");
    std::fprintf(f, "recording %s\n\n", recording.substr(recording.find_last_of('\\') + 1).c_str());
    for (const std::string& line : lines) std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
    PruneOldest(reportDir_, L"*.log", kKeptMatchLogs);
    SendReadyReports();
}

} // namespace sfv::mod::netplay
