// polarity-bot: a headless rooms player for tests (custom-rooms-plan.md M7),
// grown from weyve_probe: it finds the lobby channel, makes or joins a custom
// room and does what its script says, without the game. With --auto it
// answers its table calls (Ready), opens a made-up match room when it is P1
// and reports the set's result a moment later, so bots cycle a table's
// rotation by themselves.
//
//   polarity-bot --name Bot1 [--server 127.0.0.1:5555] [--auto] [--set-ms 3000] [--wins p1|p2|random]
//            [--hub | --browse] [--script "create 2 1 0; wait 1000; sit 0 0; ..."]
// Where it waits between rooms (docs/room-listing-plan.md): the keeper's desk
// (the default: it can ask for rooms), or in no room at all as the game does:
// --hub polls the desk's listing every 5 s (the hub open), --browse every
// 2 s and lists the public rooms whenever the desk's revision moves (the
// rooms screen open, without its desk).
// Commands (script, then stdin; ';' or newlines between them):
//   create <tables> <public 0|1> <title: 5, 3,RYU,KEN>   join <code>   joinpublic   leave
//   sit <table> <seat 0|1|-1>   stand   queue <table>   unqueue   ready <table>   decline <table>
//   rules <table> <rules...>    kick <member id>   kickname <name>   chat <text>   flood <n>   status   wait <ms>   quit
//   chara <code> (the character others see beside the name, member data "ch")
//   watch <table>   unwatch   (a headless viewer: each set of the table through
//   the keeper's relay room, from its first game, played at 60 frames a
//   second; logs each game and the set.)
// Logs one line per happening on stdout ("bot Bot1: ...").

#include "core/keeper_protocol.h"
#include "core/lobby.h"
#include "core/version.h"

#include <algorithm>
#include <functional>

#include <weyvelength.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace sfv;
using namespace sfv::keeper;

namespace {

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Str(const char* p, uint32_t len) { return p ? std::string(p, len) : std::string(); }

// A viewer with no game (watch): what a spectator's netplay driver says to the
// relay, and the stream played at 60 frames a second.
struct Viewer {
    std::string name, code;
    WeyveClient* c = nullptr;
    uint32_t host = 0;
    uint64_t build = 0;
    bool joined = false, accepted = false, done = false;
    int64_t helloMs = 0, beatMs = 0, startMs = 0, stateMs = 0;
    std::vector<lobby::MatchEntry> slices, list;
    uint64_t match = 0;
    int gameIndex = -1;
    bool hostLoaded = false; // the host's Loaded is in hand: the game can be watched
    uint32_t hostChecksum = 0;
    bool gameDone = false; // this game watched through (counted once)
    std::vector<lobby::FrameRecord> frames;
    std::map<uint32_t, lobby::FrameRecord> ahead;
    double played = 0;
    int64_t playedAtMs = 0;
    int stalls = 0;
    bool stalled = false;
    int gamesWatched = 0;
    uint64_t framesWatched = 0;
    std::function<void(const std::string&)> log;

    void Send(const lobby::Message& m)
    {
        const std::vector<uint8_t> bytes = lobby::Encode(m);
        std::vector<uint8_t> tagged(bytes.size() + 1);
        tagged[0] = 1; // the driver's control channel
        std::copy(bytes.begin(), bytes.end(), tagged.begin() + 1);
        weyve_send_p2p(c, host, tagged.data(), static_cast<uint32_t>(tagged.size()));
    }

    bool Start(const std::string& server, uint16_t port, const std::string& room)
    {
        code = room;
        c = weyve_client_create();
        if (!weyve_connect(c, server.c_str(), port)) {
            weyve_client_destroy(c);
            c = nullptr;
            return false;
        }
        weyve_set_name(c, (name + "-w").c_str());
        weyve_join_room(c, room.c_str(), nullptr);
        startMs = NowMs();
        return true;
    }

