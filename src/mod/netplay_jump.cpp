#include "mod/netplay_internal.h"

namespace sfv::mod::netplay {

void NetplayDriver::ReleaseResults() {
    if (!holdOverResults_.exchange(false)) return;
    InputIsolatePads(false);
    InputBlockPause(false);
    BattleDetachFrameDriver();
}

int NetplayDriver::ViewerSide() const { return role_ == Role::Spectator || host_ ? 0 : 1; }

void NetplayDriver::BeginJump() {
    {
        const bool table = ticket_.after == AfterMatch::Set;
        int played = 0;
        {
            std::lock_guard lock(mutex_);
            played = played_;
        }
        const bool fresh = setKey_ == 0 || (table ? played == 0 : GetState() == State::Lobby);
        if (fresh) {
            setKey_ = matchId_.load();
            setGame_ = 0;
            setWins_[0] = setWins_[1] = 0;
            setRule_ = table && ticket_.rules.firstTo > 0 ? "First to " + std::to_string(ticket_.rules.firstTo) : "";
        }
        setGame_++;
        matchWinner_ = -1;
        matchRounds_.clear();
        matchRoundWins_[0] = matchRoundWins_[1] = 0;
    }
    switchNow_ = false;
    switchesAtJump_ = FlowBattleSceneSwitches();
    ReleaseResults();
    if (nextDelay_ >= 0) options_.inputDelay = static_cast<uint8_t>(nextDelay_.exchange(-1));
    const MatchSetup shown = ShownSetup(setup_, ViewerSide(), BattleSetupLookRules());
    for (int p = 0; p < kNumPlayers; p++) {
        if (shown.players[p].color != setup_.players[p].color) {
            Log("netplay: both players on one look: P%d shows here in color %u (picked %u)", p + 1, shown.players[p].color + 1u,
                setup_.players[p].color + 1u);
        }
    }
    MatchSetup pending = shown;
    {
        std::lock_guard lock(mutex_);
        pending.played = static_cast<uint16_t>(played_);
    }
    BattleSetupSetPending(pending);
    creationsAtJump_ = BattleSetupCreations();
    warmedUp_ = false;
    frozenFrames_ = 0;
    selfLoaded_ = false;
    peerLoaded_ = false;
    startsDifferAt_ = {};
    myChoice_ = Choice::None;
    specWanted_ = 0;
    specPlayed_ = 0;
    {
        std::lock_guard lock(mutex_);
        inbound_.clear();
        for (auto& seat : seats_) seat.choice = Choice::None;
        live_ = NetView{};
    }
    watchingMatch_ = 0;
    liveMatch_ = matchId_.load();
    if (jump_.joinable()) jump_.join();
    matchLive_ = true;
    ReportsSetMatchRunning(true);
    SetState(State::Jumping);
    BattleSetFrameDriver(this);
    jump_ = std::thread([this] {
        ThreadGuard running;
        RunJump();
    });
}

void NetplayDriver::RunJump() {
    FlowSetPendingStage(setup_.stage == kKeepStage ? "" : FlowStageCode(setup_.stage));
    Log("netplay: jumping into the battle (stage %s)", setup_.stage == kKeepStage ? "as prepared" : FlowStageCode(setup_.stage).c_str());
    FlowJumpToBattle();
    const auto direct = Clock::now() + std::chrono::seconds(kDirectJumpSeconds);
    while (!stop_ && !Stopping() && GetState() == State::Jumping && BattleSetupCreations() == creationsAtJump_ &&
           Clock::now() < direct) {
        Sleep(kJumpPollMs);
    }
    if (stop_ || Stopping() || GetState() != State::Jumping || BattleSetupCreations() != creationsAtJump_) return;
    Log("netplay: no battle %d s after the jump", kDirectJumpSeconds);
    SetStatus("could not start the battle (see the log)");
    EndSession("could not start the battle (see the log)");
}

bool NetplayDriver::FrameWhileLoading(SfvGame& game) {
    if (BattleSetupCreations() == creationsAtJump_) return false;

    if (!warmedUp_) {
        game.ForgetBattleObjects();
        warmedUp_ = true;
        objectsReady_ = false;
        game.BeginWarmup();
        SetState(State::Loading);
        InputBlockPause(true);
        Log("netplay: battle created");
    }
    if (!objectsReady_) {
        if (game.WarmupStep()) {
            if (game.WarmupTicks() % kWarmupLogEvery == 0) Log("netplay: still no battle objects after %d ticks", game.WarmupTicks());
            return true;
        }
        objectsReady_ = true;
        InputIsolatePads(true);
        game.exclusiveTicks = true;
        FlowSetRoundDirectorPerTick(true, options_.roundEndRollback);
        BattleSetMatchDrawSeed(setup_.seed);
        game.Recanonicalize();
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(game.Regions(), hashes.data());
        if (g_skewStart.exchange(false) && !hashes.empty()) {
            hashes[0] ^= 1;
            Log("netplay: test: this side's frozen start hashes differently (startskew)");
        }
        frozenChecksum_ = FoldChecksum(hashes.data(), hashes.size());
        KeepFrozenState();
        BattleSetupRead(&loadedSetup_);
        Log("netplay: battle objects ready after %d neutral ticks (%zu regions, checksum %08X); frozen",
            game.ObjectsAt(), hashes.size(), frozenChecksum_);
        game.WatchCameraIfAsked();
        Log("netplay: at the freeze, %s", FlowRoundEndState().c_str());
        Log("netplay: at the freeze, %s%s%s", game.CameraSetupText().c_str(),
            game.CameraWaitTicks() ? (" (" + std::to_string(game.CameraWaitTicks()) + " ticks after the objects)").c_str() : "",
            game.CameraNotStarted() ? "; the camera has not begun its setup" : "");
        game.ArmStartProbe();
        Log("netplay: loaded %s", BattleSetupDescribe(loadedSetup_).c_str());
        BattleDataCheckSoon("match");
        BenchmarkNoteActivity("match");
    }
    if (++frozenFrames_ == kSettleFrames) {
        selfLoaded_ = true;
        Log("netplay: loaded; waiting for the %s", role_ == Role::Spectator ? "host" : "other player");
    }
    SetStatus(peerLoaded_ ? "starting" : selfLoaded_ ? "waiting for the other player"
                                                     : "loading");
    if (selfLoaded_ && peerLoaded_ && !session_ && !spectate_) {
        MatchSetup peer;
        uint32_t peerChecksum;
        {
            std::lock_guard lock(mutex_);
            peer = peerLoadedSetup_;
            peerChecksum = peerChecksum_;
        }
        const LookRules rules = BattleSetupLookRules();
        const std::string mismatch =
            SetupMismatch(RealPicks(loadedSetup_, setup_, ViewerSide(), rules), RealPicks(peer, setup_, host_ ? 1 : 0, rules));
        if (!mismatch.empty()) {
            Log("netplay: the battles differ (%s); here %s | there %s", mismatch.c_str(),
                BattleSetupDescribe(loadedSetup_).c_str(), BattleSetupDescribe(peer).c_str());
            EndSession("battles differ: " + mismatch);
            return true;
        }
        if (frozenChecksum_ != peerChecksum) {
            if (startsDifferAt_ == Clock::time_point{}) {
                startsDifferAt_ = Clock::now();
                SaveDifferentStart(game, peerChecksum);
            }
            if (Clock::now() - startsDifferAt_ >= kDifferentStartGrace) EndSession("battles differ: starting state");
            return true;
        }
        Log("netplay: frozen state checksums %08X here and there", frozenChecksum_);
        if (role_ == Role::Spectator) StartWatching(game);
        else StartSession(game);
    }
    return true;
}

void NetplayDriver::StartSession(SfvGame& game) {
    NetplayConfig nc;
    nc.localPlayer = host_ ? 0 : 1;
    nc.remotePeer = peer_;
    nc.matchId = matchId_;
    nc.setup = setup_;
    nc.modVersion = modVersion_;
    nc.inputDelay = options_.inputDelay;
    nc.predictionWindow = options_.predictionWindow;
    nc.dumpEvery = options_.dumpEvery;
    nc.skipUnloadableSaves = options_.skipSaves;
    nc.verifyLoads = options_.verifyLoads;
    nc.verifyLoadsEvery = options_.verifyLoadsEvery;
    nc.hashOnConfirm = options_.hashOnConfirm;
    nc.maxDumps = static_cast<size_t>(std::max(0, options_.maxDumps));
    nc.log = [](const std::string& m) { Log("netplay: %s", m.c_str()); };
    game.localPadSlot = 0;
    game.bothPadsLocal = false;
    game.localInput = &InputMappedLocal;
    session_ = std::make_unique<NetplaySession>(game, *gekko_, nc);
    game.CaptureStartProbe("at the session's start");
    session_->onDesync = [this, &game](const DesyncInfo& d, const ReplayFile& rf) {
        BenchmarkNoteActivity("desync");
        desyncFrame_ = d.frame;
        if (d.frame <= kStartProbeFrames) game.LogStartProbe(("a desync at frame " + std::to_string(d.frame)).c_str());
        const std::string path = SaveReport(rf, "desync", true);
        std::lock_guard lock(mutex_);
        desyncReport_ = path;
    };
    std::string error;
    if (!session_->Start(&error)) {
        Log("netplay: cannot start the session: %s", error.c_str());
        session_.reset();
        EndSession("could not start");
        return;
    }
    layoutAtStart_ = game.LayoutVersion();
    sessionSyncing_ = true;
    framesRun_ = 0;
    waitFrames_ = 0;
    waitCooldown_ = 0;
    desyncFrame_ = -1;
    recentRollbacks_ = {};
    rollbacksSeen_ = 0;
    {
        std::lock_guard lock(mutex_);
        desyncReport_.clear();
    }
    streamed_ = 0;
    predictionWindow_ = options_.predictionWindow;
    inputDelay_ = nc.inputDelay;
    roundCallFrame_ = -1;
    loweredEarly_ = false;
    sawFight_ = false;
    pace_ = {};
    PaceTakeStats();
    UpdatePredictionWindow();
    winsKnown_ = game_.MatchWins(winsAtStart_[0], winsAtStart_[1]);
    SetState(State::Running);
    {
        core::LinkStats link;
        {
            std::lock_guard lock(mutex_);
            link = core::MeasureLink(linkRtts_);
        }
        if (link.samples >= core::kSuggestSamples) {
            const int suggested = core::SuggestedDelay(link.pingMs, link.jitterMs);
            Log("netplay: link %u ms, jitter %u ms (%zu heartbeats): delay %d suggested, %u chosen%s", link.pingMs,
                link.jitterMs, link.samples, suggested, nc.inputDelay,
                nc.inputDelay < suggested ? ": rollbacks deeper than 2 frames are likely" : "");
        }
    }
    Log("netplay: session started as P%d, delay %u, prediction window %u", nc.localPlayer + 1, nc.inputDelay,
        nc.predictionWindow);
}

void NetplayDriver::StartWatching(SfvGame& game) {
    SpectateConfig sc;
    sc.matchId = matchId_;
    sc.setup = setup_;
    sc.modVersion = modVersion_;
    if (ticket_.after == AfterMatch::Set) sc.delayFrames = kRelayWatchDelayFrames;
    sc.log = [](const std::string& m) { Log("netplay: %s", m.c_str()); };
    game.localInput = nullptr;
    spectate_ = std::make_unique<SpectatorPlayback>(game, sc);
    std::string error;
    if (!spectate_->Start(&error)) {
        Log("netplay: cannot watch: %s", error.c_str());
        spectate_.reset();
        EndSession("could not start");
        return;
    }
    layoutAtStart_ = game.LayoutVersion();
    framesRun_ = 0;
    waitFrames_ = 0;
    waitCooldown_ = 0;
    desyncFrame_ = -1;
    recentRollbacks_ = {};
    rollbacksSeen_ = 0;
    {
        std::lock_guard lock(mutex_);
        desyncReport_.clear();
    }
    watchingMatch_ = matchId_.load();
    lastWatched_ = watchingMatch_.load();
    SetState(State::Running);
    Log("netplay: watching match %s", Hex64(matchId_).c_str());
    if (pickWaits_.exchange(false)) ChooseMatch(pickedMatch_);
}

void NetplayDriver::KeepFrozenState() {
    if (!host_) return;
    std::lock_guard lock(mutex_);
    KeepMatchLocked(matchId_);
    KeptMatch& k = kept_[matchId_];
    k.setup = setup_;
    k.checksum = frozenChecksum_;
}

void NetplayDriver::SaveDifferentStart(SfvGame& game, uint32_t peerChecksum) {
    Log("netplay: frozen state checksums %08X here, %08X there: the starts differ; not starting", frozenChecksum_,
        peerChecksum);
    const auto& regions = game.Regions();
    ReplayFile rf;
    rf.modVersion = modVersion_;
    rf.gameBuild = game.BuildId();
    rf.matchId = liveMatch_;
    rf.localPlayer = role_ == Role::Spectator ? -1 : host_ ? 0
                                                           : 1;
    rf.createdUnix = static_cast<uint64_t>(std::time(nullptr));
    rf.inputDelay = options_.inputDelay;
    rf.predictionWindow = options_.predictionWindow;
    rf.setup = setup_;
    rf.regions = DescribeRegions(regions);
    rf.initialHashes.resize(regions.size());
    HashRegions(regions, rf.initialHashes.data());
    StateDump& initial = rf.dumps.emplace_back();
    initial.frame = -1;
    initial.label = "initial";
    initial.bytes.resize(StateSize(regions));
    SaveRegions(regions, initial.bytes.data());
    SaveReport(rf, "desync");
    const std::string path = SaveReport(rf, "match");
    char summary[160];
    std::snprintf(summary, sizeof(summary), "not started: frozen state checksum %08X here, %08X there (%zu regions)",
                  frozenChecksum_, peerChecksum, regions.size());
    WriteMatchLog(path, rf, summary);
}

} // namespace sfv::mod::netplay
