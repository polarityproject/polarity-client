#include "mod/migrate.h"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

constexpr const wchar_t* kNote = L"Moved to Polarity.txt";

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsDir(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix)
{
    const size_t n = std::wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

// The names in `dir` matching `pattern` (FindFirstFile: case-insensitive),
// without "." and "..".
std::vector<std::wstring> List(const std::wstring& dir, const std::wstring& pattern)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fd.cFileName;
        if (name != L"." && name != L"..") out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// Whether two files hold the same bytes (the bundled font, put in the
// Polarity folder by a test script before the first start).
bool SameFile(const std::wstring& a, const std::wstring& b)
{
    HANDLE ha = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hb == INVALID_HANDLE_VALUE) {
        CloseHandle(ha);
        return false;
    }
    LARGE_INTEGER sa{}, sb{};
    bool same = GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) && sa.QuadPart == sb.QuadPart;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (same) {
        DWORD na = 0, nb = 0;
        if (!ReadFile(ha, ba.data(), static_cast<DWORD>(ba.size()), &na, nullptr) ||
            !ReadFile(hb, bb.data(), static_cast<DWORD>(bb.size()), &nb, nullptr) || na != nb) {
            same = false;
            break;
        }
        if (na == 0) break;
        same = std::memcmp(ba.data(), bb.data(), na) == 0;
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return same;
}

std::string Why(DWORD error)
{
    switch (error) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return "in use";
    case ERROR_ACCESS_DENIED: return "access denied (open, or read-only)";
    default: return "error " + std::to_string(error);
    }
}

struct Run {
    std::wstring dir;
    MigrateResult r;
    void Line(const std::string& s) { r.lines.push_back("migrate: " + s); }

