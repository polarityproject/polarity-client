#include "mod/commands.h"

#include "core/random_stage.h"
#include "core/version.h"
#include "mod/benchmark.h"
#include "mod/local_input.h"
#include "mod/battle_log_screen.h"
#include "mod/rooms_screen.h"
#include "mod/settings_screen.h"
#include "mod/room_screen.h"
#include "mod/ui_kit.h"
#include "mod/files.h"
#include "mod/report_upload.h"
#include "mod/flow.h"

#include "mod/battle_setup.h"
#include "mod/boot.h"
#include "mod/weyve_service.h"
#include "mod/status.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/overlay.h"
#include "mod/replay_driver.h"
#include "mod/ui_sound.h"
#include "mod/watch_chat.h"

namespace sfv::mod {

namespace {

constexpr DWORD kWineStubException = 0x80000100; // EXCEPTION_WINE_STUB

std::string g_testName;

std::string TestName() { return g_testName.empty() ? "Player " + std::to_string(StatusInstance() + 1) : g_testName; }

void CanceljumpCommand(const std::string&, std::stringstream&) {
    FlowCancelBattleJump();
}

void WheelCommand(const std::string&, std::stringstream& ss) {
    float x = 0, y = 0, notches = 0;
    ss >> x >> y >> notches;
    ui::TestWheel(x, y, notches);
    Log("control: wheel %.0f,%.0f %+.1f", x, y, notches);
}

void WatchchatCommand(const std::string&, std::stringstream& ss) {
    std::string what, text;
    ss >> what;
    std::getline(ss >> std::ws, text);
    if (what == "toggle") WatchChatToggle();
    else if (what == "type" || what == "say") WatchChatTestText(text, what == "say");
    else Log("control: watchchat toggle | type <text> | say <text>");
}

void BenchmarkCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    if (what == "dump") {
        std::string frame;
        ss >> frame;
        BenchmarkSetDumpFrame(frame.empty() || frame == "off" ? -1 : std::atoi(frame.c_str()));
    } else if (what == "file") {
        std::string file;
        std::getline(ss >> std::ws, file);
        const std::string path = file.find(':') != std::string::npos ? file : FilesUtf8Path(FilesMatchesDir()) + "\\" + file;
        if (!ReplayRequestBenchmark(path)) Log("benchmark: not started");
    } else if (!BenchmarkStart(kPolarityVersion)) Log("benchmark: not started");
}

void HostCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string word;
    o.randomSetup = false;
    o.randomCostumes = false;
    bool manual = false;
    while (ss >> word) {
        if (word == "random") o.randomSetup = true;
        if (word == "costumes") o.randomCostumes = true;
        if (word == "manual") manual = true;
    }
    NetplayOptions command = o;
    command.autoReady = !manual;
    command.name = TestName();
    NetHost(command);
}

void JoinCommand(const std::string& cmd, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string code, word;
    ss >> code >> word;
    NetplayOptions command = o;
    command.autoReady = word != "manual";
    command.name = TestName();
    NetJoin(code, command, cmd == "spectate");
}

void TicketCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string what, word;
    ss >> what;
    MatchTicket t = what == "host" ? DirectMatchHost() : DirectMatchJoin("", what == "watch");
    if (what != "host") ss >> t.roomCode;
    while (ss >> word) {
        if (word == "token") ss >> t.seatToken;
        else if (word == "set") t.after = AfterMatch::Set;
        else if (word == "rules") {
            std::string rest;
            std::getline(ss >> std::ws, rest);
            std::string error;
            if (!ParseRules(rest, &t.rules, &error)) {
                Log("ticket: rules '%s' not read: %s", rest.c_str(), error.c_str());
                return;
            }
        }
    }
    NetplayOptions command = o;
    command.autoReady = true;
    command.name = TestName();
    NetPlayTicket(t, command);
}

void PresenceCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    static std::unique_ptr<WeyveService> presence;
    std::string what;
    ss >> what;
    if (what == "open") {
        const std::string server = o.server;
        presence = WeyveService::Start(server, TestName() + " (presence)", server == kDefaultServer ? kDefaultServerAlsoPort : 0);
        presence->CreateRoom();
        Log("presence: connecting to %s", server.c_str());
    } else if (what == "close") {
        presence.reset();
        Log("presence: closed");
    } else if (presence) {
        const WeyveRoomView v = presence->View();
        static const char* const kServer[] = {"connecting", "connected", "failed", "lost"};
        Log("presence: %s (%s), room %s, %zu members, self %u", kServer[static_cast<int>(v.server)], v.serverText.c_str(),
            v.room.empty() ? "-" : WeyveService::DisplayCode(v.room).c_str(), v.members.size(), v.self);
        WeyveEvent e;
        while (presence->NextEvent(e)) Log("presence: event %d %s", static_cast<int>(e.type), e.text.c_str());
    } else {
        Log("presence: not open");
    }
}

void SettingsCommand(const std::string&, std::stringstream& ss) {
    std::string row, what;
    ss >> row >> what;
    if (row == "close") SettingsScreenHide();
    else if (what == "type") {
        std::string text;
        std::getline(ss >> std::ws, text);
        SettingsScreenShowTyping(row, text);
    } else SettingsScreenShow(row);
}

void RoomsCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string what;
    ss >> what;
    if (what == "open") {
        RoomsOpen(o.server, TestName(), o.server == kDefaultServer ? kDefaultServerAlsoPort : o.serverAlsoPort, o.keeperKey);
        RoomsScreenShow();
    } else if (what == "close") {
        RoomsClose();
    } else if (what == "create") {
        keeper::RoomSettings s;
        int pub = 1;
        std::string title = "0";
        ss >> s.tables >> pub >> title;
        s.isPublic = pub != 0;
        if (!keeper::ParseTitle(title, &s.title)) {
            Log("rooms: no such title '%s' (5, or 3,RYU,KEN)", title.c_str());
            return;
        }
        RoomsScreenShow();
        if (!RoomsCreate(s)) Log("rooms: not open");
    } else if (what == "join") {
        std::string code;
        ss >> code;
        if (!RoomsJoin(code)) Log("rooms: '%s' is no room code (or not open)", code.c_str());
    } else if (what == "leave") {
        RoomsLeave();
    } else if (what == "table") {
        std::string kind, a;
        ss >> kind;
        std::vector<std::string> args;
        if (kind == "rules") {
            std::string table, rest;
            ss >> table;
            std::getline(ss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            args = {table, rest};
        } else {
            while (ss >> a) args.push_back(a);
        }
        RoomsTable(kind, args);
    } else if (what == "watch") {
        int table = -1;
        ss >> table;
        RoomsWatch(table);
    } else if (what == "mute") {
        uint32_t id = 0;
        ss >> id;
        RoomToggleMuteForTest(id);
    } else if (what == "chat") {
        std::string text;
        std::getline(ss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        if (!RoomsChat(text)) Log("rooms: chat not sent (not in a room, or past the limits)");
    } else {
        const RoomsView v = RoomsSnapshot();
        Log("rooms: %s", RoomsDescribe(v).c_str());
        for (const keeper::ListedRoom& r : v.publicRooms)
            Log("rooms:   public %s '%s' by %s, %d members, %d tables, %d open seats", r.code.c_str(),
                keeper::TitleText(r.title).c_str(), r.owner.c_str(), r.members, r.tables, r.openSeats);
        for (size_t i = v.chat.size() > 5 ? v.chat.size() - 5 : 0; i < v.chat.size(); i++)
            Log("rooms:   chat %s: %s", v.chat[i].from ? v.chat[i].name.c_str() : "*", v.chat[i].text.c_str());
    }
}

void MatchchatCommand(const std::string&, std::stringstream& ss) {
    std::string text;
    std::getline(ss >> std::ws, text);
    NetTestMatchChat(text);
}

void DelayCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    int frames = 2;
    ss >> frames;
    o.inputDelay = static_cast<uint8_t>(std::clamp(frames, 0, 10));
    Log("netplay: input delay %u for the next session", o.inputDelay);
}

