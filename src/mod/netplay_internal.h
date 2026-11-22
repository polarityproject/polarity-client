#pragma once
// What the netplay driver's files share: its class, the lobby, the session's course, the jump and the match (docs/netplay.md).

#include "mod/netplay_driver.h"

#include "mod/battle_data.h"
#include "mod/benchmark.h"
#include "mod/entitlements.h"
#include "core/display_name.h"
#include "core/text.h"
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
constexpr int kWarmupLogEvery = 300;
constexpr long long kSlowLinkQueryUs = 2000;
constexpr int64_t kMaxRttMs = 10000;
constexpr int kPaceLogFrames = 600;
constexpr int kDelayUnchanged = -1, kStageRuleFromOptions = -1;
enum class PeerReports { NotHeard,
                         Sends,
                         DoesNotSend };
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

struct Said {
    std::string english, shown;
    Said() = default;

    explicit Said(const std::string& received) : english(received), shown(received) {}

    bool empty() const { return english.empty(); }
};

Said Say(Str key, ...);

inline std::mutex g_resultMutex;
inline std::string g_lastResult;

void SetResult(const Said& text);

std::string LastResult();

std::string Hex64(uint64_t v);

void CopyToClipboard(const std::string& text);

std::string SteamPersonaName();

void PressPad(int pad, uint16_t buttons, int holdMs = 90, int gapMs = 600);

bool LeaveBattleForStageSelect(SfvGame& game, const std::atomic<bool>& stop);

std::string SetupMismatch(const MatchSetup& a, const MatchSetup& b);

