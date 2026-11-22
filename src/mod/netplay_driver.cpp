#include "mod/netplay_internal.h"

namespace sfv::mod::netplay {

void SetResult(const std::string& text) {
    std::lock_guard lock(g_resultMutex);
    g_lastResult = text;
}

std::string LastResult() {
    std::lock_guard lock(g_resultMutex);
    return g_lastResult;
}

std::string Hex64(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

void CopyToClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
        GlobalUnlock(mem);
        SetClipboardData(CF_TEXT, mem);
    }
    CloseClipboard();
}

std::string SteamPersonaName() {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return "";
    using Getter = void* (*)();
    const auto friends = reinterpret_cast<Getter>(GetProcAddress(steam, "SteamFriends"));
    void* iface = friends ? friends() : nullptr;
    if (!iface) return "";
    using GetPersonaName = const char* (*)(void*);
    const char* name = (*reinterpret_cast<GetPersonaName**>(iface))[0](iface);
    return name ? name : "";
}

void PressPad(int pad, uint16_t buttons, int holdMs, int gapMs) {
    InputScriptedPad(pad, true, buttons);
    Sleep(holdMs);
    InputScriptedPad(pad, true, 0);
    Sleep(gapMs);
}

bool LeaveBattleForStageSelect(SfvGame& game, const std::atomic<bool>& stop) {
    constexpr int kPauseTries = 8;
    constexpr int kPressMs = 90, kPauseWaitMs = 1200, kMenuStepMs = 900, kResultsWaitMs = 6000, kConfirmMs = 1500, kStageSelectMs = 2500;
    for (int tries = 0; !stop && !Stopping() && game.BattleTicking(); tries++) {
        if (tries == kPauseTries) {
            Log("netplay: no pause menu; leaving through the results menu");
            PressPad(0, XINPUT_GAMEPAD_DPAD_DOWN);
            PressPad(0, XINPUT_GAMEPAD_A, kPressMs, kMenuStepMs);
            PressPad(1, XINPUT_GAMEPAD_DPAD_DOWN);
            PressPad(1, XINPUT_GAMEPAD_A, kPressMs, kResultsWaitMs);
            InputScriptedPad(1, false);
            if (game.BattleTicking()) {
                Log("netplay: the battle does not pause");
                return false;
            }
            return true;
        }
        PressPad(0, XINPUT_GAMEPAD_START, kPressMs, kPauseWaitMs);
    }
    if (stop || Stopping()) return false;
    PressPad(0, XINPUT_GAMEPAD_DPAD_DOWN);
    PressPad(0, XINPUT_GAMEPAD_DPAD_DOWN);
    PressPad(0, XINPUT_GAMEPAD_A, kPressMs, kConfirmMs);
    PressPad(0, XINPUT_GAMEPAD_A, kPressMs, kStageSelectMs);
    return true;
}

std::string SetupMismatch(const MatchSetup& a, const MatchSetup& b) {
    std::string out;
    auto add = [&](const std::string& what) { out += (out.empty() ? "" : ", ") + what; };
    for (int p = 0; p < 2; p++) {
        const PlayerSetup &x = a.players[p], &y = b.players[p];
        const std::string who = "P" + std::to_string(p + 1) + " ";
        if (x.character != y.character) add(who + "character " + UnpackCode(x.character) + " vs " + UnpackCode(y.character));
        if (x.costume != y.costume) add(who + "costume");
        if (x.color != y.color) add(who + "color");
        if (x.vskill != y.vskill) add(who + "V-Skill");
        if (x.vtrigger != y.vtrigger) add(who + "V-Trigger");
        if (x.comment != y.comment) add(who + "comment");
        if (x.version != y.version) add(who + "version " + std::to_string(x.version) + " vs " + std::to_string(y.version));
    }
    return out;
}

const char* RejectText(lobby::RejectReason r) {
    switch (r) {
        case lobby::RejectReason::WrongCode: return "wrong code";
        case lobby::RejectReason::BuildMismatch: return "different mod build";
        case lobby::RejectReason::Busy: return "the session is full";
        case lobby::RejectReason::ProtocolMismatch: return "different protocol";
        case lobby::RejectReason::NoSpectators: return "the host does not allow spectators";
        case lobby::RejectReason::ViaKeeper: return "table sets are watched from the room";
    }
    return "?";
}

