// Crash reports: the fault handler, the minidump writer, hangs and how the last run ended (docs/startup.md).

#include "mod/crash_report.h"

#include "mod/files.h"
#include "mod/log.h"
#include "mod/report_upload.h"

#include <MinHook.h>
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

constexpr int kMaxDumps = 5;
constexpr ULONGLONG kFaultBeforeExitMs = 15000;
constexpr size_t kLogTailBytes = 256 * 1024;
constexpr size_t kLogTailLines = 400;
constexpr ULONGLONG kHangMs = 20000;
constexpr int kHangThreadsLogged = 40;
constexpr DWORD kWineStubException = 0x80000100;
constexpr DWORD kDebugPrint = 0x40010006, kDebugPrintWide = 0x4001000A, kThreadName = 0x406D1388;
constexpr ULONG_PTR kMaxOrdinal = 0xFFFF;
constexpr DWORD kHangCheckMs = 1000, kWriteWaitMs = 30000, kStopWaitMs = 2000;
constexpr SIZE_T kWriterStack = 256 * 1024;

std::wstring g_dir;
std::wstring g_suffix;
std::string g_version;
thread_local int t_guarded = 0;

EXCEPTION_RECORD g_record;
alignas(16) CONTEXT g_context;
DWORD g_faultThread = 0;
HANDLE g_request = nullptr, g_done = nullptr, g_writer = nullptr;
PVOID g_handler = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_writing{false};
std::atomic<int> g_dumps{0};
std::atomic<ULONGLONG> g_lastFault{0};
std::atomic<bool> g_exited{false};
std::atomic<bool> g_quitting{false};
std::atomic<DWORD> g_gameThread{0};
std::atomic<ULONGLONG> g_lastTick{0};
bool g_hangLogged = false;

using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
TerminateProcessFn g_origTerminate = nullptr;
using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn g_origExitProcess = nullptr;
using ExitFn = void(__cdecl*)(int);
ExitFn g_origExit = nullptr;

std::atomic<bool> g_selfEnded{false};
constexpr ULONGLONG kExceptionBeforeEndMs = 10000;

struct OtherException {
    DWORD code = 0;
    DWORD64 address = 0;
    DWORD thread = 0;
    ULONGLONG tick = 0;
    char stack[640] = {};
};

OtherException g_other;
std::atomic<bool> g_otherBusy{false};

std::wstring RunningPath() { return g_dir + L"\\running" + g_suffix + L".txt"; }

std::wstring PendingPath(const wchar_t* ext) { return g_dir + L"\\pending" + g_suffix + ext; }

const char* CodeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
        case 0xC0000374: return "heap corruption";
        case 0xC0000409: return "stack buffer overrun";
        case 0x80000100: return "unimplemented function (Wine)";
    }
    return nullptr;
}

void CopyStringAt(ULONG_PTR p, char* out, size_t size) {
    size_t n = 0;
    __try {
        for (const char* c = reinterpret_cast<const char*>(p); c && *c && n + 1 < size; c++) out[n++] = *c;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    out[n] = 0;
}

std::string StringAt(ULONG_PTR p) {
    char buf[81];
    CopyStringAt(p, buf, sizeof(buf));
    return buf;
}

std::string Where(DWORD64 address) {
    HMODULE module = nullptr;
    char buf[MAX_PATH + 32];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring name(path, n);
        name = name.substr(name.find_last_of(L'\\') + 1);
        std::string narrow;
        for (wchar_t c : name) narrow += c < 128 ? static_cast<char>(c) : '_';
        std::snprintf(buf, sizeof(buf), "%s+%llX", narrow.c_str(),
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(address));
    }
    return buf;
}

