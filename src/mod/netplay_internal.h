#pragma once
// The netplay driver's class and what its source files share: the lobby,
// heartbeats and messages (netplay_lobby.cpp), the session's course, sets
// and end (netplay_course.cpp), getting into the battle and its frozen start
// (netplay_jump.cpp), the match (netplay_match.cpp), and the rest with the
// functions netplay_driver.h exposes (netplay_driver.cpp). docs/netplay.md
// has how a session runs.

#include "mod/netplay_driver.h"

#include "mod/battle_data.h"
#include "mod/benchmark.h"
#include "mod/entitlements.h"
#include "core/display_name.h"
#include "core/match_code.h"
#include "core/match_ticket.h"
#include "core/netplay.h"
#include "core/file_open.h"
#include "core/replay_file.h"
#include "core/spectate.h"
#include "core/update_info.h"
#include "core/state.h"
#include "mod/battle_driver.h"
#include "mod/battle_setup.h"
#include "mod/pacing.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/files.h"
#include "mod/flow.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/report_upload.h"
#include "mod/rooms_client.h"
#include "mod/sfv_game.h"
#include "mod/status.h"
#include "mod/sfv_offsets.h"
#include "mod/udp_transport.h"
#include "mod/weyve_service.h"

#include <windows.h>
#include <xinput.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <optional>
#include <vector>

namespace sfv::mod::netplay {

using Clock = std::chrono::steady_clock;
using lobby::Choice;
using lobby::Role;
using lobby::SessionPhase;

constexpr uint8_t kControlChannel = 1;
constexpr uint8_t kGekkoChannel = 2;
constexpr int kSettleFrames = 90;
constexpr int kDirectJumpSeconds = 60;
constexpr int kMenuAfterMatchFrames = 60 * 3;
constexpr auto kDifferentStartGrace = std::chrono::seconds(2);
constexpr uint8_t kPredictionWindow = 8;
constexpr int kMaxInputDelay = 10;
inline std::atomic<bool> g_skewStart{false};
inline std::mutex g_badPickMutex;
inline std::optional<BadPick> g_badPick;
constexpr auto kBeat = std::chrono::milliseconds(250);
constexpr auto kMinBeat = std::chrono::milliseconds(33);
constexpr auto kSilence = std::chrono::seconds(10);
constexpr auto kConnectTimeout = std::chrono::seconds(15);
constexpr auto kJoinTimeout = std::chrono::seconds(20);
constexpr auto kPreloadWait = std::chrono::seconds(90);
constexpr auto kSetNextGameAfter = std::chrono::seconds(6);
constexpr auto kAgainAnswerWait = std::chrono::seconds(5);
constexpr auto kSetVote = std::chrono::seconds(15);
inline std::atomic<int> g_forfeitSeat{-1};
constexpr size_t kMaxSpectators = 8;
constexpr int kFramesPerSecond = 60;
constexpr int kJumpPollMs = 100;
constexpr int kWarmupLogEvery = 300; // neutral ticks
constexpr long long kSlowLinkQueryUs = 2000;
constexpr int64_t kMaxRttMs = 10000;
constexpr int kPaceLogFrames = 600; // rendered frames
constexpr int kLoopsLogged = 200;
constexpr int kRelayWatchDelayFrames = 120;
constexpr int kStreamBatchesAhead = 4;
constexpr auto kStreamResend = std::chrono::milliseconds(300);

const char* const kRandomCharacters[] = {"RYU", "KEN", "CNL", "CMY", "NSH", "VEG", "KRN", "LAR", "FAN", "RSD",
                                         "RMK", "ZGF", "NCL", "DSM", "BLR", "ALX", "GUL", "IBK", "JRI", "URN"};

using State = NetView::Phase;

enum class Next { None,
                  Jump,
                  Lobby,
                  End };

inline std::mutex g_resultMutex;
inline std::string g_lastResult;

void SetResult(const std::string& text);

std::string LastResult();

std::string Hex64(uint64_t v);

void CopyToClipboard(const std::string& text);

std::string SteamPersonaName();

void PressPad(int pad, uint16_t buttons, int holdMs = 90, int gapMs = 600);

bool LeaveBattleForStageSelect(SfvGame& game, const std::atomic<bool>& stop);

std::string SetupMismatch(const MatchSetup& a, const MatchSetup& b);

const char* RejectText(lobby::RejectReason r);

struct Watcher {
    std::string name;
    Clock::time_point heard;
    uint32_t heardTime = 0;
    Clock::time_point heardTimeAt;
    uint64_t match = 0;
    bool loaded = false;
    uint32_t checksum = 0;
    bool streaming = false;
    int wanted = 0;
    int sentThrough = -1;
    Clock::time_point progressAt;
    bool keeper = false;
};

enum class NoBattle { Stay,
                      Menu };

void ReturnToMenuOnceBattleRuns(SfvGame& game, uint64_t switchesAtJump, NoBattle none, int framesLeft = 1800);

class NetplayDriver : public IFrameDriver {
public:
    NetplayDriver(SfvGame& game, std::wstring reportDir, std::string modVersion);