void ReturnToMenuOnceBattleRuns(SfvGame& game, uint64_t switchesAtJump, NoBattle none, int framesLeft) {
    FlowRunOnGameThreadAfter(10, [&game, switchesAtJump, none, framesLeft] {
        if (FlowBattleSceneSwitches() > switchesAtJump + 1 || BattleCurrentFrameDriver()) return;
        if (FlowBattleSceneSwitches() != switchesAtJump && game.BattleRunning()) {
            Log("netplay: the battle of the session that ended during its jump runs: back to the menu");
            BattleSetupClearPending();
            FlowReturnToMainMenu(1);
            return;
        }
        if (framesLeft > 10) {
            ReturnToMenuOnceBattleRuns(game, switchesAtJump, none, framesLeft - 10);
            return;
        }
        BattleSetupClearPending();
        if (none == NoBattle::Menu) {
            Log("netplay: no battle after the ended session's jump: back to the menu");
            FlowReturnToMainMenu(1);
        }
    });
}

NetplayDriver::NetplayDriver(SfvGame& game, std::wstring reportDir, std::string modVersion)
    : game_(game), reportDir_(std::move(reportDir)), modVersion_(std::move(modVersion)), rng_(GetTickCount64()), epoch_(Clock::now()) {
}

NetplayDriver::~NetplayDriver() { Stop(); }

bool NetplayDriver::Open(const NetplayOptions& o, const MatchTicket& ticket, std::string* error) {
    options_ = o;
    options_.predictionWindow = kPredictionWindow;
    ticket_ = ticket;
    const bool host = ticket.host;
    host_ = host;
    role_ = host || !ticket.spectate ? Role::Player : Role::Spectator;
    Log("netplay: ticket: %s", DescribeTicket(ticket).c_str());
    name_ = o.name.empty() ? SteamPersonaName() : o.name;
    if (name_.empty()) name_ = "Player";
    if (name_.size() > lobby::kMaxName) name_.resize(lobby::kMaxName);
    link_ = static_cast<lobby::Link>(DetectLinkType());
    if (o.udp) {
        auto t = UdpTransport::Create(host ? o.udpPort : 0, error);
        if (!t) return false;
        localId_ = UdpAddress(ParseIpv4(o.udpHost), t->Port());
        base_ = std::move(t);
    } else {
        const uint16_t alsoPort = o.serverAlsoPort ? o.serverAlsoPort : o.server == kDefaultServer ? kDefaultServerAlsoPort
                                                                                                   : 0;
        auto w = WeyveService::Start(o.server, name_, alsoPort);
        weyve_ = w.get();
        base_ = std::move(w);
    }
    conditioned_ = std::make_unique<ConditionedTransport>(*base_, rng_());
    conditioned_->SetConditions(o.conditions);
    mux_ = std::make_unique<Mux>(*conditioned_);
    control_ = &mux_->Channel(kControlChannel);
    gekko_ = &mux_->Channel(kGekkoChannel);
    build_ = Hash64(game_.BuildId().data(), game_.BuildId().size()) ^ Hash64(modVersion_.data(), modVersion_.size());
    seats_[0].present = host;
    if (host) seats_[0].name = name_;
    ready_ = o.autoReady;
    peerReports_ = -1;
    return true;
}

void NetplayDriver::StartHost() {
    LogCaptureStart();
    connectingSince_ = Clock::now();
    if (weyve_) {
        weyve_->CreateRoom();
        SetState(State::Connecting);
        Log("netplay: hosting as %s; creating a room on %s", name_.c_str(), options_.server.c_str());
    } else {
        nonce_ = static_cast<uint16_t>(rng_() | 1);
        MatchCode mc;
        mc.kind = MatchCode::Kind::Udp;
        mc.address = localId_;
        mc.nonce = nonce_;
        HostReady(EncodeMatchCode(mc));
    }
    StartThread();
}

