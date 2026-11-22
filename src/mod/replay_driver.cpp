#include "mod/replay_internal.h"

namespace sfv::mod {

using namespace replay;

namespace {

std::atomic<bool> g_stop{false};

std::mutex g_forcedMutex;

std::vector<int> g_forcedRollbacks;

} // namespace

namespace replay {

bool ForcedRollback(int frame) {
    std::lock_guard lock(g_forcedMutex);
    return std::find(g_forcedRollbacks.begin(), g_forcedRollbacks.end(), frame) != g_forcedRollbacks.end();
}

} // namespace replay

namespace {

std::string FindRecording(const std::string& name) {
    const std::string inMatches = FilesUtf8Path(FilesMatchesDir()) + "\\" + name;
    if (FILE* f = OpenFile(inMatches, "rb")) {
        std::fclose(f);
        return inMatches;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    return FilesUtf8Path(dir) + "\\reports\\" + name;
}

} // namespace

namespace replay {

ReplayDriver::ReplayDriver(ReplayFile rf, std::string name, int stress, int watchFrom, int watchTo, std::string watchRegion) : rf_(std::move(rf)), name_(std::move(name)), stress_(stress), creationsAtJump_(BattleSetupCreations()), watchFrom_(watchFrom), watchTo_(watchTo), watchRegion_(std::move(watchRegion)) {
    last_ = rf_.finalThrough >= 0 ? std::min(rf_.finalThrough, rf_.FrameCount() - 1) : rf_.FrameCount() - 1;
}

void ReplayDriver::SetViewer(std::string title, std::string path) {
    viewer_ = true;
    title_ = std::move(title);
    path_ = std::move(path);
}

void ReplayDriver::StartAt(const RestartPending& r) {
    startAt_ = r.target;
    paused_ = r.paused;
    fast_ = r.fast;
    roundStarts_ = r.starts;
    roundEnds_ = r.ends;
    roundCalls_ = r.calls;
}

void ReplayDriver::Keep(int from, int to) {
    keepFrom_ = from;
    keepTo_ = to;
}

void ReplayDriver::Hold(int frame, int seconds) {
    if (frame >= 0 && seconds > 0) holds_.push_back({frame, seconds * 1000ull});
}

void ReplayDriver::Mispredict(int delay, int remote) {
    delay_ = delay;
    remote_ = remote >= 0 ? remote & 1 : rf_.localPlayer == 1 ? 0
                                                              : 1;
}

bool ReplayDriver::OnFrame(SfvGame& game) {
    if (g_stop.exchange(false)) {
        Log("replay: stopped");
        Finish(game);
        return false;
    }
    if (!jumped_) {
        if (viewer_ || bench_) FlowPumpGameThread();
        if (BattleSetupCreations() == creationsAtJump_) return false;
        jumped_ = true;
        game.ForgetBattleObjects();
        game.BeginWarmup();
        Log("replay: battle created (%s)", FlowRoundEndState().c_str());
    }
    if (!ready_) return Warmup(game);
    if (holdUntil_) {
        if (GetTickCount64() < holdUntil_) return true;
        holdUntil_ = 0;
    }
    if (viewer_) return ViewerFrame(game);
    if (stress_ > 0) return StressFrame(game);
    if (delay_ > 0) return MispredictFrame(game);
    return StraightFrame(game);
}

} // namespace replay

void ReplaySetLatch(int latch) { g_latch = latch; }

void ReplaySetForcedRollbacks(std::vector<int> frames, uint32_t guessXor) {
    g_forcedGuessXor = guessXor;
    std::lock_guard lock(g_forcedMutex);
    g_forcedRollbacks = std::move(frames);
    std::string text;
    for (int f : g_forcedRollbacks) text += " " + std::to_string(f);
    Log("replay: forced rollbacks at frames:%s (first run on the remote input xor 0x%X)", text.empty() ? " none" : text.c_str(),
        guessXor);
}

void ReplayRequestView(const std::string& path, const std::string& title, std::function<void()> onLeave, int holdAt, int holdSeconds) {
    {
        std::lock_guard lock(g_viewMutex);
        g_viewCommands.clear();
        g_viewTaken = g_view.handled;
        g_viewLeave = std::move(onLeave);
    }
    ReplayRequest(path, 0, -1, -1, "", -1, -1, true, "", holdAt, holdSeconds, 0, -1, title.empty() ? path : title);
}

bool ReplayRequestBenchmark(const std::string& path) {
    if (BattleCurrentFrameDriver()) {
        Log("benchmark: another driver is active");
        return false;
    }
    {
        std::lock_guard lock(g_viewMutex);
        g_viewCommands.clear();
        g_viewLeave = nullptr;
        g_view = ReplayView{};
        g_viewTaken = 0;
        g_view.active = true;
        g_view.benchmark = true;
        g_view.title = "Benchmark";
    }
    ReplayRequest(path, 8, -1, -1, "", -1, -1, true, "", -1, 0, 0, -1, "", false, true);
    if (BattleCurrentFrameDriver()) return true;
    std::lock_guard lock(g_viewMutex);
    g_view = ReplayView{};
    return false;
}

void ReplaySetExperimentValue(const std::string& what, int value) {
    if (what == "callpace") g_callPace = value;
    else if (what == "callafter") g_callAfter = value;
    else if (what == "runticks") g_runTicks = value;
    Log("rview: %s %d", what.c_str(), value);
}

void ReplaySetExperiment(const std::string& what, bool on) {
    if (what == "callkeys") g_callKeys = on;
    else if (what == "crossrounds") g_crossRounds = on;
    else if (what == "anykeys") g_anyKeys = on;
    else if (what == "keysanywhere") g_keysAnywhere = on;
    else if (what == "intro") g_skipIntro = !on;
    else return Log("rview: no experiment %s", what.c_str());
    Log("rview: %s %s", what.c_str(), on ? "on" : "off");
}

uint32_t ReplayApplyControl(ReplayControl c, int arg) {
    std::lock_guard lock(g_viewMutex);
    if (!g_view.active && c != ReplayControl::Leave) return 0;
    g_viewCommands.push_back({c, arg});
    return g_viewTaken + static_cast<uint32_t>(g_viewCommands.size());
}

int ReplayTickFrame() { return g_tickFrame; }

bool ReplayTickInFight() { return g_tickFight; }

int ReplayTickRound() { return g_tickRound; }

ReplayView ReplayViewNow() {
    std::lock_guard lock(g_viewMutex);
    return g_view;
}

void ReplayRequest(const std::string& path, int stressDistance, int watchFrom, int watchTo, const std::string& watchRegion,
                   int keepFrom, int keepTo, bool preload, const std::string& traps, int holdAt, int holdSeconds,
                   int mispredictDelay, int remote, const std::string& viewTitle, bool jumpNow, bool benchmark) {
    if (path == "stop") {
        g_stop = BattleCurrentFrameDriver() != nullptr;
        return;
    }
    if (BattleCurrentFrameDriver()) {
        Log("replay: another driver is active (leave the session first)");
        return;
    }
    const bool relative = path.size() < 2 || (path[1] != ':' && path[0] != '\\' && path[0] != '/');
    const std::string full = relative ? FindRecording(path) : path;
    ReplayFile rf;
    std::string error;
    if (!LoadReplay(full, &rf, &error)) {
        Log("replay: %s: %s", full.c_str(), error.c_str());
        return;
    }
    PickVerdict check = SetupCheck(rf.setup, BattleSetupPickTables());
    for (int tries = 0; check.kind == PickVerdict::Kind::NotYet && tries < 30; tries++) {
        Sleep(100);
        check = SetupCheck(rf.setup, BattleSetupPickTables());
    }
    if (!check.Fine()) {
        Log("replay: %s: this game does not have its setup (%s)", full.c_str(),
            check.kind == PickVerdict::Kind::Bad ? check.why.c_str() : "a character's V-Comments did not load");
        return;
    }
    stressDistance = std::clamp(stressDistance, 0, 30);
    const std::string name = full.substr(full.find_last_of("\\/") + 1);
    Log("replay: %s, %d frames (confirmed through %d), build %s, %s%s", name.c_str(), rf.FrameCount(), rf.finalThrough,
        rf.gameBuild.c_str(), BattleSetupDescribe(rf.setup).c_str(),
        stressDistance ? (", stress distance " + std::to_string(stressDistance)).c_str() : "");
    std::vector<PreloadPick> picks;
    for (const PlayerSetup& p : rf.setup.players) {
        if (p.character) picks.push_back({UnpackCode(p.character), static_cast<int>(p.costume), p.version});
    }
    BattleSetupSetPending(ShownSetup(rf.setup, rf.localPlayer == 1 ? 1 : 0, BattleSetupLookRules()));
    FlowSetPendingStage(rf.setup.stage == kKeepStage ? "" : FlowStageCode(rf.setup.stage));
    Log("replay: stage %s", rf.setup.stage == kKeepStage ? "as prepared" : FlowStageCode(rf.setup.stage).c_str());
    auto* driver = new ReplayDriver(std::move(rf), name, stressDistance, watchFrom, watchTo, watchRegion);
    if (benchmark) driver->SetBenchmark();
    if (!viewTitle.empty()) {
        driver->SetViewer(viewTitle, full);
        if (g_restartPending.target > 0) {
            driver->StartAt(g_restartPending);
            g_restartPending = {};
        }
    }
    driver->Keep(keepFrom, keepTo);
    driver->Hold(holdAt, holdSeconds);
    if (mispredictDelay > 0 && stressDistance == 0) driver->Mispredict(std::clamp(mispredictDelay, 1, 8), remote);
    std::istringstream ts(traps);
    ReplayDriver::Trap t;
    while (ts >> t.frame >> t.region >> std::hex >> t.offset >> std::dec >> t.bytes) driver->AddTrap(t);
    BattleSetFrameDriver(driver);
    if (jumpNow) {
        FlowJumpToBattle();
        return;
    }
    if (!preload) picks.clear();
    FlowPreloadCharacters(std::move(picks), [] { FlowJumpToBattle(); });
}

} // namespace sfv::mod