void NameCommand(const std::string&, std::stringstream& ss) {
    std::getline(ss >> std::ws, g_testName);
    Log("netplay: test sessions play as %s", TestName().c_str());
}

void CrashCommand(const std::string&, std::stringstream& ss) {
    std::string how;
    ss >> how;
    Log("crash: %s on purpose (testing)", how.c_str());
    if (how == "thread") {
        std::thread([] { *reinterpret_cast<volatile int*>(16) = 1; }).detach();
    } else if (how == "overflow") {
        FlowRunOnGameThread([] {
            struct Deep {
#pragma warning(suppress : 4717) // endless on purpose
                static int Down(volatile int n) {
                    volatile char pad[4096];
                    pad[0] = static_cast<char>(n);
                    return Down(n + 1) + pad[0];
                }
            };
            Deep::Down(0);
        });
    } else if (how == "hang") {
        FlowRunOnGameThread([] {
            for (volatile bool spin = true; spin;) {
            }
        });
    } else if (how == "fastfail") {
        FlowRunOnGameThread([] { __fastfail(7); });
    } else if (how == "raise") {
        FlowRunOnGameThread([] { RaiseException(1, 0, 0, nullptr); });
    } else if (how == "stub") {
        FlowRunOnGameThread([] {
            static const char dll[] = "cabinet.dll";
            const ULONG_PTR args[2] = {reinterpret_cast<ULONG_PTR>(dll), 33};
            RaiseException(kWineStubException, EXCEPTION_NONCONTINUABLE, 2, args);
        });
    } else if (how == "exit") {
        FlowRunOnGameThread([] { TerminateProcess(GetCurrentProcess(), 3); });
    } else {
        FlowRunOnGameThread([] { *reinterpret_cast<volatile int*>(16) = 1; });
    }
}

void ReportsCommand(const std::string&, std::stringstream& ss) {
    std::string what, url;
    ss >> what;
    if (what == "url") {
        ss >> url;
        ReportsSetUrl(url);
        Log("reports: server %s", url.empty() ? "as configured" : url.c_str());
    } else ReportsSendReady();
}

void UpdateCommand(const std::string&, std::stringstream& ss) {
    std::string what, url;
    ss >> what >> url;
    if (what == "check") {
        if (!url.empty()) UpdateSetUrl(url);
        UpdateCheck();
    } else if (what == "install") UpdateInstall();
    else if (what == "pretend" && !url.empty()) UpdatePretendVersion(url);
    else if (what == "restart") {
        std::string error;
        if (!UpdateRestartGame(InputGameWindow(), &error)) Log("update: restart: %s", error.c_str());
    }
    const UpdateView v = UpdateViewNow();
    Log("update: state %d, this build %s, site's %s, %llu of %llu bytes%s%s", static_cast<int>(v.state), v.current.c_str(),
        v.latest.empty() ? "-" : v.latest.c_str(), static_cast<unsigned long long>(v.done),
        static_cast<unsigned long long>(v.total), v.message.empty() ? "" : ": ", v.message.c_str());
}

void PacklogsCommand(const std::string&, std::stringstream&) {
    std::string error;
    if (FilesPackLogsToShare(&error).empty()) Log("files: cannot pack the logs: %s", error.c_str());
}

void ServerCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string address;
    int alsoPort = 0;
    ss >> address >> alsoPort;
    o.server = !address.empty() ? address : !o.savedServer.empty() ? o.savedServer
                                                                   : kDefaultServer;
    o.serverAlsoPort = static_cast<uint16_t>(std::clamp(alsoPort, 0, 65535));
    Log("netplay: rooms server %s for the next sessions", o.server.c_str());
}

void TransportCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string which;
    ss >> which;
    o.udp = which == "udp";
    Log("netplay: transport %s", o.udp ? "UDP" : ("Weyvelength " + o.server).c_str());
}

void ReadyCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    NetSetReady(on != 0);
}

void ChooseCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    NetChoose(what == "rematch" ? lobby::Choice::Rematch : what == "select" ? lobby::Choice::CharacterSelect
                                                                            : lobby::Choice::None);
}

void LeaveCommand(const std::string&, std::stringstream&) {
    NetLeave();
}