bool NetplayDriver::StartJoin(const std::string& code) {
    MatchCode mc;
    if (!DecodeMatchCode(code, &mc)) return false;
    if ((mc.kind == MatchCode::Kind::Udp) != options_.udp) {
        Log("netplay: the code is for %s; switch transport in polarity.ini", mc.kind == MatchCode::Kind::Udp ? "UDP" : "rooms");
        return false;
    }
    code_ = EncodeMatchCode(mc);
    LogCaptureStart();
    connectingSince_ = Clock::now();
    if (weyve_) {
        weyve_->JoinRoom(mc.room);
        SetState(State::Connecting);
        Log("netplay: joining room %s on %s as %s%s", code_.c_str(), options_.server.c_str(), name_.c_str(),
            role_ == Role::Spectator ? " (spectator)" : "");
    } else {
        peer_ = mc.address;
        nonce_ = mc.nonce;
        BeginHello();
    }
    StartThread();
    return true;
}

void NetplayDriver::SetConditions(const NetConditions& c) {
    if (conditioned_) conditioned_->SetConditions(c);
    Log("netplay: simulating %d ms latency, %d ms jitter, %.1f%% loss", c.latencyMs, c.jitterMs, c.loss * 100.0);
}

void NetplayDriver::SetReady(bool ready) {
    std::lock_guard lock(mutex_);
    if (!ready && ready_ && BothReadyLocked()) {
        Log("netplay: both are ready: locked in until the match starts");
        return;
    }
    if (ready != ready_) Log("netplay: %sready", ready ? "" : "not ");
    ready_ = ready;
    lockedIn_ = ready;
}

void NetplayDriver::SetSelectState(uint8_t screen, uint8_t row, uint32_t hover) {
    std::lock_guard lock(mutex_);
    if (selectScreen_ == screen && selectRow_ == row && selectHover_ == hover) return;
    selectScreen_ = screen;
    selectRow_ = row;
    selectHover_ = hover;
    beatNow_ = true;
}

void NetplayDriver::SetPick(bool hasPick, const PlayerSetup& pick) {
    std::lock_guard lock(mutex_);
    if (options_.hasPick == hasPick && options_.pick == pick) return;
    if (lockedIn_ && state_ != State::Ended) {
        Log("netplay: the pick is locked in (ready)");
        return;
    }
    options_.hasPick = hasPick;
    options_.pick = pick;
    if (!(options_.autoReady && ticket_.after == AfterMatch::Vote)) ready_ = false;
}

void NetplayDriver::SetDelay(uint8_t frames) {
    nextDelay_ = std::min<int>(frames, kMaxInputDelay);
    Log("netplay: input delay %d from the next match", nextDelay_.load());
}

void NetplayDriver::TestChat(const std::string& text) {
    if (weyve_) weyve_->SendChat(text);
    Log("netplay: sent chat into the match room (test): %s", text.c_str());
}

void NetplayDriver::ChooseMatch(uint64_t id) {
    if (role_ != Role::Spectator) return;
    const State s = GetState();
    if (s == State::Jumping || s == State::Loading) {
        pickedMatch_ = id;
        pickWaits_ = true;
        Log("netplay: match pick %s waits for the switch under way", id ? Hex64(id).c_str() : "live");
        return;
    }
    if (s != State::Lobby && s != State::Running && s != State::PostMatch) return;
    lobby::MatchEntry e;
    {
        std::lock_guard lock(mutex_);
        if (matchList_.empty()) return;
        const auto it = id ? std::find_if(matchList_.begin(), matchList_.end(), [&](const lobby::MatchEntry& m) { return m.id == id; })
                           : matchList_.end() - 1;
        if (it == matchList_.end()) {
            Log("netplay: match %s is no longer kept", Hex64(id).c_str());
            return;
        }
        e = *it;
    }
    if (s == State::Running && e.id == watchingMatch_) return;
    switchNow_ = true;
    WatchMatch(e);
}

void NetplayDriver::SetStage(const std::string& code) {
    if (lockedIn_) {
        Log("netplay: the stage is locked in (ready)");
        return;
    }
    const int index = code.empty() || code == "random" ? -1 : FlowStageIndex(code);
    hostStageRule_ = code == "random" ? lobby::kRandomStage : index >= 0 ? index
                                                                         : lobby::kAnyStage;
    Log("netplay: stage %s", code.empty() ? "as prepared" : code.c_str());
}

