#include "mod/netplay_internal.h"

namespace sfv::mod::netplay {

void NetplayDriver::StartThread() {
    stop_ = false;
    thread_ = std::thread([this] {
        ThreadGuard running;
        while (!stop_ && !Stopping()) {
            ControlTick();
            Sleep(5);
        }
    });
}

void NetplayDriver::SendTo(uint64_t to, lobby::Message m) {
    m.protocol = lobby::kProtocol;
    {
        std::lock_guard lock(g_badPickMutex);
        if (g_badPick) BadPickApply(*g_badPick, m);
    }
    const auto bytes = lobby::Encode(m);
    control_->Send(to, bytes.data(), bytes.size());
}

void NetplayDriver::Send(lobby::Message m) { SendTo(peer_, std::move(m)); }

std::vector<uint64_t> NetplayDriver::Members() {
    std::vector<uint64_t> out;
    if (peer_) out.push_back(peer_);
    std::lock_guard lock(mutex_);
    for (const auto& [id, w] : watchers_) out.push_back(id);
    return out;
}

void NetplayDriver::SendBye() {
    lobby::Message bye;
    bye.type = lobby::Type::Bye;
    {
        std::lock_guard lock(mutex_);
        bye.text = std::exchange(byeText_, {});
        bye.forfeit = forfeitOnBye_;
    }
    for (uint64_t to : Members()) SendTo(to, bye);
}

void NetplayDriver::HostReady(const std::string& code) {
    {
        std::lock_guard lock(mutex_);
        code_ = code;
    }
    const bool forTable = ticket_.after == AfterMatch::Set;
    if (!forTable) CopyToClipboard(code);
    SetState(State::Hosting);
    Log(forTable ? "netplay: hosting, code %s" : "netplay: hosting, code %s (copied to the clipboard)", code.c_str());
}

void NetplayDriver::BeginHello() {
    helloSince_ = Clock::now();
    SetState(State::Joining);
    Log("netplay: joining %s", code_.c_str());
}

void NetplayDriver::ControlTick() {
    if (weyve_) RoomEvents();
    Packet p;
    while (control_->Receive(p)) {
        auto msg = lobby::Decode(p.data.data(), p.data.size());
        if (msg) OnMessage(p.from, *msg);
    }
    if (host_) StreamToWatchers();
    Transitions();

    const auto now = Clock::now();
    if (now - lastBeat_ < kMinBeat) return;
    if (!beatNow_.exchange(false) && now - lastBeat_ < kBeat) return;
    lastBeat_ = now;

    if (byePending_.exchange(false)) SendBye();
    const State s = GetState();
    if (s == State::Ended || s == State::Idle) return;
    if (s == State::Connecting && now - connectingSince_ > kConnectTimeout) {
        EndSession(weyve_ && weyve_->View().server != WeyveRoomView::Server::Connected
                       ? "cannot reach the server " + options_.server +
                             " (a firewall, antivirus or this network may block its port: allow StreetFighterV.exe, or try another network)"
                       : "no answer from the room");
        return;
    }
    if (CheckSilence(now)) return;
    UpdateLinkPath();

    if (s == State::Joining) {
        if (now - helloSince_ > kJoinTimeout) {
            EndSession("could not reach the host");
            return;
        }
        lobby::Message hello;
        hello.type = lobby::Type::Hello;
        hello.nonce = nonce_;
        hello.build = build_;
        hello.role = role_;
        hello.name = name_;
        hello.reports = ReportsOn();
        hello.seatToken = ticket_.seatToken;
        Send(hello);
        return;
    }
    if (host_) HostBeat(s);
    else GuestBeat(s);
}

void NetplayDriver::RoomEvents() {
    WeyveEvent e;
    while (weyve_->NextEvent(e)) {
        switch (e.type) {
            case WeyveEvent::Type::RoomJoined:
                if (host_) {
                    weyve_->SetRoomData("build", Hex64(build_));
                    weyve_->SetRoomData("version", modVersion_);
                    weyve_->SetRoomData("game", game_.BuildId());
                    weyve_->SetRoomData("protocol", std::to_string(lobby::kProtocol));
                    MatchCode mc;
                    mc.kind = MatchCode::Kind::Room;
                    mc.room = e.text;
                    HostReady(EncodeMatchCode(mc));
                } else {
                    joinedRoom_ = true;
                }
                break;
            case WeyveEvent::Type::RoomError:
                if (GetState() == State::Connecting) EndSession(host_ ? "cannot create a room: " + e.text : "cannot join: " + e.text);
                break;
            case WeyveEvent::Type::PeerLeft:
                if (e.peer == peer_) {
                    const State s = GetState();
                    if ((s == State::Running || s == State::PostMatch || s == State::Loading || s == State::Jumping) &&
                        weyve_->PeerLinked(e.peer)) {
                        Log("netplay: the server says the other side left the room; the link is up, so the match goes on");
                        peerLeftRoom_ = true;
                        break;
                    }
                    if (s != State::Ended) {
                        Log("netplay: the other %s left the room", host_ || role_ == Role::Player ? "player" : "side");
                        peerLeft_ = true;
                        EndSession(host_ || role_ == Role::Player ? "the other player left" : "the host left");
                    }
                } else {
                    std::lock_guard lock(mutex_);
                    if (watchers_.erase(e.peer)) Log("netplay: spectator %u left", static_cast<unsigned>(e.peer));
                }
                break;
            case WeyveEvent::Type::Removed:
            case WeyveEvent::Type::ServerLost: {
                const State s = GetState();
                if (s == State::Ended) break;
                if (s == State::Running || s == State::PostMatch || s == State::Loading || s == State::Jumping) {
                    Log("netplay: %s; the match goes on as long as the link holds", e.text.c_str());
                    serverLost_ = true;
                    break;
                }
                EndSession(e.text);
                break;
            }
            case WeyveEvent::Type::Chat:
                if (host_ && e.peer && weyve_) {
                    Log("netplay: member %u chatted in the match room (no Polarity build does): banned", e.peer);
                    weyve_->Ban(e.peer);
                }
                break;
            case WeyveEvent::Type::PeerJoined:
            case WeyveEvent::Type::HostChanged:
            case WeyveEvent::Type::RoomList:
                break;
        }
    }
    if (joinedRoom_ && GetState() == State::Connecting) {
        const WeyveRoomView v = weyve_->View();
        const auto build = v.data.find("build");
        if (v.host && build != v.data.end()) {
            joinedRoom_ = false;
            if (build->second != Hex64(build_)) {
                const auto version = v.data.find("version");
                const std::string theirs = version == v.data.end() ? "" : version->second;
                Log("netplay: the room's build is %s (Polarity %s), this one %s (Polarity %s)", build->second.c_str(),
                    theirs.empty() ? "?" : theirs.c_str(), Hex64(build_).c_str(), modVersion_.c_str());
                std::string why = "rejected: different mod build";
                if (theirs.empty() || CompareVersions(theirs, modVersion_) < 0) {
                    why = "the host runs an older Polarity" + (theirs.empty() ? std::string() : " (" + theirs + ")") +
                          "; they need to update";
                } else if (CompareVersions(theirs, modVersion_) > 0) {
                    why = "the host runs Polarity " + theirs + "; update from Polarity settings, Updates";
                } else {
                    const auto game = v.data.find("game");
                    const std::string theirGame = game == v.data.end() ? "" : game->second;
                    const std::string mine = game_.BuildId();
                    auto part = [](const std::string& id, const char* name) {
                        const size_t at = id.find(name);
                        return at == std::string::npos ? std::string() : id.substr(at, id.find('/', at) - at);
                    };
                    if (!theirGame.empty()) Log("netplay: the room's game build is %s, this one %s", theirGame.c_str(), mine.c_str());
                    if (!theirGame.empty() && part(theirGame, "sfv-") != part(mine, "sfv-")) {
                        why = "the host's Street Fighter V is another version (check for a game update in Steam)";
                    } else if (!theirGame.empty() && part(theirGame, "noise-") != part(mine, "noise-")) {
                        why = "the host's Polarity " + theirs + " checks the game state differently; one of you should reinstall it";
                    } else {
                        why = "the host runs another build of Polarity " + theirs + "; one of you should reinstall it";
                    }
                }
                EndSession(why);
                return;
            }
            peer_ = v.host;
            BeginHello();
        }
    }
}

void NetplayDriver::UpdateLinkPath() {
    if (!weyve_ || !peer_) return;
    NetView::Path path = NetView::Path::Unknown;
    const auto askedAt = Clock::now();
    switch (weyve_->PeerPath(static_cast<uint32_t>(peer_))) {
        case WeyveService::LinkPath::Direct: path = NetView::Path::Direct; break;
        case WeyveService::LinkPath::Relayed: path = NetView::Path::Relayed; break;
        default: break;
    }
    const std::string relay = weyve_->TurnHost();
    const auto took = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - askedAt).count();
    if (took >= kSlowLinkQueryUs) Log("hitch: the link path query took %.1f ms", took / 1000.0);
    std::lock_guard lock(mutex_);
    if (path != linkPath_ && path != NetView::Path::Unknown) {
        Log("netplay: the link to the other side is %s", path == NetView::Path::Relayed ? ("relayed through " + (relay.empty() ? std::string("TURN") : relay)).c_str() : "peer to peer");
    }
    linkPath_ = path;
    relayHost_ = relay;
}

