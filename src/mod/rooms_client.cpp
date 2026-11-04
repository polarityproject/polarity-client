#include "mod/rooms_client.h"

#include "core/lobby.h"
#include "core/match_ticket.h"
#include "core/update_info.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/udp_transport.h"
#include "mod/weyve_service.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace sfv::mod {

namespace {

using State = RoomsView::State;

// The keeper on the rooms server (sfvr-keeper --new-key on the rooms VPS
// prints it). All zeros: none yet; [rooms] keeper_key adds one for tests.
constexpr uint8_t kKeeperPublicKey[64] = {};
constexpr size_t kChatKept = 100;
constexpr int64_t kListAgainMs = 5000;   // no lobby channel yet: look again
constexpr int64_t kAnswerWaitMs = 20000; // the keeper's answer to a request
constexpr int64_t kVerifyMs = 4000;      // a joined room shows it is a custom room within this
constexpr int64_t kHandBackEveryMs = 2000;
constexpr int64_t kPeekEveryMs = 3000;   // the focused room's detail, again while focused
constexpr int64_t kPingEveryMs = 2000;   // each room member

int64_t ClockUs()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<int64_t>(t.QuadPart * 1000000 / f.QuadPart);
}

int64_t NowMs() { return static_cast<int64_t>(GetTickCount64()); }

struct Client {
    std::mutex mutex; // guards view and the asks below
    RoomsView view;
    std::optional<keeper::RoomSettings> createAsk;
    std::optional<std::string> joinAsk;
    bool leaveAsk = false;
    std::vector<std::string> chatOut;
    keeper::ChatLimiter limiter;
    std::string peekWanted; // RoomsPeek
    std::vector<std::pair<std::string, std::vector<std::string>>> tableAsks; // RoomsTable
    std::vector<keeper::TableTicket> tickets; // from the keeper, for the overlay
    struct Playing {
        int table = -1;
        bool host = false;
        Rules rules;
        int64_t at = 0;
        bool started = false, codeSent = false;
        bool begun = false; // the match itself (the jump), not only the session
        int sent[2] = {-1, -1};
    } playing;

    // The thread's own.
    std::unique_ptr<WeyveService> svc;
    std::vector<std::array<uint8_t, 64>> keys;
    std::string chara;
    std::thread thread;
    std::atomic<bool> stop{false};
    uint32_t seq = 0;
    uint32_t pendingSeq = 0;
    int64_t pendingUntil = 0;
    std::string target;      // the room being joined
    int64_t joinedAt = -1;   // into a room that is still to show it is a custom room
    int64_t nextListMs = 0;
    int64_t lastHandMs = -kHandBackEveryMs;
    std::map<std::string, bool> proofs; // "room id proof" -> checked
    std::map<uint32_t, std::string> names; // the room's members, for leave lines
    uint32_t lastOwner = 0;
    // Peek: the request in flight and its parts so far.
    uint32_t peekSeq = 0;
    std::string peekCode;
    int64_t peekAskedMs = -kPeekEveryMs;
    std::vector<std::string> peekParts;
    // Ping: per member, the last round trip (smoothed) and when they were pinged.
    std::map<uint32_t, int> pings;
    int64_t lastPingMs = 0;
    std::string net = "u"; // this PC's connection, for member data `net`
    std::map<uint32_t, int> mask; // the room's masked names (keeper_protocol.h kMask)
    bool toldMasked = false;
    uint64_t lastTicketToken = 0; // a ticket sent again is taken once
    bool askedAll = false;        // the second listing (every build) is out
    bool saidNoLobby = false;     // logged once until a lobby shows
    int64_t allListAt = -1;       // when to ask for it (the server takes one listing a second)
    bool abortSession = false;    // RoomsTakeAbort
    std::vector<std::pair<uint32_t, int64_t>> callSeen; // per table: the call, when it showed

    void Set(State s)
    {
        if (view.state != s) Log("rooms: %s -> %s", RoomsStateName(view.state), RoomsStateName(s));
        view.state = s;
    }