void NetplayDriver::SetStageLocked(bool locked) {
    if (!host_) return;
    if (stageLocked_.exchange(locked) != locked) Log("netplay: stage %s", locked ? "confirmed" : "open again");
}

void NetplayDriver::Choose(Choice c) {
    if (GetState() != State::PostMatch) {
        Log("netplay: no vote now; %s not taken", c == Choice::Rematch ? "rematch" : c == Choice::CharacterSelect ? "character select"
                                                                                                                  : "the choice");
        return;
    }
    myChoice_ = c;
    Log("netplay: after the match: %s", c == Choice::Rematch ? "rematch" : c == Choice::CharacterSelect ? "character select"
                                                                                                        : "none");
}

std::string NetplayDriver::Status() {
    std::lock_guard lock(mutex_);
    return status_;
}

NetView NetplayDriver::View() {
    std::lock_guard lock(mutex_);
    NetView v = live_;
    v.phase = state_;
    v.host = host_;
    v.spectator = role_ == Role::Spectator;
    v.scripted = options_.autoReady;
    v.udp = options_.udp;
    v.code = code_;
    v.status = status_;
    v.server = options_.udp ? "" : options_.server;
    for (int i = 0; i < kNumPlayers; i++) {
        v.seats[i] = seats_[i];
        if (!v.seats[i].name.empty()) v.seats[i].name = core::DisplayName(v.seats[i].name);
    }
    if (role_ == Role::Player) {
        lobby::Seat& own = v.seats[host_ ? 0 : 1];
        own.ready = ready_;
        own.choice = myChoice_;
        own.hasPick = options_.hasPick;
        own.pick = options_.pick;
        own.inputDelay = nextDelay_ >= 0 ? static_cast<uint8_t>(nextDelay_.load()) : options_.inputDelay;
        own.link = link_;
    }
    v.desyncReport = desyncReport_;
    v.you = role_ == Role::Spectator ? -1 : host_ ? 0
                                                  : 1;
    v.setPlay = ticket_.after == AfterMatch::Set;
    v.build = Hex64(build_);
    v.againVote = v.setPlay && role_ == Role::Player && state_ == State::PostMatch && SetWinnerLocked() >= 0 &&
                  forfeitSeat_ < 0 && AgainOn();
    v.againAsk = host_ && again_ == Again::Ask;
    if (v.setPlay && state_ == State::PostMatch && (SetWinnerLocked() < 0 || v.againVote) && forfeitSeat_ < 0) {
        const auto left = kSetVote - (Clock::now() - decidedAt_);
        v.voteSecondsLeft = static_cast<int>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(left).count() + 1));
    }
    v.forfeitSeat = forfeitSeat_;
    v.firstTo = ticket_.rules.firstTo;
    v.played = played_;
    v.spectators = host_ ? static_cast<int>(watchers_.size()) : spectators_;
    v.linkPingMs = linkPingMs_;
    v.link = core::MeasureLink(linkRtts_);
    v.linkPath = linkPath_;
    v.relayHost = relayHost_;
    v.stageRule = host_ ? StageRule() : stageRule_;
    v.stageLocked = stageLocked_;
    v.overResults = state_ == State::Lobby && game_.BattleRunning();
    v.haveSetup = state_ >= State::Jumping && state_ <= State::PostMatch;
    v.setup = setup_;
    if (role_ == Role::Spectator) {
        v.matches = matchList_;
        v.watchingMatch = state_ >= State::Jumping && state_ <= State::PostMatch ? matchId_.load() : 0;
    }
    return v;
}

