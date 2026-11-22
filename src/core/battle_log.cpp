// The Battle Log's sets: matches grouped, ordered and scored.

#include "core/battle_log.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace sfv::battlelog {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Newer(const Entry& a, const Entry& b) {
    if (a.header.createdUnix != b.header.createdUnix) return a.header.createdUnix > b.header.createdUnix;
    return a.path > b.path;
}

} // namespace

std::string NameOf(const Entry& e, int side) {
    const std::string& n = e.header.summary.names[side & 1];
    return n.empty() ? (side == 0 ? "P1" : "P2") : n;
}

int MySide(const Entry& e, const std::string& myName) {
    if (!myName.empty()) {
        const bool p1 = Lower(e.header.summary.names[0]) == Lower(myName);
        const bool p2 = Lower(e.header.summary.names[1]) == Lower(myName);
        if (p1 != p2) return p1 ? 0 : 1;
    }
    return e.header.localPlayer == 1 ? 1 : 0;
}

std::vector<Entry> Merge(std::vector<Entry> entries, const std::string& myName) {
    std::sort(entries.begin(), entries.end(), Newer);
    std::vector<Entry> out;
    std::map<uint64_t, size_t> byId;
    for (Entry& e : entries) {
        const uint64_t id = e.header.matchId;
        auto it = id ? byId.find(id) : byId.end();
        if (it == byId.end()) {
            if (id) byId[id] = out.size();
            out.push_back(std::move(e));
            continue;
        }
        Entry& kept = out[it->second];
        const bool keptMine = kept.header.localPlayer >= 0 && kept.header.localPlayer == MySide(kept, myName);
        const bool thisMine = e.header.localPlayer >= 0 && e.header.localPlayer == MySide(e, myName);
        if (thisMine && !keptMine) kept = std::move(e);
    }
    return out;
}

bool Passes(const Entry& e, const Filter& f, const std::string& myName) {
    const int me = MySide(e, myName);
    if (f.mine && e.header.setup.players[me].character != f.mine) return false;
    if (f.theirs && e.header.setup.players[1 - me].character != f.theirs) return false;
    if (!f.opponent.empty() && Lower(NameOf(e, 1 - me)) != Lower(f.opponent)) return false;
    return true;
}

std::vector<Set> GroupSets(const std::vector<const Entry*>& entries) {
    std::vector<Set> sets;
    std::map<uint64_t, size_t> byKey;
    for (const Entry* e : entries) {
        const uint64_t key = e->header.summary.setKey;
        auto it = key ? byKey.find(key) : byKey.end();
        if (it == byKey.end()) {
            if (key) byKey[key] = sets.size();
            Set s{};
            s.key = key;
            s.newest = static_cast<int64_t>(e->header.createdUnix);
            sets.push_back(std::move(s));
            it = key ? byKey.find(key) : byKey.end();
            sets.back().games.push_back(e);
            continue;
        }
        sets[it->second].games.push_back(e);
    }
    for (Set& s : sets) {
        std::sort(s.games.begin(), s.games.end(), [](const Entry* a, const Entry* b) {
            if (a->header.summary.setGame != b->header.summary.setGame) return a->header.summary.setGame < b->header.summary.setGame;
            return a->header.createdUnix < b->header.createdUnix;
        });
        const MatchSummary& first = s.games.front()->header.summary;
        const MatchSummary& last = s.games.back()->header.summary;
        s.rule = first.setRule;
        if (last.present && last.setGame) {
            for (int i = 0; i < kNumPlayers; i++) s.wins[i] = last.setWins[i];
        } else {
            for (int i = 0; i < kNumPlayers; i++) s.wins[i] = 0;
            for (const Entry* g : s.games) {
                if (g->header.summary.winner >= 0) s.wins[g->header.summary.winner & 1]++;
            }
        }
    }
    return sets;
}

std::vector<std::pair<uint32_t, int>> Characters(const std::vector<Entry>& entries, const std::string& myName, bool theirs) {
    std::map<uint32_t, int> counts;
    for (const Entry& e : entries) {
        const int side = MySide(e, myName) ^ (theirs ? 1 : 0);
        if (const uint32_t c = e.header.setup.players[side].character) counts[c]++;
    }
    std::vector<std::pair<uint32_t, int>> out(counts.begin(), counts.end());
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

std::vector<std::pair<std::string, int>> Opponents(const std::vector<Entry>& entries, const std::string& myName) {
    std::map<std::string, std::pair<std::string, int>> counts;
    for (const Entry& e : entries) {
        const std::string name = NameOf(e, 1 - MySide(e, myName));
        auto& c = counts[Lower(name)];
        if (c.first.empty()) c.first = name;
        c.second++;
    }
    std::vector<std::pair<std::string, int>> out;
    for (const auto& [key, c] : counts) out.push_back(c);
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

} // namespace sfv::battlelog