    bool Proven(const std::string& room, uint32_t id, const std::string& proof)
    {
        if (proof.empty() || room.empty()) return false;
        const std::string key = room + " " + std::to_string(id) + " " + proof;
        const auto it = proofs.find(key);
        if (it != proofs.end()) return it->second;
        std::vector<uint8_t> sig;
        bool ok = false;
        const std::string message = keeper::ProofMessage(room, id);
        if (keeper::Base64Decode(proof, &sig) && sig.size() == 64) {
            for (const auto& k : keys) {
                uint8_t key64[64];
                std::memcpy(key64, k.data(), 64);
                if (VerifySignature(reinterpret_cast<const uint8_t*>(message.data()), message.size(), sig.data(), sig.size(), key64)) {
                    ok = true;
                    break;
                }
            }
        }
        if (proofs.size() > 256) proofs.clear();
        proofs[key] = ok;
        if (ok) Log("rooms: member %u is the keeper of %s", id, room.c_str());
        return ok;
    }

    // The proven keeper among the room's members; 0 if none.
    uint32_t KeeperOf(const WeyveRoomView& v)
    {
        for (const WeyveMember& m : v.members) {
            const auto p = m.data.find(keeper::kProof);
            if (p != m.data.end() && Proven(v.room, m.id, p->second)) return m.id;
        }
        return 0;
    }

    void JoinLobby()
    {
        if (view.lobby.empty()) {
            Set(State::Finding);
            svc->ListRooms({{keeper::kKind, keeper::kKindLobby}, {keeper::kBuild, std::to_string(lobby::kProtocol)}});
            nextListMs = NowMs() + kListAgainMs;
            return;
        }
        target = view.lobby;
        svc->JoinRoom(view.lobby);
        Set(State::Joining);
    }

    // Out of the room: its table's set is no longer ours to report (a match
    // that already began plays out; one still being set up ends), and its
    // tickets and notices go with it.
    // `removed`: the owner took this player out; a match already playing
    // ends too (else it went on with the room calling the other player).
    void ForgetTables(bool removed = false)
    {
        if (playing.table >= 0 && (!playing.begun || removed)) {
            Log(removed ? "rooms: removed from the room during table %d's set: leaving the session"
                        : "rooms: left the room before table %d's match began: leaving the session",
                playing.table + 1);
            abortSession = true;
        }
        playing = {};
        tickets.clear();
        callSeen.clear();
        view.tableViews.clear();
        view.tableNotice.clear();
    }

    // `inRoom`: still in the room (a failed join or a kick left us in none).
    void BackToLobby(const std::string& notice, bool inRoom, bool removed = false)
    {
        ForgetTables(removed);
        if (!notice.empty()) {
            view.notice = notice;
            Log("rooms: %s", notice.c_str());
        }
        if (inRoom) svc->LeaveRoom();
        view.room.clear();
        view.members.clear();
        joinedAt = -1;
        JoinLobby();
    }

    void Line(uint32_t from, const std::string& name, const std::string& text)
    {
        view.chat.push_back({from, name, text});
        if (view.chat.size() > kChatKept) view.chat.erase(view.chat.begin());
    }