void NetplayDriver::Stop() {
    ReleaseResults();
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (jump_.joinable()) jump_.join();
    if (control_) {
        if (LeavingMidSet()) {
            const int seat = host_ ? 0 : 1;
            {
                std::lock_guard lock(mutex_);
                forfeitOnBye_ = true;
                forfeitSeat_ = seat;
            }
            g_forfeitSeat = seat;
            Log("netplay: left the table's set: a forfeit");
        }
        SendBye();
    }
    InputIsolatePads(false);
    game_.localInput = nullptr;
    game_.exclusiveTicks = false;
    FlowSetRoundDirectorPerTick(false, false);
    if (session_) {
        const ReplayFile rf = WithSummary(session_->Recording(), true);
        const auto rb = session_->RollbackStats();
        const std::string path = SaveReport(rf, "match");
        SaveToBattleLog(rf);
        Log("netplay: left at frame %d; recording %s", session_->NextFrame(), path.c_str());
        char summary[160];
        std::snprintf(summary, sizeof(summary), "left at frame %d: %u rollbacks (max %u), %d desync events",
                      session_->NextFrame(), rb.total_count, rb.max_frames, session_->DesyncCount());
        WriteMatchLog(path, rf, summary);
        SetResult("left at frame " + std::to_string(session_->NextFrame()));
        session_.reset();
    }
    spectate_.reset();
    ReportsSetMatchRunning(false);
    InputBlockPause(false);
    InputScriptedPad(0, false);
    InputScriptedPad(1, false);
    const bool midJump = (state_ == State::Jumping || state_ == State::Loading) && !game_.BattleRunning();
    if (midJump) {
        if (!leftMidJump_) {
            leftMidJump_ = true;
            Log("netplay: left during the jump: the battle keeps this session's setup, then back to the menu");
            ReturnToMenuOnceBattleRuns(game_, switchesAtJump_, NoBattle::Stay);
        }
    } else {
        BattleSetupClearPending();
    }
    control_ = gekko_ = nullptr;
    mux_.reset();
    conditioned_.reset();
    base_.reset();
    weyve_ = nullptr;
}

bool NetplayDriver::OnFrame(SfvGame& game) {
    if (holdOverResults_ && !matchLive_) return true;
    if (next_ != Next::None && matchLive_) {
        const bool watchOn = role_ == Role::Spectator && spectate_ && next_ != Next::End && !FlowMatchDecided() && !switchNow_;
        if (!watchOn) {
            FinishMatch(game);
            return false;
        }
    }
    const State s = GetState();
    if (s == State::Jumping || s == State::Loading) return FrameWhileLoading(game);
    if (s == State::Running || s == State::PostMatch) {
        if (spectate_) return FrameWhileWatching(game);
        if (session_) return FrameWhileRunning(game);
    }
    return false;
}

std::vector<PreloadPick> NetplayDriver::PreloadPicks() {
    std::lock_guard lock(mutex_);
    return PreloadPicksLocked();
}

std::vector<PreloadPick> NetplayDriver::PreloadPicksLocked() const {
    std::vector<PreloadPick> picks;
    if (state_ != State::Lobby && state_ != State::Hosting && state_ != State::Ended) return picks;
    if (options_.randomSetup) return picks;
    const bool lobby = state_ == State::Lobby, both = BothReadyLocked();
    auto add = [&](bool has, const PlayerSetup& p) {
        if (has && p.character) picks.push_back({UnpackCode(p.character), static_cast<int>(p.costume)});
    };
    if (role_ == Role::Player && (!lobby || both)) add(options_.hasPick, options_.pick);
    for (int i = 0; i < kNumPlayers; i++) {
        if (role_ == Role::Player && i == (host_ ? 0 : 1)) continue;
        if (!lobby || both) add(seats_[i].present && seats_[i].hasPick, seats_[i].pick);
    }
    return picks;
}

bool NetplayDriver::BothReadyLocked() const {
    for (int i = 0; i < kNumPlayers; i++) {
        const bool own = role_ == Role::Player && i == (host_ ? 0 : 1);
        if (own ? !ready_ : !(seats_[i].present && seats_[i].ready)) return false;
    }
    return true;
}

uint32_t NetplayDriver::Now() const {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch_).count());
}

void NetplayDriver::MaybeStartMatch() {
    lobby::Seat a, b;
    {
        std::lock_guard lock(mutex_);
        a = seats_[0];
        b = seats_[1];
    }
    if (!b.present || !a.ready || !b.ready) {
        bothReadyAt_ = {};
        return;
    }
    const auto now = Clock::now();
    if (bothReadyAt_ == Clock::time_point{}) bothReadyAt_ = now;
    if (!(a.preloaded && b.preloaded) && now - bothReadyAt_ < kPreloadWait) return;
    if (!(a.preloaded && b.preloaded)) Log("netplay: starting without both characters preloaded");
    bothReadyAt_ = {};
    StartMatch();
}

