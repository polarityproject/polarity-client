// sfvr-bot: a headless rooms player for tests (custom-rooms-plan.md M7),
// grown from weyve_probe: it finds the lobby channel, makes or joins a custom
// room and does what its script says, without the game. With --auto it
// answers its table calls (Ready), opens a made-up match room when it is P1
// and reports the set's result a moment later, so bots cycle a table's
// rotation by themselves.
//
//   sfvr-bot --name Bot1 [--server 127.0.0.1:5555] [--auto] [--set-ms 3000] [--wins p1|p2|random]
//            [--script "create 2 1 0; wait 1000; sit 0 0; ..."]
// Commands (script, then stdin; ';' or newlines between them):
//   create <tables> <public 0|1> <preset>   join <code>   joinpublic   leave
//   sit <table> <seat 0|1|-1>   stand   queue <table>   unqueue   ready <table>   decline <table>
//   rules <table> <rules...>    kick <member id>   kickname <name>   chat <text>   flood <n>   status   wait <ms>   quit
//   chara <code> (the character others see beside the name, member data "ch")
// Logs one line per happening on stdout ("bot Bot1: ...").

#include "core/keeper_protocol.h"
#include "core/lobby.h"

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

struct Bot {
    std::string name = "Bot";
    std::string host = "127.0.0.1";
    uint16_t port = 5555;
    bool autoPlay = false;
    int64_t setMs = 3000;
    std::string wins = "random";
    WeyveClient* c = nullptr;
    std::string lobby, room;
    bool inLobby = false, inRoom = false;
    uint32_t seq = 0;
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
        WeyveRoomFilter filters[2] = {{kKind, WEYVE_FILTER_EQUALS, kKindLobby},
                                      {kBuild, WEYVE_FILTER_EQUALS, nullptr}};
        static std::string build;
        build = std::to_string(lobby::kProtocol);
        filters[1].value = build.c_str();
        weyve_list_rooms(c, filters, 2);
        return true;
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
            ss >> s.tables >> pub >> s.preset;
            s.isPublic = pub != 0;
            Ask(RoomRequest(++seq, s));
            Log("asks for a room");
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
            // The first room the lobby lists.
            std::map<std::string, std::string> data;
            for (int i = 0; i < kListKeys; i++) data[ListKey(i)] = Data(ListKey(i).c_str());
            const std::vector<ListedRoom> rooms = UnpackRoomList(data);
            if (rooms.empty()) {
                Log("no public room listed yet; trying again");
                std::lock_guard lock(scriptMutex);
                script.push_front(line);
                waitUntil = NowMs() + 500;
                return;
            }
            weyve_leave_room(c);
            weyve_join_room(c, rooms[0].code.c_str(), nullptr);
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
            if (!lobby.empty()) weyve_join_room(c, lobby.c_str(), nullptr);
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
            for (int t = 0; t < kMaxTables; t++) {
                const std::string s = Data(StateKey(t).c_str());
                if (!s.empty()) Log("  table " + std::to_string(t) + " " + s + " queue " + Data(QueueKey(t).c_str()));
            }
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
                const uint32_t n = weyve_room_list_count(c);
                if (n == 0) {
                    Log("no lobby channel listed");
                    break;
                }
                uint32_t len = 0;
                const char* id = weyve_room_list_id(c, 0, &len); // len is set by the call: read it after
                lobby = Str(id, len);
                Log(std::to_string(n) + " lobby channels listed; joining " + lobby);
                weyve_join_room(c, lobby.c_str(), nullptr);
                break;
            }
            case WEYVE_EVENT_ROOM_ID_ASSIGNED: {
                const std::string code = Str(e.data.room_assigned.id, e.data.room_assigned.id_len);
                inLobby = code == lobby;
                inRoom = !inLobby;
                room = inRoom ? code : std::string();
                shown.clear();
                Log(inLobby ? "in the lobby " + code : "in room " + code + " as member " + std::to_string(weyve_id(c)));
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
                if (!lobby.empty()) weyve_join_room(c, lobby.c_str(), nullptr);
                break;
            default:
                break;
            }
        }
        uint32_t from = 0, len = 0;
        while (const uint8_t* data = weyve_next_p2p(c, &from, &len)) {
            Answer a;
            if (!ParseAnswer(data, len, &a)) continue;
            weyve_set_member_data(c, kAck, std::to_string(a.seq).c_str());
            if (a.kind == "room" && a.ok) {
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

    void Loop()
    {
        for (;;) {
            if (!weyve_poll(c)) {
                Log("lost the server");
                return;
            }
            Events();
            Tables();
            std::string line;
            bool have = false;
            if (NowMs() >= waitUntil && (inLobby || inRoom)) {
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
        else if (a == "--set-ms" && v) bot.setMs = std::atoll(argv[++i]);
        else if (a == "--wins" && v) bot.wins = argv[++i];
        else if (a == "--script" && v) Split(argv[++i], &bot.script);
        else {
            std::fprintf(stderr, "usage: sfvr-bot --name N [--server host:port] [--auto] [--set-ms ms] [--wins p1|p2|random] [--script \"...\"]\n");
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
