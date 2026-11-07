#pragma once
// Desync and crash reports sent to the SFVR server, unless the player turned
// it off (Settings, Logs; [netplay] upload_reports). After a match whose
// recording has a desync, the side's files of that match go to the server's
// sfvr-reports service (sfvr-server repository), which files them by match
// id: both players' halves of a desync end up side by side. Crash reports
// (crash_report.h, SFVR\crashes) go as they are found, to its crashes.
//
// What a match leaves in SFVR\matches (netplay_driver.cpp SaveReport):
//   <...>_<match id>_<p1|p2|spec>_desync.sfvr  each desync's report
//   <...>_<match id>_<p1|p2|spec>.sfvr / .log  the match's recording and log
// A side's files go once its match log is there (or, when the game closed or
// crashed in the match, at the next launch; within one run, once the report
// is half an hour old), at most two weeks old. What was sent is listed in
// SFVR\matches\uploaded.txt; anything not sent (no network, the server busy)
// is tried again every few minutes outside matches, and after each match.
//
// The server: [report] url in sfv_rollback.ini, else kDefaultReportUrl.

#include <string>

namespace sfv::mod {

// sfvr-reports on the public server (2.29.123.240, as kDefaultServer: the
// rooms VPS from 0.3.0, the sfvr-server Docker stack's reports container).
constexpr const char* kDefaultReportUrl = "http://2.29.123.240:5580";

// From the mod's start: looks for unsent reports a little later.
void InstallReportUploads(const std::wstring& reportDir, const std::string& url, const std::string& modVersion, bool on);
// The player's choice (the panel's checkbox).
void SetReportUploads(bool on);
// After a match's files are written: sends what is ready, in the background.
void SendReadyReports();
// While a match runs, nothing is retried (SendReadyReports still sends).
void SetMatchRunning(bool running);
// Whether this side sends reports (the other side hears it at the handshake).
bool ReportUploadsOn();
// Reports sent in this run, a desync's match or a crash (the overlay toasts
// each new one).
int ReportsSent();
// For tests: another server (empty: the configured one).
void SetReportUrl(const std::string& url);
// A report made while uploads were off (a test run's killed games and
// desyncs, or the player's choice then): listed in that folder's kept.txt and
// never sent, also once uploads are on again. A match side with any kept file
// stays home whole.
void KeepReportLocal(const std::wstring& dir, const std::string& name);

} // namespace sfv::mod
