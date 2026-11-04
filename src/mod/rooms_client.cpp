#include "mod/rooms_client.h"

#include "core/lobby.h"
#include "core/update_info.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
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

    // `inRoom`: still in the room (a failed join or a kick left us in none).
    void BackToLobby(const std::string& notice, bool inRoom)
    {
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
            std::string best;
            int bestCount = -1;
            for (const WeyveListedRoom& r : svc->RoomList()) {
                const auto count = r.listing.find(keeper::kCount);
                const int n = count != r.listing.end() ? std::atoi(count->second.c_str()) : 0;
                if (!r.joinable || n >= keeper::kLobbyCap) continue;
                if (n > bestCount) {
                    best = r.code;
                    bestCount = n;
                }
            }
            if (best.empty()) {
                if (view.state != State::NoLobby) Log("rooms: no lobby channel for build %u listed", lobby::kProtocol);
                Set(State::NoLobby);
                nextListMs = NowMs() + kListAgainMs;
                break;
            }
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
                toldMasked = false;
                Set(State::Room);
                if (!chara.empty()) svc->SetMemberData("ch", chara);
                svc->SetMemberData("net", net);
            }
            break;
        case WeyveEvent::Type::RoomError:
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
                                                                 : "Cannot join: " + e.text,
                        false);
            break;
        case WeyveEvent::Type::Removed:
            if (view.state == State::Room) BackToLobby("Removed from the room", false);
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
            for (int t = 0; t < std::min(view.tables, keeper::kMaxTables); t++) view.tableRules.push_back(data(keeper::TableKey(t).c_str()));
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
                    if (view.state == State::Lobby || view.state == State::Room) {
                        const WeyveRoomView now = svc->View();
                        Mirror(now);
                        Pings(now, KeeperOf(now));
                    }
                    Peeks();
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
    }
    if (!v.notice.empty()) s += ", notice '" + v.notice + "'";
    return s;
}

} // namespace sfv::mod
