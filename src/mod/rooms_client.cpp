// The rooms client's thread, the keeper's proofs and signatures, and the calls the overlay makes.

#include "mod/rooms_internal.h"

#include "core/update_info.h"
#include "mod/lifecycle.h"
#include "mod/rooms_screen.h"
#include "mod/udp_transport.h"
#include "mod/updater.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace sfv::mod {

using namespace rooms;

namespace rooms {

std::string MyVersion() { return UpdateViewNow().current; }

std::string OtherVersionNotice(const std::string& theirs) { return "That room is on Polarity " + theirs + ". You have " + MyVersion() + "."; }

} // namespace rooms

namespace {

constexpr size_t kKeyBytes = 64, kSignatureBytes = 64;
constexpr int kStepMs = 20;

// The rooms VPS keeper's public key; [rooms] keeper_key adds another.
constexpr uint8_t kKeeperPublicKey[kKeyBytes] = {
    0x7d, 0xd5, 0x23, 0xba, 0x3f, 0x52, 0x80, 0x80, 0x0e, 0xef, 0x41, 0x60, 0x22, 0x22, 0x9d, 0xc4,
    0xf4, 0x6f, 0x30, 0x16, 0xf8, 0x70, 0x39, 0x37, 0x5c, 0x72, 0x50, 0xec, 0xcb, 0x2d, 0x6c, 0x43,
    0x4a, 0xe8, 0xfb, 0x97, 0x45, 0x50, 0x24, 0xeb, 0x7d, 0x83, 0x33, 0x18, 0x51, 0xf1, 0x38, 0xaf,
    0x96, 0x94, 0xb8, 0x81, 0x72, 0xad, 0xe8, 0x73, 0x44, 0xb9, 0xd3, 0x53, 0x9c, 0x03, 0x0f, 0x1f};

} // namespace

namespace rooms {

int64_t ClockUs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<int64_t>(t.QuadPart * 1000000 / f.QuadPart);
}

int64_t NowMs() { return static_cast<int64_t>(GetTickCount64()); }

void Client::Set(State s) {
    if (view.state != s) Log("rooms: %s -> %s", RoomsStateName(view.state), RoomsStateName(s));
    view.state = s;
}

bool Client::KeeperSigned(const std::string& message, const std::string& signature) const {
    std::vector<uint8_t> sig;
    if (!keeper::Base64Decode(signature, &sig) || sig.size() != kSignatureBytes) return false;
    for (const auto& k : keys) {
        uint8_t key[kKeyBytes];
        std::memcpy(key, k.data(), kKeyBytes);
        if (VerifySignature(reinterpret_cast<const uint8_t*>(message.data()), message.size(), sig.data(), sig.size(), key)) return true;
    }
    return false;
}

bool Client::Proven(const std::string& room, uint32_t id, const std::string& proof) {
    if (proof.empty() || room.empty()) return false;
    const std::string key = room + " " + std::to_string(id) + " " + proof;
    const auto it = proofs.find(key);
    if (it != proofs.end()) return it->second;
    const bool ok = KeeperSigned(keeper::ProofMessage(room, id), proof);
    if (proofs.size() > kProofsKept) proofs.clear();
    proofs[key] = ok;
    if (ok) Log("rooms: member %u is the keeper of %s", id, room.c_str());
    else Log("rooms: member %u's keeper proof for %s does not verify against this build's keeper key", id, room.c_str());
    return ok;
}

uint32_t Client::KeeperOf(const WeyveRoomView& v) {
    for (const WeyveMember& m : v.members) {
        const auto p = m.data.find(keeper::kProof);
        if (p != m.data.end() && Proven(v.room, m.id, p->second)) return m.id;
    }
    return 0;
}

bool Client::Signed(const std::string& message, const keeper::Listing& listing) {
    const auto s = listing.find(keeper::kListingSig);
    if (s == listing.end() || s->second.empty()) return false;
    const std::string key = message + " " + s->second;
    const auto it = signatures.find(key);
    if (it != signatures.end()) return it->second;
    const bool ok = KeeperSigned(message, s->second);
    if (signatures.size() > kSignaturesKept) signatures.clear();
    if (!ok) Log("rooms: a listing's signature does not verify against this build's keeper key; ignored");
    signatures[key] = ok;
    return ok;
}

void Client::Run() {
    ThreadGuard running;
    while (!stop && !Stopping()) {
        const WeyveRoomView v = svc->View();
        {
            std::lock_guard lock(mutex);
            view.server = v.serverText;
            if (v.server == WeyveRoomView::Server::Connecting) Set(State::Connecting);
            else if (v.server != WeyveRoomView::Server::Connected) {
                if (view.state != State::Failed) Log("rooms: the server connection failed: %s", v.serverText.empty() ? "-" : v.serverText.c_str());
                Set(State::Failed);
            } else if (view.state == State::Connecting) ToBrowsing();
            wantDesk = RoomsScreenOpen();
            view.wordFilter = g_wordFilter;
            WeyveEvent e;
            while (svc->NextEvent(e)) OnEvent(e, v);
            if (view.state != State::Failed) {
                Answers();
                if (pendingSeq && NowMs() > pendingUntil) {
                    Log("rooms: no answer from the keeper to #%u in time (link to it: %s)", pendingSeq,
                        svc->PeerPath(KeeperOf(v)) == WeyveService::LinkPath::None ? "none" : "up");
                    pendingSeq = 0;
                    view.notice = "The rooms server didn't respond. Try again.";
                    Set(State::Lobby);
                }
                Listings();
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
                    DeskLink(KeeperOf(now));
                }
                Peeks();
                TableAsks();
                if (view.state == State::Room) Report();
                if (view.state == State::Room) Watch();
                Asks(v);
            }
        }
        Sleep(kStepMs);
    }
}

} // namespace rooms