    void OnEvent(const WeyveEvent& e, const WeyveRoomView& v)
    {
        switch (e.type) {
        case WeyveEvent::Type::RoomList: {
            if (view.state != State::Finding && view.state != State::NoLobby) break;
            if (askedAll) {
                // Lobbies of other builds: this one is out of date (or ahead).
                askedAll = false;
                std::string other;
                for (const WeyveListedRoom& r : svc->RoomList()) {
                    const auto build = r.listing.find(keeper::kBuild);
                    if (build != r.listing.end() && build->second != std::to_string(lobby::kProtocol)) other = build->second;
                }
                if (other != view.otherBuild && !other.empty()) Log("rooms: the server has build %s's lobby, not %u's", other.c_str(), lobby::kProtocol);
                view.otherBuild = other;
                break;
            }
            std::string best;
            int bestCount = -1;
            for (const WeyveListedRoom& r : svc->RoomList()) {
                const auto build = r.listing.find(keeper::kBuild);
                if (build == r.listing.end() || build->second != std::to_string(lobby::kProtocol)) continue;
                const auto count = r.listing.find(keeper::kCount);
                const int n = count != r.listing.end() ? std::atoi(count->second.c_str()) : 0;
                if (!r.joinable || n >= keeper::kLobbyCap) continue;
                if (n > bestCount) {
                    best = r.code;
                    bestCount = n;
                }
            }
            if (best.empty()) {
                if (!saidNoLobby) Log("rooms: no lobby channel for build %u listed", lobby::kProtocol);
                saidNoLobby = true;
                // Any lobby at all? Asked once the server's listing cooldown
                // (a second) is over; sooner, it refuses.
                allListAt = NowMs() + 1200;
                Set(State::NoLobby);
                nextListMs = NowMs() + kListAgainMs;
                break;
            }
            view.otherBuild.clear();
            saidNoLobby = false;
            view.lobby = best;
            target = best;
            svc->JoinRoom(best);
            Set(State::Joining);
            break;
        }
        case WeyveEvent::Type::RoomJoined:
            if (e.text == view.lobby) {
                Set(State::Lobby);
                if (!chara.empty()) svc->SetMemberData("ch", chara);
                svc->SetMemberData("net", net);
            }
            else {
                view.room = e.text;
                view.chat.clear();
                names.clear();
                lastOwner = 0;
                joinedAt = NowMs();
                pings.clear();
                mask.clear();
                tickets.clear();
                toldMasked = false;
                Set(State::Room);
                if (!chara.empty()) svc->SetMemberData("ch", chara);
                svc->SetMemberData("net", net);
            }
            break;
        case WeyveEvent::Type::RoomError:
            // A refused listing (the server's cooldown): the next listing is
            // the build filtered one again, not taken for the all-builds one.
            if (e.error == WeyveEvent::Error::RateLimited) askedAll = false;
            if (view.state != State::Joining) break;
            if (e.error == WeyveEvent::Error::Other && e.text == "not in a room") break; // a leave with nothing to leave
            if (target == view.lobby) {
                Log("rooms: cannot rejoin lobby channel %s (%s); looking again", view.lobby.c_str(), e.text.c_str());
                view.lobby.clear();
                JoinLobby();
                break;
            }
            BackToLobby(e.error == WeyveEvent::Error::NoSuchRoom ? "No room on that code"
                        : e.error == WeyveEvent::Error::Closed   ? "Room full"
                        : e.error == WeyveEvent::Error::Banned   ? "Removed from that room: its owner took you out"
                                                                 : "Cannot join: " + e.text,
                        false);
            break;
        case WeyveEvent::Type::Removed:
            if (view.state == State::Room) BackToLobby("Removed from the room", false, true);
            break;
        case WeyveEvent::Type::ServerLost:
            view.notice = e.text;
            Set(State::Failed);
            break;
        case WeyveEvent::Type::Chat:
            if (view.state == State::Room) {
                // The name as the room shows it (masked or not).
                const WeyveMember* m = v.Member(e.peer);
                const auto shown = names.find(e.peer);
                Line(e.peer, shown != names.end() ? shown->second : m ? m->name : "?", e.text);
            }
            break;
        default:
            break;
        }
    }

