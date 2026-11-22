#pragma once
// What the rooms client's files share: the client, its thread's state and timings.

#include "mod/rooms_client.h"

#include "core/lobby.h"
#include "core/match_ticket.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/weyve_service.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sfv::mod::rooms {

using State = RoomsView::State;

constexpr size_t kChatKept = 100;
constexpr int64_t kDesksScreenMs = 2000;
constexpr int64_t kDesksHubMs = 5000;
constexpr int64_t kDesksRoomMs = 10000;
constexpr int64_t kListGapMs = 1100;
constexpr int64_t kListAnswerMs = 3000;
constexpr int64_t kDeskJoinMs = 5000;
constexpr int64_t kUnkeptLobbyMs = 30000;
constexpr int64_t kStaleLobbyMs = 120000;
constexpr int64_t kAnswerWaitMs = 20000;
constexpr int64_t kVerifyMs = 4000;
constexpr int64_t kHandBackEveryMs = 2000;
constexpr int64_t kPeekEveryMs = 3000;
constexpr int64_t kPeekGapMs = 650;
constexpr int64_t kLeftHideMs = 30000;
constexpr int64_t kPingEveryMs = 2000;
constexpr int64_t kDeskDialEveryMs = 1000;
constexpr int64_t kTableMovedOnMs = 3000;
constexpr int64_t kSessionStartMs = 30000;
constexpr int kPingMaxMs = 5000;
constexpr size_t kProofsKept = 256, kSignaturesKept = 1024;

std::string MyVersion();
std::string OtherVersionNotice(const std::string& theirs);
int64_t ClockUs();
int64_t NowMs();

inline std::atomic<bool> g_wordFilter{true};

struct Client {
    std::mutex mutex;

    struct Shared {
        RoomsView view;
        std::optional<keeper::RoomSettings> createAsk;
        int64_t createAskAt = 0;
        std::optional<std::string> joinAsk;
        bool leaveAsk = false;
        std::string leftRoom;
        int64_t leftAtMs = 0;
        std::vector<std::string> chatOut;
        keeper::ChatLimiter limiter;
        std::string peekWanted;
        std::vector<std::pair<std::string, std::vector<std::string>>> tableAsks;
        std::vector<keeper::TableTicket> tickets;
        int watchTable = -1;
        std::string watchCode;
        std::vector<WatchTicket> watchTickets;
        bool watchSession = false;

        struct Playing {
            int table = -1;
            bool host = false;
            Rules rules;
            int64_t at = 0;
            bool started = false, codeSent = false;
            bool begun = false;
            int sent[2] = {-1, -1};
            uint16_t wins[2] = {0, 0};
            bool againAsked = false;
        } playing;
    } shared;

    std::unique_ptr<WeyveService> svc;
    std::vector<std::array<uint8_t, 64>> keys;
    std::string chara;
    std::thread thread;
    std::atomic<bool> stop{false};
    uint32_t seq = 0;
    uint32_t pendingSeq = 0;
    int64_t pendingUntil = 0;
    std::string target;
    int64_t joinedAt = -1;

    enum class Listed { None,
                        Desks,
                        Rooms,
                        AllBuilds } listed = Listed::None;
    int64_t listedAt = -kListGapMs;
    int64_t nextDesksMs = 0;
    uint64_t roomsRev = 0;
    bool roomsWanted = false;
    std::map<std::string, bool> signatures;
    bool wantDesk = false;
    bool deskJoining = false;
    int64_t deskJoinAt = 0;
    int64_t joinAgainAt = 0; // a join the server's cooldown refused, sent again then (0 = none)
    int64_t lastHandMs = -kHandBackEveryMs;
    std::map<std::string, bool> proofs;
    std::map<uint32_t, std::string> names;
    uint32_t lastOwner = 0;
    uint32_t peekSeq = 0;
    std::string peekCode;
    int64_t peekAskedMs = -kPeekEveryMs;
    std::vector<std::string> peekParts;
    std::string peekLogged;
    static constexpr size_t kPingSamples = 4;
    std::map<uint32_t, std::deque<int>> pings;
    std::set<uint32_t> pingLogged;
    int64_t lastPingMs = 0;
    int64_t deskDialAt = 0, deskDialFrom = 0;
    std::string net = "u";
    std::string skills;
    std::map<uint32_t, int> mask;
    std::string wordsHash;
    std::vector<std::string> words;
    bool toldMasked = false;
    uint64_t lastTicketToken = 0;
    bool saidNoLobby = false;
    int64_t lobbyUnkeptSince = -1;
    std::map<std::string, int64_t> staleLobbies;
    int64_t allListAt = -1;
    bool abortSession = false;
    std::vector<std::pair<uint32_t, int64_t>> callSeen;
    std::vector<std::string> tableLogged;

    void Set(State s);
    bool KeeperSigned(const std::string& message, const std::string& signature) const;
    bool Proven(const std::string& room, uint32_t id, const std::string& proof);
    uint32_t KeeperOf(const WeyveRoomView& v);
    bool Signed(const std::string& message, const keeper::Listing& listing);

    void ToBrowsing();
    void Listings();
    void OnDesks();
    void OnRooms();
    void Peeks();

    void ForgetTables(bool removed = false);
    void BackToLobby(RoomsView::Notice kind, const std::string& notice, bool inRoom, bool removed = false);
    void SetNotice(RoomsView::Notice kind, const std::string& notice);
    void Line(ChatLine::Kind kind, uint32_t from, const std::string& shownName, const std::string& text, const std::string& realName = {});
    std::string MaskedName(uint32_t id, const std::string& name) const;
    void OnEvent(const WeyveEvent& e, const WeyveRoomView& v);
    void Answers();
    void Mirror(const WeyveRoomView& v);

    void DeskLink(uint32_t keeperId);
    void Pings(const WeyveRoomView& v, uint32_t keeperId);
    void TableAsks();
    void Watch();
    void Report();
    void Asks(const WeyveRoomView& v);

    void Run();
};

inline std::mutex g_clientMutex;
inline std::unique_ptr<Client> g_client;

template <typename F>
bool WithClient(F f) {
    std::lock_guard lock(g_clientMutex);
    if (!g_client) return false;
    std::lock_guard inner(g_client->mutex);
    return f(*g_client);
}

} // namespace sfv::mod::rooms
