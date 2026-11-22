#include "mod/battle_data.h"

#include "core/pak_index.h"
#include "core/state.h"
#include "mod/files.h"
#include "mod/image.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/ue4.h"

#include "builtin_battle_data.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace sfv::mod {

namespace {

// A battle resource object's data (KWBAC, KWBCM, KWBCH; research `ofclass
// KWBattleResource`, 2026-11-12): the parsed file's bytes at +0x28, their
// size at +0x30.
constexpr size_t kResourceData = 0x28, kResourceSize = 0x30;
const char* const kMoveDataClasses[] = {"KWBAC", "KWBCM", "KWBCH"};

std::string Join(const std::vector<std::string>& items, const char* sep, size_t max = 8) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < max; i++) out += (i ? sep : "") + items[i];
    if (items.size() > max) out += std::string(sep) + "and " + std::to_string(items.size() - max) + " more";
    return out;
}

struct StockEntry {
    uint32_t size = 0;
    uint64_t hash = 0;
};

// data/battle_data.txt: "<key> <size> <hash hex>" per line, '#' comments.
const std::map<std::string, StockEntry>& StockTable() {
    static const std::map<std::string, StockEntry> table = [] {
        std::map<std::string, StockEntry> t;
        std::istringstream in(std::string(reinterpret_cast<const char*>(kStockBattleData), sizeof(kStockBattleData)));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '\0') continue;
            std::istringstream ls(line);
            std::string key, hash;
            StockEntry e;
            if (ls >> key >> e.size >> hash) {
                e.hash = std::strtoull(hash.c_str(), nullptr, 16);
                t[key] = e;
            }
        }
        return t;
    }();
    return table;
}

struct Loaded {
    std::string key;
    uint32_t size = 0;
    uint64_t hash = 0;
};

// The battle's move data objects now (the classes' default objects left out).
std::vector<Loaded> ReadLoaded() {
    std::vector<Loaded> out;
    if (!ue4::EnsureObjects(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) return out;
    ue4::ForEachObject([&](const void* object) {
        const std::string cls = ue4::ObjectName(ue4::ClassOf(object));
        if (std::none_of(std::begin(kMoveDataClasses), std::end(kMoveDataClasses), [&](const char* c) { return cls == c; })) return;
        if (ue4::ObjectName(object).rfind("Default__", 0) == 0) return;
        const auto* o = static_cast<const uint8_t*>(object);
        if (!IsReadable(o + kResourceData, 16)) return;
        const auto* data = *reinterpret_cast<const uint8_t* const*>(o + kResourceData);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(o + kResourceSize);
        const std::string key = PakIndexBattleDataKey(ue4::ObjectName(ue4::OuterOf(object)));
        if (key.empty() || !data || size == 0 || size > (64u << 20) || !IsReadable(data, size)) return;
        out.push_back({key, size, Hash64(data, size)});
    });
    std::sort(out.begin(), out.end(), [](const Loaded& a, const Loaded& b) { return a.key < b.key; });
    out.erase(std::unique(out.begin(), out.end(), [](const Loaded& a, const Loaded& b) { return a.key == b.key && a.hash == b.hash; }),
              out.end());
    return out;
}

std::mutex g_mutex;
DataCheck g_last;

} // namespace

bool ModScan::BattleData() const {
    return std::any_of(paks.begin(), paks.end(), [](const ModPak& p) { return p.Battle() || !p.readable; });
}

std::string ModScan::Text() const {
    if (paks.empty()) return "no mod paks";
    std::vector<std::string> parts;
    for (const ModPak& p : paks) {
        std::string s = p.name + ": ";
        if (!p.readable) s += "can't be read (" + p.error + "), so it may change anything";
        else {
            std::vector<std::string> what;
            if (!p.moveData.empty()) what.push_back("move data of " + Join(p.moveData, ", "));
            if (!p.stageData.empty()) what.push_back("stage transition data of " + Join(p.stageData, ", "));
            s += what.empty() ? std::to_string(p.files) + " files, none of them battle data" + (p.cpuAi ? " (CPU AI only)" : "")
                              : "changes " + Join(what, "; ");
        }
        parts.push_back(s);
    }
    return std::to_string(paks.size()) + " mod pak" + (paks.size() == 1 ? "" : "s") + ": " + Join(parts, " | ", 12);
}