    void Answers()
    {
        Packet p;
        while (svc->Receive(p)) {
            bool pong = false;
            uint64_t clock = 0;
            if (keeper::ParsePing(p.data.data(), p.data.size(), &pong, &clock)) {
                if (!pong) {
                    uint8_t reply[keeper::kPingSize];
                    keeper::FormatPing(true, clock, reply);
                    svc->Send(p.from, reply, sizeof(reply));
                }
                else {
                    const int ms = static_cast<int>((ClockUs() - static_cast<int64_t>(clock)) / 1000);
                    if (ms >= 0 && ms < 5000) {
                        auto it = pings.find(static_cast<uint32_t>(p.from));
                        pings[static_cast<uint32_t>(p.from)] = it == pings.end() ? ms : (it->second * 3 + ms) / 4;
                    }
                }
                continue;
            }
            keeper::Answer a;
            if (!keeper::ParseAnswer(p.data.data(), p.data.size(), &a)) continue;
            if (a.kind == "peek") {
                if (a.seq != peekSeq) continue; // an older one
                if (!a.ok) {
                    peekSeq = 0;
                    svc->SetMemberData(keeper::kAck, std::to_string(a.seq));
                    view.peek = keeper::RoomDetail{};
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
                if (keeper::JoinRoomDetail(peekParts, &d)) view.peek = std::move(d);
                peekSeq = 0;
                continue;
            }
            svc->SetMemberData(keeper::kAck, std::to_string(a.seq));
            if (a.kind == "ticket") {
                keeper::TableTicket t;
                if (keeper::ParseTicket(a.text, &t) && view.state == State::Room &&
                    std::none_of(tickets.begin(), tickets.end(), [&](const keeper::TableTicket& k) { return k.token == t.token && k.host == t.host; }) &&
                    !(playing.table == t.table && playing.host == t.host && lastTicketToken == t.token)) {
                    Log("rooms: ticket for table %d: %s", t.table + 1, t.host ? "open the match room" : ("join " + t.code).c_str());
                    lastTicketToken = t.token;
                    tickets.push_back(t);
                }
                continue;
            }
            if (a.seq != pendingSeq) continue;
            pendingSeq = 0;
            Log("rooms: the keeper answers #%u %s: %s %s", a.seq, a.kind.c_str(), a.ok ? "ok" : "no", a.text.c_str());
            if (!a.ok) {
                view.notice = a.text;
                Set(State::Lobby);
                continue;
            }
            target = a.text;
            svc->LeaveRoom();
            svc->JoinRoom(a.text);
            Set(State::Joining);
        }
    }

    void Mirror(const WeyveRoomView& v)
    {
        view.self = v.self;
        const uint32_t keeperId = KeeperOf(v);
        const bool wasKeeper = view.keeper;
        view.keeper = keeperId && v.host == keeperId;
        if (wasKeeper != view.keeper && (view.state == State::Lobby || view.state == State::Room))
            Log("rooms: %s %s", v.room.c_str(), view.keeper ? "is kept" : "is paused (its host is not the keeper)");
        // After a keeper restart a player holds host: give it back.
        if (keeperId && v.host == v.self && v.self != keeperId && NowMs() - lastHandMs >= kHandBackEveryMs) {
            lastHandMs = NowMs();
            Log("rooms: handing host of %s back to the keeper (%u)", v.room.c_str(), keeperId);
            svc->TransferHost(keeperId);
        }
        if (view.state == State::Lobby) {
            if (view.keeper) view.publicRooms = keeper::UnpackRoomList(v.data);
            return;
        }
        if (view.state != State::Room) return;
        auto data = [&](const char* key) {
            const auto it = v.data.find(key);
            return it != v.data.end() ? it->second : std::string();
        };
        if (joinedAt >= 0) {
            if (data(keeper::kKind) == keeper::kKindCustom) {
                joinedAt = -1;
                Log("rooms: in custom room %s", view.room.c_str());
            }
            else if (NowMs() - joinedAt > kVerifyMs) {
                BackToLobby("No room on that code", true);
                return;
            }
        }
        // While paused the room's data is whatever a player wrote: kept as it was.
        if (view.keeper) {
            view.preset = std::atoi(data(keeper::kPreset).c_str());
            view.isPublic = data(keeper::kPublic) == "1";
            view.tables = std::atoi(data(keeper::kTables).c_str());
            view.owner = static_cast<uint32_t>(std::strtoul(data(keeper::kOwner).c_str(), nullptr, 10));
            view.tableRules.clear();
            view.tableViews.clear();
            for (int t = 0; t < std::min(view.tables, keeper::kMaxTables); t++) {
                view.tableRules.push_back(data(keeper::TableKey(t).c_str()));
                TableView tv;
                keeper::ParseTable(data(keeper::StateKey(t).c_str()), &tv.s);
                tv.queue = keeper::ParseIds(data(keeper::QueueKey(t).c_str()));
                tv.rules = view.tableRules.back();
                // A call's time runs from when it first showed, also while
                // this player's game was still in a battle.
                if (callSeen.size() <= static_cast<size_t>(t)) callSeen.resize(t + 1);
                if (tv.s.phase == keeper::TablePhase::Call && callSeen[t].first != tv.s.call) {
                    callSeen[t] = {tv.s.call, NowMs()};
                    if (tv.s.seat[0] == view.self || tv.s.seat[1] == view.self)
                        Log("rooms: table %d calls this player (call %u, %u s%s)", t + 1, tv.s.call, tv.s.callSeconds,
                            tv.s.ready[tv.s.seat[0] == view.self ? 0 : 1] ? ", already ready" : "");
                }
                tv.callSeenMs = callSeen[t].second;
                view.tableViews.push_back(std::move(tv));
            }
        }
        std::vector<RoomMember> members;
        std::map<uint32_t, std::string> now;
        if (view.keeper) mask = keeper::ParseMask(data(keeper::kMask)); // while paused, the last one stands
        for (const WeyveMember& m : v.members) {
            if (m.id == keeperId) continue;
            RoomMember r;
            r.id = m.id;
            const auto masked = mask.find(m.id);
            r.masked = masked != mask.end();
            // "Player N", N as the keeper counts it.
            r.name = r.masked ? "Player " + std::to_string(masked->second) : m.name;
            const auto ch = m.data.find("ch");
            if (ch != m.data.end()) r.chara = ch->second;
            const auto netData = m.data.find("net");
            if (netData != m.data.end()) r.link = netData->second == "w" ? 1 : netData->second == "f" ? 2 : 0;
            r.owner = m.id == view.owner;
            const auto ping = pings.find(m.id);
            if (ping != pings.end() && m.id != v.self) {
                r.pingMs = ping->second;
                r.relayed = svc->PeerPath(m.id) == WeyveService::LinkPath::Relayed;
            }
            if (r.masked && m.id == v.self && !toldMasked) {
                toldMasked = true;
                Line(0, "", "Your name shows as " + r.name + " in public rooms.");
            }
            members.push_back(r);
            now[m.id] = r.name;
        }
        if (joinedAt < 0) {
            for (const auto& [id, name] : now) {
                if (!names.count(id) && !names.empty()) Line(0, "", name + " joined");
            }
            for (const auto& [id, name] : names) {
                if (!now.count(id)) Line(0, "", name + " left");
            }
            if (view.owner && view.owner != lastOwner && lastOwner) {
                const auto it = now.find(view.owner);
                Line(0, "", (view.owner == v.self ? std::string("You own") : (it != now.end() ? it->second : "?") + " owns") +
                                " the room now");
            }
            lastOwner = view.owner;
            names = now;
        }
        view.members = std::move(members);
    }

    void Peeks()
    {
        if (view.state != State::Lobby || !view.keeper || peekWanted.empty()) {
            if (peekWanted.empty()) view.peek = keeper::RoomDetail{};
            return;
        }
        if (peekWanted != view.peek.code) view.peek = keeper::RoomDetail{}; // another room's: not shown
        if (peekSeq && NowMs() - peekAskedMs < kPeekEveryMs * 2) return;   // one in flight
        if (peekWanted == peekCode && NowMs() - peekAskedMs < kPeekEveryMs) return;
        peekCode = peekWanted;
        peekSeq = ++seq;
        peekAskedMs = NowMs();
        peekParts.clear();
        svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(keeper::PeekRequest(peekSeq, peekCode)));
    }