void ParkCommand(const std::string&, std::stringstream&) {
    NetPark();
}

void UistartCommand(const std::string&, std::stringstream& ss) {
    int ms = 150;
    std::string buttons = "start";
    ss >> ms >> buttons;
    PadState parsed;
    if (InputParseButtons(buttons, &parsed)) InputPressUi(parsed.buttons, ms);
}

void WatchmatchCommand(const std::string&, std::stringstream& ss) {
    std::string which;
    ss >> which;
    const NetView v = NetViewNow();
    uint64_t id = 0;
    if (which != "live") {
        const int n = std::atoi(which.c_str());
        if (n < 1 || n > static_cast<int>(v.matches.size())) {
            Log("watchmatch: no match %s (%zu kept)", which.c_str(), v.matches.size());
            return;
        }
        id = v.matches[n - 1].id;
    }
    Log("watchmatch: %s of %zu", which.c_str(), v.matches.size());
    NetWatchMatch(id);
}

void ShotCommand(const std::string&, std::stringstream& ss) {
    std::string path;
    int scale = 1;
    ss >> path >> scale;
    if (path.empty()) Log("control: shot <path> [scale]");
    else OverlayRequestScreenshot(path, scale > 0 ? scale : 1);
}

void BurstonCommand(const std::string&, std::stringstream& ss) {
    int code = 5, frames = 300, scale = 4, count = 4;
    std::string dir;
    ss >> code >> dir >> frames >> scale >> count;
    FlowSetBurstOnEvent(code, dir, frames, scale, count);
}

void ShotburstCommand(const std::string&, std::stringstream& ss) {
    std::string dir;
    int frames = 60, scale = 2;
    ss >> dir >> frames >> scale;
    OverlayRequestShotBurst(dir, frames, scale);
}

void EchoCommand(const std::string&, std::stringstream& ss) {
    std::string rest;
    std::getline(ss, rest);
    Log("cmd: echo%s", rest.c_str());
}

void UipadsCommand(const std::string&, std::stringstream& ss) {
    int on = 0;
    ss >> on;
    InputIsolatePads(!on);
    InputBlockPause(!on);
    Log("control: game sees the local pads during a match: %d", on);
}

void UisoundCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    ss >> name;
    if (!UiSoundPlayNamed(name)) Log("uisound: no sound %s", name.c_str());
}

void VoicelangCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    FlowSetVoiceLanguageOverride(what == "ja" ? 0 : what == "en" ? 1
                                                                 : -1);
    Log("voicelang: %s (Ryu speaks %d)", what.c_str(), FlowVoiceLanguageFor("RYU"));
}

void VsyncCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    if (what == "on" || what == "off") OverlaySetVsync(what == "on");
    Log("overlay: %s", OverlayVsyncInfo().c_str());
}

void CountryflagCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    WeyveService::SetHideCountry(what == "off");
}

void WordfilterCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    RoomsSetWordFilter(what != "off");
}

void RelayonlyCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    WeyveService::SetRelayOnly(what != "off");
}

void FreeplayCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    BootSetFreePlayGuard(what);
}

void OverlayCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    if (what == "show") OverlaySetVisible(true);
    else if (what == "select") {
        std::string phase, watching;
        ss >> phase >> watching;
        OverlayRequestSelectPreview(phase == "grid" ? 1 : phase == "loadout" ? 2
                                                      : phase == "ready"     ? 3
                                                                             : 0,
                                    watching == "spectate");
    } else if (what == "hide") OverlaySetVisible(false);
    else if (what == "rooms") {
        HubShow();
        RoomsScreenShow();
    } else if (what == "hub") RoomsScreenHide();
    else if (what == "benchresult") {
        OverlayShowLastBenchmarkResult();
    } else if (what == "battlelog") {
        HubShow();
        BattleLogShow();
    } else if (what == "preview") {
        std::string rest;
        std::getline(ss, rest);
        OverlayPreview(rest);
    } else OverlayToggle();
}

void MenuCommand(const std::string&, std::stringstream&) {
    FlowReturnToMainMenu(1);
}

