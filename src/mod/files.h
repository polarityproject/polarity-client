#pragma once
// The Polarity folder next to the game (Binaries\Win64\Polarity): what a player zips
// up and sends when something goes wrong.
//   logs\     the event log: polarity.log (this run; polarity.<n>.log for test
//             instance n), earlier runs as polarity_<start>.log
//   matches\  per match a recording (.sfvr) and its log (.log), named by date,
//             players, characters and match id
//   crashes\  reports of runs that ended badly (crash_report.h)
// Plus "Pack logs to share": a zip of the logs and the latest matches.

#include <string>
#include <utility>
#include <vector>

namespace sfv::mod {

constexpr size_t kKeptRunLogs = 20;      // earlier runs' event logs
constexpr size_t kKeptRecordings = 300;  // .sfvr files in the matches folder (a match, its desync reports)
constexpr size_t kKeptMatchLogs = 200;   // .log files in the matches folder
constexpr size_t kSharedMatches = 10;    // the latest matches in a shared zip
constexpr size_t kKeptCrashReports = 20; // crash reports (.log, and .dmp with a fault)
constexpr size_t kSharedCrashFiles = 6;  // the latest crash reports' files in a shared zip

void SetDataDir(const std::wstring& dir);
std::wstring DataDir();     // ...\Win64\Polarity
std::wstring LogsDir();     // ...\Polarity\logs
std::wstring MatchesDir();  // ...\Polarity\matches
std::wstring ReplaysDir();  // ...\Polarity\replays: the Battle Log (kept, never pruned)
std::wstring CrashesDir();  // ...\Polarity\crashes
std::wstring ResearchDir(); // ...\Polarity\research: research dumps, made on first use

// Creates the folder and any missing parents.
bool EnsureDirectory(const std::wstring& path);
// Keeps the `keep` most recently written files matching `pattern` (a
// FindFirstFile pattern such as L"*.sfvr") in `dir` and deletes the others.
void PruneOldest(const std::wstring& dir, const std::wstring& pattern, size_t keep);
// Before a run opens <dir>\<stem>.log: the previous run's file becomes
// <stem>_<when it started>.log (from its first line, else when it was last
// written), and only the newest kKeptRunLogs of those stay.
void RotateLog(const std::wstring& dir, const std::wstring& stem);

// A name part made safe for file names: letters, digits, '-' and '_' kept,
// runs of anything else become one '-', at most `maxLen` characters; `empty`
// when nothing is left.
std::string FileSafe(const std::string& text, size_t maxLen, const std::string& empty);
// A path as UTF-8 (what core/file_open.h opens), every character kept: a game
// or user folder outside ASCII stays a real path.
std::string Utf8Path(const std::wstring& path);

// Writes a zip (stored, not compressed: the recordings already are) of the
// files, each {path on disk, name in the zip}.
bool WriteZip(const std::wstring& path, const std::vector<std::pair<std::wstring, std::string>>& files,
              std::string* error);
// The zip "Pack logs to share" makes: the event logs (this run and the
// earlier ones kept) and the latest kSharedMatches matches' logs and
// recordings, as Polarity\Polarity-logs-<date>.zip. Returns its path, empty on failure.
std::wstring PackLogsToShare(std::string* error);
// Opens the Polarity folder in Explorer, with `select` selected when given.
void OpenDataFolder(const std::wstring& select = L"");

// The game's data files, for logs and match logs: how many of the game's own
// pakchunk<n>-WindowsNoEditor.pak there are, and every other .pak under
// Content\Paks (mods, often in ~mods: the game loads them all). Desyncs seen
// on one player's machine only point at such files. Read once.
std::string GameFilesText();
// The paks besides the game's own (Content\Paks and its subfolders, as ~mods),
// as full paths, sorted.
std::vector<std::wstring> OtherPaks();

} // namespace sfv::mod