    // Renames <folder>\<name> to <folder>\<newName> unless that exists.
    // `what` names it in the log ("SFVR\logs\" + name).
    bool Rename(const std::wstring& folder, const std::wstring& name, const std::wstring& newName, const std::string& what)
    {
        const std::wstring from = folder + L"\\" + name, to = folder + L"\\" + newName;
        if (Exists(to)) {
            Line(what + Utf8(name) + " stays: " + Utf8(newName) + " is there already");
            return false;
        }
        if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
            Line(what + Utf8(name) + " stays: " + Why(GetLastError()));
            return false;
        }
        r.moved = true;
        Line(what + Utf8(name) + " -> " + Utf8(newName));
        return true;
    }

    // An ini moved from SFVR: the settings SFVR's defaults wrote that name
    // what Polarity renamed are cleared, so Polarity's defaults apply.
    void FixIni(const std::wstring& path)
    {
        wchar_t value[512] = {};
        GetPrivateProfileStringW(L"update", L"url", L"", value, 512, path.c_str());
        if (_wcsicmp(value, kSfvrUpdateUrl) == 0) {
            WritePrivateProfileStringW(L"update", L"url", L"", path.c_str());
            Line("[update] url was SFVR's site: cleared (Polarity's is the default)");
        }
        value[0] = 0;
        GetPrivateProfileStringW(L"mod", L"report_dir", L"", value, 512, path.c_str());
        const std::wstring dirValue = value;
        if (_wcsicmp(value, L"SFVR\\matches") == 0 || _wcsicmp(dirValue.c_str(), (dir + L"\\SFVR\\matches").c_str()) == 0) {
            WritePrivateProfileStringW(L"mod", L"report_dir", L"", path.c_str());
            Line("[mod] report_dir was SFVR\\matches: cleared (Polarity\\matches is the default)");
        }
    }

    // sfv_rollback.ini, sfv_rollback.<n>.ini, sfv_rollback.panel[.<n>].ini.
    void Inis()
    {
        for (const std::wstring& name : List(dir, L"sfv_rollback*.ini")) {
            if (!StartsWith(name, L"sfv_rollback")) continue;
            const std::wstring newName = L"polarity" + name.substr(std::wcslen(L"sfv_rollback"));
            if (Rename(dir, name, newName, "") && !StartsWith(newName, L"polarity.panel")) FixIni(dir + L"\\" + newName);
        }
    }

    // Moves what `from` has and `to` lacks; folders both have are merged.
    // What stays is listed in `stayed` (paths under SFVR\).
    void Merge(const std::wstring& from, const std::wstring& to, const std::wstring& rel, std::vector<std::wstring>* stayed)
    {
        for (const std::wstring& name : List(from, L"*")) {
            if (rel.empty() && _wcsicmp(name.c_str(), kNote) == 0) continue;
            const std::wstring s = from + L"\\" + name, d = to + L"\\" + name, shown = rel + name;
            if (!Exists(d)) {
                if (MoveFileExW(s.c_str(), d.c_str(), 0)) {
                    r.moved = true;
                    continue;
                }
                const DWORD error = GetLastError();
                if (IsDir(s) && CreateDirectoryW(d.c_str(), nullptr)) {
                    r.moved = true;
                    Merge(s, d, shown + L"\\", stayed);
                    RemoveDirectoryW(s.c_str());
                    continue;
                }
                const std::string why = Why(error);
                stayed->push_back(shown + L" (" + std::wstring(why.begin(), why.end()) + L")");
            }
            else if (IsDir(s) && IsDir(d)) {
                Merge(s, d, shown + L"\\", stayed);
                RemoveDirectoryW(s.c_str());
            }
            else if (!IsDir(s) && !IsDir(d) && SameFile(s, d) && DeleteFileW(s.c_str())) {
                r.moved = true; // Polarity has it already, byte for byte
            }
            else {
                stayed->push_back(shown + L" (Polarity has one of the same name)");
            }
        }
    }

    void DataFolder()
    {
        const std::wstring from = dir + L"\\SFVR", to = dir + L"\\Polarity";
        if (!IsDir(from)) return;
        DeleteFileW((from + L"\\" + kNote).c_str()); // a run before this one's
        if (!Exists(to) && MoveFileExW(from.c_str(), to.c_str(), 0)) {
            r.moved = true;
            Line("SFVR\\ -> Polarity\\");
            return;
        }
        const DWORD error = GetLastError();
        if (!Exists(to)) {
            if (!CreateDirectoryW(to.c_str(), nullptr)) {
                Line("cannot make Polarity\\ (" + Why(GetLastError()) + "); SFVR\\ stays");
                return;
            }
            Line("SFVR\\ cannot be renamed (" + Why(error) + "): moving what it holds");
        }
        std::vector<std::wstring> stayed;
        Merge(from, to, L"", &stayed);
        if (RemoveDirectoryW(from.c_str())) {
            Line("SFVR\\ merged into Polarity\\");
            return;
        }
        std::string note = "SFVR is now Polarity, and this folder's files have moved to the Polarity folder next to it.\r\n"
                           "These stayed here:\r\n";
        for (const std::wstring& s : stayed) note += "  " + Utf8(s) + "\r\n";
        note += "\r\nMove them yourself if you want them in the Polarity folder, or delete them.\r\n";
        HANDLE h = CreateFileW((from + L"\\" + kNote).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, note.data(), static_cast<DWORD>(note.size()), &written, nullptr);
            CloseHandle(h);
        }
        Line("SFVR\\ merged into Polarity\\; " + std::to_string(stayed.size()) + " stayed (listed in SFVR\\" + Utf8(kNote) + ")");
        for (const std::wstring& s : stayed) Line("  stayed: SFVR\\" + Utf8(s));
    }

    // In Polarity\: the event logs and the packed-logs zips get Polarity's
    // names, so rotation and pruning see one set.
    void Names()
    {
        const std::wstring data = dir + L"\\Polarity", logs = data + L"\\logs";
        for (const std::wstring& name : List(logs, L"sfvr*.log")) {
            if (StartsWith(name, L"sfvr")) Rename(logs, name, L"polarity" + name.substr(4), "Polarity\\logs\\");
        }
        for (const std::wstring& name : List(data, L"SFVR-logs-*.zip")) {
            Rename(data, name, L"Polarity" + name.substr(4), "Polarity\\");
        }
    }

    // SFVR's DLL and what came with it, once Polarity's is there and
    // Polarity's loader (which loads it) runs.
    void OldFiles()
    {
        if (!Exists(dir + L"\\polarity.dll") || !GetEnvironmentVariableW(L"POLARITY_LOADER", nullptr, 0)) return;
        std::vector<std::wstring> names = List(dir, L"sfv_rollback.live.*.dll");
        for (const std::wstring& n : List(dir, L"sfv_rollback.dll*.old")) names.push_back(n);
        if (Exists(dir + L"\\sfv_rollback.dll")) names.push_back(L"sfv_rollback.dll");
        if (Exists(dir + L"\\polarity_fonts_license.txt") && Exists(dir + L"\\sfvr_fonts_license.txt")) {
            names.push_back(L"sfvr_fonts_license.txt");
        }
        for (const std::wstring& n : names) {
            if (DeleteFileW((dir + L"\\" + n).c_str())) Line("deleted " + Utf8(n));
            else Line(Utf8(n) + " stays: " + Why(GetLastError()));
        }
    }
};

} // namespace

MigrateResult MigrateFromSfvr(const std::wstring& dir)
{
    Run run;
    run.dir = dir;
    // Instances that start together (multi_instance=1): one migrates, the
    // others wait for it and then find nothing left to do.
    HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\polarity_migrate");
    if (lock) {
        const DWORD w = WaitForSingleObject(lock, 0);
        if (w == WAIT_TIMEOUT) {
            run.r.waited = true;
            if (WaitForSingleObject(lock, 30000) == WAIT_TIMEOUT) {
                run.Line("another game is still moving SFVR's files after 30 s; not waiting longer");
                CloseHandle(lock);
                return run.r;
            }
        }
    }
    run.Inis();
    run.DataFolder();
    run.Names();
    run.OldFiles();
    if (lock) {
        ReleaseMutex(lock);
        CloseHandle(lock);
    }
    return run.r;
}

} // namespace sfv::mod
