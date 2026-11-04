#pragma once
// Netplay sessions with match codes.
//
//   host                 opens a room on the Weyvelength server (or, for
//                        testing, a UDP port) and shows its code (panel,
//                        clipboard)
//   join <code>          joins the room behind the code as the other player
//                        (or as a spectator when both seats are taken)
//   spectate <code>      joins it to watch
//
// A session is any number of matches between the same two players. In its
// lobby each player picks a character (shown to the other), the host's rules
// apply, and a match starts once both are ready and both games have both
// characters loaded. For each match both sides jump into a local Versus
// battle with the host's match setup (battle_setup.h), freeze it at frame 0
// until both have it loaded, and let a GekkoNet session drive it
// (battle_driver.h). When the match is decided each player chooses Rematch
// (straight into the next battle) or Character select (back to the lobby);
// leaving ends the session for both. Spectators load the same battles and
// play the host's confirmed inputs (core/spectate.h). Desync reports and the
// match recordings go to the report folder.

#include "core/game.h"
#include "core/input_delay.h"
#include "core/lobby.h"
#include "core/match_ticket.h"
#include "core/mux.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sfv::mod {

class SfvGame;

// The public rooms server (the sfvr-server repository): where a fresh
// install hosts and joins unless [weyve] server or the panel's Debug tab
// says otherwise.
inline constexpr const char* kDefaultServer = "159.69.32.141:5555";
// The public server answers on 443 too, for networks that block 5555: tried
// when 5555 does not answer within a moment, for kDefaultServer.
inline constexpr uint16_t kDefaultServerAlsoPort = 443;

struct NetplayOptions {
    bool udp = false;                      // the testing transport (U- codes) instead of Weyvelength rooms
    std::string server = kDefaultServer;    // Weyvelength server, host[:port]
    // The server the panel chose (its Debug tab, or its saved settings), the
    // one its settings file keeps; empty: none. A `server` command (test
    // scripts) changes `server` for this run only, not this.
    std::string savedServer;
    uint16_t serverAlsoPort = 0;            // testing: the server's host on this port too (0: 443 for kDefaultServer)
    std::vector<std::string> recentServers; // the Debug tab's list, newest first
    std::string keeperKey;                  // [rooms] keeper_key: another trusted keeper key (rooms_client.h; local tests)
    std::string name;                      // display name; empty: the Steam persona name
    uint16_t udpPort = 47001;              // UDP: the host listens here; a joiner binds any port
    std::string udpHost = "127.0.0.1";     // UDP: address the host writes into its code
    uint8_t inputDelay = 2;
    // SFVR settings' Input delay: kDelayAuto (each session starts at the
    // suggestion for its link, core/input_delay.h), 0-8 that many frames
    // every session (user, 2026-11-04). Either can still be changed in a
    // session.
    int8_t delaySetting = -1;
    uint8_t predictionWindow = 8;
    bool lockstepPresentation = true;      // no prediction (so no rollbacks) outside the fight (flow.h)
    bool lockstepDelay = true;             // there, a local input delay that covers the one-way trip
    bool roundEndRollback = true;          // a round's end may be rolled back (flow.h), instead of held
    bool hasPick = false;                  // this player's own character choice
    PlayerSetup pick;
    uint16_t stage = 0xFFFF;               // host: stage table index (flow.h); 0xFFFF keeps the prepared one
    std::string stageCode;                 // host: as configured; turned into `stage` when a match starts
    bool randomSetup = false;              // host: random characters, settings and stage (testing)
    bool randomCostumes = false;           // with randomSetup: costumes other than the default too
    NetConditions conditions;              // simulated latency/jitter/loss on what this side sends
    uint64_t mashSeed = 0;                 // testing: the local player mashes random inputs
    int mashFrom = 0;                      // testing: ... from this session frame on
    int dumpEvery = 600;                   // full state dumps in match recordings: every N confirmed frames
    int maxDumps = 24;                     // ... at most this many
    bool fullRecordings = false;           // recordings keep every frame's region hashes, not only around desyncs (testing)
    bool skipSaves = true;                 // saves that can never be loaded skip their copy (netplay.h)
    bool skipLoadCallbacks = true;         // loads leave out the costliest game callbacks (DefaultSkippedLoadCallbacks)
    bool verifyLoads = true;               // each load checked against its save's checksum (netplay.h)
    bool menuAfterMatch = true;            // back to the main menu when a session ends or on leave
    bool hotkeys = false;                  // F7 hosts, F8 joins (testing; capture tools bind F8)
    bool reconcileStart = true;            // joiner takes the host's values where the frozen states differ (lobby.h)
    bool allowSpectators = true;           // host: others may watch
    bool uploadReports = true;             // desync reports go to the SFVR server (report_upload.h)
    bool autoReady = false;                // ready as soon as the lobby is reached (commands, tests)
    bool showOwnFps = false;               // the match HUD also shows this game's frame rate
};

void InstallNetplay(SfvGame* game, const std::wstring& reportDir, const std::string& modVersion,
                    const NetplayOptions& defaults);