    ~NetplayDriver() override;

    bool Open(const NetplayOptions& o, const MatchTicket& ticket, std::string* error);

    void StartHost();

    bool StartJoin(const std::string& code);

    void SetConditions(const NetConditions& c);

    void SetReady(bool ready);

    void SetSelectState(uint8_t screen, uint8_t row, uint32_t hover);

    void SetPick(bool hasPick, const PlayerSetup& pick);

    void SetDelay(uint8_t frames);

    void TestChat(const std::string& text);

    void ChooseMatch(uint64_t id);

    void SetStage(const std::string& code);

    void SetStageLocked(bool locked);

    void Choose(Choice c);

    std::string Status();

    NetView View();

    void Stop();

    bool OnFrame(SfvGame& game) override;

    std::vector<PreloadPick> PreloadPicks();

    std::vector<PreloadPick> PreloadPicksLocked() const;

    bool BothReadyLocked() const;

private:
    uint32_t Now() const;

    void StartThread();

    void SendTo(uint64_t to, lobby::Message m);

    void Send(lobby::Message m);

    std::vector<uint64_t> Members();

    void SendBye();

    void HostReady(const std::string& code);

    void BeginHello();

    void ControlTick();

    void RoomEvents();

    void UpdateLinkPath();

    bool CheckSilence(Clock::time_point now);

    void Heard(uint32_t time);

    void Echo(lobby::Message& m, uint32_t heardTime, Clock::time_point heardAt);

    void TakeEcho(const lobby::Message& m);

    lobby::Seat OwnSeat();

    uint16_t StageRule();

    void HostBeat(State s);

    void GuestBeat(State s);

    lobby::Message WelcomeMessage();

    lobby::Message LoadedMessage();

    void OnMessage(uint64_t from, const lobby::Message& m);

    void OnHostMessage(uint64_t from, const lobby::Message& m, State s);

    void OnGuestMessage(uint64_t from, const lobby::Message& m, State s);

    bool IsKeeperMember(uint64_t from);

    void Reject(uint64_t to, lobby::RejectReason reason);

    void MaybeStartMatch();

    void StartMatch(bool rematch = false);

    void MaybeResolve();

    enum class Again { None,
                       Ask,
                       Granted,
                       Refused };

    bool AgainOn() const;

    void ResolveAgain(Choice mine, Choice theirs);

    void ContinueSet();

public:
    void SetAgainOffered(bool offered);

    void AgainAnswer(bool ok, const std::string& text);

private:
    void Transitions();

    void RefuseFromPeer(const std::string& why);

    void EndSessionFor(const std::string& why, const std::string& theirs);

    static bool LacksVersion(const MatchSetup& s, std::string* here, std::string* there);

    std::string SetScoreText();

    int SetWinnerLocked();

    bool LeavingMidSet();

    bool TookForfeit(int seat);

    std::string SetOverText();

    void EndSession(const std::string& why);

    PlayerSetup RollEleven(const PlayerSetup& before);

    MatchSetup ChooseSetup(bool rematch);

    void StreamToWatchers();

    void ReleaseResults();

    int ViewerSide() const;

    void BeginJump();

    void RunJump();

    bool FrameWhileLoading(SfvGame& game);

    void StartSession(SfvGame& game);

    void StartWatching(SfvGame& game);

    bool FrameWhileRunning(SfvGame& game);

    void Stream();

    void KeepMatchLocked(uint64_t id);

    lobby::Message MatchListLocked() const;

    std::optional<lobby::Message> KeptLoaded(uint64_t id);

    void WatchMatch(const lobby::MatchEntry& e);

    bool WatchNext();

    void Publish(float ahead);

    bool FrameWhileWatching(SfvGame& game);

    static constexpr int kRoundCallFrames = 196; // measured: flow event 5 to 6 (3.267 s)
    bool loggedInFight_ = false;
    bool sawFight_ = false; // this match's fight has run (from then on every stage outside the fight is a round transition)
    static constexpr int kEarlyMargin = 2;

    void UpdatePredictionWindow();

    void LogPace(float ahead, bool wait);