    void Pings(const WeyveRoomView& v, uint32_t keeperId)
    {
        if (view.state != State::Room || NowMs() - lastPingMs < kPingEveryMs) return;
        lastPingMs = NowMs();
        uint8_t ping[keeper::kPingSize];
        keeper::FormatPing(false, static_cast<uint64_t>(ClockUs()), ping);
        for (const WeyveMember& m : v.members) {
            if (m.id != v.self && m.id != keeperId) svc->Send(m.id, ping, sizeof(ping));
        }
    }

    void TableAsks()
    {
        for (auto& [kind, args] : tableAsks) {
            if (view.state != State::Room) continue;
            const keeper::Request r{++seq, kind, args};
            svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
            Log("rooms: asks the keeper: %s", keeper::FormatRequest(r).c_str());
        }
        tableAsks.clear();
    }

    // The table's session: its code (P1), the score, the result.
    void Report()
    {
        if (playing.table < 0) return;
        const NetView nv = NetViewNow();
        const bool live = nv.phase != NetView::Phase::Idle && nv.phase != NetView::Phase::Ended;
        const std::string t = std::to_string(playing.table);
        auto ask = [&](const std::string& kind, std::vector<std::string> args) {
            const keeper::Request r{++seq, kind, std::move(args)};
            svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
            Log("rooms: tells the keeper: %s", keeper::FormatRequest(r).c_str());
        };
        // The keeper moved the table on before the match began (the other
        // player left, or the match room never opened): this session goes.
        const bool matchBegun = nv.phase == NetView::Phase::Jumping || nv.phase == NetView::Phase::Loading ||
                                nv.phase == NetView::Phase::Running || nv.phase == NetView::Phase::PostMatch;
        if (matchBegun) playing.begun = true;
        if (!matchBegun && !playing.begun && playing.table < static_cast<int>(view.tableViews.size()) && NowMs() - playing.at > 3000) {
            const keeper::TableState& s = view.tableViews[playing.table].s;
            const bool seated = s.seat[0] == view.self || s.seat[1] == view.self;
            const bool stillOn = seated && (s.phase == keeper::TablePhase::Connecting || s.phase == keeper::TablePhase::Playing);
            if (!stillOn && view.keeper) {
                Log("rooms: table %d moved on before its match began: leaving the session", playing.table + 1);
                view.tableNotice = s.seat[0] == view.self || s.seat[1] == view.self || !seated
                                       ? "The match did not start: your opponent left before it began."
                                       : "The match did not start.";
                abortSession = true;
                playing = {};
                return;
            }
        }
        if (live) playing.started = true;
        if (!playing.started) {
            if (NowMs() - playing.at > 30000) {
                Log("rooms: table %d's session never started", playing.table + 1);
                view.tableNotice = "The match did not start: the match room never opened.";
                ask("result", {t, "0", "0"});
                playing = {};
            }
            return;
        }
        if (playing.host && !playing.codeSent && !nv.code.empty()) {
            playing.codeSent = true;
            ask("code", {t, nv.code});
        }
        if (live && playing.host && (nv.seats[0].wins != playing.sent[0] || nv.seats[1].wins != playing.sent[1])) {
            playing.sent[0] = nv.seats[0].wins;
            playing.sent[1] = nv.seats[1].wins;
            ask("score", {t, std::to_string(nv.seats[0].wins), std::to_string(nv.seats[1].wins)});
        }
        if (!live) {
            // Over: the set's result, or 0 - 0 when it did not finish (the
            // keeper calls the table again). P2 reports too; the keeper takes
            // it only when P1 is gone.
            const uint16_t wins[2] = {nv.seats[0].wins, nv.seats[1].wins};
            const bool done = SetWinner(playing.rules, wins) >= 0;
            if (!done) {
                view.tableNotice = std::string(playing.begun ? "The set did not finish" : "The match did not start") + (nv.result.empty() ? std::string(".") : ": " + nv.result + ".");
                Log("rooms: %s", view.tableNotice.c_str());
            }
            ask("result", {t, done ? std::to_string(wins[0]) : "0", done ? std::to_string(wins[1]) : "0"});
            playing = {};
        }
    }