std::string ModScan::Short() const {
    if (paks.empty()) return "none";
    std::set<std::string> characters;
    int battle = 0;
    for (const ModPak& p : paks) {
        if (!p.Battle() && p.readable) continue;
        battle++;
        for (const std::string& c : p.moveData) characters.insert(c.substr(0, c.find(' ')));
        for (const std::string& c : p.stageData) characters.insert(c);
        if (!p.readable) characters.insert("?");
    }
    std::string s = std::to_string(paks.size()) + (battle ? "," + std::to_string(battle) + "-battle" : "");
    if (!characters.empty()) s += ":" + Join(std::vector<std::string>(characters.begin(), characters.end()), ",", 20);
    return s;
}

const ModScan& ScanModPaks() {
    static const ModScan scan = [] {
        ModScan s;
        for (const std::wstring& full : FilesOtherPaks()) {
            ModPak p;
            const size_t at = full.find(L"\\Content\\Paks\\");
            p.name = FilesUtf8Path(at == std::wstring::npos ? full : full.substr(at + 14));
            PakIndex index;
            p.readable = PakIndexRead(FilesUtf8Path(full), &index, &p.error);
            if (p.readable) {
                p.files = index.entries.size();
                std::set<std::string> move, stage;
                for (const PakEntry& e : index.entries) {
                    const BattleFile b = PakIndexClassifyPath(e.path);
                    if (b.kind == BattleFileKind::MoveData) move.insert(b.character + (b.version.empty() ? "" : " (" + b.version + ")"));
                    else if (b.kind == BattleFileKind::StageData) stage.insert(b.character);
                    else if (b.kind == BattleFileKind::CpuAi) p.cpuAi = true;
                }
                p.moveData.assign(move.begin(), move.end());
                p.stageData.assign(stage.begin(), stage.end());
            }
            s.paks.push_back(std::move(p));
        }
        return s;
    }();
    return scan;
}

std::string DataCheck::Text() const {
    if (!done) return "not checked";
    if (files == 0) return "no move data loaded";
    char sumText[24];
    std::snprintf(sumText, sizeof(sumText), "%016llx", static_cast<unsigned long long>(sum));
    std::string s = std::to_string(files) + " move data files, sum " + sumText + ": ";
    if (!table) return s + "no table of the game's own in this build";
    if (changed.empty()) s += "the game's own";
    else s += "NOT the game's own: " + Join(changed, ", ");
    if (!unknown.empty()) s += "; not in the table: " + Join(unknown, ", ");
    return s;
}

std::string DataCheck::Characters() const {
    std::set<std::string> c;
    for (const std::string& k : changed) c.insert(k.substr(0, k.find('/')));
    return Join(std::vector<std::string>(c.begin(), c.end()), ", ", 20);
}

DataCheck CheckLoadedBattleData() {
    DataCheck c;
    const auto& table = StockTable();
    c.table = !table.empty();
    for (const Loaded& l : ReadLoaded()) {
        c.files++;
        c.sum = c.sum * 1099511628211ull ^ l.hash;
        const auto it = table.find(l.key);
        if (it == table.end()) {
            c.unknown.push_back(l.key);
            continue;
        }
        c.known++;
        if (it->second.hash != l.hash || it->second.size != l.size) c.changed.push_back(l.key);
    }
    c.done = true;
    return c;
}

void CheckBattleDataSoon(const char* why) {
    const std::string reason = why ? why : "";
    std::thread([reason] {
        ThreadGuard running;
        const DataCheck c = CheckLoadedBattleData();
        {
            std::lock_guard lock(g_mutex);
            g_last = c;
        }
        Log("battle data (%s): %s", reason.c_str(), c.Text().c_str());
    }).detach();
}

DataCheck LastDataCheck() {
    std::lock_guard lock(g_mutex);
    return g_last;
}

void DumpLoadedBattleData() {
    const std::vector<Loaded> loaded = ReadLoaded();
    const std::wstring dir = FilesResearchDir();
    const std::string path = FilesUtf8Path(dir + L"\\battle_data.txt");
    FILE* f = nullptr;
    _wfopen_s(&f, (dir + L"\\battle_data.txt").c_str(), L"ab");
    for (const Loaded& l : loaded) {
        if (f) std::fprintf(f, "%s %u %016llx\n", l.key.c_str(), l.size, static_cast<unsigned long long>(l.hash));
    }
    if (f) std::fclose(f);
    Log("battledata: %zu move data files appended to %s", loaded.size(), path.c_str());
}

} // namespace sfv::mod
