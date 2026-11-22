// Report uploads: desync and crash reports sent to the report server (docs/startup.md).

#include "mod/report_upload.h"

#include "mod/crash_report.h"
#include "mod/files.h"
#include "mod/lifecycle.h"
#include "mod/log.h"

#include <windows.h>

#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace sfv::mod {

namespace {

constexpr DWORD kTimeoutMs = 15000;
constexpr uint64_t kMaxFileBytes = 16ull << 20;
constexpr int64_t kMaxAgeSeconds = 14 * 24 * 3600;
constexpr int64_t kNoLogAfterSeconds = 30 * 60;
constexpr auto kFirstLook = std::chrono::seconds(30);
constexpr auto kRetryEvery = std::chrono::minutes(5);
constexpr auto kWakeEvery = std::chrono::milliseconds(250);
constexpr uint64_t kFileTimeTicksPerSecond = 10000000;

std::mutex g_mutex;
std::wstring g_dir;
std::string g_configured;
std::string g_url;
std::string g_version;
std::atomic<bool> g_on{true};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_again{false};
std::atomic<bool> g_left{false};
std::atomic<bool> g_match{false};
std::atomic<int> g_sent{0};
int64_t g_started = 0;

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool Ascii(const std::wstring& w, std::string* out) {
    out->clear();
    for (wchar_t c : w) {
        if (c >= 128) return false;
        *out += static_cast<char>(c);
    }
    return true;
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

DWORD Post(const std::string& url, const std::vector<uint8_t>& body, std::string* error) {
    const std::wstring wurl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        *error = "bad address " + url;
        return 0;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET session = WinHttpOpen(L"Polarity reports", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    DWORD status = 0;
    if (request) {
        WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        const std::wstring headers = L"Content-Type: application/octet-stream\r\nX-Polarity-Version: " + Wide(g_version) + L"\r\n";
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), const_cast<uint8_t*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
        } else {
            *error = "cannot reach " + url + " (error " + std::to_string(GetLastError()) + ")";
        }
    } else {
        *error = "cannot open " + url;
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return status;
}

struct Side {
    std::vector<std::string> files;
    bool desync = false, log = false;
    int64_t newest = 0;
};

int64_t Seconds(const FILETIME& t) { return static_cast<int64_t>((uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / kFileTimeTicksPerSecond); }

bool SideKey(const std::string& name, std::string* key, bool* desync, bool* log) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = name.substr(dot + 1);
    if (ext != "sfvr" && ext != "log") return false;
    std::string stem = name.substr(0, dot);
    *desync = stem.size() > 7 && stem.compare(stem.size() - 7, 7, "_desync") == 0;
    if (*desync) {
        if (ext != "sfvr") return false;
        stem.resize(stem.size() - 7);
    }
    *log = ext == "log";
    const size_t a = stem.rfind('_');
    if (a == std::string::npos || a < 9) return false;
    const std::string side = stem.substr(a + 1);
    const std::string match = stem.substr(a - 8, 8);
    if (stem[a - 9] != '_' || (side != "p1" && side != "p2" && side != "spec")) return false;
    for (char c : match) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    *key = match + "_" + side;
    return true;
}

bool CrashName(const std::string& name) {
    if (name.size() != 38 || (name.compare(34, 4, ".log") != 0 && name.compare(34, 4, ".dmp") != 0)) return false;
    if (name.compare(19, 7, "_crash_") != 0) return false;
    for (size_t i = 0; i < 19; i++) {
        const char c = name[i];
        const bool digit = c >= '0' && c <= '9';
        if (i == 4 || i == 7 || i == 13 || i == 16 ? c != '-' : i == 10 ? c != '_'
                                                                        : !digit) return false;
    }
    for (size_t i = 26; i < 34; i++) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::set<std::string> NameList(const std::wstring& dir, const wchar_t* list) {
    std::set<std::string> names;
    FILE* f = _wfopen((dir + L"\\" + list).c_str(), L"r");
    if (!f) return names;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) names.insert(s);
    }
    std::fclose(f);
    return names;
}

void AddName(const std::wstring& dir, const wchar_t* list, const std::string& name) {
    FILE* f = _wfopen((dir + L"\\" + list).c_str(), L"a");
    if (!f) return;
    std::fprintf(f, "%s\n", name.c_str());
    std::fclose(f);
}

std::set<std::string> SentList(const std::wstring& dir) { return NameList(dir, L"uploaded.txt"); }

void MarkSent(const std::wstring& dir, const std::string& name) { AddName(dir, L"uploaded.txt", name); }

enum class Sent { Now,
                  Refused,
                  Later };

Sent SendFile(const std::string& url, const char* path, const std::wstring& dir, const std::string& name) {
    std::vector<uint8_t> body;
    if (!ReadFile(dir + L"\\" + Wide(name), &body)) return Sent::Refused;
    std::string error;
    const DWORD status = Post(url + "/v1/" + path + "/" + name, body, &error);
    if (status == HTTP_STATUS_OK || status == HTTP_STATUS_CREATED) {
        Log("reports: sent %s (%zu KB)", name.c_str(), body.size() / 1024);
        MarkSent(dir, name);
        return Sent::Now;
    }
    if (status == HTTP_STATUS_BAD_REQUEST || status == HTTP_STATUS_NOT_FOUND || status == HTTP_STATUS_LENGTH_REQUIRED ||
        status == HTTP_STATUS_REQUEST_TOO_LARGE) {
        Log("reports: the server refused %s (%lu)", name.c_str(), status);
        MarkSent(dir, name);
        return Sent::Refused;
    }
    Log("reports: %s not sent: %s", name.c_str(), status ? ("the server answered " + std::to_string(status)).c_str() : error.c_str());
    return Sent::Later;
}

bool SendCrashes(const std::string& url) {
    const std::wstring dir = FilesCrashesDir();
    std::vector<std::string> names;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*_crash_*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return true;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const int64_t now = Seconds(nowFt);
    do {
        std::string name;
        if (!Ascii(fd.cFileName, &name) || !CrashName(name)) continue;
        if ((uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow) > kMaxFileBytes) continue;
        if (now - Seconds(fd.ftLastWriteTime) > kMaxAgeSeconds) continue;
        names.push_back(name);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    const std::set<std::string> sent = SentList(dir);
    const std::set<std::string> kept = NameList(dir, L"kept.txt");
    for (const std::string& name : names) {
        if (sent.count(name) || kept.count(name)) continue;
        if (!g_on || Stopping()) return true;
        const Sent s = SendFile(url, "crashes", dir, name);
        if (s == Sent::Later) return false;
        if (s == Sent::Now && name.compare(34, 4, ".log") == 0) g_sent++;
    }
    return true;
}

bool SendRound() {
    std::string url;
    {
        std::lock_guard lock(g_mutex);
        url = g_url;
    }
    if (url.empty() || g_dir.empty()) return true;
    std::map<std::string, Side> sides;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return SendCrashes(url);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name, key;
        bool desync = false, log = false;
        if (!Ascii(fd.cFileName, &name) || !SideKey(name, &key, &desync, &log)) continue;
        if ((uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow) > kMaxFileBytes) {
            continue;
        }
        Side& s = sides[key];
        s.files.push_back(name);
        s.desync |= desync;
        s.log |= log;
        s.newest = std::max(s.newest, Seconds(fd.ftLastWriteTime));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const int64_t now = Seconds(nowFt);
    const std::set<std::string> sent = SentList(g_dir);
    const std::set<std::string> kept = NameList(g_dir, L"kept.txt");
    bool waiting = false;
    for (const auto& [key, s] : sides) {
        if (!s.desync || now - s.newest > kMaxAgeSeconds) continue;
        bool keep = false;
        for (const std::string& name : s.files) keep = keep || kept.count(name);
        if (keep) continue;
        if (!s.log && s.newest >= g_started && now - s.newest < kNoLogAfterSeconds) {
            for (const std::string& name : s.files) waiting = waiting || !sent.count(name);
            continue;
        }
        int fresh = 0;
        for (const std::string& name : s.files) {
            if (sent.count(name)) continue;
            if (!g_on || Stopping()) return true;
            const Sent result = SendFile(url, "reports", g_dir, name);
            if (result == Sent::Later) return false;
            if (result == Sent::Now) fresh++;
        }
        if (fresh) g_sent++;
    }
    return SendCrashes(url) && !waiting;
}

void Round() {
    if (g_busy.exchange(true)) {
        g_again = true;
        return;
    }
    std::thread([] {
        ThreadGuard running;
        do {
            g_again = false;
            if (g_on && !Stopping()) g_left = !SendRound();
        } while (g_again && !Stopping());
        g_busy = false;
    }).detach();
}

} // namespace

void ReportsInstall(const std::wstring& reportDir, const std::string& url, const std::string& modVersion, bool on) {
    g_dir = reportDir;
    g_version = modVersion;
    g_on = on;
    CrashNoteRunUploads(on);
    g_configured = url.empty() ? kDefaultReportUrl : url;
    while (!g_configured.empty() && g_configured.back() == '/') g_configured.pop_back();
    {
        std::lock_guard lock(g_mutex);
        g_url = g_configured;
    }
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    g_started = Seconds(nowFt);
    Log("reports: desync reports %s (%s)", on ? "are sent to" : "are not sent; the server would be", g_configured.c_str());
    std::thread([] {
        ThreadGuard running;
        auto next = std::chrono::steady_clock::now() + kFirstLook;
        bool first = true;
        while (!Stopping()) {
            std::this_thread::sleep_for(kWakeEvery);
            if (std::chrono::steady_clock::now() < next || g_match) continue;
            if (first || g_left) Round();
            first = false;
            next = std::chrono::steady_clock::now() + kRetryEvery;
        }
    }).detach();
}

void ReportsKeepLocal(const std::wstring& dir, const std::string& name) { AddName(dir, L"kept.txt", name); }

void ReportsSetOn(bool on) {
    CrashNoteRunUploads(on);
    if (g_on.exchange(on) == on) return;
    Log("reports: desync reports %s", on ? "are sent" : "are not sent");
    if (on) Round();
}

void ReportsSendReady() { Round(); }

void ReportsSetMatchRunning(bool running) { g_match = running; }

bool ReportsOn() { return g_on; }

int ReportsSent() { return g_sent; }

void ReportsSetUrl(const std::string& url) {
    std::string u = url.empty() ? g_configured : url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    std::lock_guard lock(g_mutex);
    g_url = u;
}

} // namespace sfv::mod