    void Asks(const WeyveRoomView& v)
    {
        (void)v;
        if (createAsk) {
            const keeper::RoomSettings s = *createAsk;
            createAsk.reset();
            if (view.state != State::Lobby || !view.keeper) {
                view.notice = "Custom rooms are not available right now";
            }
            else {
                const keeper::Request r = keeper::RoomRequest(++seq, s);
                svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
                pendingSeq = r.seq;
                pendingUntil = NowMs() + kAnswerWaitMs;
                view.notice.clear();
                Log("rooms: asking the keeper for a room (#%u: %s, %d tables, %s)", r.seq, keeper::PresetName(s.preset), s.tables,
                    s.isPublic ? "public" : "private");
                Set(State::Creating);
            }
        }
        if (joinAsk) {
            const std::string code = *joinAsk;
            joinAsk.reset();
            if ((view.state == State::Lobby || view.state == State::Room) && code != view.room) {
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
                Log("rooms: leaving %s", view.room.c_str());
                BackToLobby("", true);
            }
        }
        for (const std::string& text : chatOut) {
            if (view.state == State::Room) svc->SendChat(text);
        }
        chatOut.clear();
    }

    void Run()
    {
        ThreadGuard running;
        while (!stop && !Stopping()) {
            const WeyveRoomView v = svc->View();
            {
                std::lock_guard lock(mutex);
                view.server = v.serverText;
                if (v.server == WeyveRoomView::Server::Connecting) Set(State::Connecting);
                else if (v.server != WeyveRoomView::Server::Connected) Set(State::Failed);
                else if (view.state == State::Connecting) JoinLobby();
                WeyveEvent e;
                while (svc->NextEvent(e)) OnEvent(e, v);
                if (view.state != State::Failed) {
                    Answers();
                    if (pendingSeq && NowMs() > pendingUntil) {
                        pendingSeq = 0;
                        view.notice = "The keeper did not answer; try again";
                        Set(State::Lobby);
                    }
                    if ((view.state == State::Finding || view.state == State::NoLobby) && NowMs() >= nextListMs) JoinLobby();
                    if (allListAt >= 0 && NowMs() >= allListAt) {
                        allListAt = -1;
                        if (view.state == State::NoLobby) {
                            askedAll = true;
                            svc->ListRooms({{keeper::kKind, keeper::kKindLobby}});
                        }
                    }
                    if (view.state == State::Lobby || view.state == State::Room) {
                        const WeyveRoomView now = svc->View();
                        Mirror(now);
                        Pings(now, KeeperOf(now));
                    }
                    Peeks();
                    TableAsks();
                    if (view.state == State::Room) Report();
                    Asks(v);
                }
            }
            Sleep(20);
        }
    }
};

