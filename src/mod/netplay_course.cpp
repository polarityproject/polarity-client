#include "mod/netplay_internal.h"

namespace sfv::mod::netplay {

bool NetplayDriver::AgainOn() const {
    const Again a = again_;
    if (a == Again::Refused) return false;
    return a != Again::None || againOffered_;
}

void NetplayDriver::ResolveAgain(Choice mine, Choice theirs) {
    const auto now = Clock::now();
    const Again a = again_;
    if (a == Again::Granted) {
        ContinueSet();
        return;
    }
    if (a == Again::Ask) {
        if (now - againAskedAt_ > kAgainAnswerWait) {
            Log("netplay: no word from the keeper on the next set; back to the room");
            again_ = Again::Refused;
        }
        return;
    }
    const bool late = now - decidedAt_ >= kSetVote;
    if (late && mine == Choice::None && theirs == Choice::None) {
        Log("netplay: %s; neither chose by the vote's end: back to the room", SetOverText().c_str());
        again_ = Again::Refused;
        return;
    }
    if (late) {
        if (mine == Choice::None) mine = Choice::Rematch;
        if (theirs == Choice::None) theirs = Choice::Rematch;
    }
    const bool select = mine == Choice::CharacterSelect || theirs == Choice::CharacterSelect;
    if (!select && (mine == Choice::None || theirs == Choice::None)) return;
    againSelect_ = select;
    againAskedAt_ = now;
    again_ = Again::Ask;
    Log("netplay: %s; both play on (%s)%s: asking the keeper for the next set", SetOverText().c_str(),
        select ? "character select" : "rematch", late ? ", the vote's time up" : "");
}

void NetplayDriver::ContinueSet() {
    Rules rules = againRules_;
    {
        std::lock_guard lock(mutex_);
        for (auto& seat : seats_) seat.wins = 0;
        played_ = 0;
        for (auto& streak : streaks_) streak = 0;
        forfeitSeat_ = -1;
        ticket_.rules = rules;
    }
    g_forfeitSeat = -1;
    again_ = Again::None;
    Log("netplay: the next set: %s", FormatRules(rules).c_str());
    if (againSelect_) {
        std::lock_guard lock(mutex_);
        next_ = Next::Lobby;
    } else {
        StartMatch(true);
    }
}

void NetplayDriver::SetAgainOffered(bool offered) { againOffered_ = offered; }

void NetplayDriver::AgainAnswer(bool ok, const std::string& text) {
    Rules rules;
    if (ok && !ParseRules(text, &rules)) ok = false;
    if (host_) {
        if (again_ != Again::Ask) return;
        againRules_ = rules;
        again_ = ok ? Again::Granted : Again::Refused;
        Log("netplay: the keeper %s", ok ? ("starts the next set: " + text).c_str() : "has someone waiting: back to the room");
        return;
    }
    if (ok) {
        std::lock_guard lock(mutex_);
        ticket_.rules = rules;
        Log("netplay: the next set: %s", text.c_str());
    }
}

void NetplayDriver::Transitions() {
    const Next next = next_;
    if (next == Next::None) return;
    if (matchLive_) {
        if (GetState() != State::Jumping || game_.BattleRunning()) return;
        DetachFrameDriver();
        FinishMatch(game_, next == Next::End || next == Next::Lobby);
        if (next == Next::Lobby) {
            Log("netplay: back to the lobby during the jump: calling it off");
            FlowCancelBattleJump();
            ReturnToMenuOnceBattleRuns(game_, switchesAtJump_, NoBattle::Stay);
        }
    }
    next_ = Next::None;
    switch (next) {
        case Next::Jump:
            BeginJump();
            break;
        case Next::Lobby:
            myChoice_ = Choice::None;
            ready_ = options_.autoReady && ticket_.after != AfterMatch::Set;
            lockedIn_ = false;
            {
                std::lock_guard lock(mutex_);
                selectScreen_ = 0;
                selectHover_ = 0;
                for (auto& seat : seats_) {
                    seat.choice = Choice::None;
                    seat.ready = false;
                }
            }
            SetState(State::Lobby);
            if (host_ && ticket_.after == AfterMatch::Set) SetStageLocked(false);
            if (game_.BattleRunning()) {
                FlowSetHoldResults(true);
                holdOverResults_ = true;
                SetFrameDriver(this);
                InputIsolatePads(true);
                InputBlockPause(true);
            }
            break;
        case Next::End: {
            const State before = GetState();
            ReleaseResults();
            SetState(State::Ended);
            if (!options_.menuAfterMatch) FlowSetHoldResults(false);
            if (options_.menuAfterMatch && (before == State::Jumping || before == State::Loading)) {
                Log("netplay: the session ended during a jump: back to the menu once its battle is up");
                ReturnToMenuOnceBattleRuns(game_, switchesAtJump_, NoBattle::Menu);
            } else if (options_.menuAfterMatch && game_.BattleRunning()) {
                FlowReturnToMainMenu(kMenuAfterMatchFrames);
            }
            if (weyve_) weyve_->LeaveRoom();
            break;
        }
        case Next::None:
            break;
    }
}

void NetplayDriver::RefuseFromPeer(const std::string& why) {
    if (GetState() == State::Ended || next_ == Next::End) return;
    Log("netplay: refused from the other side: %s", why.c_str());
    EndSessionFor("the other side sent a pick this game does not have (" + why + ")",
                  "the other side's game does not have a pick sent from here (" + why + ")");
}

void NetplayDriver::EndSessionFor(const std::string& why, const std::string& theirs) {
    {
        std::lock_guard lock(mutex_);
        byeText_ = theirs;
    }
    SendBye();
    EndSession(why);
}

bool NetplayDriver::LacksVersion(const MatchSetup& s, std::string* here, std::string* there) {
    for (const PlayerSetup& p : s.players) {
        const std::string code = UnpackCode(p.character);
        if (p.version < 0 || FlowHasCharaVersion(code, p.version)) continue;
        const int index = FlowCharaIndex(code);
        const std::string name = index >= 0 ? FlowCharaName(index) : code;
        const int newest = FlowNewestLocalVersion(code);
        Log("netplay: this game lacks %s version %d (its newest: %d)", code.c_str(), p.version, newest);
        *here = "this game lacks " + name + "'s final version (" + std::to_string(p.version) + ", newest here " +
                std::to_string(newest) + "): verify the game files in Steam";
        *there = "the other player's game lacks " + name + "'s final version (" + std::to_string(p.version) +
                 "): they should verify their game files in Steam";
        return true;
    }
    return false;
}

std::string NetplayDriver::SetScoreText() {
    std::lock_guard lock(mutex_);
    return "P1 " + std::to_string(seats_[0].wins) + " - " + std::to_string(seats_[1].wins) + " P2, first to " +
           std::to_string(ticket_.rules.firstTo);
}

int NetplayDriver::SetWinnerLocked() {
    if (ticket_.after != AfterMatch::Set) return -1;
    uint16_t wins[kNumPlayers];
    for (int i = 0; i < kNumPlayers; i++) wins[i] = seats_[i].wins;
    return SetWinner(ticket_.rules, wins);
}

bool NetplayDriver::LeavingMidSet() {
    if (ticket_.after != AfterMatch::Set || role_ != Role::Player) return false;
    std::lock_guard lock(mutex_);
    const bool begun = played_ > 0 || state_ == State::Jumping || state_ == State::Loading || state_ == State::Running ||
                       state_ == State::PostMatch;
    return begun && state_ != State::Ended && state_ != State::Idle && SetWinnerLocked() < 0 && forfeitSeat_ < 0;
}

bool NetplayDriver::TookForfeit(int seat) {
    if (ticket_.after != AfterMatch::Set || role_ != Role::Player) return false;
    {
        std::lock_guard lock(mutex_);
        if (SetWinnerLocked() >= 0) return false;
        forfeitSeat_ = seat;
    }
    g_forfeitSeat = seat;
    EndSession("the other player left: the set ends here");
    return true;
}

std::string NetplayDriver::SetOverText() {
    if (ticket_.after != AfterMatch::Set) return "";
    uint16_t wins[kNumPlayers];
    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < kNumPlayers; i++) wins[i] = seats_[i].wins;
    }
    const int winner = SetWinner(ticket_.rules, wins);
    if (winner < 0) return "";
    return "P" + std::to_string(winner + 1) + " won the set " + std::to_string(wins[winner]) + " - " + std::to_string(wins[1 - winner]);
}

