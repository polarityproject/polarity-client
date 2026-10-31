#include "mod/report_upload.h"

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
constexpr uint64_t kMaxFileBytes = 16ull << 20;          // the server's limit
constexpr int64_t kMaxAgeSeconds = 14 * 24 * 3600;       // older reports stay home
constexpr int64_t kNoLogAfterSeconds = 30 * 60;          // a report without its match log (a crash mid-match)
constexpr auto kFirstLook = std::chrono::seconds(30);    // after the mod's start

std::mutex g_mutex;
std::wstring g_dir;
std::string g_configured; // [report] url, else kDefaultReportUrl
std::string g_url;        // under g_mutex
std::string g_version;
std::atomic<bool> g_on{true};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_again{false}; // asked while a round ran: one more after it
std::atomic<int> g_sent{0};

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// The name's ASCII (the mod's own report names are; anything else is not one).
bool Ascii(const std::wstring& w, std::string* out)
{
    out->clear();
    for (wchar_t c : w) {
        if (c >= 128) return false;
        *out += static_cast<char>(c);
    }
    return true;
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>* out)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// POST `body` to `url`; the HTTP status, or 0 when the server was not reached.
DWORD Post(const std::string& url, const std::vector<uint8_t>& body, std::string* error)
{
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
    HINTERNET session = WinHttpOpen(L"SFVR reports", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    DWORD status = 0;
    if (request) {
        WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        const std::wstring headers = L"Content-Type: application/octet-stream\r\nX-SFVR-Version: " + Wide(g_version) + L"\r\n";
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), const_cast<uint8_t*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
        }
        else {
            *error = "cannot reach " + url + " (error " + std::to_string(GetLastError()) + ")";
        }
    }
    else {
        *error = "cannot open " + url;
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return status;
}

// One side's files of one match.
struct Side {
    std::vector<std::string> files;
    bool desync = false, log = false;
    int64_t newest = 0; // seconds since 1601 (the newest file)
};

int64_t Seconds(const FILETIME& t) { return static_cast<int64_t>((uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 10000000); }

// "<...>_<match id>_<p1|p2|spec>[_desync].<sfvr|log>" -> "<match id>_<side>".
bool SideKey(const std::string& name, std::string* key, bool* desync, bool* log)
{
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

std::set<std::string> SentList()
{
    std::set<std::string> sent;
    FILE* f = _wfopen((g_dir + L"\\uploaded.txt").c_str(), L"r");
    if (!f) return sent;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) sent.insert(s);
    }
    std::fclose(f);
    return sent;
}

void MarkSent(const std::string& name)
{
    FILE* f = _wfopen((g_dir + L"\\uploaded.txt").c_str(), L"a");
    if (!f) return;
    std::fprintf(f, "%s\n", name.c_str());
    std::fclose(f);
}

// Sends every side that is ready; false when the server could not take more
// (not reached, busy): the rest waits for the next round.
bool SendRound()
{
    std::string url;
    {
        std::lock_guard lock(g_mutex);
        url = g_url;
    }
    if (url.empty() || g_dir.empty()) return true;
    std::map<std::string, Side> sides;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((g_dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return true;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name, key;
        bool desync = false, log = false;
        if (!Ascii(fd.cFileName, &name) || !SideKey(name, &key, &desync, &log)) continue;
        if ((uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow) > kMaxFileBytes) {
            continue; // a full recording (testing): too large for the server
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
    const std::set<std::string> sent = SentList();
    for (const auto& [key, s] : sides) {
        if (!s.desync || now - s.newest > kMaxAgeSeconds) continue;
        if (!s.log && now - s.newest < kNoLogAfterSeconds) continue; // the match is still on
        int fresh = 0;
        for (const std::string& name : s.files) {
            if (sent.count(name)) continue;
            if (!g_on || Stopping()) return true;
            std::vector<uint8_t> body;
            if (!ReadFile(g_dir + L"\\" + Wide(name), &body)) continue;
            std::string error;
            const DWORD status = Post(url + "/v1/reports/" + name, body, &error);
            if (status == 200 || status == 201) {
                Log("reports: sent %s (%zu KB)", name.c_str(), body.size() / 1024);
                MarkSent(name);
                fresh++;
            }
            else if (status == 400 || status == 404 || status == 411 || status == 413) {
                // Refused for what it is: sending it again would not help.
                Log("reports: the server refused %s (%lu)", name.c_str(), status);
                MarkSent(name);
            }
            else {
                Log("reports: %s not sent: %s", name.c_str(), status ? ("the server answered " + std::to_string(status)).c_str() : error.c_str());
                return false;
            }
        }
        if (fresh) g_sent++;
    }
    return true;
}

void Round()
{
    if (g_busy.exchange(true)) {
        g_again = true;
        return;
    }
    std::thread([] {
        ThreadGuard running;
        do {
            g_again = false;
            if (g_on && !Stopping()) SendRound();
        } while (g_again && !Stopping());
        g_busy = false;
    }).detach();
}

} // namespace

void InstallReportUploads(const std::wstring& reportDir, const std::string& url, const std::string& modVersion, bool on)
{
    g_dir = reportDir;
    g_version = modVersion;
    g_on = on;
    g_configured = url.empty() ? kDefaultReportUrl : url;
    while (!g_configured.empty() && g_configured.back() == '/') g_configured.pop_back();
    {
        std::lock_guard lock(g_mutex);
        g_url = g_configured;
    }
    Log("reports: desync reports %s (%s)", on ? "are sent to" : "are not sent; the server would be", g_configured.c_str());
    std::thread([] {
        ThreadGuard running;
        const auto until = std::chrono::steady_clock::now() + kFirstLook;
        while (std::chrono::steady_clock::now() < until) {
            if (Stopping()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        Round();
    }).detach();
}

void SetReportUploads(bool on)
{
    if (g_on.exchange(on) == on) return;
    Log("reports: desync reports %s", on ? "are sent" : "are not sent");
    if (on) Round();
}

void SendReadyReports() { Round(); }

int ReportsSent() { return g_sent; }

void SetReportUrl(const std::string& url)
{
    std::string u = url.empty() ? g_configured : url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    std::lock_guard lock(g_mutex);
    g_url = u;
}

} // namespace sfv::mod