std::mutex g_clientMutex; // guards g_client itself
std::unique_ptr<Client> g_client;

} // namespace

const char* RoomsStateName(RoomsView::State state)
{
    switch (state) {
    case State::Off: return "off";
    case State::Connecting: return "connecting";
    case State::Finding: return "finding the lobby";
    case State::NoLobby: return "no lobby";
    case State::Lobby: return "lobby";
    case State::Creating: return "creating a room";
    case State::Joining: return "joining";
    case State::Room: return "room";
    case State::Failed: return "failed";
    }
    return "?";
}

void RoomsOpen(const std::string& server, const std::string& name, uint16_t alsoPort, const std::string& keeperKey)
{
    RoomsClose();
    auto c = std::make_unique<Client>();
    std::array<uint8_t, 64> builtIn;
    std::memcpy(builtIn.data(), kKeeperPublicKey, 64);
    c->keys.push_back(builtIn);
    if (!keeperKey.empty()) {
        std::array<uint8_t, 64> extra{};
        bool ok = keeperKey.size() == 128;
        for (size_t i = 0; ok && i < 64; i++) {
            const std::string pair = keeperKey.substr(2 * i, 2);
            char* end = nullptr;
            extra[i] = static_cast<uint8_t>(std::strtoul(pair.c_str(), &end, 16));
            ok = end == pair.c_str() + 2;
        }
        if (ok) c->keys.push_back(extra);
        else Log("rooms: [rooms] keeper_key is not 128 hex digits; ignored");
    }
    c->view.state = State::Connecting;
    const uint8_t link = DetectLinkType();
    c->net = link == 1 ? "w" : link == 2 ? "f" : "u";
    c->svc = WeyveService::Start(server, name, alsoPort);
    Log("rooms: opening the hub's connection to %s as %s", server.c_str(), name.c_str());
    Client* raw = c.get();
    c->thread = std::thread([raw] { raw->Run(); });
    std::lock_guard lock(g_clientMutex);
    g_client = std::move(c);
}

void RoomsClose()
{
    std::unique_ptr<Client> c;
    {
        std::lock_guard lock(g_clientMutex);
        c = std::move(g_client);
    }
    if (!c) return;
    c->stop = true;
    if (c->thread.joinable()) c->thread.join();
    c->svc.reset();
    Log("rooms: closed the hub's connection");
}

bool RoomsOpened()
{
    std::lock_guard lock(g_clientMutex);
    return g_client != nullptr;
}