bool NetplayDriver::CheckSilence(Clock::time_point now) {
    const State s = GetState();
    const bool waiting = s == State::Connecting || s == State::Hosting || s == State::Joining;
    if (!waiting && peer_ && now - heard_ > kSilence) {
        Log("netplay: nothing from the other side for %lld s",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - heard_).count()));
        const std::string over = SetOverText();
        EndSession(!over.empty() ? over : "connection lost");
        return true;
    }
    if (host_) {
        std::lock_guard lock(mutex_);
        for (auto it = watchers_.begin(); it != watchers_.end();) {
            if (now - it->second.heard > kSilence) {
                Log("netplay: spectator %s silent; dropped", it->second.name.c_str());
                it = watchers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return false;
}

void NetplayDriver::Heard(uint32_t time) {
    heard_ = Clock::now();
    heardTime_ = time;
    heardTimeAt_ = heard_;
}

void NetplayDriver::Echo(lobby::Message& m, uint32_t heardTime, Clock::time_point heardAt) {
    m.time = Now();
    m.echo = heardTime;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - heardAt).count();
    m.echoAge = static_cast<uint16_t>(std::clamp<long long>(age, 0, std::numeric_limits<uint16_t>::max()));
}

void NetplayDriver::TakeEcho(const lobby::Message& m) {
    if (!m.echo) return;
    const int64_t rtt = int64_t(Now()) - int64_t(m.echo) - m.echoAge;
    if (rtt < 0 || rtt >= kMaxRttMs) return;
    linkPingMs_ = static_cast<unsigned>(rtt);
    std::lock_guard lock(mutex_);
    linkRtts_.push_back(static_cast<uint16_t>(rtt));
    if (linkRtts_.size() > core::kLinkSamples) linkRtts_.erase(linkRtts_.begin());
}

lobby::Seat NetplayDriver::OwnSeat() {
    lobby::Seat seat;
    std::lock_guard lock(mutex_);
    seat.present = true;
    seat.name = name_;
    seat.hasPick = options_.hasPick;
    seat.pick = options_.pick;
    seat.ready = ready_;
    std::string picks;
    for (const PreloadPick& p : PreloadPicksLocked()) picks += p.code + "/" + std::to_string(p.costume) + " ";
    const bool otherKnown = options_.randomSetup || role_ != Role::Player ||
                            (seats_[host_ ? 1 : 0].present && seats_[host_ ? 1 : 0].hasPick);
    const bool settled = options_.randomSetup || state_ != State::Lobby || BothReadyLocked();
    seat.preloaded = otherKnown && settled && FlowPreloadsDoneFor(picks);
    seat.choice = myChoice_;
    seat.screen = selectScreen_;
    seat.row = selectRow_;
    seat.hover = selectHover_;
    seat.inputDelay = nextDelay_ >= 0 ? static_cast<uint8_t>(nextDelay_.load()) : options_.inputDelay;
    seat.fps = static_cast<uint8_t>(std::clamp(OverlayRenderFps() + 0.5f, 0.0f, 255.0f));
    seat.link = link_;
    return seat;
}

uint16_t NetplayDriver::StageRule() {
    if (hostStageRule_ >= 0) return static_cast<uint16_t>(hostStageRule_.load());
    if (options_.stageCode == "random") return lobby::kRandomStage;
    if (!options_.stageCode.empty()) {
        const int i = FlowStageIndex(options_.stageCode);
        return i >= 0 ? static_cast<uint16_t>(i) : lobby::kAnyStage;
    }
    return options_.stage;
}

void NetplayDriver::HostBeat(State s) {
    lobby::Message st;
    st.type = lobby::Type::SessionState;
    {
        const lobby::Seat own = OwnSeat();
        std::lock_guard lock(mutex_);
        const uint16_t wins = seats_[0].wins;
        seats_[0] = own;
        seats_[0].wins = wins;
        st.seats[0] = seats_[0];
        st.seats[1] = seats_[1];
        st.played = static_cast<uint16_t>(played_);
        st.spectators = static_cast<uint8_t>(watchers_.size());
    }
    st.phase = s == State::PostMatch                        ? SessionPhase::PostMatch
               : s >= State::Jumping && s <= State::Running ? SessionPhase::Match
                                                            : SessionPhase::Lobby;
    st.matchId = matchId_;
    st.stage = StageRule();
    st.stageLocked = stageLocked_;
    if (peer_) {
        Echo(st, heardTime_, heardTimeAt_);
        Send(st);
    }
    std::map<uint64_t, Watcher> watchers;
    {
        std::lock_guard lock(mutex_);
        watchers = watchers_;
    }
    for (const auto& [id, w] : watchers) {
        Echo(st, w.heardTime, w.heardTimeAt);
        SendTo(id, st);
    }

    const bool matchOn = s == State::Jumping || s == State::Loading || s == State::Running;
    if (matchOn && peer_ && !peerLoaded_) Send(WelcomeMessage());
    for (const auto& [id, w] : watchers) {
        if ((matchOn || s == State::PostMatch) && w.match != matchId_) SendTo(id, WelcomeMessage());
    }
    if (selfLoaded_ && (s == State::Loading || s == State::Running || s == State::PostMatch)) {
        const lobby::Message loaded = LoadedMessage();
        if (s == State::Loading || sessionSyncing_) Send(loaded);
        for (const auto& [id, w] : watchers) {
            if (w.match == matchId_ && !w.streaming) SendTo(id, loaded);
        }
    }
    if (!watchers.empty() && Clock::now() - matchListSentAt_ > std::chrono::seconds(1)) {
        matchListSentAt_ = Clock::now();
        lobby::Message list;
        {
            std::lock_guard lock(mutex_);
            list = MatchListLocked();
        }
        const size_t total = list.matches.size();
        for (size_t at = 0; at < std::max<size_t>(total, 1); at += lobby::kMatchesPerMessage) {
            lobby::Message slice;
            slice.type = lobby::Type::Matches;
            slice.sliceAt = static_cast<uint8_t>(at);
            slice.sliceOf = static_cast<uint8_t>(total);
            const size_t end = std::min(total, at + lobby::kMatchesPerMessage);
            slice.matches.assign(list.matches.begin() + static_cast<std::ptrdiff_t>(std::min(at, total)),
                                 list.matches.begin() + static_cast<std::ptrdiff_t>(end));
            for (const auto& [id, w] : watchers) SendTo(id, slice);
        }
    }

    if (s == State::Lobby) MaybeStartMatch();
    if (s == State::PostMatch) MaybeResolve();
}

void NetplayDriver::GuestBeat(State s) {
    if (role_ == Role::Player && s == State::PostMatch && ticket_.after == AfterMatch::Set && !againOffered_ &&
        Clock::now() - decidedAt_ > kSetNextGameAfter + std::chrono::seconds(1)) {
        if (const std::string over = SetOverText(); !over.empty()) {
            EndSession(over);
            return;
        }
    }
    if (role_ == Role::Player) {
        lobby::Message ps;
        ps.type = lobby::Type::PlayerState;
        ps.seat = OwnSeat();
        Echo(ps, heardTime_, heardTimeAt_);
        Send(ps);
    } else {
        lobby::Message ack;
        ack.type = lobby::Type::InputsAck;
        ack.matchId = watchingMatch_;
        ack.first = static_cast<uint32_t>(std::max(0, specWanted_.load()));
        ack.playedTo = static_cast<uint32_t>(std::max(0, specPlayed_.load()));
        ack.decided = watchingMatch_ && s == State::PostMatch;
        ack.time = Now();
        Send(ack);
    }
    if ((s == State::Loading && selfLoaded_) || (s == State::Running && sessionSyncing_)) Send(LoadedMessage());
}

lobby::Message NetplayDriver::WelcomeMessage() {
    lobby::Message w;
    w.type = lobby::Type::Welcome;
    w.matchId = matchId_;
    w.setup = setup_;
    w.inputDelay = options_.inputDelay;
    w.predictionWindow = options_.predictionWindow;
    return w;
}

lobby::Message NetplayDriver::LoadedMessage() {
    lobby::Message loaded;
    loaded.type = lobby::Type::Loaded;
    loaded.matchId = matchId_;
    loaded.setup = loadedSetup_;
    loaded.stateChecksum = frozenChecksum_;
    return loaded;
}

void NetplayDriver::OnMessage(uint64_t from, const lobby::Message& m) {
    const State s = GetState();
    if (m.protocol != lobby::kProtocol) {
        if (host_ && m.type == lobby::Type::Hello) Reject(from, lobby::RejectReason::ProtocolMismatch);
        return;
    }
    const auto checkFrom = Clock::now();
    const PickVerdict check = MessageCheck(m, pickTables_);
    if (const auto took = Clock::now() - checkFrom; took > std::chrono::milliseconds(50)) {
        Log("netplay: checking a message's picks took %lld ms", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
    }
    if (check.kind == PickVerdict::Kind::NotYet) return;
    if (check.kind == PickVerdict::Kind::Bad) {
        if (from == peer_) RefuseFromPeer(check.why);
        else Log("netplay: dropped a message from %s: %s", Hex64(from).c_str(), check.why.c_str());
        return;
    }
    if (from == peer_ && s != State::Joining) heard_ = Clock::now();
    if (host_) OnHostMessage(from, m, s);
    else OnGuestMessage(from, m, s);
}

void NetplayDriver::OnHostMessage(uint64_t from, const lobby::Message& m, State s) {
    Watcher* w = nullptr;
    std::unique_lock lock(mutex_);
    if (auto it = watchers_.find(from); it != watchers_.end()) {
        w = &it->second;
        w->heard = Clock::now();
    }
    lock.unlock();
    switch (m.type) {
        case lobby::Type::Hello: {
            if (options_.udp && m.nonce != nonce_) {
                Reject(from, lobby::RejectReason::WrongCode);
                break;
            }
            if (m.build != build_) {
                Reject(from, lobby::RejectReason::BuildMismatch);
                break;
            }
            lobby::Message accept;
            accept.type = lobby::Type::Accept;
            accept.name = name_;
            accept.reports = ReportsOn();
            if (from == peer_) {
                NotePeerReports(m.reports);
                accept.role = Role::Player;
                SendTo(from, accept);
                break;
            }
            if (w) {
                accept.role = Role::Spectator;
                SendTo(from, accept);
                break;
            }
            const bool seatHolder = !ticket_.seatToken || m.seatToken == ticket_.seatToken;
            if (m.role == Role::Player && !seatHolder) Log("netplay: %s has no seat token for P2: watches", m.name.c_str());
            if (m.role == Role::Player && seatHolder && !peer_ && s != State::Ended) {
                peer_ = from;
                heard_ = Clock::now();
                {
                    std::lock_guard l(mutex_);
                    seats_[1] = {};
                    seats_[1].present = true;
                    seats_[1].name = m.name;
                }
                Log("netplay: %s joined from %s", m.name.c_str(), Hex64(from).c_str());
                peerReports_ = -1;
                NotePeerReports(m.reports);
                if (s == State::Hosting) SetState(State::Lobby);
                accept.role = Role::Player;
                SendTo(from, accept);
                break;
            }
            const bool tableSet = ticket_.after == AfterMatch::Set && weyve_;
            const bool isKeeper = tableSet && IsKeeperMember(from);
            if (tableSet && !isKeeper) {
                Log("netplay: %s asked to watch; table sets are watched through the keeper's relay", m.name.c_str());
                Reject(from, lobby::RejectReason::ViaKeeper);
                break;
            }
            std::lock_guard l(mutex_);
            if (!options_.allowSpectators || (!isKeeper && watchers_.size() >= kMaxSpectators)) {
                Reject(from, options_.allowSpectators ? lobby::RejectReason::Busy : lobby::RejectReason::NoSpectators);
                break;
            }
            Watcher& nw = watchers_[from];
            nw.name = m.name;
            nw.heard = Clock::now();
            nw.keeper = isKeeper;
            Log("netplay: %s watches (from %s)%s", m.name.c_str(), Hex64(from).c_str(), isKeeper ? ": the keeper's relay" : "");
            accept.role = Role::Spectator;
            SendTo(from, accept);
            break;
        }
        case lobby::Type::PlayerState:
            if (from != peer_) break;
            Heard(m.time);
            TakeEcho(m);
            {
                std::lock_guard l(mutex_);
                lobby::Seat& seat = seats_[1];
                seat.hasPick = m.seat.hasPick;
                seat.pick = m.seat.pick;
                seat.ready = m.seat.ready && m.seat.choice == Choice::None;
                seat.preloaded = m.seat.preloaded;
                seat.choice = m.seat.choice;
                seat.inputDelay = m.seat.inputDelay;
                seat.fps = m.seat.fps;
                seat.link = m.seat.link;
                if (seat.screen != m.seat.screen || seat.row != m.seat.row || seat.hover != m.seat.hover) beatNow_ = true;
                seat.screen = m.seat.screen;
                seat.row = m.seat.row;
                seat.hover = m.seat.hover;
            }
            break;
        case lobby::Type::Loaded:
            if (from == peer_ && m.matchId == matchId_) {
                {
                    std::lock_guard l(mutex_);
                    peerLoadedSetup_ = m.setup;
                    peerChecksum_ = m.stateChecksum;
                }
                peerLoaded_ = true;
            } else if (w && (m.matchId == matchId_ || KeptLoaded(m.matchId))) {
                if (m.matchId != matchId_) {
                    if (const auto loaded = KeptLoaded(m.matchId)) SendTo(from, *loaded);
                }
                std::lock_guard l(mutex_);
                if (w->match != m.matchId) {
                    w->streaming = false;
                    w->wanted = 0;
                    w->sentThrough = -1;
                }
                w->match = m.matchId;
                w->loaded = true;
                w->checksum = m.stateChecksum;
            }
            break;
        case lobby::Type::InputsAck:
            if (w) {
                std::lock_guard l(mutex_);
                w->heardTime = m.time;
                w->heardTimeAt = Clock::now();
                if (!m.matchId) {
                    if (w->streaming) Log("netplay: spectator %s finished watching", w->name.c_str());
                    w->streaming = false;
                } else if (streams_.count(m.matchId)) {
                    if (!w->streaming || w->match != m.matchId) {
                        w->streaming = true;
                        w->match = m.matchId;
                        w->sentThrough = static_cast<int>(m.first) - 1;
                        w->progressAt = Clock::now();
                        Log("netplay: spectator %s is watching from frame %u", w->name.c_str(), m.first);
                    }
                    if (static_cast<int>(m.first) > w->wanted) {
                        w->wanted = static_cast<int>(m.first);
                        w->progressAt = Clock::now();
                    }
                }
            }
            break;
        case lobby::Type::Bye:
            if (from == peer_ && s != State::Ended && s != State::Idle) {
                Log("netplay: the other player left%s%s%s", m.text.empty() ? "" : ": ", m.text.c_str(), m.forfeit ? " (a forfeit)" : "");
                peerLeft_ = true;
                if (m.forfeit && TookForfeit(1)) break;
                const std::string over = SetOverText();
                EndSession(!over.empty() ? over : m.text.empty() ? "the other player left"
                                                                 : m.text);
            } else if (w) {
                std::lock_guard l(mutex_);
                Log("netplay: spectator %s left", w->name.c_str());
                watchers_.erase(from);
            }
            break;
        default:
            break;
    }
}

void NetplayDriver::OnGuestMessage(uint64_t from, const lobby::Message& m, State s) {
    if (from != peer_) return;
    switch (m.type) {
        case lobby::Type::Accept:
            if (s != State::Joining) break;
            heard_ = Clock::now();
            role_ = m.role;
            {
                std::lock_guard l(mutex_);
                seats_[0].name = m.name;
            }
            Log("netplay: in %s's session as %s", m.name.c_str(), role_ == Role::Player ? "P2" : "a spectator");
            NotePeerReports(m.reports);
            if (role_ == Role::Spectator && options_.mashSeed) options_.mashSeed = 0;
            SetState(State::Lobby);
            break;
        case lobby::Type::Reject:
            if (s == State::Joining) {
                Log("netplay: rejected: %s", RejectText(m.reason));
                EndSession(std::string("rejected: ") + RejectText(m.reason));
            }
            break;
        case lobby::Type::SessionState: {
            Heard(m.time);
            TakeEcho(m);
            {
                std::lock_guard l(mutex_);
                for (int i = 0; i < kNumPlayers; i++) seats_[i] = m.seats[i];
                played_ = m.played;
                spectators_ = m.spectators;
                stageRule_ = m.stage;
            }
            stageLocked_ = m.stageLocked;
            if (m.phase == SessionPhase::Lobby && s >= State::Jumping && s <= State::PostMatch && next_ == Next::None) {
                Log("netplay: back to the lobby");
                next_ = Next::Lobby;
            }
            break;
        }
        case lobby::Type::Welcome: {
            if (m.matchId == matchId_ || s == State::Joining || s == State::Connecting || s == State::Ended) break;
            if (role_ == Role::Spectator) {
                {
                    std::lock_guard l(mutex_);
                    if (std::none_of(matchList_.begin(), matchList_.end(), [&](const lobby::MatchEntry& e) { return e.id == m.matchId; })) {
                        lobby::MatchEntry e;
                        e.id = m.matchId;
                        e.setup = m.setup;
                        matchList_.push_back(e);
                    }
                }
                if (s == State::Jumping || s == State::Loading || s == State::Running || switchNow_) break;
                if (WatchNext()) break;
            }
            if (role_ == Role::Player) {
                std::string here, there;
                if (LacksVersion(m.setup, &here, &there)) {
                    EndSessionFor(here, there);
                    break;
                }
            }
            {
                std::lock_guard l(mutex_);
                setup_ = m.setup;
            }
            matchId_ = m.matchId;
            options_.predictionWindow = kPredictionWindow;
            Log("netplay: match %s; %s", Hex64(matchId_).c_str(), DescribeSetup(setup_).c_str());
            next_ = Next::Jump;
            break;
        }
        case lobby::Type::Loaded:
            if (m.matchId == matchId_) {
                {
                    std::lock_guard l(mutex_);
                    peerLoadedSetup_ = m.setup;
                    peerChecksum_ = m.stateChecksum;
                }
                peerLoaded_ = true;
            }
            break;
        case lobby::Type::Matches:
            if (role_ == Role::Spectator) {
                uint64_t first = 0;
                {
                    std::lock_guard l(mutex_);
                    std::vector<lobby::MatchEntry>& list = sliceList_;
                    if (list.size() != m.sliceOf) {
                        std::vector<lobby::MatchEntry> fresh(m.sliceOf);
                        list = std::move(fresh);
                    }
                    for (size_t i = 0; i < m.matches.size() && m.sliceAt + i < list.size(); i++) list[m.sliceAt + i] = m.matches[i];
                    if (std::all_of(list.begin(), list.end(), [](const lobby::MatchEntry& e) { return e.id != 0; })) {
                        matchList_ = list;
                        if (ticket_.watchFromFirst && !watchedFirst_ && !matchList_.empty()) first = matchList_.front().id;
                    }
                }
                const State st = GetState();
                if (first && (st == State::Lobby || st == State::Running || st == State::PostMatch || st == State::Jumping || st == State::Loading)) {
                    watchedFirst_ = true;
                    if (first != watchingMatch_) {
                        Log("netplay: watching the table from its set's first game (%s)", Hex64(first).c_str());
                        ChooseMatch(first);
                    }
                }
            }
            break;
        case lobby::Type::Inputs:
            if (role_ == Role::Spectator && m.matchId && m.matchId == watchingMatch_) {
                std::lock_guard l(mutex_);
                inbound_.push_back(m);
            }
            break;
        case lobby::Type::Bye:
            if (s != State::Ended && s != State::Idle) {
                Log("netplay: the host left%s%s%s", m.text.empty() ? "" : ": ", m.text.c_str(), m.forfeit ? " (a forfeit)" : "");
                peerLeft_ = true;
                if (m.forfeit && TookForfeit(0)) break;
                const std::string over = SetOverText();
                EndSession(!over.empty() ? over : !m.text.empty()     ? m.text
                                              : role_ == Role::Player ? "the other player left"
                                                                      : "the host left");
            }
            break;
        default:
            break;
    }
}

bool NetplayDriver::IsKeeperMember(uint64_t from) {
    if (!weyve_) return false;
    const WeyveRoomView v = weyve_->View();
    for (const WeyveMember& member : v.members) {
        if (member.id != from) continue;
        const auto p = member.data.find(keeper::kProof);
        return p != member.data.end() && RoomsIsKeeper(v.room, member.id, p->second);
    }
    return false;
}

void NetplayDriver::Reject(uint64_t to, lobby::RejectReason reason) {
    lobby::Message m;
    m.type = lobby::Type::Reject;
    m.reason = reason;
    SendTo(to, m);
}

} // namespace sfv::mod::netplay
