#pragma once
// Crash reports, written in the game and sent at the next launch (by the
// desync reports' uploader, report_upload.h, with its opt-out).
//
// While the game runs, SFVR\crashes\running[.N].txt says so (pid, version).
// A clean end deletes it: the process exiting, or ending itself with no
// fault just before, or a hot reload. A file still there at the next start
// means the run ended badly (a crash, a freeze ended by force, a kill), and
// that run's report is made from the end of its log.
//
// A fault the game would die of (an access violation, a stack overflow...)
// is caught as it happens, by a vectored handler: the engine's own handler
// ends the process before any filter of ours would run. A thread started at
// launch writes a small minidump (threads, their stacks, the modules; no
// heap) and a note with the fault and its stack as module+offset, as
// pending[.N].dmp/.txt; the faulting thread then goes on as if nothing had
// looked. Faults code catches on purpose are left out (CrashGuardEnter).
// The next start's report takes the pending files when they are that run's.
//
// A report is SFVR\crashes\<when>_crash_<id>.log, with <...>.dmp when there
// was a fault: names the server's sfvr-reports takes.

#include <string>

namespace sfv::mod {

// At start, before the log rotates: when the previous run of this instance
// ended badly, makes its report. What it found, for the log once it is open
// ("" when the previous run ended well).
std::string CheckPreviousRun(const std::wstring& suffix, const std::string& version);
// Marks this run as running; installs the fault handler and its writer.
void InstallCrashHandler(const std::wstring& suffix, const std::string& version);
// The process ends itself or exits: a clean end unless a fault came just
// before (then the engine is ending it for that fault).
void OnProcessExit(const char* how);
// Before a hot reload: a clean end, and the handler and writer go.
void RemoveCrashHandler();
// Hooks TerminateProcess (the engine ends the game with it, also after a
// crash) and the C runtime's exit (the game quitting): needs MinHook
// initialised.
void HookProcessEnd();
// The game is quitting (its window closed, or the runtime's exit): a fault
// from here on is not reported, and the run ends well. The game itself
// faulted in its exit now and then (quit while a character loads).
void OnQuitting(const char* how);

// Around code that faults on purpose and catches it (guarded reads; calls,
// not an object: a function with __try cannot hold one): the fault handler
// leaves those faults alone.
void CrashGuardEnter();
void CrashGuardLeave();

} // namespace sfv::mod
