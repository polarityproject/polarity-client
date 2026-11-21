#pragma once
// A recording's build id, "sfv-<exe>/adapter-<n>/sim-<n>/map-<hash>/noise-<hash>"
// (SfvGame::BuildId), and what of it decides whether a recording can be
// played on another build.
//
// Only two parts change what a battle computes from the same start and
// inputs: the game exe ("sfv-<timestamp>") and the mod's sim version. The
// adapter version also moved with checksum-only changes, and map / noise are
// the checksum masks: none of those stop a recording from playing. Ids from
// before the sim part have the adapter version in its place (adapter-12 was
// sim 12).

#include <cstdlib>
#include <string>

namespace sfv {

// The number after "<key>-" in a build id's parts, or -1.
inline int BuildIdNumber(const std::string& id, const char* key) {
    const std::string tag = std::string("/") + key + "-";
    const size_t at = id.find(tag);
    if (at == std::string::npos) return -1;
    return std::atoi(id.c_str() + at + tag.size());
}

inline int BuildAdapterVersion(const std::string& id) { return BuildIdNumber(id, "adapter"); }

inline int BuildSimVersion(const std::string& id) {
    const int sim = BuildIdNumber(id, "sim");
    return sim >= 0 ? sim : BuildAdapterVersion(id);
}

// "sfv-<timestamp>": the game exe the id was made on.
inline std::string BuildExe(const std::string& id) { return id.substr(0, id.find('/')); }

// Whether a recording made on `recorded` plays on `live`.
inline bool ReplayCompatible(const std::string& recorded, const std::string& live) {
    const int sim = BuildSimVersion(recorded);
    return sim > 0 && sim == BuildSimVersion(live) && BuildExe(recorded) == BuildExe(live);
}

} // namespace sfv