void NetplayDriver::EndSession(const std::string& why) {
    if (GetState() == State::Ended || next_ == Next::End) return;
    Log("netplay: session over: %s", why.c_str());
    SetStatus(why);
    SetResult(why);
    if (!peerLeft_) byePending_ = true;
    if (!peer_ || GetState() < State::Lobby) {
        SetState(State::Ended);
        if (weyve_) weyve_->LeaveRoom();
        return;
    }
    next_ = Next::End;
}

PlayerSetup NetplayDriver::RollEleven(const PlayerSetup& before) {
    std::string code;
    int costume = 0;
    for (int tries = 0; tries < 8; tries++) {
        if (!FlowElevenStandIn(static_cast<uint32_t>(rng_()), &code, &costume)) return before;
        if (PackCode(code.c_str()) != before.character) break;
    }
    PlayerSetup p;
    p.character = PackCode(code.c_str());
    p.costume = static_cast<uint16_t>(costume);
    p.vskill = static_cast<uint8_t>(rng_() % 2);
    p.vtrigger = static_cast<uint8_t>(rng_() % 2);
    return p;
}

MatchSetup NetplayDriver::ChooseSetup(bool rematch) {
    MatchSetup s;
    if (options_.randomSetup) {
        for (auto& p : s.players) {
            p.character = PackCode(kRandomCharacters[rng_() % std::size(kRandomCharacters)]);
            p.costume = options_.randomCostumes ? static_cast<uint16_t>(rng_() % 3) : 0;
            p.color = static_cast<uint8_t>(rng_() % 10);
            p.vskill = static_cast<uint8_t>(rng_() % 2);
            p.vtrigger = static_cast<uint8_t>(rng_() % 2);
            p.comment = 0;
        }
        const int stage = FlowRandomStage(rng_(), false);
        s.stage = stage < 0 ? kKeepStage : static_cast<uint16_t>(stage);
    } else {
        MatchSetup current;
        ReadMatchSetup(&current);
        lobby::Seat joiner;
        MatchSetup last;
        {
            std::lock_guard lock(mutex_);
            joiner = seats_[1];
            s.players[0] = options_.hasPick ? options_.pick : current.players[0];
            last = setup_;
        }
        s.players[1] = joiner.hasPick ? joiner.pick : current.players[1];
        for (int i = 0; rematch && i < kNumPlayers; i++) {
            if (!IsElevenPick(s.players[i])) continue;
            s.players[i] = RollEleven(last.players[i]);
            Log("netplay: rematch: P%d's Eleven plays %s", i + 1, UnpackCode(s.players[i].character).c_str());
        }
        const uint16_t rule = StageRule();
        s.stage = rule == lobby::kAnyStage ? kKeepStage : rule;
        if (rule == lobby::kRandomStage) {
            const int stage = FlowRandomStage(rng_(), true, options_.randomOff);
            s.stage = stage < 0 ? kKeepStage : static_cast<uint16_t>(stage);
        }
    }
    for (auto& p : s.players) p.version = static_cast<int16_t>(FlowFinalCharaVersion(UnpackCode(p.character)));
    ApplyRulesToSetup(ticket_.rules, &s);
    s.seed = rng_();
    return s;
}

