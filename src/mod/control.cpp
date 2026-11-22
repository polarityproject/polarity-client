#include "mod/control.h"

#include "mod/battle_setup.h"
#include "mod/commands.h"
#include "mod/lifecycle.h"
#include "mod/local_input.h"
#include "mod/log.h"
#include "mod/memscan.h"
#include "mod/netplay_driver.h"
#include "mod/tracer.h"
#include "mod/watch.h"

#include <fstream>

namespace sfv::mod {

namespace {

struct Step {
    enum Kind { Press,
                Wait,
                Connect,
                Disconnect,
                Game,
                Setup,
                ShowSetup,
                ClearSetup,
                Scan,
                Rescan,
                ScanShow,
                ScanSmall,
                ScanDelta,
                ScanRecord,
                Trace,
                TraceReport,
                Net } kind;

    std::string line;
    MatchSetup setup;
    std::vector<uint32_t> rvas;
    int user = 0;
    PadState pad;
    int holdMs = 0;
    int gapMs = 0;
    GameRequest request;
};

std::mutex g_mutex;
std::deque<GameRequest> g_gameRequests;

const Command* FindCommand(const std::string& name) {
    static const std::unordered_map<std::string, const Command*> table = [] {
        std::unordered_map<std::string, const Command*> t;
        for (const std::span<const Command> area : {CommandsSession(), CommandsObjects(), CommandsBattle()}) {
            for (const Command& c : area) t.emplace(c.name, &c);
        }
        return t;
    }();
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

void RunCommand(const std::string& line) {
    std::stringstream ss(line);
    std::string name;
    ss >> name;
    if (const Command* c = FindCommand(name)) c->run(name, ss);
}

constexpr int kBothPads = -2; // Step::user

bool Parse(const std::string& line, Step& step) {
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd == "pad") {
        std::string user, buttons;
        step.kind = Step::Press;
        step.gapMs = 50;
        ss >> user >> buttons >> step.holdMs >> step.gapMs;
        step.user = user == "both" ? kBothPads : std::atoi(user.c_str());
        return (step.user == kBothPads || (step.user >= 0 && step.user < XUSER_MAX_COUNT)) && InputParseButtons(buttons, &step.pad);
    }
    if (cmd == "wait") {
        step.kind = Step::Wait;
        ss >> step.holdMs;
        return true;
    }
    if (cmd == "scan" || cmd == "rescan") {
        step.kind = cmd == "scan" ? Step::Scan : Step::Rescan;
        ss >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "trace") {
        step.kind = Step::Trace;
        uint32_t rva;
        while (ss >> std::hex >> rva) step.rvas.push_back(rva);
        return !step.rvas.empty();
    }
    if (cmd == "tracereport") {
        step.kind = Step::TraceReport;
        return true;
    }
    if (cmd == "scanrecord") {
        step.kind = Step::ScanRecord;
        ss >> std::hex >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "scanshow") {
        step.kind = Step::ScanShow;
        return true;
    }
    if (cmd == "scansmall") {
        step.kind = Step::ScanSmall;
        return true;
    }
    if (cmd == "scandelta") {
        step.kind = Step::ScanDelta;
        ss >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "setup") {
        step.kind = Step::Setup;
        for (auto& p : step.setup.players) {
            std::string code;
            int costume, color, comment, vskill, vtrigger;
            ss >> code >> costume >> color >> comment >> vskill >> vtrigger;
            if (const size_t colon = code.find(':'); colon != std::string::npos) {
                p.variation = static_cast<uint8_t>(std::atoi(code.c_str() + colon + 1));
                code.resize(colon);
            }
            p.character = PackCode(code.c_str());
            p.costume = static_cast<uint16_t>(costume);
            p.color = static_cast<uint8_t>(color - 1);
            p.comment = static_cast<uint8_t>(comment);
            p.vskill = static_cast<uint8_t>(vskill - 1);
            p.vtrigger = static_cast<uint8_t>(vtrigger - 1);
        }
        int stage = 0;
        ss >> stage;
        step.setup.stage = static_cast<uint16_t>(stage);
        return !ss.fail();
    }
    if (FindCommand(cmd)) {
        step.kind = Step::Net;
        step.line = line;
        return true;
    }
    if (cmd == "showsetup" || cmd == "clearsetup") {
        step.kind = cmd == "showsetup" ? Step::ShowSetup : Step::ClearSetup;
        return true;
    }
    if (cmd == "connect" || cmd == "disconnect") {
        step.kind = cmd == "connect" ? Step::Connect : Step::Disconnect;
        ss >> step.user;
        return step.user >= 0 && step.user < XUSER_MAX_COUNT;
    }
    step.kind = Step::Game;
    GameCommand& c = step.request.command;
    if (cmd == "save") c = GameCommand::Save;
    else if (cmd == "load") c = GameCommand::Load;
    else if (cmd == "ringsave") c = GameCommand::RingSave;
    else if (cmd == "ringload") c = GameCommand::RingLoad;
    else if (cmd == "layout") c = GameCommand::Layout;
    else if (cmd == "watch") {
        c = GameCommand::Watch;
        step.request.arg2 = 4;
        ss >> step.request.arg0 >> std::hex >> step.request.arg1 >> std::dec >> step.request.arg2;
    } else if (cmd == "watchaddr") {
        c = GameCommand::WatchAddr;
        step.request.arg2 = 4;
        std::string rw;
        ss >> std::hex >> step.request.address >> std::dec >> step.request.arg2 >> rw;
        step.request.flag = rw == "rw";
    } else if (cmd == "watchreport") c = GameCommand::WatchReport;
    else if (cmd == "stress" || cmd == "learnnoise") {
        c = cmd == "stress" ? GameCommand::Stress : GameCommand::LearnNoise;
        step.request.arg0 = 600;
        step.request.arg1 = 7;
        step.request.arg2 = 1;
        ss >> step.request.arg0 >> step.request.arg1 >> step.request.arg2;
    } else if (cmd == "unwatch") c = GameCommand::Unwatch;
    else if (cmd == "synctest") {
        c = GameCommand::SyncTest;
        step.request.arg0 = 300;
        step.request.arg1 = 1;
        ss >> step.request.arg0 >> step.request.arg1;
    } else return false;
    return true;
}

void Run(const Step& s) {
    switch (s.kind) {
        case Step::Press: {
            const int first = s.user == kBothPads ? 0 : s.user;
            const int last = s.user == kBothPads ? 1 : s.user;
            for (int u = first; u <= last; u++) InputSetVirtual(u, s.pad);
            if (s.holdMs < 0) break;
            Sleep(s.holdMs);
            for (int u = first; u <= last; u++) InputSetVirtual(u, {});
            Sleep(s.gapMs);
            break;
        }
        case Step::Wait:
            Sleep(s.holdMs);
            break;
        case Step::Connect:
            InputConnectVirtual(s.user, true);
            break;
        case Step::Setup:
            BattleSetupSetPending(s.setup);
            Log("setup pending: %s", BattleSetupDescribe(s.setup).c_str());
            break;
        case Step::Net:
            RunCommand(s.line);
            break;
        case Step::ShowSetup:
            BattleSetupLog("now");
            break;
        case Step::ClearSetup:
            BattleSetupClearPending();
            Log("setup cleared");
            break;
        case Step::Scan:
            ScanStart(s.holdMs);
            break;
        case Step::Rescan:
            ScanNext(s.holdMs);
            break;
        case Step::ScanShow:
            ScanShow();
            break;
        case Step::ScanSmall:
            ScanSmall();
            break;
        case Step::ScanDelta:
            ScanDelta(s.holdMs);
            break;
        case Step::ScanRecord:
            ScanRecordFilter(static_cast<uint32_t>(s.holdMs));
            break;
        case Step::Trace:
            TraceFunctions(ImageBase(), s.rvas);
            break;
        case Step::TraceReport:
            TraceReport(ImageBase());
            break;
        case Step::Disconnect:
            InputConnectVirtual(s.user, false);
            break;
        case Step::Game: {
            const GameCommand c = s.request.command;
            if (c == GameCommand::WatchAddr || c == GameCommand::WatchReport || c == GameCommand::Unwatch) {
                if (c == GameCommand::WatchAddr) {
                    ArmWatch(0, reinterpret_cast<const void*>(s.request.address), s.request.arg2, ImageBase(),
                             s.request.flag);
                }
                if (c == GameCommand::WatchReport) WatchReport();
                if (c == GameCommand::Unwatch) DisarmWatches();
                break;
            }
            std::lock_guard lock(g_mutex);
            g_gameRequests.push_back(s.request);
            break;
        }
    }
}

std::mutex g_stepMutex;
std::condition_variable g_stepReady;
std::deque<Step> g_stepQueue;

void StepRunner() {
    ThreadGuard running;
    for (;;) {
        Step s;
        {
            std::unique_lock lock(g_stepMutex);
            g_stepReady.wait_for(lock, std::chrono::milliseconds(200), [] { return !g_stepQueue.empty() || Stopping(); });
            if (Stopping()) return;
            if (g_stepQueue.empty()) continue;
            s = g_stepQueue.front();
            g_stepQueue.pop_front();
        }
        Run(s);
    }
}

void CommandLoop(std::wstring path) {
    std::thread(StepRunner).detach();
    ThreadGuard running;
    for (;;) {
        std::ifstream in(path);
        if (Stopping()) return;
        NetPollHotkeys();
        InputTick();
        if (in) {
            std::string line;
            std::deque<Step> steps;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                Step s;
                if (Parse(line, s)) steps.push_back(s);
                else Log("cmd: cannot parse '%s'", line.c_str());
            }
            in.close();
            DeleteFileW(path.c_str());
            size_t ahead = 0;
            {
                std::lock_guard lock(g_stepMutex);
                ahead = g_stepQueue.size();
                for (const Step& s : steps) g_stepQueue.push_back(s);
            }
            g_stepReady.notify_one();
            if (ahead) Log("cmd: running %zu steps (after %zu still queued)", steps.size(), ahead);
            else Log("cmd: running %zu steps", steps.size());
        }
        Sleep(50);
    }
}

} // namespace

bool ControlInstall(const std::wstring& commandFile) {
    std::thread(CommandLoop, commandFile).detach();
    Log("control: commands from sfv_cmd.txt");
    return true;
}

GameRequest ControlTakeGameRequest() {
    std::lock_guard lock(g_mutex);
    if (g_gameRequests.empty()) return {};
    const GameRequest r = g_gameRequests.front();
    g_gameRequests.pop_front();
    return r;
}

} // namespace sfv::mod
