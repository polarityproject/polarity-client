// The unit tests: each Test* function runs its checks; main runs them all
// and counts the failures.

#include "test_support.h"

namespace {

struct Test {
    const char* name;
    void (*fn)();
};

} // namespace

int main() {
    // Unbuffered: a crash keeps what was printed before it (a pipe buffers).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const Test tests[] = {
        {"hashes", TestHashes},
        {"input delay suggestion", TestInputDelaySuggestion},
        {"display names", TestDisplayName},
        {"loader choice", TestLoaderChoice},
        {"ping tiers", TestPingTiers},
        {"ignore mask", TestIgnoreMask},
        {"skipped saves", TestSkippedSaves},
        {"replay file round trip", TestReplayRoundTrip},
        {"xpress huffman decoder", TestXpressHuffmanDecoder},
        {"recording limits", TestRecordingLimits},
        {"compact recording", TestCompactRecording},
        {"clean match", TestCleanMatch},
        {"hash on confirm", TestHashOnConfirm},
        {"setup mismatch", TestSetupMismatch},
        {"state outside snapshot", TestStateOutsideSnapshot},
        {"machine specific value", TestMachineSpecificValue},
        {"effect uses gameplay rng", TestEffectUsesGameplayRng},
        {"stress session", TestStressSession},
        {"match codes", TestMatchCodes},
        {"lobby messages", TestLobbyMessages},
        {"mux and net conditions", TestMuxAndConditions},
        {"wrong build refused", TestWrongBuildIsRefused},
        {"spectator", TestSpectator},
        {"spectator pacing", TestSpectatorPacing},
        {"update info", TestUpdateInfo},
        {"migrate from sfvr", TestMigrateFromSfvr},
        {"update signature", TestUpdateSignature},
        {"texture package", TestTexturePackage},
        {"pak index", TestPakIndex},
        {"key names", TestKeyNames},
        {"player text", TestText},
        {"shown setup", TestShownSetup},
        {"pick check", TestPickCheck},
        {"rules format", TestRulesFormat},
        {"random stage", TestRandomStage},
        {"battle log", TestBattleLog},
        {"match ticket", TestMatchTicket},
        {"keeper connector", TestKeeperConnector},
        {"keeper protocol", TestKeeperProtocol},
        {"spectate relay", TestSpectateRelay},
        {"png", TestPng},
        {"build ids", TestBuildIds},
    };
    for (const auto& t : tests) {
        const int before = g_failures;
        const auto start = std::chrono::steady_clock::now();
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::printf("[ %s ] %s (%lld ms)\n", g_failures == before ? " OK " : "FAIL", t.name,
                    static_cast<long long>(ms));
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