bool Unwind(CONTEXT* c) {
    __try {
        DWORD64 base = 0;
        RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(c->Rip, &base, nullptr);
        if (!fn) {
            c->Rip = *reinterpret_cast<DWORD64*>(c->Rsp);
            c->Rsp += 8;
        } else {
            PVOID handlerData = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, fn, c, &handlerData, &frame, nullptr);
        }
        return c->Rip != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WriteText(const std::wstring& path, const std::string& text) {
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

std::string ReadText(const std::wstring& path, size_t tailBytes = 0) {
    std::string text;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return text;
    if (tailBytes) {
        _fseeki64(f, 0, SEEK_END);
        const long long size = _ftelli64(f);
        if (size > static_cast<long long>(tailBytes)) _fseeki64(f, size - static_cast<long long>(tailBytes), SEEK_SET);
        else _fseeki64(f, 0, SEEK_SET);
    }
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    return text;
}

std::string Stamp(const SYSTEMTIME& t, const char* format = "%04d-%02d-%02d_%02d-%02d-%02d") {
    char buf[32];
    std::snprintf(buf, sizeof(buf), format, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

void WritePending() {
    t_guarded++;
    EXCEPTION_POINTERS pointers{&g_record, &g_context};
    MINIDUMP_EXCEPTION_INFORMATION info{g_faultThread, &pointers, FALSE};
    const HANDLE file = CreateFileW(PendingPath(L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumped = false;
    if (file != INVALID_HANDLE_VALUE) {
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
        CloseHandle(file);
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    const char* name = CodeName(g_record.ExceptionCode);
    char line[256];
    std::string note;
    std::snprintf(line, sizeof(line), "pid %lu\n", GetCurrentProcessId());
    note += line;
    std::snprintf(line, sizeof(line), "fault %s (%08lX) at %s, thread %lu, %s\n", name ? name : "?", g_record.ExceptionCode,
                  Where(reinterpret_cast<DWORD64>(g_record.ExceptionAddress)).c_str(), g_faultThread,
                  Stamp(t, "%04d-%02d-%02d %02d:%02d:%02d").c_str());
    note += line;
    if (g_record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_record.NumberParameters >= 2) {
        const ULONG_PTR kind = g_record.ExceptionInformation[0];
        std::snprintf(line, sizeof(line), "%s %016llX\n", kind == EXCEPTION_READ_FAULT ? "reading" : kind == EXCEPTION_WRITE_FAULT ? "writing"
                                                                                                                                   : "executing",
                      static_cast<unsigned long long>(g_record.ExceptionInformation[1]));
        note += line;
    }
    if (g_record.ExceptionCode == kWineStubException && g_record.NumberParameters >= 2) {
        const ULONG_PTR fn = g_record.ExceptionInformation[1];
        note += "function " + StringAt(g_record.ExceptionInformation[0]) + "." +
                (fn <= kMaxOrdinal ? "#" + std::to_string(fn) : StringAt(fn)) + "\n";
    }
    note += dumped ? "minidump written\n" : "no minidump (" + std::to_string(GetLastError()) + ")\n";
    note += "stack:\n";
    CONTEXT c = g_context;
    for (int i = 0; i < 48; i++) {
        note += "  " + Where(c.Rip) + "\n";
        if (!Unwind(&c)) break;
    }
    WriteText(PendingPath(L".txt"), note);
    t_guarded--;
}

std::wstring GameDumpSince(const std::wstring& since) {
    WIN32_FILE_ATTRIBUTE_DATA mark;
    if (!GetFileAttributesExW(since.c_str(), GetFileExInfoStandard, &mark)) return {};
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    const std::wstring dir = std::wstring(local) + L"\\StreetFighterV\\Saved\\Logs";
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((dir + L"\\Dump*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring newest;
    FILETIME newestTime = mark.ftCreationTime;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &newestTime) > 0) {
            newestTime = fd.ftLastWriteTime;
            newest = dir + L"\\" + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return newest;
}

void AppendRunning(const std::string& line) {
    if (g_dir.empty() || g_exited) return;
    WriteText(RunningPath(), ReadText(RunningPath()) + line + "\n");
}

std::string ThreadStack(DWORD id, int frames, bool* ours = nullptr) {
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
    if (!thread) return {};
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool read = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        read = GetThreadContext(thread, &c) != 0;
        ResumeThread(thread);
    }
    CloseHandle(thread);
    if (!read) return {};
    std::string stack;
    t_guarded++;
    for (int i = 0; i < frames; i++) {
        const std::string where = Where(c.Rip);
        if (ours && (where.rfind("StreetFighterV.exe", 0) == 0 || where.rfind("polarity", 0) == 0 || where.rfind("sfv_rollback", 0) == 0)) *ours = true;
        stack += " " + where;
        if (!Unwind(&c)) break;
    }
    t_guarded--;
    return stack;
}

std::string ThreadName(DWORD id) {
    using GetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get = reinterpret_cast<GetDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (!get) return {};
    const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!thread) return {};
    std::string name;
    PWSTR text = nullptr;
    if (SUCCEEDED(get(thread, &text)) && text) {
        for (const wchar_t* p = text; *p && name.size() < 48; p++) name += *p < 128 ? static_cast<char>(*p) : '_';
        LocalFree(text);
    }
    CloseHandle(thread);
    return name;
}

void CheckHang() {
    const DWORD id = g_gameThread;
    const ULONGLONG last = g_lastTick;
    if (!id || !last || g_quitting) return;
    const ULONGLONG idle = GetTickCount64() - last;
    if (idle < kHangMs) {
        if (g_hangLogged) Log("crash report: the game thread ticks again");
        g_hangLogged = false;
        return;
    }
    if (g_hangLogged) return;
    g_hangLogged = true;
    const std::string stack = ThreadStack(id, 32);
    if (stack.empty()) return;
    Log("crash report: the game thread (%lu) has not ticked for %llu s; it is at:%s", id,
        static_cast<unsigned long long>(idle / 1000), stack.c_str());

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<std::pair<std::string, std::string>> groups;
    int ours = 0, others = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == id || entry.th32ThreadID == self) continue;
        bool mine = false;
        const std::string other = ThreadStack(entry.th32ThreadID, 24, &mine);
        if (!mine) {
            others++;
            continue;
        }
        ours++;
        const std::string name = ThreadName(entry.th32ThreadID);
        const std::string who = std::to_string(entry.th32ThreadID) + (name.empty() ? "" : " (" + name + ")");
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == other; });
        if (it != groups.end()) it->second += ", " + who;
        else if (static_cast<int>(groups.size()) < kHangThreadsLogged) groups.emplace_back(other, who);
    }
    CloseHandle(snapshot);
    for (const auto& [stack, who] : groups) {
        Log("crash report:   %s %s at:%s", who.find(',') == std::string::npos ? "thread" : "threads", who.c_str(), stack.c_str());
    }
    Log("crash report:   %d threads with game or mod frames, %d others", ours, others);
}

DWORD WINAPI Writer(LPVOID) {
    for (;;) {
        if (WaitForSingleObject(g_request, kHangCheckMs) == WAIT_TIMEOUT) {
            CheckHang();
            continue;
        }
        if (g_stop) return 0;
        WritePending();
        SetEvent(g_done);
    }
}

bool Fatal(DWORD code) { return CodeName(code) != nullptr; }

bool Chatter(DWORD code) { return code == kDebugPrint || code == kDebugPrintWide || code == kThreadName; }

void NoteOtherException(const EXCEPTION_POINTERS* e) {
    if (g_otherBusy.exchange(true)) return;
    t_guarded++;
    g_other.code = e->ExceptionRecord->ExceptionCode;
    g_other.address = reinterpret_cast<DWORD64>(e->ExceptionRecord->ExceptionAddress);
    g_other.thread = GetCurrentThreadId();
    g_other.tick = GetTickCount64();
    std::string stack;
    CONTEXT c = *e->ContextRecord;
    for (int i = 0; i < 16 && stack.size() + 64 < sizeof(g_other.stack); i++) {
        stack += " " + Where(c.Rip);
        if (!Unwind(&c)) break;
    }
    std::snprintf(g_other.stack, sizeof(g_other.stack), "%s", stack.c_str());
    t_guarded--;
    g_otherBusy = false;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (t_guarded > 0 || g_quitting) return EXCEPTION_CONTINUE_SEARCH;
    if (!Fatal(code)) {
        if (!Chatter(code)) NoteOtherException(e);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_lastFault = GetTickCount64();
    if (g_dumps >= kMaxDumps || g_writing.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    g_dumps++;
    g_record = *e->ExceptionRecord;
    g_context = *e->ContextRecord;
    g_faultThread = GetCurrentThreadId();
    SetEvent(g_request);
    WaitForSingleObject(g_done, kWriteWaitMs);
    g_writing = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string Value(const std::string& text, const char* key) {
    const std::string k = std::string(key) + " ";
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, k.size(), k) == 0) return text.substr(at + k.size(), end - at - k.size());
        at = end + 1;
    }
    return "";
}

std::string LastValue(const std::string& text, const char* key) {
    const std::string k = std::string(key) + " ";
    std::string value;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, k.size(), k) == 0) value = text.substr(at + k.size(), end - at - k.size());
        at = end + 1;
    }
    return value;
}

} // namespace

