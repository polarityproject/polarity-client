#pragma once
// What the test files share: the counters, CHECK, the mock-match helpers
// and every test, by file.

#include "core/battle_log.h"
#include "core/build_id.h"
#include "core/compress.h"
#include "core/pak_index.h"
#include "core/display_name.h"
#include "core/input_delay.h"
#include "core/lobby.h"
#include "core/look.h"
#include "core/pick_check.h"
#include "core/bad_pick.h"
#include "core/match_ticket.h"
#include "core/country_flags.h"
#include "core/keeper_protocol.h"
#include "keeper/connector.h"
#include "keeper/sign.h"
#include "core/rules.h"
#include "core/random_stage.h"
#include "core/match_code.h"
#include "core/mux.h"
#include "core/replay_diff.h"
#include "core/json.h"
#include "core/replay_file.h"
#include "core/update_info.h"
#include "core/spectate.h"
#include "core/spectate_relay.h"
#include "core/png.h"
#include "core/state.h"
#include "core/key_names.h"
#include "core/texture_package.h"
#include "core/text.h"
#include "core/verifier.h"
#include "mod/migrate.h"
#include "proxy/loader_choice.h"
#include "testbed/match_runner.h"
#include <cctype>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <string>

using namespace sfv;

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

inline const char* RegionName(const ReplayFile& rf, size_t i) { return rf.regions[i].name.c_str(); }

inline bool HasRegion(const ReplayFile& rf, const std::vector<size_t>& regions, const char* name) {
    for (size_t r : regions) {
        if (rf.regions[r].name == name) return true;
    }
    return false;
}

inline VerifyResult Verify(const ReplayFile& rf, MockBug bug, uint32_t machineValue, int stress) {
    MockGameOptions go;
    go.bug = bug;
    go.machineValue = machineValue;
    MockGame game(go);
    VerifyOptions vo;
    vo.stressDistance = stress;
    return VerifyReplay(game, rf, vo);
}

inline void PrintVerify(const char* label, const VerifyResult& v, const ReplayFile& rf) {
    std::printf("    verify %-22s %s at frame %d (%s)", label, ToString(v.status), v.frame, v.message.c_str());
    for (const auto& m : v.regions) std::printf(" [%s]", RegionName(rf, m.region));
    std::printf("\n");
}

inline int FirstDesync(const MockMatchResult& r) {
    int first = -1;
    for (const auto& list : r.desyncs) {
        for (const auto& d : list) {
            if (first < 0 || d.frame < first) first = d.frame;
        }
    }
    return first;
}

inline void PrintMatch(const MockMatchResult& r) {
    for (int p = 0; p < kNumPlayers; p++) {
        std::printf("    P%d: %d frames, final through %d, %u rollbacks (max %u frames), %u stalls, %d desync events\n",
                    p + 1, r.recording[p].FrameCount(), r.recording[p].finalThrough, r.rollbacks[p].total_count,
                    r.rollbacks[p].max_frames, r.rollbacks[p].total_stalls, r.desyncCount[p]);
    }
}

// ---------------------------------------------------------------------------

// Through the compact format and back.
inline ReplayFile Compacted(const ReplayFile& rf, size_t* size = nullptr) {
    const std::vector<uint8_t> bytes = SerializeReplay(rf, ReplayDetail::Compact);
    if (size) *size = bytes.size();
    ReplayFile back;
    std::string error;
    if (!DeserializeReplay(bytes.data(), bytes.size(), &back, &error)) std::printf("    compact read failed: %s\n", error.c_str());
    return back;
}

// test_recording.cpp
void TestHashes();
void TestBuildIds();
void TestPingTiers();
void TestLoaderChoice();
void TestDisplayName();
void TestInputDelaySuggestion();
void TestIgnoreMask();
void TestSkippedSaves();
void TestBattleLog();
void TestReplayRoundTrip();
void TestHashOnConfirm();
void TestXpressHuffmanDecoder();
void TestRecordingLimits();
void TestCompactRecording();

// test_netplay.cpp
void TestCleanMatch();
void TestSetupMismatch();
void TestStateOutsideSnapshot();
void TestMachineSpecificValue();
void TestEffectUsesGameplayRng();
void TestStressSession();
void TestMatchCodes();
void TestLobbyMessages();
void TestMuxAndConditions();
void TestWrongBuildIsRefused();
void TestSpectator();
void TestSpectatorPacing();

// test_files.cpp
void TestUpdateInfo();
void TestMigrateFromSfvr();
void TestShownSetup();
void TestPickCheck();
void TestRulesFormat();
void TestRandomStage();
void TestMatchTicket();

// test_keeper.cpp
void TestSpectateRelay();
void TestKeeperConnector();
void TestKeeperProtocol();

// test_assets.cpp
void TestPng();
void TestPakIndex();
void TestTexturePackage();
void TestKeyNames();
void TestText();
void TestUpdateSignature();