    void Stop()
    {
        if (!c) return;
        if (!done) log("stopped: " + State());
        Quit();
    }

    std::string State() const
    {
        if (gameIndex >= 0 && gameIndex < static_cast<int>(list.size())) {
            const lobby::MatchEntry& e = list[gameIndex];
            return ("game " + std::to_string(gameIndex + 1) + " of " + std::to_string(list.size()) + ": played " +
                std::to_string(static_cast<int>(played)) + ", in hand " + std::to_string(frames.size()) + ", the list says " +
                std::to_string(e.frames) + (e.finished ? " finished" : " not finished") + (hostLoaded ? "" : ", not loaded") +
                (gameDone ? ", done" : ""));
        }
        return "no game yet (" + std::to_string(list.size()) + " listed)";
    }

    void Quit()
    {
        if (accepted) {
            lobby::Message bye;
            bye.type = lobby::Type::Bye;
            Send(bye);
        }
        weyve_leave_room(c);
        for (int i = 0; i < 3 && weyve_poll(c); i++) {
        }
        weyve_client_destroy(c);
        c = nullptr;
    }

    void Load(int index)
    {
        gameIndex = index;
        match = list[index].id;
        hostLoaded = gameDone = false;
        frames.clear();
        ahead.clear();
        played = 0;
        playedAtMs = NowMs();
        stalled = true; // fills its buffer first
        log("loads game " + std::to_string(index + 1));
        SendLoaded();
    }

    void SendLoaded()
    {
        lobby::Message l;
        l.type = lobby::Type::Loaded;
        l.matchId = match;
        l.stateChecksum = hostChecksum;
        Send(l);
    }

    void OnMessage(const lobby::Message& m)
    {
        switch (m.type) {
        case lobby::Type::Accept:
            if (!accepted) log("watching " + code + " (host " + m.name + ")");
            accepted = true;
            break;
        case lobby::Type::Reject:
            log("the relay turned this viewer away (" + std::to_string(static_cast<int>(m.reason)) + ")");
            done = true;
            break;
        case lobby::Type::Matches: {
            if (slices.size() != m.sliceOf) slices.assign(m.sliceOf, lobby::MatchEntry{});
            for (size_t i = 0; i < m.matches.size() && m.sliceAt + i < slices.size(); i++) slices[m.sliceAt + i] = m.matches[i];
            if (!std::all_of(slices.begin(), slices.end(), [](const lobby::MatchEntry& e) { return e.id != 0; })) break;
            list = slices;
            // Its game by id (the list may have moved it).
            if (match) {
                for (size_t i = 0; i < list.size(); i++) {
                    if (list[i].id == match) gameIndex = static_cast<int>(i);
                }
            }
            if (gameIndex < 0 && !list.empty()) Load(0);
            break;
        }
        case lobby::Type::Loaded:
            if (m.matchId == match && !hostLoaded) {
                hostLoaded = true;
                hostChecksum = m.stateChecksum;
                log("game " + std::to_string(gameIndex + 1) + ": the host's Loaded");
                SendLoaded();
            }
            break;
        case lobby::Type::Inputs:
            if (m.matchId != match) break;
            for (size_t i = 0; i < m.frames.size(); i++) {
                const uint32_t at = m.first + static_cast<uint32_t>(i);
                if (at < frames.size()) continue;
                if (at == frames.size()) frames.push_back(m.frames[i]);
                else ahead[at] = m.frames[i];
            }
            for (auto it = ahead.begin(); it != ahead.end() && it->first <= frames.size();) {
                if (it->first == frames.size()) frames.push_back(it->second);
                it = ahead.erase(it);
            }
            break;
        case lobby::Type::Bye:
            log("set watched: " + std::to_string(gamesWatched) + " games, " + std::to_string(framesWatched) + " frames, " +
                std::to_string(stalls) + " stalls" + (m.text.empty() ? std::string() : " (" + m.text + ")"));
            done = true;
            break;
        default:
            break;
        }
    }