// The options the next host/join uses; commands change them.
NetplayOptions& NetOptions();
// The name others see: [netplay] name, else the Steam persona name ("" without Steam).
std::string LocalPlayerName();
// With [netplay] hotkeys=1: F7 hosts, F8 joins with the code on the clipboard
// (game window focused, no session running).
void NetPollHotkeys();
// Into a match with a ticket (core/match_ticket.h): who hosts, the seat
// token, the rules and what follows a match. NetHost and NetJoin make
// Direct Match's tickets.
void NetPlayTicket(const MatchTicket& ticket, const NetplayOptions& options);
void NetHost(const NetplayOptions& options);
void NetJoin(const std::string& code, const NetplayOptions& options, bool spectate = false);
void NetLeave();
// In the lobby: ready to start (a player), and this player's pick.
void NetSetReady(bool ready);
void NetSetPick(bool hasPick, const PlayerSetup& pick);
struct NetView;
// The delay a session starts at: the setting's, or with Auto the suggestion
// for this link (NetView::SuggestedDelay; -1 until it is measured).
int PreferredDelay(const NetView& v);
// The same where only a ping is known (a room's table card): the setting's, or
// the suggestion with the testers' typical jitter; 1 frame while unmeasured.
int PreferredDelay(unsigned pingMs);
constexpr int8_t kDelayAuto = -1;
constexpr int kMaxDelaySetting = 8;
// Between matches: this player's input delay for the next match; the host's
// stage for the next matches (a stage code, "random", or "" as prepared).
void NetSetDelay(uint8_t frames);
void NetSetStage(const std::string& code);
// The host: the stage is confirmed (the select screen moves on to the
// characters for everyone), or open again. Every new lobby starts open.
void NetSetStageLocked(bool locked);
// Where this player is on the select screen (lobby::Seat::screen, row,
// hover): the others' screens follow it.
void NetSetSelectState(uint8_t screen, uint8_t row, uint32_t hover);
// Spectator: watch this match of the session (0: the newest) from its start.
void NetWatchMatch(uint64_t id);
// After a match: Rematch or CharacterSelect.
void NetChoose(lobby::Choice choice);
// Testing: leaves a running battle for stage select (pause menu).
void NetPark();
void NetSetConditions(const NetConditions& conditions);
std::string NetStatus();

// Netplay as the overlay shows it: one consistent copy, safe from any thread
// (never blocks: while the driver is being replaced it returns the previous
// copy).
struct NetView {
    enum class Phase { Idle, Connecting, Hosting, Joining, Lobby, Jumping, Loading, Running, PostMatch, Ended };
    Phase phase = Phase::Idle;
    bool host = false;
    bool spectator = false;
    bool scripted = false; // started by a command (tests): it keeps the delay the command set
    bool udp = false;
    std::string code;     // the match code (hosting or joining)
    std::string status;   // the status line
    std::string result;   // how the last session ended, or the last failure
    std::string server;   // the Weyvelength server
    // The session: both seats (P1 is the host), whose seat is this one's (-1
    // for a spectator), matches finished, spectators watching.
    lobby::Seat seats[kNumPlayers];
    int you = -1;
    bool setPlay = false; // a table's set (AfterMatch::Set): the next game on its own, no vote
    uint8_t firstTo = 0;  // ... games to win it
    int played = 0;
    int spectators = 0;
    unsigned linkPingMs = 0; // round trip of the session's heartbeat
    // The heartbeat's last 10 seconds (core/input_delay.h): the median round
    // trip and the jitter the delay suggestion goes by.
    core::LinkStats link;
    // The input delay suggested for this link, -1 until enough heartbeats
    // were measured (core::kSuggestSamples, 2 seconds of the lobby).
    int SuggestedDelay() const
    {
        return link.samples >= core::kSuggestSamples ? core::SuggestedDelay(link.pingMs, link.jitterMs) : -1;
    }
    // How the link to the other side (a spectator's: to the host) travels,
    // from its selected ICE candidate pair; the TURN server's name when one
    // was handed out ("turn.cloudflare.com").
    enum class Path { Unknown, Direct, Relayed };
    Path linkPath = Path::Unknown;
    std::string relayHost;
    uint16_t stageRule = 0xFFFF; // the host's: a stage table index, lobby::kRandomStage or kAnyStage
    bool stageLocked = false;    // the host confirmed the stage (NetSetStageLocked)
    bool overResults = false; // the lobby after a match: over its results screen (character select stays there)
    bool haveSetup = false;
    MatchSetup setup;     // what both sides play, once agreed
    // While a match runs (copied by the game thread every frame):
    bool syncing = false; // connected, waiting for the first inputs
    int frame = 0;
    unsigned pingMs = 0;
    float avgPingMs = 0;
    float jitterMs = 0;
    float framesAhead = 0;
    unsigned rollbacks = 0;
    unsigned longestRollback = 0;
    unsigned recentRollback = 0; // the longest in the last second
    int desyncFrame = -1;
    std::string desyncReport; // where the desync report was saved
    int inputDelay = 0;
    int predictionWindow = 0; // the fight's (the lockstep outside the fight uses 0)
    int behind = 0;           // spectator: frames in hand not yet played
    // Spectator: the host's kept matches (oldest first), and the one being
    // watched or loaded (0: none).
    std::vector<lobby::MatchEntry> matches;
    uint64_t watchingMatch = 0;
};
NetView NetViewNow();
// Whether a battle is running and ticking, and its frame counters (diagnostics).
std::string BattleInfo();

} // namespace sfv::mod