    uint8_t LockstepDelay();

    void PlayInputLoop(int frame);

    int loopsLogged_ = 0, loopLoggedAt_ = -1;

    void Mash(int frame);

    void FinishMatch(SfvGame& game, bool keepSetup = false);

    ReplayFile WithSummary(ReplayFile rf, bool leftEarly);

    void SaveToBattleLog(const ReplayFile& rf);

    std::string SaveReport(const ReplayFile& rf, const char* kind, bool background = false);

    void NotePeerReports(bool reports);

    void WriteMatchLog(const std::string& recording, const ReplayFile& rf, const std::string& summary);

    State GetState();

    void SetState(State s);

    void SetStatus(const std::string& text);

    SfvGame& game_;
    std::wstring reportDir_;
    std::string modVersion_;
    std::mt19937_64 rng_;
    const Clock::time_point epoch_;
    NetplayOptions options_; // pick fields guarded by mutex_
    bool host_ = false;
    std::atomic<Role> role_{Role::Player};
    std::string name_;
    lobby::Link link_ = lobby::Link::Unknown;

    std::unique_ptr<ITransport> base_;
    WeyveService* weyve_ = nullptr; // base_, when it is the Weyvelength client
    std::unique_ptr<ConditionedTransport> conditioned_;
    std::unique_ptr<Mux> mux_;
    ITransport* control_ = nullptr;
    ITransport* gekko_ = nullptr;
    uint64_t localId_ = 0;
    uint64_t build_ = 0;

    std::mutex mutex_;
    State state_ = State::Idle;
    std::string status_;
    std::string code_;
    std::atomic<uint64_t> peer_{0}; // the host, or (host) the other player
    uint16_t nonce_ = 0;
    std::atomic<uint64_t> matchId_{0};
    MatchSetup setup_;                         // guarded by mutex_ where both threads touch it
    lobby::Seat seats_[kNumPlayers];           // guarded by mutex_
    int played_ = 0;                           // guarded by mutex_
    std::atomic<bool> holdOverResults_{false}; // the lobby holds the last match's results screen (OnFrame)
    std::atomic<int> nextDelay_{-1};           // the input delay for the next match (-1: unchanged)
    std::atomic<bool> lockedIn_{false};        // the player said ready: their pick (and the host's stage) hold
    uint8_t selectScreen_ = 0, selectRow_ = 0; // SetSelectState, under mutex_
    uint32_t selectHover_ = 0;
    std::atomic<bool> beatNow_{false};
    std::atomic<bool> againOffered_{false}; // the keeper's again mark on this player's table (rooms_client)
    std::atomic<Again> again_{Again::None};
    Clock::time_point againAskedAt_{};
    Rules againRules_;
    bool againSelect_ = false;           // the vote said character select     // the next ControlTick beats at once (a select screen change)
    std::atomic<int> hostStageRule_{-1}; // host: the stage rule set in the lobby (-1: from the options)
    uint16_t streaks_[kNumPlayers] = {}; // host: each player's current win streak this set (session)
    uint32_t winsAtStart_[2] = {};       // the game's match win counts as the match started
    uint64_t setKey_ = 0;
    uint16_t setGame_ = 0;
    uint8_t setWins_[2] = {};
    std::string setRule_;
    int matchWinner_ = -1;
    std::vector<RoundResult> matchRounds_;
    uint8_t matchRoundWins_[2] = {};
    bool winsKnown_ = false;
    int spectators_ = 0; // guest: as the host says
    uint16_t stageRule_ = lobby::kAnyStage;
    std::atomic<bool> stageLocked_{false}; // the host's stage confirmed (the host's own, or as last heard)
    std::map<uint64_t, Watcher> watchers_; // host; guarded by mutex_
    std::atomic<bool> ready_{false};
    std::atomic<Choice> myChoice_{Choice::None};
    std::atomic<Next> next_{Next::None};
    std::atomic<bool> matchLive_{false}; // the game thread drives a match (frame driver set)
    std::atomic<int> peerReports_{-1};   // the other side sends desync reports: 1 yes, 0 no, -1 not heard
    std::atomic<uint64_t> liveMatch_{0}; // ... this one (matchId_ moves on to the next before it is let go)
    bool joinedRoom_ = false;
    std::atomic<bool> serverLost_{false};
    Clock::time_point bothReadyAt_{};
    Clock::time_point helloSince_{};
    Clock::time_point connectingSince_{};