namespace {

std::recursive_mutex g_openMutex;

} // namespace

void RoomsSetWordFilter(bool on) {
    if (g_wordFilter.exchange(on) != on) Log("rooms: word filter %s", on ? "on" : "off");
}

bool RoomsWordFilter() { return g_wordFilter; }

const char* RoomsStateName(RoomsView::State state) {
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

void RoomsOpen(const std::string& server, const std::string& name, uint16_t alsoPort, const std::string& keeperKey) {
    std::lock_guard open(g_openMutex);
    RoomsClose();
    auto c = std::make_unique<Client>();
    std::array<uint8_t, kKeyBytes> builtIn;
    std::memcpy(builtIn.data(), kKeeperPublicKey, kKeyBytes);
    c->keys.push_back(builtIn);
    if (!keeperKey.empty()) {
        std::array<uint8_t, kKeyBytes> extra{};
        bool ok = keeperKey.size() == 2 * kKeyBytes;
        for (size_t i = 0; ok && i < kKeyBytes; i++) {
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
    c->net = link == 1 ? "w" : link == 2 ? "f"
                                         : "u";
    c->svc = WeyveService::Start(server, name, alsoPort);
    Log("rooms: opening the hub's connection to %s as %s", server.c_str(), name.c_str());
    Client* raw = c.get();
    c->thread = std::thread([raw] { raw->Run(); });
    std::lock_guard lock(g_clientMutex);
    g_client = std::move(c);
}

void RoomsClose() {
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

bool RoomsOpened() {
    std::lock_guard lock(g_clientMutex);
    return g_client != nullptr;
}

bool RoomsCreate(const keeper::RoomSettings& settings) {
    return WithClient([&](Client& c) {
        c.createAsk = settings;
        c.createAskAt = NowMs();
        return true;
    });
}

bool RoomsJoin(const std::string& code) {
    std::string normal;
    if (!WeyveService::NormaliseCode(code, &normal)) return false;
    return WithClient([&](Client& c) {
        c.joinAsk = normal;
        return true;
    });
}

void RoomsLeave() {
    WithClient([](Client& c) {
        c.leaveAsk = true;
        return true;
    });
}

bool RoomsChat(const std::string& text) {
    return WithClient([&](Client& c) {
        if (c.view.state != State::Room || !c.limiter.Allow(NowMs(), text)) return false;
        c.chatOut.push_back(text);
        return true;
    });
}

void RoomsSetCharacter(const std::string& code) {
    WithClient([&](Client& c) {
        if (c.chara == code) return true;
        c.chara = code;
        if (c.view.state == State::Lobby || c.view.state == State::Room) c.svc->SetMemberData("ch", code);
        return true;
    });
}

void RoomsSetSkills(int vskill, int vtrigger) {
    const std::string value = vskill <= 0 || vtrigger <= 0 ? "r" : std::to_string(std::min(vskill, 9)) + std::to_string(std::min(vtrigger, 9));
    WithClient([&](Client& c) {
        if (c.skills == value) return true;
        c.skills = value;
        Log("rooms: V-Skill and V-Trigger for the tables: %s", value.c_str());
        if (c.view.state == State::Lobby || c.view.state == State::Room) c.svc->SetMemberData("vs", value);
        return true;
    });
}

void RoomsTable(const std::string& kind, const std::vector<std::string>& args) {
    WithClient([&](Client& c) {
        c.tableAsks.push_back({kind, args});
        return true;
    });
}

void RoomsWatch(int table) {
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
        if (table >= 0) Log("rooms: watching table %d", table + 1);
        else Log("rooms: stopped watching");
        return true;
    });
}

bool RoomsIsKeeper(const std::string& room, uint32_t id, const std::string& proof) {
    return WithClient([&](Client& c) { return c.Proven(room, id, proof); });
}

bool RoomsTakeWatch(WatchTicket* out) {
    return WithClient([&](Client& c) {
        if (c.watchTickets.empty()) return false;
        *out = c.watchTickets.front();
        c.watchTickets.erase(c.watchTickets.begin());
        c.watchSession = true;
        return true;
    });
}

bool RoomsTakeTicket(keeper::TableTicket* out) {
    return WithClient([&](Client& c) {
        if (c.tickets.empty()) return false;
        *out = c.tickets.front();
        c.tickets.erase(c.tickets.begin());
        return true;
    });
}

void RoomsTableSession(int table, bool host, const std::string& rules) {
    NetTakeForfeit();
    WithClient([&](Client& c) {
        c.playing = {};
        c.playing.table = table;
        c.playing.host = host;
        ParseRules(rules, &c.playing.rules);
        c.playing.at = NowMs();
        return true;
    });
}

int64_t RoomsClockMs() {
    return NowMs();
}

bool RoomsTakeAbort() {
    return WithClient([](Client& c) { return std::exchange(c.abortSession, false); });
}

void RoomsCancelHandoff() {
    WithClient([](Client& c) {
        if (c.playing.table >= 0 && c.playing.begun) return false;
        Log("rooms: leaves table %d before its match", c.playing.table + 1);
        c.tableAsks.push_back({"stand", {}});
        if (c.playing.table >= 0) c.abortSession = true;
        c.playing = {};
        return true;
    });
}

void RoomsPeek(const std::string& code) {
    WithClient([&](Client& c) {
        c.peekWanted = code;
        return true;
    });
}

void RoomsClearNotice() {
    WithClient([](Client& c) {
        c.view.notice.clear();
        c.view.tableNotice.clear();
        return true;
    });
}

RoomsView RoomsSnapshot() {
    RoomsView v;
    WithClient([&](Client& c) {
        v = c.view;
        return true;
    });
    return v;
}

std::string RoomsDescribe(const RoomsView& v) {
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