void ReplayCommand(const std::string&, std::stringstream& ss) {
    std::string file, word, region, traps;
    int stress = 0, from = -1, to = -1, keepFrom = -1, keepTo = -1, holdAt = -1, holdSeconds = 0, mispredict = 0,
        remote = 0;
    bool cold = false;
    ss >> file;
    if (file == "view") {
        ss >> file;
        if (ss >> word && word == "hold") ss >> holdAt >> holdSeconds;
        ReplayRequestView(file, file, nullptr, holdAt, holdSeconds);
        return;
    }
    while (ss >> word) {
        if (word == "stress") ss >> stress;
        if (word == "watch") ss >> from >> to >> region;
        if (word == "keep") ss >> keepFrom >> keepTo;
        if (word == "cold") cold = true;
        if (word == "hold") ss >> holdAt >> holdSeconds;
        if (word == "mispredict") ss >> mispredict;
        if (word == "remote") ss >> remote;
        if (word == "latch") {
            std::string how;
            ss >> how;
            ReplaySetLatch(how == "tick" ? 1 : how == "game" ? 0
                                                             : -1);
        }
        if (word == "trap") {
            std::string f, r, off, b;
            ss >> f >> r >> off >> b;
            traps += f + " " + r + " " + off + " " + b + " ";
        }
    }
    ReplayRequest(file, stress, from, to, region, keepFrom, keepTo, !cold, traps, holdAt, holdSeconds, mispredict, remote - 1);
}

void RviewCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    int arg = 0;
    ss >> what >> arg;
    if (what == "callkeys" || what == "crossrounds" || what == "anykeys" || what == "keysanywhere" || what == "intro") {
        ReplaySetExperiment(what, arg != 0);
        return;
    }
    if (what == "callpace" || what == "callafter" || what == "runticks") {
        ReplaySetExperimentValue(what, arg);
        return;
    }
    if (what == "status") {
        const ReplayView v = ReplayViewNow();
        Log("rview: %s frame %d of %d, round %d, %s%s, fast %d, %d keyframe loads, %d frames differ, back to %d, "
            "round frame %d of %d, forward %d, progress %.3f, opening %d, next round %d",
            v.active ? "active" : "none", v.frame, v.frames, v.round, v.paused ? "paused" : "playing", v.seeking ? " (seeking)" : "",
            v.fast, v.seeks, v.mismatches, v.backLimit, v.roundFrame, v.roundFrames, v.canStepForward ? 1 : 0,
            static_cast<double>(v.roundProgress), v.opening ? 1 : 0, v.canRoundForward ? 1 : 0);
        Log("rview: takeover %d, countdown %.1f, ending %d", v.takeover, static_cast<double>(v.takeoverCountdown), v.takeoverEnding ? 1 : 0);
        return;
    }
    const ReplayControl c = what == "pause" ? ReplayControl::Pause : what == "play"       ? ReplayControl::Play
                                                                 : what == "step"         ? ReplayControl::Step
                                                                 : what == "seek"         ? ReplayControl::Seek
                                                                 : what == "round"        ? ReplayControl::Round
                                                                 : what == "fast"         ? ReplayControl::Fast
                                                                 : what == "takeover"     ? ReplayControl::Takeover
                                                                 : what == "takeoverstop" ? ReplayControl::TakeoverStop
                                                                 : what == "leave"        ? ReplayControl::Leave
                                                                 : what == "outofsync"    ? ReplayControl::OutOfSync
                                                                 : what == "holdat"       ? ReplayControl::HoldAt
                                                                                          : ReplayControl::Toggle;
    ReplayApplyControl(c, arg);
}

void NextstageCommand(const std::string&, std::stringstream& ss) {
    std::string code;
    ss >> code;
    FlowSetPendingStage(code);
    Log("next stage: %s", code.empty() ? "(game's choice)" : code.c_str());
}

void RandomoffCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string list;
    ss >> list;
    o.randomOff = ParseStageMask(list);
    Log("random stages: leaving out '%s'", FormatStageMask(o.randomOff).c_str());
}

void RandomstagesCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    int n = 200;
    ss >> n;
    std::mt19937_64 rng{std::random_device{}()};
    std::map<std::string, int> counts;
    for (int i = 0; i < n; i++) {
        const int owned = FlowRandomStage(rng(), true, o.randomOff), any = FlowRandomStage(rng(), false);
        counts["owned " + (owned < 0 ? std::string("none") : FlowStageCode(owned))]++;
        counts["any " + (any < 0 ? std::string("none") : FlowStageCode(any))]++;
    }
    for (const auto& [code, count] : counts) Log("randomstages: %s %d", code.c_str(), count);
}

void BattlenowCommand(const std::string&, std::stringstream&) {
    FlowJumpToBattle();
}

void NetsimCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    double loss = 0;
    ss >> o.conditions.latencyMs >> o.conditions.jitterMs >> loss;
    o.conditions.loss = loss / 100.0;
    NetSetConditions(o.conditions);
}

void MashCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    o.mashFrom = 0;
    o.inputLoop.clear();
    std::string first;
    ss >> first;
    if (first == "loop") {
        o.mashSeed = 0;
        std::string word;
        while (ss >> word) {
            const size_t colon = word.find(':');
            PadState step;
            if (colon == std::string::npos || !InputParseButtons(word.substr(0, colon), &step)) {
                Log("netplay: mash loop: cannot read '%s'", word.c_str());
                continue;
            }
            o.inputLoop.push_back({step.buttons, static_cast<uint16_t>(std::max(1, std::atoi(word.c_str() + colon + 1)))});
        }
        Log("netplay: local player plays a loop of %zu inputs", o.inputLoop.size());
    } else {
        o.mashSeed = std::strtoull(first.c_str(), nullptr, 10);
        ss >> o.mashFrom;
        Log("netplay: local player mashes with seed %llu", static_cast<unsigned long long>(o.mashSeed));
    }
}

void DumpsCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    ss >> o.dumpEvery >> o.maxDumps;
    Log("netplay: recordings dump the state every %d frames, at most %d times", o.dumpEvery, o.maxDumps);
}

void RecordingsCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string what;
    ss >> what;
    if (what == "full" || what == "compact") o.fullRecordings = what == "full";
    Log("netplay: recordings are %s", o.fullRecordings ? "full (every frame's region hashes)" : "compact (region hashes around desyncs)");
}

void PickCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string code;
    int costume = 0, color = 1, comment = 0, vskill = 1, vtrigger = 1;
    ss >> code >> costume >> color >> comment >> vskill >> vtrigger;
    o.pick.variation = 0;
    if (const size_t colon = code.find(':'); colon != std::string::npos) {
        o.pick.variation = static_cast<uint8_t>(std::atoi(code.c_str() + colon + 1));
        code.resize(colon);
    }
    o.hasPick = !code.empty();
    o.pick.character = PackCode(code.c_str());
    OverlayReloadChoices();
    o.pick.costume = static_cast<uint16_t>(costume);
    o.pick.color = static_cast<uint8_t>(color - 1);
    o.pick.comment = static_cast<uint8_t>(comment);
    o.pick.vskill = static_cast<uint8_t>(vskill - 1);
    o.pick.vtrigger = static_cast<uint8_t>(vtrigger - 1);
    if (code == "ELV") {
        const uint32_t roll = static_cast<uint32_t>(GetTickCount64());
        std::string as;
        int silver = 0;
        if (FlowElevenStandIn(roll, &as, &silver)) {
            o.pick = PlayerSetup{};
            o.pick.character = PackCode(as.c_str());
            o.pick.costume = static_cast<uint16_t>(silver);
            o.pick.vskill = static_cast<uint8_t>((roll >> 8) & 1);
            o.pick.vtrigger = static_cast<uint8_t>((roll >> 9) & 1);
            Log("netplay: Eleven plays %s in costume %d", as.c_str(), silver);
        }
    }
    NetSetPick(o.hasPick, o.pick);
}

void StageCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    std::string v;
    ss >> v;
    o.stageCode.clear();
    o.stage = kKeepStage;
    if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) o.stage = static_cast<uint16_t>(std::stoi(v));
    else o.stageCode = v;
    if (!o.stageCode.empty()) NetSetStage(o.stageCode);
}

