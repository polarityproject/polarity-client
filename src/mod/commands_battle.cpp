#include "mod/commands.h"

#include "mod/local_input.h"
#include "mod/battle_data.h"
#include "mod/battle_driver.h"
#include "mod/frame_meter.h"
#include "mod/hitbox_display.h"
#include "mod/flow.h"

#include "mod/battle_setup.h"
#include "mod/camera_rollback.h"
#include "mod/hud_step.h"
#include "mod/watch.h"
#include "mod/log.h"
#include "mod/ue4.h"
#include "mod/netplay_driver.h"
#include "mod/outside_probe.h"
#include "mod/replay_driver.h"

namespace sfv::mod {

namespace {

void WatchnameCommand(const std::string&, std::stringstream& ss) {
    std::string name, off;
    int len = 4;
    ss >> name >> off >> len;
    const size_t offset = std::stoull(off.empty() ? "0" : off, nullptr, 16);
    BattleWatchRegionNamed(name, offset, len); // from this thread: a watch covers every thread but the one arming it
}

void WatchphaseCommand(const std::string&, std::stringstream&) {
    constexpr uint32_t kRequestedPhaseOff = 0x134;
    uint8_t* m = FlowPhaseMachineObject();
    if (!m) Log("watchphase: no phase machine");
    else Log("watchphase: %s", ArmWatch(1, m + kRequestedPhaseOff, 4, ImageBase()) ? "armed on the requested phase" : "not armed");
}

void CamrollbackCommand(const std::string& cmd, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    if (cmd == "camlog") FlowRunOnGameThread([on] { SetCameraRollbackLog(on != 0); });
    else FlowRunOnGameThread([on] { SetCameraRollback(on != 0); });
}

void HudstepCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    FlowRunOnGameThread([on] { SetHudStep(on); });
}

void CamtraceCommand(const std::string&, std::stringstream& ss) {
    uint32_t from = 0, to = 0;
    std::string text = "Camera";
    ss >> from >> to >> text;
    FlowRunOnGameThread([=] { BattleTraceCameraObjects(from, to, text); });
}

void CinelogCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    FlowRunOnGameThread([on] { BattleLogCinematics(on); });
}

void ActorlogCommand(const std::string&, std::stringstream& ss) {
    int ticks = 120;
    ss >> ticks;
    FlowRunOnGameThread([ticks] { BattleLogActors(ticks); });
}

void RcmpCommand(const std::string&, std::stringstream& ss) {
    std::string a, b, f, t;
    ss >> a >> b >> f >> t;
    auto hex = [](const std::string& s) { return s.rfind('-', 0) == 0 ? -std::stoll(s.substr(1), nullptr, 16) : std::stoll(s, nullptr, 16); };
    const long long from = hex(f), to = hex(t);
    FlowRunOnGameThread([=] { BattleCompareRegions(a, b, from, to); });
}

void RpeekCommand(const std::string&, std::stringstream& ss) {
    std::string name, off;
    int qwords = 4;
    ss >> name >> off >> qwords;
    const long long offset = off.rfind('-', 0) == 0 ? -std::stoll(off.substr(1), nullptr, 16) : std::stoll(off, nullptr, 16);
    FlowRunOnGameThread([=] { BattlePeekRegion(name, offset, qwords); });
}

void PtrscanCommand(const std::string&, std::stringstream& ss) {
    std::string from, to;
    size_t before = 0, after = 0;
    ss >> from >> std::hex >> before >> after >> std::dec >> to;
    FlowRunOnGameThread([=] { BattleScanForPointers(from, before, after, to); });
}

void RsnapCommand(const std::string&, std::stringstream&) {
    FlowRunOnGameThread([] { BattleSnapRegions(); });
}

void BoxlogCommand(const std::string&, std::stringstream& ss) {
    int ticks = 60;
    ss >> ticks;
    LogHitboxes(ticks);
}