    void Step()
    {
        if (!c || done) return;
        if (!weyve_poll(c)) {
            log("viewer: lost the server");
            done = true;
            return;
        }
        WeyveEvent e;
        while (weyve_next(c, &e)) {
            if (e.type == WEYVE_EVENT_ROOM_ID_ASSIGNED) joined = true;
            else if (e.type == WEYVE_EVENT_ROOM_ERROR && !joined) {
                log("viewer: cannot join " + code);
                done = true;
            }
            else if (e.type == WEYVE_EVENT_PEER_LEFT && e.data.peer_left.id == host && host) {
                log("viewer: the relay left");
                done = true;
            }
        }
        const int64_t now = NowMs();
        if (joined && !host) {
            uint32_t len = 0;
            const char* b = weyve_room_data(c, "build", &len);
            const uint32_t h = weyve_host_id(c);
            if (b && len && h && h != weyve_id(c)) {
                host = h;
                build = std::strtoull(Str(b, len).c_str(), nullptr, 16);
            }
        }
        if (host && !accepted && now - helloMs >= 1000) {
            helloMs = now;
            lobby::Message hello;
            hello.type = lobby::Type::Hello;
            hello.build = build;
            hello.role = lobby::Role::Spectator;
            hello.name = name;
            Send(hello);
        }
        uint32_t from = 0, len = 0;
        while (const uint8_t* d = weyve_next_p2p(c, &from, &len)) {
            if (len < 2 || d[0] != 1 || from != host) continue;
            if (auto m = lobby::Decode(d + 1, len - 1)) OnMessage(*m);
            if (done) return;
        }
        // Play at 60 a second, from the frames in hand.
        if (gameIndex >= 0 && hostLoaded) {
            // As the driver's table viewer: 120 frames in hand before
            // playing, and again after running dry (kRelayWatchDelayFrames).
            const lobby::MatchEntry& entry = list[gameIndex];
            const bool all = entry.finished && frames.size() >= entry.frames;
            if (stalled && (frames.size() - played >= 120 || all)) stalled = false;
            const double want = played + (now - playedAtMs) * 0.06;
            playedAtMs = now;
            if (!stalled) {
                if (want > frames.size()) {
                    played = static_cast<double>(frames.size());
                    if (!all) {
                        stalls++;
                        stalled = true;
                    }
                }
                else {
                    played = want;
                }
            }
            // Through a finished game: the next one, once there is one.
            if (!gameDone && entry.finished && played >= entry.frames && frames.size() >= entry.frames) {
                gameDone = true;
                gamesWatched++;
                framesWatched += frames.size();
                log("watched game " + std::to_string(gameIndex + 1) + ": " + std::to_string(frames.size()) + " frames");
            }
            if (gameDone && gameIndex + 1 < static_cast<int>(list.size())) Load(gameIndex + 1);
        }
        if (now - stateMs >= 15000) {
            stateMs = now;
            log(State());
        }
        if (host && accepted && now - beatMs >= 250) {
            beatMs = now;
            lobby::Message ack;
            ack.type = lobby::Type::InputsAck;
            ack.matchId = hostLoaded ? match : 0;
            ack.first = static_cast<uint32_t>(frames.size());
            ack.playedTo = static_cast<uint32_t>(played);
            ack.decided = gameDone;
            Send(ack);
            if (gameIndex >= 0 && !hostLoaded) SendLoaded();
        }
    }
};

struct Bot {
    std::string name = "Bot";
    std::string version = sfv::kPolarityVersion; // member data `ver` ("--version ''": none)
    std::string host = "127.0.0.1";
    uint16_t port = 5555;
    bool autoPlay = false;
    int64_t setMs = 3000;
    std::string wins = "random";
    int watchTable = -1;
    std::string watchedCode;
    std::unique_ptr<Viewer> viewer;
    WeyveClient* c = nullptr;
    std::string lobby, room;
    bool inLobby = false, inRoom = false;
    // --hub / --browse: in no room between rooms, polling the listings.
    enum class Mode { Desk, Hub, Browse } mode = Mode::Desk;
    enum class Listed { None, Desks, Rooms } listed = Listed::None;
    int64_t listedAt = -2000, nextDesksMs = 0;
    bool deskSeen = false, roomsWanted = false;
    uint64_t rev = 0, roomsRev = 0;
    std::vector<ListedRoom> publicRooms;
    int64_t roomLists = 0, deskLists = 0;
    Listing firstListing; // the first public room's listing as listed (forge)
    bool forging = false;
    uint32_t seq = 0;
    std::string madeRoom; // the room the keeper made for this bot (answered once)
    std::mutex scriptMutex; // stdin's thread adds to the script
    std::deque<std::string> script;
    int64_t waitUntil = 0;
    std::map<std::string, std::string> shown; // table keys as last logged
    std::mt19937 rng{std::random_device{}()};
    // Auto play.
    std::map<int, int64_t> readyAt;     // table -> when to ready
    struct Hosting {
        int table = -1;
        int64_t codeAt = 0, resultAt = 0;
        bool codeSent = false;
    } hosting;