void NetstatusCommand(const std::string&, std::stringstream&) {
    Log("netplay status: %s", NetStatus().c_str());
}

void BattleCommand(const std::string&, std::stringstream&) {
    Log("battle: %s | rendering %.1f fps", NetBattleInfo().c_str(), OverlayRenderFps());
}

void SetstageCommand(const std::string&, std::stringstream& ss) {
    int stage = 0;
    ss >> stage;
    uint8_t* data = BattleData();
    if (data) *reinterpret_cast<int32_t*>(data + battledata::kStage) = stage;
    Log("setstage %d: %s", stage, data ? "written" : "no battle data now");
}

void StartskewCommand(const std::string&, std::stringstream&) {
    NetSkewNextStartForTest();
    Log("startskew: the next frozen start hashes differently");
}

void PickcheckCommand(const std::string&, std::stringstream&) {
    BattleSetupLogPickCheck();
}

void BadpickCommand(const std::string&, std::stringstream& ss) {
    std::string field, message;
    ss >> field >> message;
    const auto bad = BadPickParse(field, message);
    NetSendBadPickForTest(bad);
    Log(bad ? "badpick %s %s: sent from now on" : "badpick %s %s: off", field.c_str(), message.c_str());
}

void SetroundsCommand(const std::string&, std::stringstream& ss) {
    int rounds = 0;
    ss >> rounds;
    BattleSetupSetRoundsForTest(rounds);
    Log("setrounds %d: match setups write it from now on", rounds);
}

constexpr Command kCommands[] = {
    {"canceljump", CanceljumpCommand},
    {"wheel", WheelCommand},
    {"watchchat", WatchchatCommand},
    {"benchmark", BenchmarkCommand},
    {"host", HostCommand},
    {"join", JoinCommand},
    {"spectate", JoinCommand},
    {"ticket", TicketCommand},
    {"presence", PresenceCommand},
    {"settings", SettingsCommand},
    {"rooms", RoomsCommand},
    {"matchchat", MatchchatCommand},
    {"delay", DelayCommand},
    {"name", NameCommand},
    {"crash", CrashCommand},
    {"reports", ReportsCommand},
    {"update", UpdateCommand},
    {"packlogs", PacklogsCommand},
    {"server", ServerCommand},
    {"transport", TransportCommand},
    {"ready", ReadyCommand},
    {"choose", ChooseCommand},
    {"leave", LeaveCommand},
    {"park", ParkCommand},
    {"uistart", UistartCommand},
    {"watchmatch", WatchmatchCommand},
    {"shot", ShotCommand},
    {"burston", BurstonCommand},
    {"shotburst", ShotburstCommand},
    {"echo", EchoCommand},
    {"uipads", UipadsCommand},
    {"uisound", UisoundCommand},
    {"voicelang", VoicelangCommand},
    {"vsync", VsyncCommand},
    {"countryflag", CountryflagCommand},
    {"wordfilter", WordfilterCommand},
    {"relayonly", RelayonlyCommand},
    {"freeplay", FreeplayCommand},
    {"overlay", OverlayCommand},
    {"menu", MenuCommand},
    {"replay", ReplayCommand},
    {"rview", RviewCommand},
    {"nextstage", NextstageCommand},
    {"randomoff", RandomoffCommand},
    {"randomstages", RandomstagesCommand},
    {"battlenow", BattlenowCommand},
    {"netsim", NetsimCommand},
    {"mash", MashCommand},
    {"dumps", DumpsCommand},
    {"recordings", RecordingsCommand},
    {"pick", PickCommand},
    {"stage", StageCommand},
    {"netstatus", NetstatusCommand},
    {"battle", BattleCommand},
    {"setstage", SetstageCommand},
    {"startskew", StartskewCommand},
    {"pickcheck", PickcheckCommand},
    {"badpick", BadpickCommand},
    {"setrounds", SetroundsCommand},
};

} // namespace

std::span<const Command> CommandsSession() { return kCommands; }

} // namespace sfv::mod