void CrashGuardEnter() { t_guarded++; }

void CrashGuardLeave() { t_guarded--; }

std::string CrashCheckPreviousRun(const std::wstring& suffix, const std::string& version) {
    g_dir = FilesCrashesDir();
    g_suffix = suffix;
    FilesEnsureDirectory(g_dir);
    const std::string running = ReadText(RunningPath());
    const std::string pending = ReadText(PendingPath(L".txt"));
    if (running.empty()) {
        DeleteFileW(PendingPath(L".dmp").c_str());
        DeleteFileW(PendingPath(L".txt").c_str());
        return "";
    }
    const bool fault = !pending.empty() && Value(pending, "pid") == Value(running, "pid");
    const std::string quitting = Value(running, "quitting");
    if (!fault && (!quitting.empty() || Value(running, "hooked") != "1")) {
        DeleteFileW(PendingPath(L".dmp").c_str());
        DeleteFileW(PendingPath(L".txt").c_str());
        DeleteFileW(RunningPath().c_str());
        return "crash report: the last run (Polarity " + Value(running, "version") + ") ended " +
               (!quitting.empty() ? "while quitting (" + quitting + ")" : "during its start, before the exit hooks") +
               ": not reported";
    }
    const std::wstring logPath = FilesLogsDir() + L"\\polarity" + suffix + L".log";
    SYSTEMTIME when{};
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &info)) {
        FILETIME local;
        FileTimeToLocalFileTime(&info.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &when);
    } else {
        GetLocalTime(&when);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    char id[16];
    std::snprintf(id, sizeof(id), "%08lx", static_cast<unsigned long>(counter.QuadPart ^ (counter.QuadPart >> 32) ^ GetCurrentProcessId()));
    const std::string name = Stamp(when) + "_crash_" + id;

    std::string report = "Polarity " + Value(running, "version") + " crash report\n";
    report += "run started " + Value(running, "started") + ", last logged " + Stamp(when, "%04d-%02d-%02d %02d:%02d:%02d") + "\n";
    report += "reported by Polarity " + version + "\n";
    const std::string ended = Value(running, "ended");
    if (fault) {
        report += "how it ended: a fault\n";
        const size_t at = pending.find('\n');
        report += pending.substr(at == std::string::npos ? 0 : at + 1);
    } else if (!ended.empty()) {
        const size_t from = ended.find(" from");
        report += "how it ended: the game ended itself (" + ended.substr(0, from) + "), the player not quitting\n";
        if (from != std::string::npos) report += "called from:" + ended.substr(from + 5) + "\n";
        const std::string exception = Value(running, "exception");
        if (!exception.empty()) report += "the last exception before it: " + exception + "\n";
    } else {
        report += "how it ended: no clean exit and no fault seen (closed by force, a freeze ended by force, or a crash "
                  "past the handlers)\n";
    }
    const std::wstring gameDump = GameDumpSince(RunningPath());
    if (!gameDump.empty()) {
        std::string file;
        for (wchar_t c : gameDump.substr(gameDump.find_last_of(L'\\') + 1)) file += c < 128 ? static_cast<char>(c) : '_';
        report += "the game wrote its own crash dump: " + file + (fault ? "" : " (sent as this report's dump)") + "\n";
    }
    std::string tail = ReadText(logPath, kLogTailBytes);
    size_t lines = 0, from = tail.size();
    while (from > 0 && lines <= kLogTailLines) {
        from = tail.rfind('\n', from - 1);
        if (from == std::string::npos) {
            from = 0;
            break;
        }
        lines++;
    }
    report += "\nthe end of the run's log:\n" + tail.substr(from == 0 ? 0 : from + 1);
    WriteText(g_dir + L"\\" + std::wstring(name.begin(), name.end()) + L".log", report);
    const std::wstring dumpPath = g_dir + L"\\" + std::wstring(name.begin(), name.end()) + L".dmp";
    if (fault) MoveFileExW(PendingPath(L".dmp").c_str(), dumpPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    else if (!gameDump.empty()) CopyFileW(gameDump.c_str(), dumpPath.c_str(), FALSE);
    const bool withDump = fault || (!gameDump.empty() && GetFileAttributesW(dumpPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    DeleteFileW(PendingPath(L".dmp").c_str());
    DeleteFileW(PendingPath(L".txt").c_str());
    DeleteFileW(RunningPath().c_str());
    const bool keep = LastValue(running, "uploads") == "0";
    if (keep) {
        ReportsKeepLocal(g_dir, name + ".log");
        if (withDump) ReportsKeepLocal(g_dir, name + ".dmp");
    }
    FilesPruneOldest(g_dir, L"*_crash_*.log", kKeptCrashReports);
    FilesPruneOldest(g_dir, L"*_crash_*.dmp", kKeptCrashReports);
    std::string found = "crash report: the last run (Polarity " + Value(running, "version") + ") ended " +
                        (fault            ? "with a fault: " + Value(pending, "fault")
                         : !ended.empty() ? "by itself (" + ended.substr(0, ended.find(" from")) + ")"
                                          : "without a clean exit (no fault seen)") +
                        "; " + name + ".log" + (keep ? " (kept on this PC: that run did not send reports)" : "");
    return found;
}

void CrashInstallHandler(const std::wstring& suffix, const std::string& version) {
    g_dir = FilesCrashesDir();
    g_suffix = suffix;
    g_version = version;
    FilesEnsureDirectory(g_dir);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char text[256];
    std::snprintf(text, sizeof(text), "pid %lu\nversion %s\nstarted %s\n", GetCurrentProcessId(), version.c_str(),
                  Stamp(t, "%04d-%02d-%02d %02d:%02d:%02d").c_str());
    WriteText(RunningPath(), text);
    g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_writer = CreateThread(nullptr, kWriterStack, Writer, nullptr, 0, nullptr);
    if (g_request && g_done && g_writer) g_handler = AddVectoredExceptionHandler(0, Handler);
}

void CrashOnProcessExit(const char* how) {
    (void)how;
    if (g_dir.empty() || g_exited.exchange(true)) return;
    const ULONGLONG fault = g_lastFault;
    if (!g_quitting && fault && GetTickCount64() - fault < kFaultBeforeExitMs) return;
    if (g_selfEnded) return;
    DeleteFileW(RunningPath().c_str());
    DeleteFileW(PendingPath(L".dmp").c_str());
    DeleteFileW(PendingPath(L".txt").c_str());
}

namespace {
void NoteSelfEnd(const char* how, UINT code) {
    if (g_dir.empty() || g_quitting || g_exited || g_selfEnded) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG fault = g_lastFault;
    if (fault && now - fault < kFaultBeforeExitMs) return;
    const bool otherJustBefore = g_other.tick && now - g_other.tick < kExceptionBeforeEndMs;
    if (code == 0 && !otherJustBefore) return;
    g_selfEnded = true;
    void* frames[24];
    const USHORT n = RtlCaptureStackBackTrace(2, 24, frames, nullptr);
    std::string stack;
    t_guarded++;
    for (USHORT i = 0; i < n; i++) stack += " " + Where(reinterpret_cast<DWORD64>(frames[i]));
    t_guarded--;
    char line[160];
    std::snprintf(line, sizeof(line), "ended %s code %u from", how, code);
    Log("crash report: the game ends itself (%s, code %u) while not quitting; called from:%s", how, code, stack.c_str());
    AppendRunning(line + stack);
    if (otherJustBefore) {
        std::snprintf(line, sizeof(line), "exception %08lX at %s, thread %lu, %llu ms before the end, at:", g_other.code,
                      Where(g_other.address).c_str(), g_other.thread, static_cast<unsigned long long>(now - g_other.tick));
        Log("crash report: the last exception before it: %s%s", line, g_other.stack);
        AppendRunning(std::string(line) + g_other.stack);
    }
}

BOOL WINAPI TerminateDetour(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
        NoteSelfEnd("TerminateProcess", code);
        CrashOnProcessExit("terminate");
    }
    return g_origTerminate(process, code);
}

void WINAPI ExitProcessDetour(UINT code) {
    NoteSelfEnd("ExitProcess", code);
    g_origExitProcess(code);
}

void __cdecl ExitDetour(int code) {
    CrashOnQuitting("exit");
    g_origExit(code);
}
} // namespace

void CrashOnQuitting(const char* how) {
    if (g_quitting.exchange(true) || g_dir.empty()) return;
    Log("crash report: the game is quitting (%s): faults from here are not reported", how);
    AppendRunning(std::string("quitting ") + how);
}

void CrashNoteRunUploads(bool on) {
    static int noted = -1;
    if (noted == (on ? 1 : 0)) return;
    noted = on ? 1 : 0;
    AppendRunning(on ? "uploads 1" : "uploads 0");
}

void CrashNoteGameTick() {
    if (!g_gameThread) g_gameThread = GetCurrentThreadId();
    g_lastTick = GetTickCount64();
}

void CrashHookProcessEnd() {
    LPVOID target = nullptr;
    if (MH_CreateHookApiEx(L"kernel32", "TerminateProcess", reinterpret_cast<LPVOID>(&TerminateDetour),
                           reinterpret_cast<LPVOID*>(&g_origTerminate), &target) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("crash report: cannot hook TerminateProcess (an engine exit then counts as a bad end)");
    }
    LPVOID exitProcessTarget = nullptr;
    if (MH_CreateHookApiEx(L"kernel32", "ExitProcess", reinterpret_cast<LPVOID>(&ExitProcessDetour),
                           reinterpret_cast<LPVOID*>(&g_origExitProcess), &exitProcessTarget) != MH_OK ||
        MH_EnableHook(exitProcessTarget) != MH_OK) {
        Log("crash report: cannot hook ExitProcess (the game ending itself that way is not reported)");
    }
    LPVOID exitTarget = nullptr;
    if (MH_CreateHookApiEx(L"msvcr120", "exit", reinterpret_cast<LPVOID>(&ExitDetour), reinterpret_cast<LPVOID*>(&g_origExit),
                           &exitTarget) != MH_OK ||
        MH_EnableHook(exitTarget) != MH_OK) {
        Log("crash report: cannot hook the runtime's exit (a fault while quitting may be reported)");
    }
    AppendRunning("hooked 1");
}

void CrashRemoveHandler() {
    CrashOnProcessExit("reload");
    if (g_handler) RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
    if (g_writer) {
        g_stop = true;
        SetEvent(g_request);
        WaitForSingleObject(g_writer, kStopWaitMs);
        CloseHandle(g_writer);
        g_writer = nullptr;
    }
}

} // namespace sfv::mod