void NetplayDriver::StartMatch(bool rematch) {
    MatchSetup setup = ChooseSetup(rematch);
    std::string here, there;
    if (LacksVersion(setup, &here, &there)) {
        EndSessionFor(here, there);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < kNumPlayers; i++) {
            setup.wins[i] = seats_[i].wins;
            setup.streaks[i] = streaks_[i];
        }
        setup_ = setup;
    }
    matchId_ = rng_();
    Log("netplay: match %s; %s", Hex64(matchId_).c_str(), BattleSetupDescribe(setup_).c_str());
    next_ = Next::Jump;
}

void NetplayDriver::MaybeResolve() {
    if (next_ != Next::None) return;
    Choice theirs;
    {
        std::lock_guard lock(mutex_);
        theirs = seats_[1].choice;
    }
    Choice mine = myChoice_;
    if (ticket_.after == AfterMatch::Set) {
        if (const std::string over = SetOverText(); !over.empty()) {
            if (AgainOn()) {
                ResolveAgain(mine, theirs);
                return;
            }
            if (Clock::now() - decidedAt_ >= kSetNextGameAfter) EndSession(over);
            return;
        }
        const bool late = Clock::now() - decidedAt_ >= kSetVote;
        if (late) {
            if (mine == Choice::None) mine = Choice::Rematch;
            if (theirs == Choice::None) theirs = Choice::Rematch;
        }
        const bool select = mine == Choice::CharacterSelect || theirs == Choice::CharacterSelect;
        if (!select && (mine == Choice::None || theirs == Choice::None)) return;
        if (mine == Choice::Rematch && theirs == Choice::Rematch) {
            Log("netplay: next game of the set (%s)%s", SetScoreText().c_str(), late ? ", the vote's time up" : "");
            StartMatch(true);
            return;
        }
        Log("netplay: character select within the set (%s)", SetScoreText().c_str());
        std::lock_guard lock(mutex_);
        next_ = Next::Lobby;
        return;
    }
    if (mine == Choice::None || theirs == Choice::None) return;
    if (mine == Choice::Rematch && theirs == Choice::Rematch) {
        Log("netplay: both chose a rematch");
        StartMatch(true);
        return;
    }
    Log("netplay: back to the lobby");
    next_ = Next::Lobby;
}

State NetplayDriver::GetState() {
    std::lock_guard lock(mutex_);
    return state_;
}

void NetplayDriver::SetState(State s) {
    {
        std::lock_guard lock(mutex_);
        state_ = s;
    }
    if (s == State::Connecting) SetStatus("connecting to " + options_.server);
    else if (s == State::Hosting) SetStatus("HOST " + code_ + " (copied) - waiting for a player");
    else if (s == State::Joining) SetStatus("joining " + code_);
    else if (s == State::Lobby) SetStatus(role_ == Role::Spectator ? "watching: waiting for a match" : "lobby");
    if (s == State::Lobby && host_) stageLocked_ = ticket_.after == AfterMatch::Set;
    else if (s == State::Jumping) SetStatus("jumping into battle");
    else if (s == State::PostMatch) SetStatus("match over: rematch or character select?");
}

void NetplayDriver::SetStatus(const std::string& text) {
    {
        std::lock_guard lock(mutex_);
        status_ = text;
    }
    sfv::mod::StatusSet(text);
}

} // namespace sfv::mod::netplay

namespace sfv::mod {

using namespace netplay;

namespace {

SfvGame* g_game = nullptr;
NetplayOptions g_options;
bool g_keyDown[2] = {};
std::wstring g_reportDir;
std::string g_modVersion;
std::unique_ptr<NetplayDriver> g_driver;
std::mutex g_driverMutex;

void Replace(std::unique_ptr<NetplayDriver> next) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) {
        if (BattleCurrentFrameDriver() == g_driver.get()) BattleDetachFrameDriver();
        g_driver->Stop();
    }
    g_driver = std::move(next);
}