Str RejectKey(lobby::RejectReason r);

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

    void EndSessionFor(const Said& why, const std::string& theirs);

    static bool LacksVersion(const MatchSetup& s, Said* here, std::string* there);

    std::string SetScoreText();

    int SetWinnerLocked();

    bool LeavingMidSet();

    bool TookForfeit(int seat);

    Said SetOverText();

    void EndSession(const Said& why);

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

    static constexpr int kRoundCallFrames = 196;
    bool loggedInFight_ = false;
    bool sawFight_ = false;
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

    void SetStatus(const Said& text);

    SfvGame& game_;
    std::wstring reportDir_;
    std::string modVersion_;
    std::mt19937_64 rng_;
    const Clock::time_point epoch_;
    NetplayOptions options_;
    bool host_ = false;
    std::atomic<Role> role_{Role::Player};
    std::string name_;
    lobby::Link link_ = lobby::Link::Unknown;

    std::unique_ptr<ITransport> base_;
    WeyveService* weyve_ = nullptr;
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
    std::atomic<uint64_t> peer_{0};
    uint16_t nonce_ = 0;
    std::atomic<uint64_t> matchId_{0};
    std::atomic<bool> holdOverResults_{false};
    std::atomic<int> nextDelay_{kDelayUnchanged};
    std::atomic<bool> lockedIn_{false};
    uint32_t selectHover_ = 0;
    std::atomic<bool> beatNow_{false};
    std::atomic<bool> againOffered_{false};
    std::atomic<Again> again_{Again::None};
    Clock::time_point againAskedAt_{};
    Rules againRules_;
    bool againSelect_ = false;
    std::atomic<int> hostStageRule_{kStageRuleFromOptions};
    uint16_t streaks_[kNumPlayers] = {};
    uint32_t winsAtStart_[2] = {};
    uint64_t setKey_ = 0;
    uint16_t setGame_ = 0;
    uint8_t setWins_[2] = {};
    std::string setRule_;
    int matchWinner_ = -1;
    std::vector<RoundResult> matchRounds_;
    uint8_t matchRoundWins_[2] = {};
    bool winsKnown_ = false;
    int spectators_ = 0;
    uint16_t stageRule_ = lobby::kAnyStage;
    std::atomic<bool> stageLocked_{false};
    std::atomic<bool> ready_{false};
    std::atomic<Choice> myChoice_{Choice::None};
    std::atomic<Next> next_{Next::None};
    std::atomic<bool> matchLive_{false};
    std::atomic<PeerReports> peerReports_{PeerReports::NotHeard};
    std::atomic<uint64_t> liveMatch_{0};
    bool joinedRoom_ = false;
    std::atomic<bool> serverLost_{false};
    Clock::time_point bothReadyAt_{};
    Clock::time_point helloSince_{};
    Clock::time_point connectingSince_{};

    Clock::time_point heard_ = Clock::now();
    uint32_t heardTime_ = 0;
    Clock::time_point heardTimeAt_{};
    std::atomic<unsigned> linkPingMs_{0};
    int roundCallFrame_ = -1;
    bool loweredEarly_ = false;

    std::thread thread_;
    const PickTables pickTables_ = BattleSetupPickTables();
    std::thread jump_;
    std::atomic<bool> stop_{false};
    Clock::time_point lastBeat_;

    int creationsAtJump_ = 0;
    bool warmedUp_ = false;
    int frozenFrames_ = 0;
    bool objectsReady_ = false;
    MatchSetup loadedSetup_;
    static constexpr int kStartProbeFrames = 3;
    MatchTicket ticket_;
    Clock::time_point decidedAt_{};
    uint32_t frozenChecksum_ = 0;
    uint32_t peerChecksum_ = 0;
    std::atomic<bool> byePending_{false};
    std::atomic<bool> selfLoaded_{false};
    std::atomic<bool> peerLoaded_{false};
    Clock::time_point startsDifferAt_{};
    std::atomic<bool> peerLeft_{false};
    std::atomic<bool> peerLeftRoom_{false};
    uint64_t switchesAtJump_ = 0;
    bool leftMidJump_ = false;
    std::atomic<bool> sessionSyncing_{false};

    std::unique_ptr<NetplaySession> session_;
    int layoutAtStart_ = 0;
    int framesRun_ = 0;
    int waitFrames_ = 0;
    int waitCooldown_ = 0;
    int desyncFrame_ = -1;
    std::array<uint8_t, 60> recentRollbacks_{};
    unsigned rollbacksSeen_ = 0;
    uint8_t predictionWindow_ = 0;
    uint8_t inputDelay_ = 0;

    struct Pace {
        std::chrono::steady_clock::time_point since{};
        int frame = 0;
        int rendered = 0;
        int waits = 0;
        int focused = 0;
        float aheadSum = 0;
    } pace_;

    std::vector<uint64_t> streamOrder_;

    struct KeptMatch {
        MatchSetup setup;
        uint32_t checksum = 0;
        bool finished = false;
        int8_t winner = -1;
    };

    Clock::time_point matchListSentAt_{};
    bool watchedFirst_ = false;
    std::atomic<uint64_t> lastWatched_{0};
    std::atomic<bool> switchNow_{false};
    std::atomic<uint64_t> pickedMatch_{0};
    std::atomic<bool> pickWaits_{false};
    int streamed_ = 0;
    std::unique_ptr<SpectatorPlayback> spectate_;
    std::atomic<int> specWanted_{0};
    std::atomic<int> specPlayed_{0};
    std::atomic<uint64_t> watchingMatch_{0};

    struct Shared {
        MatchSetup setup;
        lobby::Seat seats[kNumPlayers];
        int played = 0;
        uint8_t selectScreen = 0, selectRow = 0;
        std::map<uint64_t, Watcher> watchers;
        std::vector<uint16_t> linkRtts;
        NetView::Path linkPath = NetView::Path::Unknown;
        std::string relayHost;
        MatchSetup peerLoadedSetup;
        std::string byeText;
        bool forfeitOnBye = false;
        int forfeitSeat = -1;
        std::string desyncReport;
        NetView live;
        std::map<uint64_t, std::vector<lobby::FrameRecord>> streams;
        std::map<uint64_t, KeptMatch> kept;
        std::vector<lobby::MatchEntry> matchList;
        std::vector<lobby::MatchEntry> sliceList;
        std::vector<lobby::Message> inbound;
    } shared_;

    void KeepFrozenState();

    void SaveDifferentStart(SfvGame& game, uint32_t peerChecksum);
};

} // namespace sfv::mod::netplay
