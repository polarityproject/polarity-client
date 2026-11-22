// Pings, table requests, watching, a table set's reports.

#include "mod/rooms_internal.h"

#include <algorithm>

namespace sfv::mod {

using namespace rooms;

namespace rooms {

void Client::DeskLink(uint32_t keeperId) {
    if (view.state != State::Lobby || !keeperId) {
        deskDialFrom = 0;
        return;
    }
    if (svc->PeerPath(keeperId) != WeyveService::LinkPath::None) {
        if (deskDialFrom) Log("rooms: link to the desk's keeper up after %lld ms", static_cast<long long>(NowMs() - deskDialFrom));
        deskDialFrom = 0;
        return;
    }
    if (NowMs() - deskDialAt < kDeskDialEveryMs) return;
    if (!deskDialFrom) deskDialFrom = NowMs();
    deskDialAt = NowMs();
    uint8_t dial[keeper::kPingSize];
    keeper::FormatPing(false, 0, dial);
    svc->Send(keeperId, dial, sizeof(dial));
}

void Client::Pings(const WeyveRoomView& v, uint32_t keeperId) {
    if (view.state != State::Room || NowMs() - lastPingMs < kPingEveryMs) return;
    lastPingMs = NowMs();
    uint8_t ping[keeper::kPingSize], building[keeper::kPingSize];
    keeper::FormatPing(false, static_cast<uint64_t>(ClockUs()), ping);
    keeper::FormatPing(false, 0, building);
    for (const WeyveMember& m : v.members) {
        if (m.id == v.self || m.id == keeperId) continue;
        const bool up = svc->PeerPath(m.id) != WeyveService::LinkPath::None;
        svc->Send(m.id, up ? ping : building, sizeof(ping));
    }
}

void Client::TableAsks() {
    for (auto& [kind, args] : tableAsks) {
        if (view.state != State::Room) continue;
        const keeper::Request r{++seq, kind, args};
        svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
        Log("rooms: asks the keeper: %s", keeper::FormatRequest(r).c_str());
    }
    tableAsks.clear();
}

void Client::Watch() {
    if (watchTable < 0) return;
    for (const TableView& tv : view.tableViews) {
        const bool mine = tv.s.seat[0] == view.self || tv.s.seat[1] == view.self;
        if (mine && tv.s.phase != keeper::TablePhase::Open) {
            Log("rooms: called to play: no longer watching table %d", watchTable + 1);
            if (watchSession && NetViewNow().spectator) abortSession = true;
            watchTable = -1;
            view.watching = -1;
            watchSession = false;
            watchTickets.clear();
            svc->SetMemberData("w", "");
            return;
        }
    }
    if (watchTable >= static_cast<int>(view.tableViews.size()) || !view.keeper) return;
    const TableView& tv = view.tableViews[watchTable];
    const bool on = tv.s.phase == keeper::TablePhase::Connecting || tv.s.phase == keeper::TablePhase::Playing;
    if (!on || tv.s.watch.empty() || tv.s.watch == watchCode) return;
    if (watchSession) {
        const NetView nv = NetViewNow();
        if (nv.spectator && nv.phase != NetView::Phase::Idle && nv.phase != NetView::Phase::Ended) return;
    }
    watchCode = tv.s.watch;
    watchTickets.push_back({watchTable, tv.s.watch, tv.rules});
    Log("rooms: table %d's set: watching through the keeper's relay room %s", watchTable + 1, tv.s.watch.c_str());
}

void Client::Report() {
    if (playing.table < 0) return;
    const NetView nv = NetViewNow();
    const bool live = nv.phase != NetView::Phase::Idle && nv.phase != NetView::Phase::Ended;
    const std::string t = std::to_string(playing.table);
    auto ask = [&](const std::string& kind, std::vector<std::string> args) {
        const keeper::Request r{++seq, kind, std::move(args)};
        svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
        Log("rooms: tells the keeper: %s", keeper::FormatRequest(r).c_str());
    };
    const bool matchBegun = nv.phase == NetView::Phase::Jumping || nv.phase == NetView::Phase::Loading ||
                            nv.phase == NetView::Phase::Running || nv.phase == NetView::Phase::PostMatch;
    if (matchBegun) playing.begun = true;
    if (!matchBegun && !playing.begun && playing.table < static_cast<int>(view.tableViews.size()) && NowMs() - playing.at > kTableMovedOnMs) {
        const keeper::TableState& s = view.tableViews[playing.table].s;
        const bool seated = s.seat[0] == view.self || s.seat[1] == view.self;
        const bool stillOn = seated && (s.phase == keeper::TablePhase::Connecting || s.phase == keeper::TablePhase::Playing);
        if (!stillOn && view.keeper) {
            Log("rooms: table %d moved on before its match began: leaving the session", playing.table + 1);
            view.tableNotice = s.seat[0] == view.self || s.seat[1] == view.self || !seated
                                   ? "Your opponent left before the match started."
                                   : "The match did not start.";
            abortSession = true;
            playing = {};
            return;
        }
    }
    if (live) {
        playing.started = true;
        playing.wins[0] = nv.seats[0].wins;
        playing.wins[1] = nv.seats[1].wins;
    }
    if (!playing.started) {
        if (NowMs() - playing.at > kSessionStartMs) {
            Log("rooms: table %d's session never started", playing.table + 1);
            view.tableNotice = "Couldn't connect to your opponent, so the match didn't start.";
            ask("result", {t, "0", "0"});
            playing = {};
        }
        return;
    }
    if (playing.host && !playing.codeSent && !nv.code.empty()) {
        playing.codeSent = true;
        ask("code", {t, nv.code, nv.build});
    }
    if (playing.table < static_cast<int>(view.tableViews.size())) {
        const keeper::TableState& s = view.tableViews[playing.table].s;
        NetSetAgainOffered(s.again && s.phase == keeper::TablePhase::Playing);
    }
    if (live && playing.host && nv.againAsk && !playing.againAsked) {
        playing.againAsked = true;
        ask("again", {t, std::to_string(nv.seats[0].wins), std::to_string(nv.seats[1].wins)});
    }
    if (!nv.againAsk) playing.againAsked = false;
    if (live && playing.host && (nv.seats[0].wins != playing.sent[0] || nv.seats[1].wins != playing.sent[1])) {
        playing.sent[0] = nv.seats[0].wins;
        playing.sent[1] = nv.seats[1].wins;
        ask("score", {t, std::to_string(nv.seats[0].wins), std::to_string(nv.seats[1].wins)});
    }
    if (!live) {
        if (const int seat = NetTakeForfeit(); seat >= 0) {
            Log("rooms: table %d's set: P%d left it (a forfeit)", playing.table + 1, seat + 1);
            ask("forfeit", {t, std::to_string(seat)});
            playing = {};
            return;
        }
        uint16_t wins[2] = {nv.seats[0].wins, nv.seats[1].wins};
        if (SetWinner(playing.rules, wins) < 0) {
            wins[0] = playing.wins[0];
            wins[1] = playing.wins[1];
        }
        const bool done = SetWinner(playing.rules, wins) >= 0;
        if (!done) {
            view.tableNotice = std::string(playing.begun ? "The set did not finish" : "The match did not start") + (nv.result.empty() ? std::string(".") : ": " + nv.result + ".");
            Log("rooms: %s", view.tableNotice.c_str());
        }
        ask("result", {t, done ? std::to_string(wins[0]) : "0", done ? std::to_string(wins[1]) : "0"});
        playing = {};
    }
}

void Client::Asks(const WeyveRoomView& v) {
    (void)v;
    const bool deskNotReady = view.state == State::Browsing || (view.state == State::Lobby && !view.keeper);
    if (createAsk && deskNotReady && NowMs() - std::max(createAskAt, deskJoinAt) < kDeskJoinMs) return;
    if (createAsk) {
        const keeper::RoomSettings s = *createAsk;
        createAsk.reset();
        if (view.state != State::Lobby || !view.keeper) {
            view.notice = "Custom Rooms are unavailable right now";
            Log("rooms: create: not available (%s%s)", RoomsStateName(view.state), view.keeper ? "" : ", no keeper");
        } else {
            const keeper::Request r = keeper::RoomRequest(++seq, s);
            svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
            pendingSeq = r.seq;
            pendingUntil = NowMs() + kAnswerWaitMs;
            view.notice.clear();
            Log("rooms: asking the keeper for a room (#%u: %s, %d tables, %s)", r.seq, keeper::FormatTitle(s.title).c_str(), s.tables,
                s.isPublic ? "public" : "private");
            Set(State::Creating);
        }
    }
    if (joinAsk) {
        const std::string code = *joinAsk;
        joinAsk.reset();
        if ((view.state == State::Lobby || view.state == State::Browsing || view.state == State::Room) && code != view.room) {
            view.notice.clear();
            target = code;
            ForgetTables();
            svc->LeaveRoom();
            view.room.clear();
            view.members.clear();
            svc->JoinRoom(code);
            Log("rooms: joining %s", code.c_str());
            Set(State::Joining);
        }
    }
    if (leaveAsk) {
        leaveAsk = false;
        if (view.state == State::Room) {
            if (playing.table >= 0 && playing.table < static_cast<int>(view.tableViews.size())) {
                const keeper::TableState& s = view.tableViews[playing.table].s;
                const int seat = s.seat[0] == view.self ? 0 : s.seat[1] == view.self ? 1
                                                                                     : -1;
                if (seat >= 0 && (s.phase == keeper::TablePhase::Playing || s.phase == keeper::TablePhase::Connecting)) {
                    const keeper::Request r{++seq, "forfeit", {std::to_string(playing.table), std::to_string(seat)}};
                    svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
                    Log("rooms: leaving during table %d's set: a forfeit (%s)", playing.table + 1, keeper::FormatRequest(r).c_str());
                    abortSession = true;
                }
            }
            Log("rooms: leaving %s", view.room.c_str());
            leftRoom = view.room;
            leftAtMs = NowMs();
            std::erase_if(view.publicRooms, [&](const keeper::ListedRoom& r) { return r.code == leftRoom && r.members <= 1; });
            BackToLobby("", true);
        }
    }
    for (const std::string& text : chatOut) {
        if (view.state == State::Room) svc->SendChat(text);
    }
    chatOut.clear();
}

} // namespace rooms

std::string RoomsAfterSet(int seat, int winnerSeat) {
    std::string out;
    WithClient([&](Client& c) {
        const int t = c.playing.table;
        if (t < 0 || t >= static_cast<int>(c.view.tableViews.size()) || seat < 0 || seat > 1) return false;
        keeper::TableState s = c.view.tableViews[t].s;
        std::vector<uint32_t> queue = c.view.tableViews[t].queue;
        const uint32_t who = s.seat[seat];
        if (!who || !s.seat[1 - seat]) return false;
        const uint8_t games[2] = {s.wins[0], s.wins[1]};
        keeper::ApplySetResult(c.playing.rules, winnerSeat, games, &s, &queue);
        auto nameOf = [&](uint32_t id) {
            for (const RoomMember& m : c.view.members) {
                if (m.id == id) return m.name;
            }
            return std::string("the next player");
        };
        const std::string table = "Table " + std::to_string(t + 1);
        if (s.seat[0] == who || s.seat[1] == who) {
            out = "Stays on " + table;
            const uint32_t other = s.seat[0] == who ? s.seat[1] : s.seat[0];
            const uint32_t next = other ? other : queue.empty() ? 0
                                                                : queue.front();
            if (next) out += " \xC2\xB7 plays " + nameOf(next) + " next";
            return true;
        }
        const auto at = std::find(queue.begin(), queue.end(), who);
        if (at == queue.end()) {
            out = "Back to the room";
            return true;
        }
        static const char* const kNth[] = {"next", "2nd", "3rd"};
        const size_t n = static_cast<size_t>(at - queue.begin());
        out = "Back in the queue \xC2\xB7 " + (n < 3 ? std::string(kNth[n]) : std::to_string(n + 1) + "th");
        return true;
    });
    return out;
}

std::string RoomsPlace(const RoomsView& v, uint32_t id) {
    static const char* const kOrdinal[] = {"1st", "2nd", "3rd"};
    for (size_t t = 0; t < v.tableViews.size(); t++) {
        const TableView& tv = v.tableViews[t];
        const std::string table = "Table " + std::to_string(t + 1) + " \xC2\xB7 ";
        if (tv.s.seat[0] == id || tv.s.seat[1] == id) {
            switch (tv.s.phase) {
                case keeper::TablePhase::Open: return table + "seated";
                case keeper::TablePhase::Call: return table + "next";
                case keeper::TablePhase::After: return table + "set over";
                default: return table + "playing";
            }
        }
        for (size_t k = 0; k < tv.queue.size(); k++) {
            if (tv.queue[k] == id) return table + (k < 3 ? kOrdinal[k] : std::to_string(k + 1) + "th");
        }
    }
    for (const RoomMember& m : v.members) {
        if (m.id == id && m.watching >= 0) return "Watching Table " + std::to_string(m.watching + 1);
    }
    return "Idle";
}

} // namespace sfv::mod
