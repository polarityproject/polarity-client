// The benchmark: the built-in round under rollback stress, timed (docs/research.md).

#include "mod/benchmark.h"
#include "mod/battle_data.h"

#include "builtin_benchmark.h"
#include "core/file_open.h"
#include "mod/files.h"
#include "mod/flow.h"
#include "mod/log.h"
#include "mod/replay_driver.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace sfv::mod {
namespace {

std::mutex g_mutex;
BenchmarkResult g_last;
bool g_lastRead = false;
bool g_fresh = false;
std::string g_version;

std::wstring ResultsPath() { return FilesLogsDir() + L"\\benchmark.txt"; }

std::string Line(const BenchmarkResult& r) {
    char b[1400];
    std::snprintf(b, sizeof(b),
                  "when=%llu version=%s pass=%d matched=%d first_differ=%d frames=%d seconds=%.2f average_fps=%.2f low_fps=%.2f "
                  "slowest_ms=%.2f slow_frames=%d rollback_ms=%.2f game_ms=%.2f draw_ms=%.2f mods=%s data=%s differ=%s "
                  "straight_frames=%d resim_frames=%d regions=%s session=%s dump=%s",
                  static_cast<unsigned long long>(r.when), r.version.c_str(), r.Pass() ? 1 : 0, r.matched ? 1 : 0, r.firstDiffer, r.frames,
                  r.seconds, r.averageFps, r.lowFps, r.slowestMs, r.slowFrames, r.rollbackMs, r.gameMs, r.drawMs,
                  r.mods.empty() ? "-" : r.mods.c_str(), r.data.empty() ? "-" : r.data.c_str(), r.differ.empty() ? "-" : r.differ.c_str(),
                  r.straightFrames, r.resimFrames, r.regions.empty() ? "-" : r.regions.c_str(), r.session.empty() ? "-" : r.session.c_str(),
                  r.dump.empty() ? "-" : r.dump.c_str());
    return b;
}

BenchmarkResult Parse(const std::string& line) {
    BenchmarkResult r;
    std::istringstream ss(line);
    std::string kv;
    while (ss >> kv) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "when") r.when = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "version") r.version = v;
        else if (k == "matched") r.matched = v == "1";
        else if (k == "first_differ") r.firstDiffer = std::atoi(v.c_str());
        else if (k == "frames") r.frames = std::atoi(v.c_str());
        else if (k == "seconds") r.seconds = std::atof(v.c_str());
        else if (k == "average_fps") r.averageFps = std::atof(v.c_str());
        else if (k == "low_fps") r.lowFps = std::atof(v.c_str());
        else if (k == "slowest_ms") r.slowestMs = std::atof(v.c_str());
        else if (k == "slow_frames") r.slowFrames = std::atoi(v.c_str());
        else if (k == "rollback_ms") r.rollbackMs = std::atof(v.c_str());
        else if (k == "game_ms") r.gameMs = std::atof(v.c_str());
        else if (k == "draw_ms") r.drawMs = std::atof(v.c_str());
        else if (k == "mods") r.mods = v == "-" ? "" : v;
        else if (k == "data") r.data = v == "-" ? "" : v;
        else if (k == "differ") r.differ = v == "-" ? "" : v;
        else if (k == "straight_frames") r.straightFrames = std::atoi(v.c_str());
        else if (k == "resim_frames") r.resimFrames = std::atoi(v.c_str());
        else if (k == "regions") r.regions = v == "-" ? "" : v;
        else if (k == "session") r.session = v == "-" ? "" : v;
        else if (k == "dump") r.dump = v == "-" ? "" : v;
    }
    r.valid = r.frames > 0;
    return r;
}

} // namespace

namespace {
std::mutex g_activityMutex;
std::map<std::string, int> g_activity;
std::atomic<int> g_dumpFrame{-1};
} // namespace

void BenchmarkNoteActivity(const char* kind) {
    std::lock_guard lock(g_activityMutex);
    g_activity[kind ? kind : "?"]++;
}