    void Log(const std::string& line) { std::printf("bot %s: %s\n", name.c_str(), line.c_str()), std::fflush(stdout); }

    std::string Data(const char* key)
    {
        uint32_t len = 0;
        const char* v = weyve_room_data(c, key, &len);
        return Str(v, len);
    }

    void Ask(const Request& r)
    {
        weyve_set_member_data(c, kRequest, FormatRequest(r).c_str());
    }

    void Ask(const std::string& kind, std::vector<std::string> args = {}) { Ask({++seq, kind, std::move(args)}); }

    bool Connect()
    {
        c = weyve_client_create();
        if (!weyve_connect(c, host.c_str(), port)) {
            Log("cannot connect to " + host + ":" + std::to_string(port));
            return false;
        }
        weyve_set_name(c, name.c_str());
        ListDesks();
        return true;
    }

    void ListDesks()
    {
        static std::string build;
        build = std::to_string(lobby::kProtocol);
        WeyveRoomFilter filters[2] = {{kKind, WEYVE_FILTER_EQUALS, kKindLobby}, {kBuild, WEYVE_FILTER_EQUALS, build.c_str()}};
        weyve_list_rooms(c, filters, 2);
        listed = Listed::Desks;
        listedAt = NowMs();
        deskLists++;
    }

    void ListPublicRooms()
    {
        static std::string kind;
        kind = ListingKindValue(lobby::kProtocol, version);
        WeyveRoomFilter filter{kListingKind, WEYVE_FILTER_EQUALS, kind.c_str()};
        weyve_list_rooms(c, &filter, 1);
        listed = Listed::Rooms;
        listedAt = NowMs();
        roomLists++;
    }

    // One listing at a time, a second apart (the server's limit).
    void Listings()
    {
        const int64_t now = NowMs();
        if (listed != Listed::None && now - listedAt < 3000) return;
        if (now - listedAt < 1100) return;
        listed = Listed::None;
        if (roomsWanted) {
            roomsWanted = false;
            ListPublicRooms();
            return;
        }
        if (mode == Mode::Desk || now < nextDesksMs) return;
        nextDesksMs = now + (mode == Mode::Browse ? 2000 : 5000);
        ListDesks();
    }

    Listing ListingOf(uint32_t i)
    {
        Listing l;
        const uint32_t n = weyve_room_list_listing_count(c, i);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t klen = 0, vlen = 0;
            const char* key = weyve_room_list_listing_key_at(c, i, k, &klen);
            const std::string ks = Str(key, klen);
            const char* value = weyve_room_list_listing(c, i, ks.c_str(), &vlen);
            l[ks] = Str(value, vlen);
        }
        return l;
    }

