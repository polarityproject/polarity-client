#include "mod/rooms_client.h"

#include "core/lobby.h"
#include "core/match_ticket.h"
#include "core/update_info.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/rooms_screen.h"
#include "mod/udp_transport.h"
#include "mod/weyve_service.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
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
// The listings (docs/room-listing-plan.md): the desk's every so often (its
// revision says when the rooms changed), the rooms when it moved. The server
// takes one listing a second from a connection.
constexpr int64_t kDesksScreenMs = 2000;  // the rooms screen open
constexpr int64_t kDesksHubMs = 5000;     // the hub alone
constexpr int64_t kDesksRoomMs = 10000;   // in a custom room
constexpr int64_t kListGapMs = 1100;
constexpr int64_t kListAnswerMs = 3000;   // a listing not answered by then is asked again
constexpr int64_t kDeskJoinMs = 5000;     // a create asked while the desk is being joined waits this long
constexpr int64_t kUnkeptLobbyMs = 30000; // a lobby channel with no keeper this long is left
constexpr int64_t kStaleLobbyMs = 120000; // ... and passed over this long
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

std::atomic<bool> g_wordFilter{true};

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
    // Watching (RoomsWatch): the table, the match room last handed out (one
    // ticket a set), the tickets for the overlay; watchSession: one of them
    // runs (a call ends it).
    int watchTable = -1;
    std::string watchCode;
    std::vector<WatchTicket> watchTickets;
    bool watchSession = false;
    struct Playing {
        int table = -1;
        bool host = false;
        Rules rules;
        int64_t at = 0;
        bool started = false, codeSent = false;
        bool begun = false; // the match itself (the jump), not only the session
        int sent[2] = {-1, -1};
        // The score while the session was live: once this side leaves, the
        // driver is gone and the view's score reads 0 - 0.
        uint16_t wins[2] = {0, 0};
        bool againAsked = false; // P1: the next set asked for (again)
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
    // The listings: the one asked for and not yet answered, and when.
    enum class Listed { None, Desks, Rooms, AllBuilds } listed = Listed::None;
    int64_t listedAt = -kListGapMs;
    int64_t nextDesksMs = 0;
    uint64_t roomsRev = 0;   // the desk revision publicRooms is from
    bool roomsWanted = false;
    std::map<std::string, bool> signatures; // "message signature" -> checked
    // The desk: wanted while the rooms screen is open; being joined.
    bool wantDesk = false;
    bool deskJoining = false;
    int64_t deskJoinAt = 0;
    int64_t lastHandMs = -kHandBackEveryMs;
    std::map<std::string, bool> proofs; // "room id proof" -> checked
    std::map<uint32_t, std::string> names; // the room's members, for leave lines
    uint32_t lastOwner = 0;
    // Peek: the request in flight and its parts so far.
    uint32_t peekSeq = 0;
    std::string peekCode;
    int64_t peekAskedMs = -kPeekEveryMs;
    std::vector<std::string> peekParts;
    // Ping: per member, the last few round trips (shown: the lowest; each
    // side answers from a loop that wakes every 20 ms, which the lowest of a
    // few mostly leaves out) and when they were pinged.
    static constexpr size_t kPingSamples = 4;
    std::map<uint32_t, std::deque<int>> pings;
    std::set<uint32_t> pingLogged;
    int64_t lastPingMs = 0;
    std::string net = "u"; // this PC's connection, for member data `net`
    std::map<uint32_t, int> mask; // the room's masked names (keeper_protocol.h kMask)
    // The keeper's word list (a public room's wl0.., by its hash wlv): kept
    // across rooms, read again when the hash changes.
    std::string wordsHash;
    std::vector<std::string> words;
    bool toldMasked = false;
    uint64_t lastTicketToken = 0; // a ticket sent again is taken once
    bool saidNoLobby = false;     // logged once until a lobby shows
    int64_t lobbyUnkeptSince = -1;            // in the lobby channel with no keeper since (ms)
    std::map<std::string, int64_t> staleLobbies; // lobby channels passed over, until (ms)
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

    // A listing signed by the keeper (kListingSig: base64 of `message`).
    bool Signed(const std::string& message, const keeper::Listing& listing)
    {
        const auto s = listing.find(keeper::kListingSig);
        if (s == listing.end() || s->second.empty()) return false;
        const std::string key = message + " " + s->second;
        const auto it = signatures.find(key);
        if (it != signatures.end()) return it->second;
        std::vector<uint8_t> sig;
        bool ok = false;
        if (keeper::Base64Decode(s->second, &sig) && sig.size() == 64) {
            for (const auto& k : keys) {
                uint8_t key64[64];
                std::memcpy(key64, k.data(), 64);
                if (VerifySignature(reinterpret_cast<const uint8_t*>(message.data()), message.size(), sig.data(), sig.size(), key64)) {
                    ok = true;
                    break;
                }
            }
        }
        if (signatures.size() > 1024) signatures.clear();
        signatures[key] = ok;
        return ok;
    }

    // Out of every room (connected, or back from a room): the hub's place.
    // The desk is joined from here while the rooms screen is open (Run).
    void ToBrowsing()
    {
        deskJoining = false;
        Set(view.keeper && !view.lobby.empty() ? State::Browsing : State::Finding);
        nextDesksMs = 0;
    }

    // The listings, one at a time, a second apart (the server's limit).
    void Listings()
    {
        const int64_t now = NowMs();
        if (listed != Listed::None) {
            if (now - listedAt < kListAnswerMs) return;
            listed = Listed::None; // not answered: asked again below
        }
        if (now - listedAt < kListGapMs) return;
        if (roomsWanted && wantDesk && view.keeper) {
            roomsWanted = false;
            listed = Listed::Rooms;
            listedAt = now;
            svc->ListRooms({{keeper::kListingKind, keeper::ListingKindValue(lobby::kProtocol)}});
            return;
        }
        if (allListAt >= 0 && now >= allListAt) {
            allListAt = -1;
            if (view.state == State::NoLobby) {
                listed = Listed::AllBuilds;
                listedAt = now;
                svc->ListRooms({{keeper::kKind, keeper::kKindLobby}});
                return;
            }
        }
        if (now < nextDesksMs) return;
        listed = Listed::Desks;
        listedAt = now;
        nextDesksMs = now + (view.state == State::Room ? kDesksRoomMs : wantDesk ? kDesksScreenMs : kDesksHubMs);
        svc->ListRooms({{keeper::kKind, keeper::kKindLobby}, {keeper::kBuild, std::to_string(lobby::kProtocol)}});
    }

    // The desks listed: the keeper's (signed) ones, the summary, and the
    // desk to join (the fullest with room).
    void OnDesks()
    {
        std::string best;
        int bestCount = -1;
        bool any = false;
        keeper::DeskSummary top;
        for (const WeyveListedRoom& r : svc->RoomList()) {
            if (!Signed(keeper::DeskMessage(r.code, r.listing), r.listing)) continue;
            const auto summary = r.listing.find(keeper::kDeskSummary);
            keeper::DeskSummary s;
            if (summary == r.listing.end() || !keeper::ParseDeskSummary(summary->second, &s)) continue;
            if (!any || s.rev > top.rev) top = s;
            any = true;
            const auto count = r.listing.find(keeper::kCount);
            const int n = count != r.listing.end() ? std::atoi(count->second.c_str()) : 0;
            if (!r.joinable || n >= keeper::kLobbyCap) continue;
            if (const auto stale = staleLobbies.find(r.code); stale != staleLobbies.end() && NowMs() < stale->second) continue;
            if (n > bestCount) {
                best = r.code;
                bestCount = n;
            }
        }
        const bool atDeskOrRoom = view.state == State::Lobby || view.state == State::Room || view.state == State::Creating ||
                                  view.state == State::Joining;
        if (!any) {
            if (!saidNoLobby) Log("rooms: no desk for build %u listed", lobby::kProtocol);
            saidNoLobby = true;
            view.summary = {};
            if (!atDeskOrRoom) {
                view.keeper = false;
                view.publicRooms.clear();
                view.lobby.clear();
                Set(State::NoLobby);
                // Any desk at all? Asked once the server's listing cooldown is over.
                allListAt = NowMs() + kListGapMs;
            }
            return;
        }
        if (saidNoLobby) Log("rooms: desk listed (revision %llu, %d public rooms)", static_cast<unsigned long long>(top.rev), top.publicRooms);
        saidNoLobby = false;
        view.otherBuild.clear();
        view.summary = top;
        if (!atDeskOrRoom) {
            view.keeper = true;
            if (!deskJoining) view.lobby = best;
            if (view.state == State::Finding || view.state == State::NoLobby) Set(State::Browsing);
        }
        if (top.rev != roomsRev) roomsWanted = true;
    }

    // The public rooms listed: those the keeper signed, sorted.
    void OnRooms()
    {
        std::vector<keeper::ListedRoom> list;
        for (const WeyveListedRoom& r : svc->RoomList()) {
            keeper::ListedRoom l;
            if (!keeper::ParseRoomListing(r.code, r.listing, lobby::kProtocol, &l)) continue;
            if (!Signed(keeper::ListingMessage(r.code, r.listing), r.listing)) continue;
            list.push_back(std::move(l));
        }
        keeper::SortRoomList(&list);
        view.publicRooms = std::move(list);
        roomsRev = view.summary.rev;
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
        if (watchTable >= 0) Log("rooms: out of the room: no longer watching table %d", watchTable + 1);
        if (watchSession && NetViewNow().spectator) abortSession = true;
        watchTable = -1;
        view.watching = -1;
        watchCode.clear();
        watchTickets.clear();
        watchSession = false;
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
        ToBrowsing();
    }

    void Line(uint32_t from, const std::string& name, const std::string& text, const std::string& realName = {})
    {
        // Starred once, as it comes: the setting picks which is shown.
        const std::string censored = from && view.isPublic && !words.empty() ? keeper::CensorChat(text, words) : text;
        view.chat.push_back({from, name, text, realName.empty() ? name : realName, censored});
        if (view.chat.size() > kChatKept) view.chat.erase(view.chat.begin());
    }

    // A member's name as the keeper's mask shows it ("Player 1234"), whatever
    // the setting.
    std::string MaskedName(uint32_t id, const std::string& name) const
    {
        const auto it = mask.find(id);
        return it != mask.end() ? "Player " + std::to_string(it->second) : name;
    }

    void OnEvent(const WeyveEvent& e, const WeyveRoomView& v)
    {
        switch (e.type) {
        case WeyveEvent::Type::RoomList: {
            const Listed which = listed;
            listed = Listed::None;
            if (which == Listed::Desks) OnDesks();
            else if (which == Listed::Rooms) OnRooms();
            else if (which == Listed::AllBuilds) {
                // Desks of other builds: this one is out of date (or ahead).
                std::string other;
                for (const WeyveListedRoom& r : svc->RoomList()) {
                    const auto build = r.listing.find(keeper::kBuild);
                    if (build != r.listing.end() && build->second != std::to_string(lobby::kProtocol)) other = build->second;
                }
                if (other != view.otherBuild && !other.empty()) Log("rooms: the server has build %s's desk, not %u's", other.c_str(), lobby::kProtocol);
                view.otherBuild = other;
            }
            break;
        }
        case WeyveEvent::Type::RoomJoined:
            if (deskJoining || e.text == view.lobby) { // the desk (a join the screen gave up on too: left again in Run)
                deskJoining = false;
                Set(State::Lobby);
            }
            else {
                view.room = e.text;
                view.chat.clear();
                names.clear();
                lastOwner = 0;
                joinedAt = NowMs();
                pings.clear();
                pingLogged.clear();
                mask.clear();
                tickets.clear();
                toldMasked = false;
                Set(State::Room);
                if (!chara.empty()) svc->SetMemberData("ch", chara);
                svc->SetMemberData("net", net);
            }
            break;
        case WeyveEvent::Type::RoomError:
            // A refused listing (the server's cooldown): asked again later.
            if (e.error == WeyveEvent::Error::RateLimited) {
                listed = Listed::None;
                break;
            }
            if (deskJoining) {
                // Full or gone since it was listed: another one, at the next listing.
                Log("rooms: cannot join desk %s (%s); looking again", view.lobby.c_str(), e.text.c_str());
                deskJoining = false;
                staleLobbies[view.lobby] = NowMs() + kStaleLobbyMs;
                view.lobby.clear();
                nextDesksMs = 0;
                break;
            }
            if (view.state != State::Joining) break;
            if (e.error == WeyveEvent::Error::Other && e.text == "not in a room") break; // a leave with nothing to leave
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
                const std::string real = m ? m->name : "?";
                Line(e.peer, MaskedName(e.peer, real), e.text, real);
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
                    // Clock 0: sent before the link was up (see Pings).
                    if (clock != 0 && ms >= 0 && ms < 5000) {
                        auto& samples = pings[static_cast<uint32_t>(p.from)];
                        samples.push_back(ms);
                        if (samples.size() > kPingSamples) samples.pop_front();
                        // The first reading and the first full window, for the log.
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
            if (a.kind == "again") {
                // The next set with the same two (ok: its rules), or someone
                // waits (no: the session ends as a set's does).
                Log("rooms: the keeper on the next set: %s %s", a.ok ? "ok" : "no", a.text.c_str());
                Rules rules;
                if (a.ok && ParseRules(a.text, &rules)) playing.rules = rules;
                NetAgainAnswer(a.ok, a.text);
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
            // No keeper for kUnkeptLobbyMs: a channel an earlier keeper left
            // (the players' presence kept it alive) while the keeper hosts
            // another; the hubs chose the busiest and stayed here. Leave it
            // and look again without it (a keeper restart takes seconds).
            if (view.keeper) lobbyUnkeptSince = -1;
            else if (lobbyUnkeptSince < 0) lobbyUnkeptSince = NowMs();
            else if (NowMs() - lobbyUnkeptSince >= kUnkeptLobbyMs) {
                Log("rooms: lobby channel %s has had no keeper for %lld s; looking for another", view.lobby.c_str(),
                    static_cast<long long>(kUnkeptLobbyMs / 1000));
                staleLobbies[view.lobby] = NowMs() + kStaleLobbyMs;
                lobbyUnkeptSince = -1;
                svc->LeaveRoom();
                view.lobby.clear();
                view.publicRooms.clear();
                view.keeper = false;
                ToBrowsing();
            }
            return;
        }
        lobbyUnkeptSince = -1;
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
            if (!keeper::ParseTitle(data(keeper::kTitle), &view.title)) view.title = keeper::RoomTitle{};
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
        // The word list for chat (public rooms only carry it).
        if (const std::string hash = data(keeper::kWordListHash); view.isPublic && !hash.empty() && hash != wordsHash) {
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
            // "Player 1234", the keeper's number (every viewer the same),
            // unless this player turned the word filter off.
            const std::string maskedName = MaskedName(m.id, m.name);
            r.name = g_wordFilter ? maskedName : m.name;
            const auto ch = m.data.find("ch");
            if (ch != m.data.end()) r.chara = ch->second;
            // The rooms server's own lookup of their address (never their
            // claim); missing for a player who keeps it hidden.
            const auto cc = m.data.find("@cc");
            if (cc != m.data.end()) r.country = keeper::CleanCountry(cc->second);
            const auto netData = m.data.find("net");
            if (netData != m.data.end()) r.link = netData->second == "w" ? 1 : netData->second == "f" ? 2 : 0;
            const auto watch = m.data.find("w");
            if (watch != m.data.end() && !watch->second.empty()) r.watching = std::atoi(watch->second.c_str()) - 1;
            r.owner = m.id == view.owner;
            const auto ping = pings.find(m.id);
            if (ping != pings.end() && !ping->second.empty() && m.id != v.self) {
                r.pingMs = *std::min_element(ping->second.begin(), ping->second.end());
                r.relayed = svc->PeerPath(m.id) == WeyveService::LinkPath::Relayed;
            }
            if (r.masked && m.id == v.self && !toldMasked) {
                toldMasked = true;
                Line(0, "", "Your name shows as " + maskedName + " in public rooms.");
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
        uint8_t ping[keeper::kPingSize], building[keeper::kPingSize];
        keeper::FormatPing(false, static_cast<uint64_t>(ClockUs()), ping);
        // A send to a member with no link up yet builds one and waits in the
        // link's queue until it is up, seconds later: timed, that read as a
        // far ping for a while after joining. Those go without a clock, and
        // their answers are not counted.
        keeper::FormatPing(false, 0, building);
        for (const WeyveMember& m : v.members) {
            if (m.id == v.self || m.id == keeperId) continue;
            const bool up = svc->PeerPath(m.id) != WeyveService::LinkPath::None;
            svc->Send(m.id, up ? ping : building, sizeof(ping));
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

    // Watching: a table's new match room is a ticket, once; a call to play
    // (any table) stops it.
    void Watch()
    {
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
        // The set's viewers watch in the keeper's relay room (`watch`), which
        // the host streams to once, never in the match room.
        const bool on = tv.s.phase == keeper::TablePhase::Connecting || tv.s.phase == keeper::TablePhase::Playing;
        if (!on || tv.s.watch.empty() || tv.s.watch == watchCode) return;
        // Still on the last set's games (behind the players): those first;
        // the relay keeps them, and this set waits its turn.
        if (watchSession) {
            const NetView nv = NetViewNow();
            if (nv.spectator && nv.phase != NetView::Phase::Idle && nv.phase != NetView::Phase::Ended) return;
        }
        watchCode = tv.s.watch;
        watchTickets.push_back({watchTable, tv.s.watch, tv.rules});
        Log("rooms: table %d's set: watching through the keeper's relay room %s", watchTable + 1, tv.s.watch.c_str());
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
        if (live) {
            playing.started = true;
            playing.wins[0] = nv.seats[0].wins;
            playing.wins[1] = nv.seats[1].wins;
        }
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
            // With the build: the keeper's relay room shows it to viewers.
            ask("code", {t, nv.code, nv.build});
        }
        // Nobody else waits for the table (the keeper's mark): the set's end
        // is a vote for the next one; both on, P1 asks the keeper.
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
            // A player left the set (this one, or the other's Bye said so):
            // a forfeit, which both tell the keeper (it takes the first).
            if (const int seat = NetTakeForfeit(); seat >= 0) {
                Log("rooms: table %d's set: P%d left it (a forfeit)", playing.table + 1, seat + 1);
                ask("forfeit", {t, std::to_string(seat)});
                playing = {};
                return;
            }
            // Over: the set's result, or 0 - 0 when it did not finish (the
            // keeper calls the table again). P2 reports too; the keeper takes
            // it only when P1 is gone. The view keeps the score when the other
            // side ended the session; after this side's own leave (from the
            // vote, a won set) it is empty, so the score seen last while live.
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

    void Asks(const WeyveRoomView& v)
    {
        (void)v;
        // Asked while the desk is still being joined: waits for it.
        if (createAsk && view.state == State::Browsing && wantDesk && NowMs() - deskJoinAt < kDeskJoinMs) return;
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
                // Leaving the room during this player's table set is leaving
                // the set: a forfeit (as Leave in the vote), told to the
                // keeper before the room hears of the leave (it only sees a
                // member go, as when a game crashes: that breaks the set
                // off). The session ends too (its Bye tells the opponent).
                if (playing.table >= 0 && playing.table < static_cast<int>(view.tableViews.size())) {
                    const keeper::TableState& s = view.tableViews[playing.table].s;
                    const int seat = s.seat[0] == view.self ? 0 : s.seat[1] == view.self ? 1 : -1;
                    if (seat >= 0 && (s.phase == keeper::TablePhase::Playing || s.phase == keeper::TablePhase::Connecting)) {
                        const keeper::Request r{++seq, "forfeit", {std::to_string(playing.table), std::to_string(seat)}};
                        svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(r));
                        Log("rooms: leaving during table %d's set: a forfeit (%s)", playing.table + 1, keeper::FormatRequest(r).c_str());
                        abortSession = true;
                    }
                }
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
                else if (view.state == State::Connecting) ToBrowsing();
                wantDesk = RoomsScreenOpen();
                view.wordFilter = g_wordFilter;
                WeyveEvent e;
                while (svc->NextEvent(e)) OnEvent(e, v);
                if (view.state != State::Failed) {
                    Answers();
                    if (pendingSeq && NowMs() > pendingUntil) {
                        pendingSeq = 0;
                        view.notice = "The keeper did not answer; try again";
                        Set(State::Lobby);
                    }
                    Listings();
                    // Into the desk while the rooms screen is open, out of it after.
                    if (view.state == State::Browsing && wantDesk && view.keeper && !view.lobby.empty() && !deskJoining) {
                        deskJoining = true;
                        deskJoinAt = NowMs();
                        target = view.lobby;
                        svc->JoinRoom(view.lobby);
                        Log("rooms: into desk %s", view.lobby.c_str());
                    }
                    if (view.state == State::Lobby && !wantDesk && !pendingSeq) {
                        svc->LeaveRoom();
                        Log("rooms: out of the desk");
                        ToBrowsing();
                    }
                    if (view.state == State::Lobby || view.state == State::Room) {
                        const WeyveRoomView now = svc->View();
                        Mirror(now);
                        Pings(now, KeeperOf(now));
                    }
                    Peeks();
                    TableAsks();
                    if (view.state == State::Room) Report();
                    if (view.state == State::Room) Watch();
                    Asks(v);
                }
            }
            Sleep(20);
        }
    }
};

std::mutex g_clientMutex; // guards g_client itself
std::unique_ptr<Client> g_client;
// Held through a whole open or close: two at once (the hub frame reopening
// on the render thread while a `rooms open` ran on the game thread) each
// made a client, and the second replaced the first with its thread still
// running: std::terminate (2026-11-05, the second test PC).
std::recursive_mutex g_openMutex;

} // namespace

void SetRoomsWordFilter(bool on)
{
    if (g_wordFilter.exchange(on) != on) Log("rooms: word filter %s", on ? "on" : "off");
}

bool RoomsWordFilter() { return g_wordFilter; }

const char* RoomsStateName(RoomsView::State state)
{
    switch (state) {
    case State::Off: return "off";
    case State::Connecting: return "connecting";
    case State::Finding: return "finding the lobby";
    case State::NoLobby: return "no lobby";
    case State::Browsing: return "browsing";
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
    std::lock_guard open(g_openMutex);
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
    std::lock_guard open(g_openMutex);
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

void RoomsWatch(int table)
{
    WithClient([&](Client& c) {
        if (c.view.state != State::Room) return false;
        if (table >= c.view.tables) table = -1;
        if (table == c.watchTable) return true;
        if (c.watchSession && table != c.watchTable && NetViewNow().spectator) c.abortSession = true;
        c.watchTable = table;
        c.view.watching = table;
        c.watchCode.clear();
        c.watchTickets.clear();
        c.watchSession = false;
        c.svc->SetMemberData("w", table >= 0 ? std::to_string(table + 1) : "");
        Log(table >= 0 ? "rooms: watching table %d" : "rooms: stopped watching%s", table >= 0 ? table + 1 : 0);
        return true;
    });
}

bool RoomsIsKeeper(const std::string& room, uint32_t id, const std::string& proof)
{
    return WithClient([&](Client& c) { return c.Proven(room, id, proof); });
}

bool RoomsTakeWatch(WatchTicket* out)
{
    return WithClient([&](Client& c) {
        if (c.watchTickets.empty()) return false;
        *out = c.watchTickets.front();
        c.watchTickets.erase(c.watchTickets.begin());
        c.watchSession = true;
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
    NetTakeForfeit(); // a forfeit left from an earlier set (its room left meanwhile) is not this one's
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

std::string RoomsAfterSet(int seat, int winnerSeat)
{
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
            const uint32_t next = other ? other : queue.empty() ? 0 : queue.front();
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

bool RoomsTakeAbort()
{
    return WithClient([](Client& c) { return std::exchange(c.abortSession, false); });
}

void RoomsCancelHandoff()
{
    WithClient([](Client& c) {
        if (c.playing.table >= 0 && c.playing.begun) return false; // the match is on: leaving is the panel's
        Log("rooms: leaves table %d before its match", c.playing.table + 1);
        c.tableAsks.push_back({"stand", {}});
        if (c.playing.table >= 0) c.abortSession = true;
        c.playing = {};
        return true;
    });
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
    for (const RoomMember& m : v.members) {
        if (m.id == id && m.watching >= 0) return "Watching Table " + std::to_string(m.watching + 1);
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
        s += ", room " + v.room + " '" + keeper::FormatTitle(v.title) + "' " + (v.isPublic ? "public" : "private") + " " +
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