namespace {
template <typename F> bool WithClient(F f)
{
    std::lock_guard lock(g_clientMutex);
    if (!g_client) return false;
    std::lock_guard inner(g_client->mutex);
    return f(*g_client);
}
} // namespace

bool RoomsCreate(const keeper::RoomSettings& settings)
{
    return WithClient([&](Client& c) {
        c.createAsk = settings;
        return true;
    });
}

bool RoomsJoin(const std::string& code)
{
    std::string normal;
    if (!WeyveService::NormaliseCode(code, &normal)) return false;
    return WithClient([&](Client& c) {
        c.joinAsk = normal;
        return true;
    });
}

void RoomsLeave()
{
    WithClient([](Client& c) {
        c.leaveAsk = true;
        return true;
    });
}

bool RoomsChat(const std::string& text)
{
    return WithClient([&](Client& c) {
        if (c.view.state != State::Room || !c.limiter.Allow(NowMs(), text)) return false;
        c.chatOut.push_back(text);
        return true;
    });
}

void RoomsSetCharacter(const std::string& code)
{
    WithClient([&](Client& c) {
        if (c.chara == code) return true;
        c.chara = code;
        if (c.view.state == State::Lobby || c.view.state == State::Room) c.svc->SetMemberData("ch", code);
        return true;
    });
}

void RoomsTable(const std::string& kind, const std::vector<std::string>& args)
{
    WithClient([&](Client& c) {
        c.tableAsks.push_back({kind, args});
        return true;
    });
}

bool RoomsTakeTicket(keeper::TableTicket* out)
{
    return WithClient([&](Client& c) {
        if (c.tickets.empty()) return false;
        *out = c.tickets.front();
        c.tickets.erase(c.tickets.begin());
        return true;
    });
}

void RoomsTableSession(int table, bool host, const std::string& rules)
{
    WithClient([&](Client& c) {
        c.playing = {};
        c.playing.table = table;
        c.playing.host = host;
        ParseRules(rules, &c.playing.rules);
        c.playing.at = NowMs();
        return true;
    });
}

int64_t RoomsClockMs()
{
    return NowMs();
}

bool RoomsTakeAbort()
{
    return WithClient([](Client& c) { return std::exchange(c.abortSession, false); });
}

std::string RoomsPlace(const RoomsView& v, uint32_t id)
{
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
    return "Idle";
}

void RoomsPeek(const std::string& code)
{
    WithClient([&](Client& c) {
        c.peekWanted = code;
        return true;
    });
}

void RoomsClearNotice()
{
    WithClient([](Client& c) {
        c.view.notice.clear();
        c.view.tableNotice.clear();
        return true;
    });
}

RoomsView RoomsSnapshot()
{
    RoomsView v;
    WithClient([&](Client& c) {
        v = c.view;
        return true;
    });
    return v;
}

std::string DescribeRooms(const RoomsView& v)
{
    std::string s = std::string(RoomsStateName(v.state)) + " on " + (v.server.empty() ? "-" : v.server) + ", self " +
                    std::to_string(v.self) + ", keeper " + (v.keeper ? "yes" : "no");
    if (!v.lobby.empty()) s += ", lobby " + v.lobby + " (" + std::to_string(v.publicRooms.size()) + " public rooms)";
    if (!v.room.empty()) {
        s += ", room " + v.room + " '" + keeper::PresetName(v.preset) + "' " + (v.isPublic ? "public" : "private") + " " +
             std::to_string(v.tables) + " tables, owner " + std::to_string(v.owner) + ", members";
        for (const RoomMember& m : v.members) s += " " + m.name + "(" + std::to_string(m.id) + (m.owner ? ",owner" : "") + ")";
        s += ", " + std::to_string(v.chat.size()) + " chat lines";
        for (size_t t = 0; t < v.tableViews.size(); t++)
            s += ", t" + std::to_string(t) + "=" + keeper::FormatTable(v.tableViews[t].s) + " q=" + keeper::FormatIds(v.tableViews[t].queue);
    }
    if (!v.notice.empty()) s += ", notice '" + v.notice + "'";
    return s;
}

} // namespace sfv::mod