std::string BenchmarkActivityText() {
    FILETIME created{}, exited{}, kernel{}, user{};
    long long minutes = -1;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const auto ticks = [](const FILETIME& f) { return (static_cast<long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        minutes = (ticks(now) - ticks(created)) / 600000000ll;
    }
    std::lock_guard lock(g_activityMutex);
    auto count = [](const char* k) {
        const auto it = g_activity.find(k);
        return it == g_activity.end() ? 0 : it->second;
    };
    char b[160];
    std::snprintf(b, sizeof(b), "up%lldm,battles%llu,match%d,replay%d,training%d,bench%d,desync%d", minutes,
                  static_cast<unsigned long long>(FlowBattleSceneSwitches()), count("match"), count("replay"), count("training"),
                  count("benchmark"), count("desync"));
    return b;
}

void BenchmarkSetDumpFrame(int frame) {
    g_dumpFrame = frame;
    Log("benchmark: %s", frame >= 0 ? ("runs save frame " + std::to_string(frame) + "'s state").c_str() : "runs save no reference state");
}

int BenchmarkDumpFrame() { return g_dumpFrame; }

bool BenchmarkStart(const std::string& modVersion) {
    if (sizeof(kBenchmarkRound) < 64) {
        Log("benchmark: this build has no benchmark round");
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        g_version = modVersion;
    }
    FilesEnsureDirectory(FilesDataDir());
    const std::string path = FilesUtf8Path(FilesDataDir() + L"\\benchmark.sfvr");
    if (FILE* f = OpenFile(path, "wb")) {
        std::fwrite(kBenchmarkRound, 1, sizeof(kBenchmarkRound), f);
        std::fclose(f);
    } else {
        Log("benchmark: cannot write %s", path.c_str());
        return false;
    }
    Log("benchmark: starting (%zu bytes of round)", sizeof(kBenchmarkRound));
    return ReplayRequestBenchmark(path);
}

bool BenchmarkRunning(int* frame, int* frames) {
    const ReplayView v = ReplayViewNow();
    if (!v.active || !v.benchmark) return false;
    if (frame) *frame = v.frame;
    if (frames) *frames = v.frames;
    return true;
}

void BenchmarkFinish(const CapturedRun& run) {
    if (run.stopped || run.micros.size() < 30) {
        Log("benchmark: %s (%zu frames measured): no result", run.stopped ? "stopped" : "too short", run.micros.size());
        return;
    }
    BenchmarkResult r;
    r.valid = true;
    r.matched = run.matched;
    r.firstDiffer = run.firstDiffer;
    r.differ = run.straightFrames && run.resimFrames ? "both" : run.straightFrames ? "straight"
                                                            : run.resimFrames      ? "resim"
                                                                                   : "";
    r.straightFrames = run.straightFrames;
    r.resimFrames = run.resimFrames;
    r.regions = run.regions;
    r.session = run.session;
    r.dump = run.dump;
    r.when = static_cast<uint64_t>(std::time(nullptr));
    r.mods = BattleDataScanModPaks().Short();
    const DataCheck check = BattleDataLastCheck();
    r.data = !check.done || !check.table ? "unknown"
             : check.changed.empty()     ? "stock"
                                         : "changed:" + [&] {
                                           std::string c = check.Characters();
                                           c.erase(std::remove(c.begin(), c.end(), ' '), c.end());
                                           return c;
                                       }();
    {
        std::lock_guard lock(g_mutex);
        r.version = g_version;
    }
    constexpr size_t skip = 2;
    std::vector<double> ms;
    double total = 0, net = 0, battle = 0, draw = 0;
    for (size_t i = skip; i < run.micros.size(); i++) {
        const double m = run.micros[i] / 1000.0;
        ms.push_back(m);
        total += m;
        net += run.net[i] / 1000.0;
        battle += run.battle[i] / 1000.0;
        draw += run.draw[i] / 1000.0;
    }
    const double n = static_cast<double>(ms.size());
    r.frames = static_cast<int>(ms.size());
    r.seconds = total / 1000.0;
    r.averageFps = total > 0 ? n * 1000.0 / total : 0;
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const double p99 = sorted[std::min(sorted.size() - 1, static_cast<size_t>(sorted.size() * 0.99))];
    r.lowFps = p99 > 0 ? 1000.0 / p99 : 0;
    r.slowestMs = sorted.back();
    r.slowFrames = static_cast<int>(std::count_if(ms.begin(), ms.end(), [](double m) { return m > 20.0; }));
    r.rollbackMs = net / n;
    r.gameMs = battle / n;
    r.drawMs = draw / n;
    const std::string line = Line(r);
    Log("benchmark: %s: %s", r.Pass() ? "PASS" : "FAIL", line.c_str());
    FilesEnsureDirectory(FilesLogsDir());
    if (FILE* f = OpenFile(FilesUtf8Path(ResultsPath()), "ab")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
    std::lock_guard lock(g_mutex);
    g_last = r;
    g_lastRead = true;
    g_fresh = true;
}

BenchmarkResult BenchmarkLast() {
    std::lock_guard lock(g_mutex);
    if (!g_lastRead) {
        g_lastRead = true;
        std::ifstream in(FilesUtf8Path(ResultsPath()));
        std::string line, last;
        while (std::getline(in, line)) {
            if (!line.empty()) last = line;
        }
        if (!last.empty()) g_last = Parse(last);
    }
    return g_last;
}

bool BenchmarkTakeResult(BenchmarkResult* out) {
    std::lock_guard lock(g_mutex);
    if (!g_fresh) return false;
    g_fresh = false;
    *out = g_last;
    return true;
}

} // namespace sfv::mod