    void Run(const std::string& line)
    {
        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;
        if (cmd.empty()) return;
        if (cmd == "wait") {
            int64_t ms = 0;
            ss >> ms;
            waitUntil = NowMs() + ms;
        }
        else if (cmd == "create") {
            RoomSettings s;
            int pub = 1;
            std::string title = "0";
            ss >> s.tables >> pub >> title;
            // Sent as typed (even one the keeper refuses: what it answers is the test).
            if (!ParseTitle(title, &s.title)) {
                const size_t comma = title.find(',');
                s.title.id = std::atoi(title.c_str());
                if (comma != std::string::npos) s.title.chara[0] = title.substr(comma + 1);
            }
            s.isPublic = pub != 0;
            Ask(RoomRequest(++seq, s));
            Log("asks for a room");
        }
        else if (cmd == "setdata") {
            // setdata <key> <value>: any member data (testing what the server refuses, e.g. "@cc").
            std::string key, value;
            ss >> key >> value;
            weyve_set_member_data(c, key.c_str(), value.c_str());
            Log("sets member data " + key + "=" + value);
        }
        else if (cmd == "chara") {
            std::string code;
            ss >> code;
            weyve_set_member_data(c, "ch", code.c_str());
            Log("plays " + code);
        }
        else if (cmd == "join") {
            std::string code;
            ss >> code;
            weyve_leave_room(c);
            weyve_join_room(c, code.c_str(), nullptr);
        }
        else if (cmd == "joinpublic") {
            // The first room the server's listing has (the keeper's order).
            if (publicRooms.empty()) {
                Log("no public room listed yet; trying again");
                roomsWanted = true;
                std::lock_guard lock(scriptMutex);
                script.push_front(line);
                waitUntil = NowMs() + 1500;
                return;
            }
            const std::string code = publicRooms[0].code;
            publicRooms.clear(); // listed again next time
            weyve_leave_room(c);
            weyve_join_room(c, code.c_str(), nullptr);
        }
        else if (cmd == "kickname") {
            std::string who;
            ss >> who;
            uint32_t count = 0;
            const uint32_t* ids = weyve_members(c, &count);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t len = 0;
                const char* peer = weyve_peer_name(c, ids[i], &len);
                if (Str(peer, len) == who) {
                    Ask("kick", {std::to_string(ids[i])});
                    Log("asks the keeper to remove " + who);
                }
            }
        }
        else if (cmd == "leave") {
            weyve_leave_room(c);
            inRoom = false;
            room.clear();
            if (mode == Mode::Desk && !lobby.empty()) weyve_join_room(c, lobby.c_str(), nullptr);
        }
        else if (cmd == "sit" || cmd == "queue" || cmd == "ready" || cmd == "decline" || cmd == "stand" || cmd == "unqueue" ||
                 cmd == "kick") {
            std::vector<std::string> args;
            std::string a;
            while (ss >> a) args.push_back(a);
            if (cmd == "sit") cmd = "seat";
            Ask(cmd, args);
        }
        else if (cmd == "rules") {
            std::string table, rest;
            ss >> table;
            std::getline(ss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            Ask("rules", {table, rest});
        }
        else if (cmd == "chat") {
            std::string text;
            std::getline(ss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            weyve_send_chat(c, text.c_str());
        }
        else if (cmd == "flood") {
            int n = 10;
            ss >> n;
            for (int i = 0; i < n; i++) weyve_send_chat(c, ("flood " + std::to_string(i)).c_str());
            Log("sent " + std::to_string(n) + " chat lines at once");
        }
        else if (cmd == "status") {
            Log("room " + (room.empty() ? std::string("-") : room) + ", self " + std::to_string(weyve_id(c)));
            Log("listings: " + std::to_string(deskLists) + " of the desks, " + std::to_string(roomLists) + " of the rooms; " +
                std::to_string(publicRooms.size()) + " public rooms, revision " + std::to_string(rev));
            for (int t = 0; t < kMaxTables; t++) {
                const std::string s = Data(StateKey(t).c_str());
                if (!s.empty()) Log("  table " + std::to_string(t) + " " + s + " queue " + Data(QueueKey(t).c_str()));
            }
        }
        else if (cmd == "watch") {
            ss >> watchTable;
            Log("watches table " + std::to_string(watchTable));
        }
        else if (cmd == "unwatch") {
            watchTable = -1;
            if (viewer) viewer->Stop();
            viewer.reset();
        }
        else if (cmd == "forge") {
            // A room of its own, listed with a real room's signed listing
            // copied on (the game must not show it: the signature names the
            // other room). Needs a room list first (joinpublic or --browse).
            if (firstListing.empty()) {
                roomsWanted = true;
                std::lock_guard lock(scriptMutex);
                script.push_front(line);
                waitUntil = NowMs() + 1500;
                return;
            }
            forging = true;
            weyve_leave_room(c);
            weyve_create_room(c);
        }
        else if (cmd == "quit") {
            std::exit(0);
        }
        else {
            Log("unknown command: " + cmd);
        }
    }

    void Events()
    {
        WeyveEvent e;
        while (weyve_next(c, &e)) {
            switch (e.type) {
            case WEYVE_EVENT_ROOM_LIST: {
                const Listed which = listed;
                listed = Listed::None;
                const uint32_t n = weyve_room_list_count(c);
                if (which == Listed::Rooms) {
                    publicRooms.clear();
                    for (uint32_t i = 0; i < n; i++) {
                        uint32_t len = 0;
                        const char* id = weyve_room_list_id(c, i, &len);
                        const std::string code = Str(id, len);
                        ListedRoom r;
                        const Listing l = ListingOf(i);
                        if (i == 0) firstListing = l;
                        if (ParseRoomListing(code, l, lobby::kProtocol, &r)) publicRooms.push_back(r);
                    }
                    SortRoomList(&publicRooms);
                    roomsRev = rev;
                    break;
                }
                if (n == 0) {
                    Log("no desk listed");
                    if (mode == Mode::Desk) ListDesks(); // again (one a second)
                    break;
                }
                uint32_t len = 0;
                const char* id = weyve_room_list_id(c, 0, &len); // len is set by the call: read it after
                const std::string first = Str(id, len);
                DeskSummary summary;
                const Listing l = ListingOf(0);
                const auto r = l.find(kDeskSummary);
                if (r != l.end() && ParseDeskSummary(r->second, &summary)) rev = summary.rev;
                if (mode == Mode::Browse && rev != roomsRev) roomsWanted = true;
                if (!deskSeen) {
                    deskSeen = true;
                    lobby = first;
                    if (mode == Mode::Desk) {
                        Log(std::to_string(n) + " desks listed; joining " + lobby);
                        weyve_join_room(c, lobby.c_str(), nullptr);
                    }
                    else Log(std::to_string(n) + " desks listed; staying out of rooms (" + (mode == Mode::Hub ? "hub" : "browse") + ")");
                }
                break;
            }
            case WEYVE_EVENT_ROOM_ID_ASSIGNED: {
                const std::string code = Str(e.data.room_assigned.id, e.data.room_assigned.id_len);
                inLobby = code == lobby;
                inRoom = !inLobby;
                room = inRoom ? code : std::string();
                // Its version, as a game's: the keeper gives a room its maker's.
                if (!version.empty()) weyve_set_member_data(c, kVersion, version.c_str());
                shown.clear();
                Log(inLobby ? "in the lobby " + code : "in room " + code + " as member " + std::to_string(weyve_id(c)));
                if (forging && inRoom) {
                    forging = false;
                    weyve_set_room_listed(c, true);
                    for (const auto& [k, v] : firstListing) weyve_set_room_listing(c, k.c_str(), v.c_str());
                    Log("forged a listing on room " + code);
                }
                break;
            }
            case WEYVE_EVENT_ROOM_ERROR:
                Log("room error " + std::to_string(e.data.room_error.code) + " " + Str(e.data.room_error.context, e.data.room_error.context_len));
                break;
            case WEYVE_EVENT_CHAT:
                Log("chat from " + std::to_string(e.data.chat.from) + ": " + Str(e.data.chat.text, e.data.chat.text_len));
                break;
            case WEYVE_EVENT_KICKED:
            case WEYVE_EVENT_BANNED:
                Log("removed from the room");
                inRoom = false;
                room.clear();
                if (mode == Mode::Desk && !lobby.empty()) weyve_join_room(c, lobby.c_str(), nullptr);
                break;
            default:
                break;
            }
        }
        uint32_t from = 0, len = 0;
        while (const uint8_t* data = weyve_next_p2p(c, &from, &len)) {
            // A player's ping (the room's player list shows the round trip): answered as the game does.
            bool pong = false;
            uint64_t clock = 0;
            if (ParsePing(data, len, &pong, &clock)) {
                if (!pong) {
                    uint8_t reply[kPingSize];
                    FormatPing(true, clock, reply);
                    weyve_send_p2p(c, from, reply, sizeof(reply));
                }
                continue;
            }
            Answer a;
            if (!ParseAnswer(data, len, &a)) continue;
            weyve_set_member_data(c, kAck, std::to_string(a.seq).c_str());
            if (a.kind == "room" && a.ok) {
                // Once (the keeper resends until the ack lands; each join
                // would leave and rejoin, the owner handing the room on).
                if (a.text == madeRoom) continue;
                madeRoom = a.text;
                Log("the keeper made room " + a.text);
                weyve_leave_room(c);
                weyve_join_room(c, a.text.c_str(), nullptr);
            }
            else if (a.kind == "ticket") {
                TableTicket t;
                if (!ParseTicket(a.text, &t)) continue;
                Log("ticket: " + a.text);
                if (autoPlay && t.host && hosting.table != t.table) {
                    hosting = {t.table, NowMs() + 800, 0, false};
                }
            }
            else if (!a.ok) {
                Log("the keeper says no to #" + std::to_string(a.seq) + " " + a.kind + ": " + a.text);
            }
        }
    }

    // Logs each table's state as it changes, and plays it with --auto.
    void Tables()
    {
        if (!inRoom) return;
        const uint32_t self = weyve_id(c);
        for (int t = 0; t < kMaxTables; t++) {
            const std::string sk = StateKey(t), qk = QueueKey(t);
            const std::string s = Data(sk.c_str()), q = Data(qk.c_str());
            if (s.empty()) continue;
            if (shown[sk] != s || shown[qk] != q) {
                shown[sk] = s;
                shown[qk] = q;
                Log("table " + std::to_string(t) + " " + s + " queue " + q);
            }
            if (!autoPlay) continue;
            TableState st;
            if (!ParseTable(s, &st)) continue;
            const int seat = st.seat[0] == self ? 0 : st.seat[1] == self ? 1 : -1;
            if (st.phase == TablePhase::Call && seat >= 0 && !st.ready[seat]) {
                auto it = readyAt.find(t);
                if (it == readyAt.end()) readyAt[t] = NowMs() + 300 + rng() % 400;
                else if (NowMs() >= it->second) {
                    Ask("ready", {std::to_string(t)});
                    readyAt.erase(it);
                }
            }
            else {
                readyAt.erase(t);
            }
        }
        // P1: the made-up match room's code, then the set's result.
        if (hosting.table >= 0) {
            const int64_t now = NowMs();
            if (!hosting.codeSent && now >= hosting.codeAt) {
                static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
                std::string code;
                for (int i = 0; i < 6; i++) code += kAlphabet[rng() % 32];
                Ask("code", {std::to_string(hosting.table), code});
                hosting.codeSent = true;
                hosting.resultAt = now + setMs;
                Log("opens match room " + code + " for table " + std::to_string(hosting.table));
            }
            else if (hosting.codeSent && now >= hosting.resultAt) {
                const bool p1 = wins == "p1" || (wins == "random" && rng() % 2);
                Ask("result", {std::to_string(hosting.table), p1 ? "2" : "1", p1 ? "1" : "2"});
                Log(std::string("reports table ") + std::to_string(hosting.table) + ": " + (p1 ? "P1 2-1" : "P2 2-1"));
                hosting = {};
            }
        }
    }

    // watch: each set of the table, through its relay room (`watch` in the
    // table's state), one after another.
    void Watch()
    {
        if (viewer) {
            viewer->Step();
            if (viewer->done) {
                viewer->Stop();
                viewer.reset();
            }
            return;
        }
        if (watchTable < 0 || !inRoom) return;
        TableState st;
        if (!ParseTable(Data(StateKey(watchTable).c_str()), &st)) return;
        const bool on = st.phase == TablePhase::Connecting || st.phase == TablePhase::Playing;
        if (!on || st.watch.empty() || st.watch == watchedCode) return;
        watchedCode = st.watch;
        viewer = std::make_unique<Viewer>();
        viewer->name = name;
        viewer->log = [this](const std::string& line) { Log("viewer: " + line); };
        Log("watches table " + std::to_string(watchTable) + "'s set in relay room " + st.watch);
        if (!viewer->Start(host, port, st.watch)) {
            Log("viewer: cannot connect");
            viewer.reset();
        }
    }

    void Loop()
    {
        for (;;) {
            if (!weyve_poll(c)) {
                Log("lost the server");
                return;
            }
            Events();
            Listings();
            Tables();
            Watch();
            std::string line;
            bool have = false;
            if (NowMs() >= waitUntil && (inLobby || inRoom || (mode != Mode::Desk && deskSeen))) {
                std::lock_guard lock(scriptMutex);
                if (!script.empty()) {
                    line = script.front();
                    script.pop_front();
                    have = true;
                }
            }
            if (have) Run(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

void Split(const std::string& text, std::deque<std::string>* out)
{
    std::string line;
    for (char ch : text) {
        if (ch == ';' || ch == '\n') {
            out->push_back(line);
            line.clear();
        }
        else line += ch;
    }
    if (!line.empty()) out->push_back(line);
}

} // namespace

int main(int argc, char** argv)
{
    Bot bot;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        if (a == "--name" && v) bot.name = argv[++i];
        else if (a == "--server" && v) {
            std::string s = argv[++i];
            const size_t colon = s.rfind(':');
            if (colon != std::string::npos) {
                bot.port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
                s.resize(colon);
            }
            bot.host = s;
        }
        else if (a == "--auto") bot.autoPlay = true;
        else if (a == "--hub") bot.mode = Bot::Mode::Hub;
        else if (a == "--browse") bot.mode = Bot::Mode::Browse;
        else if (a == "--set-ms" && v) bot.setMs = std::atoll(argv[++i]);
        else if (a == "--version" && v) bot.version = argv[++i];
        else if (a == "--wins" && v) bot.wins = argv[++i];
        else if (a == "--script" && v) Split(argv[++i], &bot.script);
        else {
            std::fprintf(stderr, "usage: polarity-bot --name N [--server host:port] [--auto] [--set-ms ms] [--wins p1|p2|random] [--version v] [--hub | --browse] [--script \"...\"]\n");
            return 2;
        }
    }
    if (!bot.Connect()) return 1;
    // stdin: more commands as they come (a test can drive a bot live).
    std::thread([&bot] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard lock(bot.scriptMutex);
            bot.script.push_back(line);
        }
    }).detach();
    bot.Loop();
    return 1;
}
