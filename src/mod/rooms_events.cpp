// The server's events, the keeper's answers, and the room as it shows.

#include "mod/rooms_internal.h"

#include "core/text.h"
#include "core/display_name.h"

#include <algorithm>
#include <cstdlib>

namespace sfv::mod {

using namespace rooms;

namespace rooms {

void Client::ForgetTables(bool removed) {
    if (shared.playing.table >= 0 && (!shared.playing.begun || removed)) {
        Log(removed ? "rooms: removed from the room during table %d's set: leaving the session"
                    : "rooms: left the room before table %d's match began: leaving the session",
            shared.playing.table + 1);
        abortSession = true;
    }
    shared.playing = {};
    shared.tickets.clear();
    callSeen.clear();
    tableLogged.clear();
    if (shared.watchTable >= 0) Log("rooms: out of the room: no longer watching table %d", shared.watchTable + 1);
    if (shared.watchSession && NetViewNow().spectator) abortSession = true;
    shared.watchTable = -1;
    shared.view.watching = -1;
    shared.watchCode.clear();
    shared.watchTickets.clear();
    shared.watchSession = false;
    shared.view.tableViews.clear();
    shared.view.tableNotice.clear();
}

void Client::SetNotice(RoomsView::Notice kind, const std::string& notice) {
    shared.view.noticeKind = kind;
    shared.view.notice = notice;
}

void Client::BackToLobby(RoomsView::Notice kind, const std::string& notice, bool inRoom, bool removed) {
    ForgetTables(removed);
    if (!notice.empty()) {
        SetNotice(kind, notice);
        Log("rooms: %s", notice.c_str());
    }
    if (inRoom) svc->LeaveRoom();
    shared.view.room.clear();
    shared.view.members.clear();
    joinedAt = -1;
    ToBrowsing();
}

void Client::Line(ChatLine::Kind kind, uint32_t from, const std::string& shownName, const std::string& text, const std::string& realName) {
    const std::string name = core::DisplayName(shownName);
    const std::string censored = from && shared.view.isPublic && !words.empty() ? keeper::CensorChat(text, words) : text;
    shared.view.chat.push_back({kind, from, name, text, realName.empty() ? name : core::DisplayName(realName), censored});
    if (shared.view.chat.size() > kChatKept) shared.view.chat.erase(shared.view.chat.begin());
}

std::string Client::MaskedName(uint32_t id, const std::string& name) const {
    const auto it = mask.find(id);
    return it != mask.end() ? TRF(Str::rooms_masked_name, static_cast<int>(it->second)) : name;
}

void Client::OnEvent(const WeyveEvent& e, const WeyveRoomView& v) {
    switch (e.type) {
        case WeyveEvent::Type::RoomList: {
            const Listed which = listed;
            listed = Listed::None;
            if (which == Listed::Desks) OnDesks();
            else if (which == Listed::Rooms) OnRooms();
            else if (which == Listed::AllBuilds) {
                std::string other;
                for (const WeyveListedRoom& r : svc->RoomList()) {
                    const auto build = r.listing.find(keeper::kBuild);
                    if (build != r.listing.end() && build->second != std::to_string(lobby::kProtocol)) other = build->second;
                }
                if (other != shared.view.otherBuild && !other.empty()) Log("rooms: the server has build %s's desk, not %u's", other.c_str(), lobby::kProtocol);
                shared.view.otherBuild = other;
            }
            break;
        }
        case WeyveEvent::Type::RoomJoined:
            svc->SetMemberData(keeper::kVersion, MyVersion());
            if (deskJoining || e.text == shared.view.lobby) {
                deskJoining = false;
                Set(State::Lobby);
            } else {
                shared.view.room = e.text;
                shared.view.chat.clear();
                names.clear();
                lastOwner = 0;
                joinedAt = NowMs();
                pings.clear();
                pingLogged.clear();
                mask.clear();
                shared.tickets.clear();
                toldMasked = false;
                Set(State::Room);
                if (!chara.empty()) svc->SetMemberData("ch", chara);
                if (!skills.empty()) svc->SetMemberData("vs", skills);
                svc->SetMemberData("net", net);
            }
            break;
        case WeyveEvent::Type::RoomError:
            if (e.error == WeyveEvent::Error::RateLimited) {
                listed = Listed::None;
                break;
            }
            if (deskJoining) {
                Log("rooms: cannot join desk %s (%s); looking again", shared.view.lobby.c_str(), e.text.c_str());
                deskJoining = false;
                staleLobbies[shared.view.lobby] = NowMs() + kStaleLobbyMs;
                shared.view.lobby.clear();
                nextDesksMs = 0;
                break;
            }
            if (shared.view.state != State::Joining) break;
            if (e.error == WeyveEvent::Error::Other && e.text == "not in a room") break;
            if (e.error == WeyveEvent::Error::NoSuchRoom) BackToLobby(RoomsView::Notice::NoRoom, TR(Str::rooms_notice_no_room), false);
            else if (e.error == WeyveEvent::Error::Closed) BackToLobby(RoomsView::Notice::Full, TR(Str::rooms_notice_full), false);
            else if (e.error == WeyveEvent::Error::Banned) BackToLobby(RoomsView::Notice::Removed, TR(Str::rooms_notice_removed_by_owner), false);
            else BackToLobby(RoomsView::Notice::CannotJoin, TRF(Str::rooms_notice_cannot_join, e.text.c_str()), false);
            break;
        case WeyveEvent::Type::Removed:
            if (shared.view.state == State::Room) BackToLobby(RoomsView::Notice::Removed, TR(Str::rooms_notice_removed), false, true);
            break;
        case WeyveEvent::Type::ServerLost:
            SetNotice(RoomsView::Notice::ServerLost, e.text);
            Set(State::Failed);
            break;
        case WeyveEvent::Type::Chat:
            if (shared.view.state == State::Room) {
                const WeyveMember* m = v.Member(e.peer);
                const std::string real = m ? m->name : "?";
                Line(ChatLine::Kind::Chat, e.peer, MaskedName(e.peer, real), e.text, real);
            }
            break;
        default:
            break;
    }
}

void Client::Answers() {
    Packet p;
    while (svc->Receive(p)) {
        bool pong = false;
        uint64_t clock = 0;
        if (keeper::ParsePing(p.data.data(), p.data.size(), &pong, &clock)) {
            if (!pong) {
                uint8_t reply[keeper::kPingSize];
                keeper::FormatPing(true, clock, reply);
                svc->Send(p.from, reply, sizeof(reply));
            } else {
                const int ms = static_cast<int>((ClockUs() - static_cast<int64_t>(clock)) / 1000);
                if (clock != 0 && ms >= 0 && ms < kPingMaxMs) {
                    auto& samples = pings[static_cast<uint32_t>(p.from)];
                    samples.push_back(ms);
                    if (samples.size() > kPingSamples) samples.pop_front();
                    if (samples.size() == 1 || (samples.size() == kPingSamples && !pingLogged.count(static_cast<uint32_t>(p.from)))) {
                        if (samples.size() == kPingSamples) pingLogged.insert(static_cast<uint32_t>(p.from));
                        Log("rooms: ping to member %u: %d ms (%s)", static_cast<unsigned>(p.from),
                            *std::min_element(samples.begin(), samples.end()),
                            samples.size() == 1 ? "first reading" : "lowest of the last 4");
                    }
                }
            }
            continue;
        }
        keeper::Answer a;
        if (!keeper::ParseAnswer(p.data.data(), p.data.size(), &a)) continue;
        if (a.kind == "peek") {
            if (a.seq != peekSeq) continue;
            if (!a.ok) {
                Log("rooms: peek %s refused: %s", peekCode.c_str(), a.text.c_str());
                peekSeq = 0;
                svc->SetMemberData(keeper::kAck, std::to_string(a.seq));
                shared.view.peek = keeper::RoomDetail{};
                continue;
            }
            int part = 0, parts = 0;
            std::string records;
            if (!keeper::ReadDetailPart(a.text, &part, &parts, &records)) continue;
            if (static_cast<int>(peekParts.size()) != parts) peekParts.assign(parts, std::string("\x01"));
            peekParts[part - 1] = records;
            if (std::any_of(peekParts.begin(), peekParts.end(), [](const std::string& r) { return r == "\x01"; })) continue;
            svc->SetMemberData(keeper::kAck, std::to_string(a.seq));
            keeper::RoomDetail d;
            d.code = peekCode;
            const bool joined = keeper::JoinRoomDetail(peekParts, &d);
            if (peekLogged != peekCode) {
                peekLogged = peekCode;
                Log("rooms: peek %s answered: %s, %zu tables, %zu people", peekCode.c_str(), joined ? "ok" : "unreadable", d.tables.size(),
                    d.people.size());
            }
            for (auto& person : d.people) person.name = core::DisplayName(person.name);
            if (joined) shared.view.peek = std::move(d);
            peekSeq = 0;
            continue;
        }
        svc->SetMemberData(keeper::kAck, std::to_string(a.seq));
        if (a.kind == "ticket") {
            keeper::TableTicket t;
            if (keeper::ParseTicket(a.text, &t) && shared.view.state == State::Room &&
                std::none_of(shared.tickets.begin(), shared.tickets.end(), [&](const keeper::TableTicket& k) { return k.token == t.token && k.host == t.host; }) &&
                !(shared.playing.table == t.table && shared.playing.host == t.host && lastTicketToken == t.token)) {
                Log("rooms: ticket for table %d: %s", t.table + 1, t.host ? "open the match room" : ("join " + t.code).c_str());
                lastTicketToken = t.token;
                shared.tickets.push_back(t);
            }
            continue;
        }
        if (a.kind == "again") {
            Log("rooms: the keeper on the next set: %s %s", a.ok ? "ok" : "no", a.text.c_str());
            Rules rules;
            if (a.ok && ParseRules(a.text, &rules)) shared.playing.rules = rules;
            NetAgainAnswer(a.ok, a.text);
            continue;
        }
        if (a.seq != pendingSeq) continue;
        pendingSeq = 0;
        Log("rooms: the keeper answers #%u %s: %s %s", a.seq, a.kind.c_str(), a.ok ? "ok" : "no", a.text.c_str());
        if (!a.ok) {
            SetNotice(RoomsView::Notice::Refused, a.text);
            Set(State::Lobby);
            continue;
        }
        target = a.text;
        svc->LeaveRoom();
        svc->JoinRoom(a.text);
        Set(State::Joining);
    }
}

void Client::Mirror(const WeyveRoomView& v) {
    shared.view.self = v.self;
    const uint32_t keeperId = KeeperOf(v);
    const bool wasKeeper = shared.view.keeper;
    shared.view.keeper = keeperId && v.host == keeperId;
    if (wasKeeper != shared.view.keeper && (shared.view.state == State::Lobby || shared.view.state == State::Room))
        Log("rooms: %s %s", v.room.c_str(), shared.view.keeper ? "is kept" : "is paused (its host is not the keeper)");
    if (keeperId && v.host == v.self && v.self != keeperId && NowMs() - lastHandMs >= kHandBackEveryMs) {
        lastHandMs = NowMs();
        Log("rooms: handing host of %s back to the keeper (%u)", v.room.c_str(), keeperId);
        svc->TransferHost(keeperId);
    }
    if (shared.view.state == State::Lobby) {
        if (shared.view.keeper) lobbyUnkeptSince = -1;
        else if (lobbyUnkeptSince < 0) lobbyUnkeptSince = NowMs();
        else if (NowMs() - lobbyUnkeptSince >= kUnkeptLobbyMs) {
            Log("rooms: lobby channel %s has had no keeper for %lld s; looking for another", shared.view.lobby.c_str(),
                static_cast<long long>(kUnkeptLobbyMs / 1000));
            staleLobbies[shared.view.lobby] = NowMs() + kStaleLobbyMs;
            lobbyUnkeptSince = -1;
            svc->LeaveRoom();
            shared.view.lobby.clear();
            shared.view.publicRooms.clear();
            shared.view.keeper = false;
            ToBrowsing();
        }
        return;
    }
    lobbyUnkeptSince = -1;
    if (shared.view.state != State::Room) return;
    auto data = [&](const char* key) {
        const auto it = v.data.find(key);
        return it != v.data.end() ? it->second : std::string();
    };
    if (joinedAt >= 0) {
        if (data(keeper::kKind) == keeper::kKindCustom) {
            joinedAt = -1;
            Log("rooms: in custom room %s", shared.view.room.c_str());
        } else if (NowMs() - joinedAt > kVerifyMs) {
            BackToLobby(RoomsView::Notice::NoRoom, TR(Str::rooms_notice_no_room), true);
            return;
        }
    }
    if (shared.view.keeper && shared.view.state == State::Room) {
        const std::string theirs = data(keeper::kVersion);
        if (!theirs.empty() && theirs != MyVersion()) {
            Log("rooms: %s plays Polarity %s, this game %s: leaving it", shared.view.room.c_str(), theirs.c_str(), MyVersion().c_str());
            BackToLobby(RoomsView::Notice::OtherVersion, OtherVersionNotice(theirs), true);
            return;
        }
    }
    if (shared.view.keeper) {
        if (!keeper::ParseTitle(data(keeper::kTitle), &shared.view.title)) shared.view.title = keeper::RoomTitle{};
        shared.view.isPublic = data(keeper::kPublic) == "1";
        shared.view.tables = std::atoi(data(keeper::kTables).c_str());
        shared.view.owner = static_cast<uint32_t>(std::strtoul(data(keeper::kOwner).c_str(), nullptr, 10));
        shared.view.tableRules.clear();
        shared.view.tableViews.clear();
        for (int t = 0; t < std::min(shared.view.tables, keeper::kMaxTables); t++) {
            shared.view.tableRules.push_back(data(keeper::TableKey(t).c_str()));
            TableView tv;
            keeper::ParseTable(data(keeper::StateKey(t).c_str()), &tv.s);
            tv.queue = keeper::ParseIds(data(keeper::QueueKey(t).c_str()));
            tv.rules = shared.view.tableRules.back();
            if (callSeen.size() <= static_cast<size_t>(t)) callSeen.resize(t + 1);
            if (tv.s.phase == keeper::TablePhase::Call && callSeen[t].first != tv.s.call) {
                callSeen[t] = {tv.s.call, NowMs()};
                if (tv.s.seat[0] == shared.view.self || tv.s.seat[1] == shared.view.self)
                    Log("rooms: table %d calls this player (call %u, %u s%s)", t + 1, tv.s.call, tv.s.callSeconds,
                        tv.s.ready[tv.s.seat[0] == shared.view.self ? 0 : 1] ? ", already ready" : "");
            }
            tv.callSeenMs = callSeen[t].second;
            if (tableLogged.size() <= static_cast<size_t>(t)) tableLogged.resize(t + 1);
            std::string state = keeper::FormatTable(tv.s) + " q=" + keeper::FormatIds(tv.queue) + " rules=" + tv.rules;
            if (state != tableLogged[t]) {
                Log("rooms: table %d: %s", t + 1, state.c_str());
                tableLogged[t] = std::move(state);
            }
            shared.view.tableViews.push_back(std::move(tv));
        }
    }
    std::vector<RoomMember> members;
    std::map<uint32_t, std::string> now;
    if (shared.view.keeper) mask = keeper::ParseMask(data(keeper::kMask));
    if (const std::string hash = data(keeper::kWordListHash); shared.view.isPublic && !hash.empty() && hash != wordsHash) {
        std::vector<std::string> values;
        for (int i = 0; i < keeper::kWordListKeys; i++) {
            const std::string value = data(keeper::WordListKey(i).c_str());
            if (!value.empty()) values.push_back(value);
        }
        words = keeper::UnpackWordList(values);
        wordsHash = hash;
        Log("rooms: word list %s: %zu entries", hash.c_str(), words.size());
    }
    for (const WeyveMember& m : v.members) {
        if (m.id == keeperId) continue;
        RoomMember r;
        r.id = m.id;
        const auto masked = mask.find(m.id);
        r.masked = masked != mask.end();
        const std::string maskedName = MaskedName(m.id, m.name);
        r.name = core::DisplayName(g_wordFilter ? maskedName : m.name);
        const auto ch = m.data.find("ch");
        if (ch != m.data.end()) r.chara = ch->second;
        const auto cc = m.data.find("@cc");
        if (cc != m.data.end()) r.country = keeper::CleanCountry(cc->second);
        const auto vs = m.data.find("vs");
        if (vs != m.data.end()) {
            if (vs->second == "r") r.vskill = r.vtrigger = -1;
            else if (vs->second.size() == 2 && vs->second[0] >= '1' && vs->second[0] <= '9' && vs->second[1] >= '1' && vs->second[1] <= '9') {
                r.vskill = vs->second[0] - '0';
                r.vtrigger = vs->second[1] - '0';
            }
        }
        const auto netData = m.data.find("net");
        if (netData != m.data.end()) r.link = netData->second == "w" ? 1 : netData->second == "f" ? 2
                                                                                                  : 0;
        const auto watch = m.data.find("w");
        if (watch != m.data.end() && !watch->second.empty()) r.watching = std::atoi(watch->second.c_str()) - 1;
        r.owner = m.id == shared.view.owner;
        const auto ping = pings.find(m.id);
        if (ping != pings.end() && !ping->second.empty() && m.id != v.self) {
            r.pingMs = *std::min_element(ping->second.begin(), ping->second.end());
            r.relayed = svc->PeerPath(m.id) == WeyveService::LinkPath::Relayed;
        }
        if (r.masked && m.id == v.self && !toldMasked) {
            toldMasked = true;
            Line(ChatLine::Kind::Note, 0, "", TRF(Str::rooms_chat_masked, maskedName.c_str()));
            Log("rooms: this player's name is masked as %s (the word filter)", maskedName.c_str());
        }
        members.push_back(r);
        now[m.id] = r.name;
    }
    if (joinedAt < 0) {
        for (const auto& [id, name] : now) {
            if (!names.count(id) && !names.empty()) {
                Line(ChatLine::Kind::Joined, 0, "", TRF(Str::rooms_chat_joined, name.c_str()));
                Log("rooms: %s (%u) joined %s, %zu members", name.c_str(), id, shared.view.room.c_str(), now.size());
            }
        }
        for (const auto& [id, name] : names) {
            if (!now.count(id) && id != keeperId) {
                Line(ChatLine::Kind::Left, 0, "", TRF(Str::rooms_chat_left, name.c_str()));
                Log("rooms: %s (%u) left %s, %zu members", name.c_str(), id, shared.view.room.c_str(), now.size());
            }
        }
        if (shared.view.owner && shared.view.owner != lastOwner && lastOwner) {
            const auto it = now.find(shared.view.owner);
            Log("rooms: %s (%u) owns %s now", it != now.end() ? it->second.c_str() : "?", shared.view.owner, shared.view.room.c_str());
            Line(ChatLine::Kind::Owner, 0, "", shared.view.owner == v.self ? std::string(TR(Str::rooms_chat_you_own)) : TRF(Str::rooms_chat_owns, it != now.end() ? it->second.c_str() : "?"));
        }
        lastOwner = shared.view.owner;
        names = now;
    }
    shared.view.members = std::move(members);
}

} // namespace rooms

} // namespace sfv::mod