void EndForNewSession() {
    bool had = false;
    {
        std::lock_guard lock(g_driverMutex);
        had = g_driver != nullptr;
    }
    Replace(nullptr);
    if (had && g_options.menuAfterMatch && g_game && g_game->BattleRunning()) FlowReturnToMainMenu(1);
}

} // namespace

void NetInstall(SfvGame* game, const std::wstring& reportDir, const std::string& modVersion,
                const NetplayOptions& defaults) {
    g_options = defaults;
    g_game = game;
    g_reportDir = reportDir;
    g_modVersion = modVersion;
    FlowSetPreloadSource([] {
        std::vector<PreloadPick> picks;
        {
            std::unique_lock lock(g_driverMutex, std::try_to_lock);
            if (!lock.owns_lock()) return picks;
            if (g_driver) return g_driver->PreloadPicks();
        }
        const NetplayOptions& o = g_options;
        if (o.hasPick && o.pick.character && !o.randomSetup) {
            picks.push_back({UnpackCode(o.pick.character), static_cast<int>(o.pick.costume)});
        }
        return picks;
    });
}

void NetPlayTicket(const MatchTicket& ticket, const NetplayOptions& options) {
    if (!g_game) return;
    EndForNewSession();
    auto d = std::make_unique<NetplayDriver>(*g_game, g_reportDir, g_modVersion);
    std::string error;
    if (!d->Open(options, ticket, &error)) {
        const std::string what = ticket.host ? "cannot host: " : "cannot join: ";
        Log("netplay: %s%s", what.c_str(), error.c_str());
        StatusSet(what + error);
        SetResult(what + error);
        return;
    }
    if (ticket.host) {
        d->StartHost();
        Replace(std::move(d));
        return;
    }
    const std::string& code = ticket.roomCode;
    if (!d->StartJoin(code)) {
        Log("netplay: not a valid match code: %s", code.c_str());
        StatusSet("not a valid match code");
        SetResult("not a valid match code");
        return;
    }
    Replace(std::move(d));
}

void NetHost(const NetplayOptions& options) { NetPlayTicket(DirectMatchHost(), options); }

void NetJoin(const std::string& code, const NetplayOptions& options, bool spectate) {
    NetPlayTicket(DirectMatchJoin(code, spectate), options);
}

void NetLeave() {
    Replace(nullptr);
    if (g_options.menuAfterMatch && g_game && g_game->BattleRunning()) FlowReturnToMainMenu(1);
    StatusSet("");
    Log("netplay: left");
}

void NetSetReady(bool ready) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetReady(ready);
}

void NetSetPick(bool hasPick, const PlayerSetup& pick) {
    g_options.hasPick = hasPick;
    g_options.pick = pick;
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetPick(hasPick, pick);
}

int NetPreferredDelay(const NetView& v) {
    const int fixed = g_options.delaySetting;
    return fixed >= 0 && fixed <= kMaxDelaySetting ? fixed : v.SuggestedDelay();
}

int NetPreferredDelay(unsigned pingMs) {
    const int fixed = g_options.delaySetting;
    if (fixed >= 0 && fixed <= kMaxDelaySetting) return fixed;
    return pingMs ? core::SuggestedDelay(pingMs, core::kTypicalJitterMs) : 1;
}

void NetSetDelay(uint8_t frames) {
    g_options.inputDelay = frames;
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetDelay(frames);
}

void NetTestMatchChat(const std::string& text) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->TestChat(text);
}

void NetWatchMatch(uint64_t id) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->ChooseMatch(id);
}

void NetSetStage(const std::string& code) {
    g_options.stageCode = code;
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetStage(code);
}

void NetSetStageLocked(bool locked) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetStageLocked(locked);
}

void NetSetSelectState(uint8_t screen, uint8_t row, uint32_t hover) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetSelectState(screen, row, hover);
}

void NetChoose(lobby::Choice choice) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->Choose(choice);
}

void NetSetAgainOffered(bool offered) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetAgainOffered(offered);
}

void NetAgainAnswer(bool ok, const std::string& rules) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->AgainAnswer(ok, rules);
}