void WinsizeCommand(const std::string&, std::stringstream& ss) {
    int w = 0, h = 0;
    ss >> w >> h;
    if (HWND win = InputGameWindow(); win && w > 0 && h > 0) {
        RECT r{0, 0, w, h};
        AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongPtrW(win, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(win, GWL_EXSTYLE)));
        SetWindowPos(win, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT c{};
        GetClientRect(win, &c);
        Log("winsize: client %ldx%ld", c.right, c.bottom);
    }
}

void FtraceCommand(const std::string&, std::stringstream& ss) {
    int ticks = 600;
    ss >> ticks;
    FlowRunOnGameThread([ticks] { BattleTraceRegions(ticks); });
}

void FreezeCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    BattleSetFreeze(on != 0);
}

void FramemeterCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    SetFrameMeterForced(on != 0);
}

void BattledataCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    if (what == "dump") FlowRunOnGameThread([] { DumpLoadedBattleData(); });
    else CheckBattleDataSoon("asked");
    Log("battledata: mod paks: %s", ScanModPaks().Text().c_str());
}

void FmlogCommand(const std::string&, std::stringstream& ss) {
    int ticks = 120;
    ss >> ticks;
    LogFrameMeter(ticks);
}

void HitboxesCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    SetHitboxesForced(on != 0);
}

void RdumpCommand(const std::string&, std::stringstream& ss) {
    std::string name, tag;
    ss >> name >> tag;
    FlowRunOnGameThread([=] { BattleDumpRegion(name, tag); });
}

void RdiffCommand(const std::string&, std::stringstream& ss) {
    std::string mode = "side", prefix;
    ss >> mode >> prefix;
    FlowRunOnGameThread([mode, prefix] { BattleDiffRegions(mode, prefix); });
}

void DumpbacCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    ss >> name;
    FlowRunOnGameThread([name] { BattleDumpMoveFile(name); });
}

void RfindCommand(const std::string&, std::stringstream& ss) {
    std::string kind, a, b;
    ss >> kind >> a >> b;
    FlowRunOnGameThread([kind, a, b] { BattleFindInRegions(kind, a, b); });
}

void WarmupsleepCommand(const std::string&, std::stringstream& ss) {
    int ms = 0;
    ss >> ms;
    GameSetWarmupSleep(ms);
    Log("control: warm-up ticks wait %d ms", ms);
}

void WatchcameraCommand(const std::string&, std::stringstream& ss) {
    std::string when;
    ss >> when;
    if (when == "new") {
        GameWatchNextCamera();
        Log("control: the next battle camera is watched from its construction");
    } else {
        GameWatchCameraAtFreeze();
        Log("control: the camera is watched from the next freeze");
    }
}

void EmptystageCommand(const std::string&, std::stringstream&) {
    FlowTestEmptyStageRequest();
    Log("control: the next jump's stage request is emptied (test)");
}

void WarmupextraCommand(const std::string&, std::stringstream& ss) {
    int n = 0;
    ss >> n;
    GameSetWarmupExtra(n);
    Log("control: %d warm-up ticks after the objects", n);
}

void MappedlogCommand(const std::string&, std::stringstream& ss) {
    int updates = 0;
    ss >> updates;
    InputLogMapped(updates);
    Log("mappedlog: the input manager's pad 0 for the next %d updates, on change", updates);
}

void InputlogCommand(const std::string&, std::stringstream& ss) {
    int ticks = 0;
    ss >> ticks;
    BattleLogGameInputs(ticks);
}

void ResimtasksCommand(const std::string&, std::stringstream& ss) {
    std::vector<uintptr_t> rvas;
    std::string word;
    bool standard = false;
    while (ss >> word) {
        if (word == "off") continue;
        if (word == "default") {
            standard = true;
            continue;
        }
        if (word[0] == 'v') rvas.push_back(kResimTaskByClass | static_cast<uintptr_t>(std::stoull(word.substr(1), nullptr, 16)));
        else rvas.push_back(static_cast<uintptr_t>(std::stoull(word, nullptr, 16)));
    }
    FlowSetResimTaskSkip(standard ? FlowDefaultLeanResimTasks() : std::move(rvas));
}

void ResimskipCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    BattleSetResimPresentationSkip(on != 0);
}

void PhasecostCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    BattleSetPhaseTiming(on != 0);
}

void SerialjobsCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    BattleSetSerialJobs(on != 0);
}

void JoborderCommand(const std::string&, std::stringstream& ss) {
    int calls = 1;
    ss >> calls;
    BattleLogJobOrderNext(calls);
}

void StartprobeCommand(const std::string&, std::stringstream&) {
    BattleLogStartProbe();
}

void SoundrollbackCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    FlowSetSoundRollback(on != 0);
}

void FrameidsCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    GameSetFrameIds(on != 0);
    Log("restore: callbacks get %s frame id", on ? "the game's" : "the mod's old");
}

void LeanresimCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    GameSetLeanResim(on != 0);
    FlowSetResimTaskSkip(on ? FlowDefaultLeanResimTasks() : std::vector<uintptr_t>{});
    Log("resim: replayed ticks %s", on ? "without presentation the newest tick makes up for" : "each a whole battle update");
}

void ForcerollbackCommand(const std::string&, std::stringstream& ss) {
    std::vector<int> frames;
    uint32_t guessXor = 0;
    std::string w;
    while (ss >> w) {
        if (w == "xor") ss >> std::hex >> guessXor >> std::dec;
        else frames.push_back(std::atoi(w.c_str()));
    }
    ReplaySetForcedRollbacks(std::move(frames), guessXor);
}

void PtrtestCommand(const std::string&, std::stringstream& ss) {
    std::string how;
    ss >> how;
    FlowSetLegacyPointerTest(how == "old");
}

void KowaitmodeCommand(const std::string&, std::stringstream& ss) {
    int on = 1;
    ss >> on;
    FlowSetKoWaitRollback(on != 0);
}

void SoundlogCommand(const std::string&, std::stringstream& ss) {
    int count = 400;
    ss >> count;
    BattleLogSoundPlays(count);
}

void BonelogCommand(const std::string&, std::stringstream& ss) {
    int count = 200;
    std::string from;
    ss >> count >> from;
    BattleLogBonePositions(count, from.empty() ? 0 : std::stoull(from, nullptr, 16));
}

void DrawlogCommand(const std::string&, std::stringstream& ss) {
    std::string first;
    ss >> first;
    if (first == "ticks") {
        uint32_t from = 0, to = 0;
        ss >> from >> to;
        BattleLogRandomDrawsInTicks(from, to);
    } else {
        BattleLogRandomDraws(std::atoi(first.c_str()));
    }
}

void UniqueparamCommand(const std::string&, std::stringstream& ss) {
    int player = 0, index = 0, value = -1;
    ss >> player >> index >> value;
    SetUniqueParamOverride(player, index, value);
    Log("uniqueparam: P%d [%d] = %d", player + 1, index, value);
}

void WatchregionCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    size_t offset = 0;
    int bytes = 8;
    ss >> name >> std::hex >> offset >> std::dec >> bytes;
    GameWatchRegionOfNextBattle(name, offset, bytes);
}

void RoundinfoCommand(const std::string&, std::stringstream&) {
    FlowRunOnGameThread([] { FlowLogRoundInfo(); });
}

void RegionsCommand(const std::string&, std::stringstream& ss) {
    size_t size = 0;
    ss >> size;
    BattleLogRegionsOfSize(size);
}

void PhasesCommand(const std::string&, std::stringstream&) {
    FlowLogPhaseMachine();
}

void TasklistCommand(const std::string&, std::stringstream& ss) {
    int calls = 3000;
    ss >> calls;
    FlowTaskCensus(calls);
}

void OutsidetestCommand(const std::string&, std::stringstream& ss) {
    int ticks = 8;
    std::string mode;
    ss >> ticks >> mode;
    if (ue4::EnsureObjects(ImageBase())) RequestOutsideProbe(nullptr, ticks, mode == "all");
    else Log("outside: object array not found");
}