void NetplayDriver::StreamToWatchers() {
    const auto now = Clock::now();
    std::vector<std::pair<uint64_t, lobby::Message>> out;
    {
        std::lock_guard lock(mutex_);
        if (watchers_.empty()) return;
        for (auto& [id, w] : watchers_) {
            if (!w.streaming) continue;
            const auto found = streams_.find(w.match);
            if (found == streams_.end()) continue;
            const std::vector<lobby::FrameRecord>& stream = found->second;
            const int have = static_cast<int>(stream.size());
            if (now - w.progressAt > kStreamResend && w.sentThrough >= w.wanted) {
                w.sentThrough = w.wanted - 1;
                w.progressAt = now;
            }
            const int limit = w.wanted + kStreamBatchesAhead * static_cast<int>(lobby::kInputsPerMessage);
            while (w.sentThrough + 1 < have && w.sentThrough + 1 < limit) {
                lobby::Message m;
                m.type = lobby::Type::Inputs;
                m.matchId = w.match;
                m.first = static_cast<uint32_t>(w.sentThrough + 1);
                const int end = std::min({have, limit, w.sentThrough + 1 + static_cast<int>(lobby::kInputsPerMessage)});
                m.frames.assign(stream.begin() + (w.sentThrough + 1), stream.begin() + end);
                w.sentThrough = end - 1;
                out.emplace_back(id, std::move(m));
            }
        }
    }
    for (auto& [id, m] : out) SendTo(id, m);
}

} // namespace sfv::mod::netplay