void NetSkewNextStartForTest() { g_skewStart = true; }

void NetSendBadPickForTest(const std::optional<BadPick>& bad) {
    std::lock_guard lock(g_badPickMutex);
    g_badPick = bad;
}

void NetPark() {
    if (!g_game || !g_game->BattleTicking()) {
        Log("park: no battle running");
        return;
    }
    std::thread([] {
        ThreadGuard running;
        static const std::atomic<bool> never{false};
        Log("park: %s", LeaveBattleForStageSelect(*g_game, never) ? "at stage select" : "could not pause the battle");
        InputScriptedPad(0, false);
    }).detach();
}

void NetSetConditions(const NetConditions& conditions) {
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetConditions(conditions);
}

NetplayOptions& NetOptions() { return g_options; }

std::string NetLocalPlayerName() { return g_options.name.empty() ? SteamPersonaName() : g_options.name; }

std::string NetBattleInfo() {
    if (!g_game) return "no game";
    std::string s = g_game->BattleRunning() ? "running" : "not running";
    s += g_game->BattleTicking() ? ", ticking" : ", not ticking";
    auto words = [&](const char* name, const uint8_t* p, size_t size) {
        if (!p) return;
        s += std::string(" | ") + name;
        char buf[16];
        for (size_t i = 0; i < size; i += 4) {
            uint32_t v;
            std::memcpy(&v, p + i, 4);
            std::snprintf(buf, sizeof(buf), " %x", v);
            s += buf;
        }
    };
    if (uint8_t* bf = g_game->BattleFrameObject()) {
        char at[48];
        std::snprintf(at, sizeof(at), " | battleframe at %p:", static_cast<void*>(bf));
        s += at;
        words("", bf + game::battleframe::kStateBegin, game::battleframe::kStateSize);
        words("extra", bf + game::battleframe::kExtraTicks, 4);
    }
    if (uint8_t* iu = g_game->InputUnit()) words("inputunit", iu + game::inputunit::kCountersBegin, game::inputunit::kCountersSize);
    s += " | regions " + std::to_string(g_game->Regions().size());
    if (uint8_t* data = BattleData()) {
        char at[48];
        std::snprintf(at, sizeof(at), " | battledata at %p:", static_cast<void*>(data));
        s += at;
        words("", data, battledata::kCharaCode);
    }
    return s;
}

void NetPollHotkeys() {
    if (!g_options.hotkeys) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(InputRealForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    constexpr SHORT kKeyDown = static_cast<SHORT>(0x8000); // GetAsyncKeyState's high bit
    const int keys[2] = {VK_F7, VK_F8};
    for (int i = 0; i < 2; i++) {
        const bool down = focused && (GetAsyncKeyState(keys[i]) & kKeyDown) != 0;
        const bool pressed = down && !g_keyDown[i];
        g_keyDown[i] = down;
        if (!pressed) continue;
        const NetView::Phase phase = NetViewNow().phase;
        if (phase != NetView::Phase::Idle && phase != NetView::Phase::Ended) {
            Log("netplay: %s ignored: a session is running (leave it first)", i == 0 ? "F7" : "F8");
            continue;
        }
        if (i == 0) {
            NetHost(g_options);
            continue;
        }
        std::string code;
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_TEXT)) {
                if (const char* text = static_cast<const char*>(GlobalLock(h))) {
                    code = text;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        NetJoin(code, g_options);
    }
}

std::string NetStatus() {
    std::lock_guard lock(g_driverMutex);
    return g_driver ? g_driver->Status() : "idle";
}

int NetTakeForfeit() { return g_forfeitSeat.exchange(-1); }

NetView NetViewNow() {
    static std::mutex lastMutex;
    static NetView last;
    std::unique_lock lock(g_driverMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::lock_guard l(lastMutex);
        return last;
    }
    NetView v = g_driver ? g_driver->View() : NetView{};
    lock.unlock();
    v.result = LastResult();
    std::lock_guard l(lastMutex);
    last = v;
    return v;
}

std::string NetGameBuildId() { return g_game ? g_game->BuildId() : std::string(); }

} // namespace sfv::mod