void RecdiffCommand(const std::string&, std::stringstream&) {
    uint8_t* data = BattleData();
    if (!data) {
        Log("recdiff: no battle data");
    } else {
        for (uint32_t off = 0; off < battledata::kRecordStride; off += 4) {
            const int32_t a = *reinterpret_cast<int32_t*>(data + off);
            const int32_t b = *reinterpret_cast<int32_t*>(data + battledata::kRecordStride + off);
            if (a != b) Log("recdiff +0x%03x: P1 %d (%08x) P2 %d (%08x)", off, a, a, b, b);
        }
    }
}

void TransitionsCommand(const std::string&, std::stringstream& ss) {
    NetplayOptions& o = NetOptions();
    int on = 1;
    ss >> on;
    o.lockstepTransitions = on != 0;
    Log("netplay: round transitions %s for the next sessions", on ? "in lockstep" : "rolled back");
}

void SkipcallbacksCommand(const std::string&, std::stringstream& ss) {
    std::vector<uint64_t> keys;
    std::string word;
    while (ss >> word) {
        const size_t slash = word.find('/');
        if (slash == std::string::npos) continue;
        keys.push_back(std::stoull(word.substr(0, slash), nullptr, 16) << 32 | std::stoull(word.substr(slash + 1), nullptr, 16));
    }
    GameSetSkippedLoadCallbacks(std::move(keys));
}

constexpr Command kCommands[] = {
    {"watchname", WatchnameCommand},
    {"watchphase", WatchphaseCommand},
    {"camrollback", CamrollbackCommand},
    {"camlog", CamrollbackCommand},
    {"hudstep", HudstepCommand},
    {"camtrace", CamtraceCommand},
    {"cinelog", CinelogCommand},
    {"actorlog", ActorlogCommand},
    {"rcmp", RcmpCommand},
    {"rpeek", RpeekCommand},
    {"ptrscan", PtrscanCommand},
    {"rsnap", RsnapCommand},
    {"boxlog", BoxlogCommand},
    {"winsize", WinsizeCommand},
    {"ftrace", FtraceCommand},
    {"freeze", FreezeCommand},
    {"framemeter", FramemeterCommand},
    {"battledata", BattledataCommand},
    {"fmlog", FmlogCommand},
    {"hitboxes", HitboxesCommand},
    {"rdump", RdumpCommand},
    {"rdiff", RdiffCommand},
    {"dumpbac", DumpbacCommand},
    {"rfind", RfindCommand},
    {"warmupsleep", WarmupsleepCommand},
    {"watchcamera", WatchcameraCommand},
    {"emptystage", EmptystageCommand},
    {"warmupextra", WarmupextraCommand},
    {"mappedlog", MappedlogCommand},
    {"inputlog", InputlogCommand},
    {"resimtasks", ResimtasksCommand},
    {"resimskip", ResimskipCommand},
    {"phasecost", PhasecostCommand},
    {"serialjobs", SerialjobsCommand},
    {"joborder", JoborderCommand},
    {"startprobe", StartprobeCommand},
    {"soundrollback", SoundrollbackCommand},
    {"frameids", FrameidsCommand},
    {"leanresim", LeanresimCommand},
    {"forcerollback", ForcerollbackCommand},
    {"ptrtest", PtrtestCommand},
    {"kowaitmode", KowaitmodeCommand},
    {"soundlog", SoundlogCommand},
    {"bonelog", BonelogCommand},
    {"drawlog", DrawlogCommand},
    {"uniqueparam", UniqueparamCommand},
    {"watchregion", WatchregionCommand},
    {"roundinfo", RoundinfoCommand},
    {"regions", RegionsCommand},
    {"phases", PhasesCommand},
    {"tasklist", TasklistCommand},
    {"outsidetest", OutsidetestCommand},
    {"recdiff", RecdiffCommand},
    {"transitions", TransitionsCommand},
    {"skipcallbacks", SkipcallbacksCommand},
};

} // namespace

std::span<const Command> CommandsBattle() { return kCommands; }

} // namespace sfv::mod