    Clock::time_point heard_ = Clock::now();
    uint32_t heardTime_ = 0;
    Clock::time_point heardTimeAt_{};
    std::atomic<unsigned> linkPingMs_{0};
    std::vector<uint16_t> linkRtts_;                  // under mutex_: the last core::kLinkSamples round trips, oldest first
    NetView::Path linkPath_ = NetView::Path::Unknown; // under mutex_
    int roundCallFrame_ = -1;                         // game thread: the session frame the round call began, -1 outside it
    bool loweredEarly_ = false;                       // ... and the delay is already back to the player's
    std::string relayHost_;                           // under mutex_

    std::thread thread_;
    const PickTables pickTables_ = BattleSetupPickTables();
    std::thread jump_;
    std::atomic<bool> stop_{false};
    Clock::time_point lastBeat_;

    int creationsAtJump_ = 0;
    bool warmedUp_ = false;
    int frozenFrames_ = 0;
    bool objectsReady_ = false;
    MatchSetup loadedSetup_; // what this side's game loaded
    static constexpr int kStartProbeFrames = 3;
    MatchTicket ticket_;            // what brought this player here (core/match_ticket.h)
    Clock::time_point decidedAt_{}; // when the last match was decided
    uint32_t frozenChecksum_ = 0;
    MatchSetup peerLoadedSetup_; // guarded by mutex_
    uint32_t peerChecksum_ = 0;
    std::atomic<bool> byePending_{false};
    std::string byeText_;       // the Bye's reason, for the other side (mutex_)
    bool forfeitOnBye_ = false; // the Bye gives up the table set (mutex_)
    int forfeitSeat_ = -1;      // the seat that left the set, -1 none (mutex_)
    std::atomic<bool> selfLoaded_{false};
    std::atomic<bool> peerLoaded_{false};
    Clock::time_point startsDifferAt_{}; // game thread: when the frozen starts were found to differ
    std::atomic<bool> peerLeft_{false};
    std::atomic<bool> peerLeftRoom_{false}; // the server said so while the link held (the match went on)
    uint64_t switchesAtJump_ = 0;
    bool leftMidJump_ = false; // Stop found the jump under way and set up the menu return
    std::atomic<bool> sessionSyncing_{false};

    std::unique_ptr<NetplaySession> session_;
    int layoutAtStart_ = 0;
    int framesRun_ = 0;
    int waitFrames_ = 0;   // time sync: frames still to wait
    int waitCooldown_ = 0; // time sync: frames before the next wait may start
    int desyncFrame_ = -1;
    std::array<uint8_t, 60> recentRollbacks_{}; // frames rolled back in each of the last 60 frames
    unsigned rollbacksSeen_ = 0;                // the session's rollback count at the last frame
    std::string desyncReport_;                  // guarded by mutex_
    uint8_t predictionWindow_ = 0;
    uint8_t inputDelay_ = 0; // the local delay now (UpdatePredictionWindow)

    struct Pace {
        std::chrono::steady_clock::time_point since{};
        int frame = 0;
        int rendered = 0;
        int waits = 0;
        int focused = 0; // rendered frames with the game's window in front
        float aheadSum = 0;
    } pace_;

    NetView live_; // match figures for View(), guarded by mutex_

    std::map<uint64_t, std::vector<lobby::FrameRecord>> streams_; // per match; guarded by mutex_
    std::vector<uint64_t> streamOrder_;                           // oldest first, at most lobby::kKeptMatches

    struct KeptMatch {
        MatchSetup setup;
        uint32_t checksum = 0;
        bool finished = false;
        int8_t winner = -1;
    };

    std::map<uint64_t, KeptMatch> kept_; // guarded by mutex_
    Clock::time_point matchListSentAt_{};
    std::vector<lobby::MatchEntry> matchList_; // guarded by mutex_
    std::vector<lobby::MatchEntry> sliceList_; // the host's list as its slices come in (mutex_)
    bool watchedFirst_ = false;                // watchFromFirst done (a table watch starts at game 1)
    std::atomic<uint64_t> lastWatched_{0};
    std::atomic<bool> switchNow_{false};
    std::atomic<uint64_t> pickedMatch_{0};
    std::atomic<bool> pickWaits_{false};
    int streamed_ = 0; // game thread
    std::unique_ptr<SpectatorPlayback> spectate_;
    std::vector<lobby::Message> inbound_; // guarded by mutex_
    std::atomic<int> specWanted_{0};
    std::atomic<int> specPlayed_{0}; // the next frame its playback shows (InputsAck.played)
    std::atomic<uint64_t> watchingMatch_{0};

    void KeepFrozenState();

    void SaveDifferentStart(SfvGame& game, uint32_t peerChecksum);
};

} // namespace sfv::mod::netplay
