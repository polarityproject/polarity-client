// End-to-end tests of the netplay driver, the .sfvr recorder and the desync
// tooling, using MockGame over the loopback network.

#include "core/input_delay.h"
#include "core/lobby.h"
#include "core/look.h"
#include "core/match_ticket.h"
#include "core/keeper_protocol.h"
#include "keeper/sign.h"
#include "core/rules.h"
#include "core/match_code.h"
#include "core/mux.h"
#include "core/replay_diff.h"
#include "core/json.h"
#include "core/replay_file.h"
#include "core/update_info.h"
#include "core/spectate.h"
#include "core/state.h"
#include "core/texture_package.h"
#include "core/verifier.h"
#include "testbed/match_runner.h"

#include <cctype>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <random>

#include <windows.h>

#include <bcrypt.h>
#include <string>

using namespace sfv;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        g_checks++;                                                                                 \
        if (!(cond)) {                                                                              \
            g_failures++;                                                                           \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
        }                                                                                           \
    } while (0)

const char* RegionName(const ReplayFile& rf, size_t i) { return rf.regions[i].name.c_str(); }

bool HasRegion(const ReplayFile& rf, const std::vector<size_t>& regions, const char* name)
{
    for (size_t r : regions) {
        if (rf.regions[r].name == name) return true;
    }
    return false;
}

VerifyResult Verify(const ReplayFile& rf, MockBug bug, uint32_t machineValue, int stress)
{
    MockGameOptions go;
    go.bug = bug;
    go.machineValue = machineValue;
    MockGame game(go);
    VerifyOptions vo;
    vo.stressDistance = stress;
    return VerifyReplay(game, rf, vo);
}

void PrintVerify(const char* label, const VerifyResult& v, const ReplayFile& rf)
{
    std::printf("    verify %-22s %s at frame %d (%s)", label, ToString(v.status), v.frame, v.message.c_str());
    for (const auto& m : v.regions) std::printf(" [%s]", RegionName(rf, m.region));
    std::printf("\n");
}

int FirstDesync(const MockMatchResult& r)
{
    int first = -1;
    for (const auto& list : r.desyncs) {
        for (const auto& d : list) {
            if (first < 0 || d.frame < first) first = d.frame;
        }
    }
    return first;
}

void PrintMatch(const MockMatchResult& r)
{
    for (int p = 0; p < kNumPlayers; p++) {
        std::printf("    P%d: %d frames, final through %d, %u rollbacks (max %u frames), %u stalls, %d desync events\n",
                    p + 1, r.recording[p].FrameCount(), r.recording[p].finalThrough, r.rollbacks[p].total_count,
                    r.rollbacks[p].max_frames, r.rollbacks[p].total_stalls, r.desyncCount[p]);
    }
}

// ---------------------------------------------------------------------------

void TestHashes()
{
    CHECK(Hash64("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(Hash64("abc", 3) == 0x44BC2CF5AD770999ull);
    const char* longer = "Nobody inspects the spammish repetition";
    CHECK(Hash64(longer, std::strlen(longer)) == 0xFBCEA83C8A378BF1ull);
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);
}

void TestInputDelaySuggestion()
{
    using core::SuggestedDelay;
    // Steady link (no jitter): 0 frames up to 66 ms, then a frame per 33 ms, 3 at most.
    CHECK(SuggestedDelay(0, 0) == 0);
    CHECK(SuggestedDelay(66, 0) == 0);
    CHECK(SuggestedDelay(67, 0) == 1);
    CHECK(SuggestedDelay(100, 0) == 1);
    CHECK(SuggestedDelay(101, 0) == 2);
    CHECK(SuggestedDelay(133, 0) == 2);
    CHECK(SuggestedDelay(134, 0) == 3);
    CHECK(SuggestedDelay(400, 0) == 3);
    // Typical jitter (8 ms) moves each step 16 ms lower.
    CHECK(SuggestedDelay(50, 8) == 0);
    CHECK(SuggestedDelay(51, 8) == 1);
    CHECK(SuggestedDelay(84, 8) == 1);
    CHECK(SuggestedDelay(85, 8) == 2);
    CHECK(SuggestedDelay(118, 8) == 3);
    // Jitter counts one frame (17 ms) at most.
    CHECK(SuggestedDelay(32, 17) == 0);
    CHECK(SuggestedDelay(33, 17) == 1);
    CHECK(SuggestedDelay(32, 90) == 0);

    std::vector<uint16_t> rtts{40, 44, 38, 42, 40};
    core::LinkStats s = core::MeasureLink(rtts);
    CHECK(s.samples == 5);
    CHECK(s.pingMs == 40);
    CHECK(s.jitterMs == 4); // steps 4, 6, 4, 2
    rtts = {40, 41};
    CHECK(core::MeasureLink(rtts).pingMs == 41); // even count: the middle two, rounded up
    // A lone spike: each of its two steps counts 50 ms at most.
    rtts = {40, 40, 400, 40, 40, 40, 40, 40, 40};
    s = core::MeasureLink(rtts);
    CHECK(s.pingMs == 40);
    CHECK(s.jitterMs == 13); // (50 + 50) / 8
    CHECK(core::MeasureLink({}).samples == 0);
}

void TestIgnoreMask()
{
    uint8_t mem[24] = {};
    StateRegion r{"r", mem, sizeof(mem), {0x02}}; // ignore bytes 8..15
    std::vector<StateRegion> regions{r};
    uint64_t before, after;
    HashRegions(regions, &before);
    mem[9] = 0xFF; // inside the masked word: same hash
    HashRegions(regions, &after);
    CHECK(before == after);
    mem[16] = 0xFF; // outside: different hash
    HashRegions(regions, &after);
    CHECK(before != after);

    std::vector<uint8_t> a(24, 0), b(24, 0);
    b[9] = 1;
    b[20] = 1;
    const auto ranges = DiffStates({{"r", 24, {0x02}}}, a, b);
    CHECK(ranges.size() == 1 && ranges[0].offset == 16 && ranges[0].length == 8); // whole words

    // The masked hash is the hash of the region with its masked words zeroed,
    // for any size (a partial last word too) and any mask, longer ones included.
    std::mt19937 rng(7);
    bool same = true;
    for (int round = 0; round < 200; round++) {
        const size_t size = 1 + rng() % 700;
        std::vector<uint8_t> data(size);
        for (auto& v : data) v = static_cast<uint8_t>(rng());
        std::vector<uint8_t> ignore(rng() % ((size + 63) / 64 + 2));
        for (auto& v : ignore) v = static_cast<uint8_t>(rng() % 3 == 0 ? 0 : rng());
        std::vector<uint8_t> zeroed = data;
        for (size_t w = 0; w * 8 < size; w++) {
            if (IsIgnored(ignore, w * 8)) std::memset(&zeroed[w * 8], 0, std::min<size_t>(8, size - w * 8));
        }
        uint64_t got = 0;
        HashRegions({{"r", data.data(), size, ignore}}, &got);
        same = same && got == Hash64(zeroed.data(), size);
    }
    CHECK(same);
}

// A delay that covers the latency: most saves can never be loaded and skip
// their copy. The match stays in sync and every load (jitter still causes
// some) restores its saved state; with skipping off nothing is skipped.
void TestSkippedSaves()
{
    MockMatchOptions mo;
    mo.seed = 12;
    mo.link = {2, 1, 0.0};
    mo.inputDelay = 4;
    const auto r = RunMockMatch(mo);
    CHECK(r.started);
    CHECK(FirstDesync(r) < 0);
    for (int p = 0; p < kNumPlayers; p++) {
        const auto& s = r.saves[p];
        std::printf("  P%d: %d saves, %d skipped, %d loads, %d mismatches\n", p + 1, s.saves, s.skipped, s.loads,
                    s.loadMismatches);
        // (the mock has no time sync: the side ahead has fewer frames confirmed)
        CHECK(s.saves > 2500 && s.skipped > s.saves / 3);
        CHECK(s.loadMismatches == 0);
    }
    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    CHECK(d.comparable && d.firstInputMismatch < 0 && d.firstStateMismatch < 0);

    // No delay: the newest frames are always predicted, so few saves skip.
    MockMatchOptions zero = mo;
    zero.inputDelay = 0;
    const auto z = RunMockMatch(zero);
    CHECK(FirstDesync(z) < 0);
    for (int p = 0; p < kNumPlayers; p++) {
        CHECK(z.saves[p].skipped < z.saves[p].saves / 2 && z.saves[p].loadMismatches == 0);
    }
}

void TestReplayRoundTrip()
{
    ReplayFile rf;
    rf.modVersion = "0.1.0";
    rf.gameBuild = "mockfighter-1";
    rf.matchId = 42;
    rf.localPlayer = 1;
    rf.inputDelay = 2;
    rf.predictionWindow = 8;
    rf.setup.players[0].character = 7;
    rf.setup.players[0].version = 60;
    rf.setup.players[1].version = 61;
    rf.setup.stage = 3;
    rf.setup.seed = 0x1234567890ull;
    rf.regions = {{"a", 16, {0x02}}, {"b", 4, {}}, {"c", 1000, {}}};
    rf.initialHashes = {1, 2, 3};
    for (int f = 0; f < 500; f++) {
        // Region b changes every frame, c every 100: exercises the delta coding.
        const uint64_t h[3] = {1, 1000u + f, 3u + f / 100};
        rf.SetFrame(f, FrameInputs{{static_cast<PackedInput>(f), static_cast<PackedInput>(f * 3)}}, h);
    }
    rf.finalThrough = 490;
    rf.events.push_back({100, ReplayEventType::Rollback, 4, 0});
    rf.events.push_back({200, ReplayEventType::Desync, 0xAAAA, 0xBBBB});
    rf.dumps.push_back({200, "desync", std::vector<uint8_t>(1020, 0x5A)});

    const std::vector<uint8_t> bytes = SerializeReplay(rf);
    ReplayFile back;
    std::string error;
    CHECK(DeserializeReplay(bytes.data(), bytes.size(), &back, &error));
    CHECK(back.modVersion == rf.modVersion && back.gameBuild == rf.gameBuild);
    CHECK(back.matchId == 42 && back.localPlayer == 1 && back.finalThrough == 490);
    CHECK(back.setup == rf.setup);
    CHECK(back.regions == rf.regions);
    CHECK(back.inputs == rf.inputs);
    CHECK(back.hashes == rf.hashes);
    CHECK(back.initialHashes == rf.initialHashes);
    CHECK(back.events.size() == 2 && back.events[1].a == 0xAAAA && back.events[1].type == ReplayEventType::Desync);
    CHECK(back.dumps.size() == 1 && back.dumps[0].bytes == rf.dumps[0].bytes);
    // 500 frames x 3 regions would be 12000 bytes of hashes without delta coding.
    std::printf("    %zu bytes serialized\n", bytes.size());

    std::vector<uint8_t> corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x01;
    CHECK(!DeserializeReplay(corrupt.data(), corrupt.size(), &back, &error));
    CHECK(error.find("corrupt") != std::string::npos || error.find("malformed") != std::string::npos);

    CHECK(!DeserializeReplay(bytes.data(), bytes.size() - 6, &back, &error));
}

// Through the compact format and back.
ReplayFile Compacted(const ReplayFile& rf, size_t* size = nullptr)
{
    const std::vector<uint8_t> bytes = SerializeReplay(rf, ReplayDetail::Compact);
    if (size) *size = bytes.size();
    ReplayFile back;
    std::string error;
    if (!DeserializeReplay(bytes.data(), bytes.size(), &back, &error)) std::printf("    compact read failed: %s\n", error.c_str());
    return back;
}

void TestCompactRecording()
{
    ReplayFile rf;
    rf.regions = {{"a", 16, {}}, {"b", 4, {}}, {"c", 1000, {}}};
    rf.initialHashes = {1, 2, 3};
    for (int f = 0; f < 1000; f++) {
        const uint64_t h[3] = {1, 1000u + f, 3u + f / 100};
        rf.SetFrame(f, FrameInputs{{static_cast<PackedInput>(f % 7), 0}}, h);
    }
    rf.finalThrough = 990;
    rf.events.push_back({500, ReplayEventType::Desync, 0xAAAA, 0xBBBB});
    rf.events.push_back({505, ReplayEventType::Desync, 0xCCCC, 0xDDDD});
    // A state dump compresses: game state is mostly zeros and repeats.
    std::vector<uint8_t> state(200000, 0);
    for (size_t i = 0; i < state.size(); i += 64) state[i] = static_cast<uint8_t>(i / 64);
    rf.dumps.push_back({500, "desync", state});

    size_t compactSize = 0;
    const ReplayFile back = Compacted(rf, &compactSize);
    const size_t fullSize = SerializeReplay(rf, ReplayDetail::Full).size();
    std::printf("    full %zu bytes, compact %zu bytes\n", fullSize, compactSize);
    CHECK(compactSize < fullSize && compactSize < 20000);
    CHECK(back.inputs == rf.inputs && back.events.size() == 2 && back.finalThrough == 990);
    CHECK(back.dumps.size() == 1 && back.dumps[0].bytes == state);
    // Region hashes from 120 frames before the first desync to 30 after the last.
    CHECK(!back.HasRegionHashes(379) && back.HasRegionHashes(380) && back.HasRegionHashes(535) && !back.HasRegionHashes(536));
    CHECK(back.HasRegionHashes(-1));
    bool same = true;
    for (int f = 0; f < rf.FrameCount(); f++) {
        same &= back.FrameChecksum(f) == rf.FrameChecksum(f);
        if (back.HasRegionHashes(f)) same &= std::memcmp(back.FrameHashes(f), rf.FrameHashes(f), 3 * 8) == 0;
    }
    CHECK(same);
    // A compact file saved again (by a tool) keeps what it has.
    const std::vector<uint8_t> again = SerializeReplay(back, ReplayDetail::Full);
    ReplayFile twice;
    std::string error;
    CHECK(DeserializeReplay(again.data(), again.size(), &twice, &error));
    CHECK(!twice.HasRegionHashes(379) && twice.HasRegionHashes(400) && twice.FrameChecksum(100) == rf.FrameChecksum(100));
    // A full file reads back like the live recording.
    const std::vector<uint8_t> full = SerializeReplay(rf, ReplayDetail::Full);
    ReplayFile fullBack;
    CHECK(DeserializeReplay(full.data(), full.size(), &fullBack, &error));
    CHECK(fullBack.hashes == rf.hashes && fullBack.hashesKept.empty());
}

void TestCleanMatch()
{
    MockMatchOptions mo;
    mo.seed = 11;
    mo.link = {4, 3, 0.05};
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    CHECK(r.started);
    CHECK(FirstDesync(r) < 0);
    CHECK(r.rollbacks[0].total_count > 0 && r.rollbacks[1].total_count > 0);
    CHECK(r.recording[0].finalThrough > 2500);
    for (int p = 0; p < kNumPlayers; p++) {
        // Every load restored the state its save held.
        CHECK(r.saves[p].loads > 0 && r.saves[p].loadMismatches == 0);
    }

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.comparable && !d.setupDiffers && !d.initialStateDiffers);
    CHECK(d.firstInputMismatch < 0 && d.firstStateMismatch < 0);

    for (int p = 0; p < kNumPlayers; p++) {
        const auto straight = Verify(r.recording[p], MockBug::None, 0, 0);
        PrintVerify(p == 0 ? "P1 straight" : "P2 straight", straight, r.recording[p]);
        CHECK(straight.status == VerifyResult::Status::Match);
        CHECK(straight.framesChecked == r.recording[p].finalThrough + 1);
    }
    const auto stress = Verify(r.recording[0], MockBug::None, 0, 7);
    PrintVerify("P1 stress 7", stress, r.recording[0]);
    CHECK(stress.status == VerifyResult::Status::Match);
    // Without desyncs a compact recording has checksums only, and still verifies.
    const ReplayFile compact = Compacted(r.recording[0]);
    CHECK(!compact.HasRegionHashes(0));
    const auto compactStraight = Verify(compact, MockBug::None, 0, 0);
    CHECK(compactStraight.status == VerifyResult::Status::Match);
    CHECK(DiffReplays(compact, Compacted(r.recording[1])).firstStateMismatch < 0);

    // A report survives the trip through a file.
    const std::string path = "test_clean_p1.sfvr";
    std::string error;
    CHECK(SaveReplay(r.recording[0], path, &error));
    ReplayFile loaded;
    CHECK(LoadReplay(path, &loaded, &error));
    CHECK(loaded.hashes == r.recording[0].hashes && loaded.inputs == r.recording[0].inputs);
    std::remove(path.c_str());

    // And in a folder named outside ASCII (a game or user folder in Chinese):
    // the path goes as UTF-8, opened through the wide API.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"sfvr-测试-Ж";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string dirUtf8(dir.size() * 3, '\0');
    dirUtf8.resize(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, dir.data(), static_cast<int>(dir.size()), dirUtf8.data(),
                                                           static_cast<int>(dirUtf8.size()), nullptr, nullptr)));
    const std::string wide = dirUtf8 + "\\test_clean_p1.sfvr";
    CHECK(SaveReplay(r.recording[0], wide, &error));
    ReplayFile loadedWide;
    CHECK(LoadReplay(wide, &loadedWide, &error));
    CHECK(loadedWide.inputs == r.recording[0].inputs);
    CHECK(SaveReplay(r.recording[0], wide, &error)); // over the existing file
    DeleteFileW((dir + L"\\test_clean_p1.sfvr").c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestSetupMismatch()
{
    MockMatchOptions mo;
    mo.seed = 12;
    mo.renderedFrames = 600;
    mo.setup[1].players[0].character = 3; // P2's machine thinks P1 picked someone else
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    CHECK(FirstDesync(r) >= 0);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.setupDiffers);
    CHECK(d.initialStateDiffers);
}

void TestStateOutsideSnapshot()
{
    MockMatchOptions mo;
    mo.seed = 13;
    mo.link = {5, 3, 0.05};
    mo.bug[0] = mo.bug[1] = MockBug::StateOutsideSnapshot;
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    const int desync = FirstDesync(r);
    CHECK(desync >= 0);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.firstInputMismatch < 0 || d.firstInputMismatch > d.firstStateMismatch);
    CHECK(d.firstStateMismatch >= 0 && d.firstStateMismatch <= desync);
    CHECK(HasRegion(r.recording[0], d.regionsAtMismatch, "fighters"));

    // Neither recording is a straight simulation of its inputs, and resimulating
    // with the reported inputs exposes it.
    for (int p = 0; p < kNumPlayers; p++) {
        const auto v = Verify(r.recording[p], MockBug::StateOutsideSnapshot, 0, 7);
        PrintVerify(p == 0 ? "P1 stress 7" : "P2 stress 7", v, r.recording[p]);
        CHECK(v.status == VerifyResult::Status::Diverged || v.status == VerifyResult::Status::StressMismatch);
    }

    // On a clean recording the straight run passes and only rollback stress
    // finds the bug: the sync-test use of the verifier.
    MockMatchOptions clean = mo;
    clean.link = {0, 0, 0.0};
    const auto c = RunMockMatch(clean);
    CHECK(c.rollbacks[0].total_count == 0 && FirstDesync(c) < 0);
    const auto straight = Verify(c.recording[0], MockBug::StateOutsideSnapshot, 0, 0);
    const auto stress = Verify(c.recording[0], MockBug::StateOutsideSnapshot, 0, 7);
    PrintVerify("no-latency straight", straight, c.recording[0]);
    PrintVerify("no-latency stress 7", stress, c.recording[0]);
    CHECK(straight.status == VerifyResult::Status::Match);
    CHECK(stress.status == VerifyResult::Status::StressMismatch);
    CHECK(HasRegion(c.recording[0], [&] {
        std::vector<size_t> idx;
        for (const auto& m : stress.regions) idx.push_back(m.region);
        return idx;
    }(), "fighters"));
}

void TestMachineSpecificValue()
{
    MockMatchOptions mo;
    mo.seed = 14;
    mo.renderedFrames = 6000;
    mo.bug[0] = mo.bug[1] = MockBug::MachineSpecificValue;
    mo.machineValue[1] = 3; // only P2's machine is off
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    const int desync = FirstDesync(r);
    CHECK(desync >= 0);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.firstInputMismatch < 0 || d.firstInputMismatch > d.firstStateMismatch);
    CHECK(d.firstStateMismatch >= 0);
    CHECK(HasRegion(r.recording[0], d.regionsAtMismatch, "fighters"));
    CHECK(!d.bytesAtMismatch.empty());

    // The developer's machine (machineValue 0) reproduces P1 and not P2: the
    // nondeterminism is on P2's side, starting exactly at the diverging frame.
    const auto v1 = Verify(r.recording[0], MockBug::MachineSpecificValue, 0, 0);
    const auto v2 = Verify(r.recording[1], MockBug::MachineSpecificValue, 0, 0);
    PrintVerify("P1 straight", v1, r.recording[0]);
    PrintVerify("P2 straight", v2, r.recording[1]);
    CHECK(v1.status == VerifyResult::Status::Match);
    CHECK(v2.status == VerifyResult::Status::Diverged);
    CHECK(v2.frame == d.firstStateMismatch);

    // The compact recordings players send tell the same story.
    size_t fullSize = SerializeReplay(r.recording[0]).size(), compactSize = 0;
    const ReplayFile c1 = Compacted(r.recording[0], &compactSize);
    const ReplayFile c2 = Compacted(r.recording[1]);
    std::printf("    P1's recording: full %zu bytes, compact %zu bytes\n", fullSize, compactSize);
    const DiffReport dc = DiffReplays(c1, c2);
    CHECK(dc.firstStateMismatch == d.firstStateMismatch);
    CHECK(HasRegion(c1, dc.regionsAtMismatch, "fighters"));
    const auto cv1 = Verify(c1, MockBug::MachineSpecificValue, 0, 0);
    const auto cv2 = Verify(c2, MockBug::MachineSpecificValue, 0, 0);
    CHECK(cv1.status == VerifyResult::Status::Match && cv1.framesChecked == v1.framesChecked);
    CHECK(cv2.status == VerifyResult::Status::Diverged && cv2.frame == v2.frame);
}

void TestEffectUsesGameplayRng()
{
    MockMatchOptions mo;
    mo.seed = 15;
    mo.link = {5, 3, 0.05};
    mo.bug[0] = mo.bug[1] = MockBug::EffectUsesGameplayRng;
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    CHECK(FirstDesync(r) >= 0);

    CHECK(r.reportsRaised[0] == 1 && r.reportsRaised[1] == 1);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(HasRegion(r.recording[0], d.regionsAtMismatch, "rng"));
    // Health checks arrive reordered under jitter; both peers must still end up
    // dumping the same, earliest desynced frame.
    CHECK(d.dumpFrame == d.firstStateMismatch);
    CHECK(!d.bytesAtMismatch.empty());

    const auto v = Verify(r.recording[0], MockBug::EffectUsesGameplayRng, 0, 7);
    PrintVerify("P1 stress 7", v, r.recording[0]);
    CHECK(v.status != VerifyResult::Status::Match);
}

int RunStress(MockBug bug, int frames)
{
    MockGameOptions go;
    go.bug = bug;
    go.input = [](int poll, int player) { return MasherInput(21, poll, player); };
    MockGame game(go);
    LoopbackNetwork unused(1, {});
    NetplayConfig nc;
    nc.stressCheckDistance = 7;
    NetplaySession session(game, unused.Endpoint(1), nc);
    if (!session.Start(nullptr)) return -2;
    for (int f = 0; f < frames; f++) session.Update();
    std::printf("    stress %d frames: next frame %d, %d desync events\n", frames, session.NextFrame(),
                session.DesyncCount());
    return session.DesyncCount();
}

void TestStressSession()
{
    CHECK(RunStress(MockBug::None, 1200) == 0);
    CHECK(RunStress(MockBug::EffectUsesGameplayRng, 1200) > 0);
    CHECK(RunStress(MockBug::StateOutsideSnapshot, 3000) > 0);
}

void TestMatchCodes()
{
    MatchCode room;
    room.kind = MatchCode::Kind::Room;
    room.room = "HWRH49";
    MatchCode back;
    CHECK(EncodeMatchCode(room) == "HWRH49");
    CHECK(DecodeMatchCode("HWRH49", &back) && back == room);
    CHECK(DecodeMatchCode(" w-hwr h49 ", &back) && back == room);
    CHECK(!DecodeMatchCode("HWRH40", &back)); // 0 is not in the room alphabet
    CHECK(!DecodeMatchCode("HWRH4", &back));
    CHECK(!DecodeMatchCode("HWRH49DD", &back)); // the old 8-character codes

    MatchCode udp{MatchCode::Kind::Udp, "", UdpAddress(0x7F000001, 47001), 0xBEEF};
    const std::string u = EncodeMatchCode(udp);
    CHECK(DecodeMatchCode(u, &back) && back == udp);
    std::printf("    udp code %s\n", u.c_str());

    // Case, dashes and look-alike letters do not matter; a typo is caught.
    std::string sloppy;
    for (char c : u) {
        if (c == '-') continue;
        sloppy += char(std::tolower(static_cast<unsigned char>(c == '0' ? 'O' : c == '1' ? 'I' : c)));
    }
    CHECK(DecodeMatchCode(sloppy, &back) && back == udp);
    std::string typo = u;
    typo[3] = typo[3] == 'A' ? 'B' : 'A';
    CHECK(!DecodeMatchCode(typo, &back));
    CHECK(!DecodeMatchCode("X-1234", &back));
}

void TestLobbyMessages()
{
    lobby::Message hello;
    hello.type = lobby::Type::Hello;
    hello.nonce = 77;
    hello.build = 0x1122334455667788ull;
    hello.role = lobby::Role::Spectator;
    hello.name = "Watcher";
    hello.reports = true;
    hello.seatToken = 0xA1B2C3D4E5F60718ull;
    auto bytes = lobby::Encode(hello);
    auto back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->type == lobby::Type::Hello && back->nonce == 77 && back->build == hello.build &&
          back->role == lobby::Role::Spectator && back->name == "Watcher" && back->reports && back->seatToken == hello.seatToken);

    // Accept: the host's name and whether it sends desync reports.
    lobby::Message accept;
    accept.type = lobby::Type::Accept;
    accept.role = lobby::Role::Player;
    accept.name = "Host";
    for (bool reports : {false, true}) {
        accept.reports = reports;
        bytes = lobby::Encode(accept);
        back = lobby::Decode(bytes.data(), bytes.size());
        CHECK(back && back->type == lobby::Type::Accept && back->role == lobby::Role::Player && back->name == "Host" &&
              back->reports == reports);
    }

    // UTF-8 names: kept whole up to kMaxName bytes, longer ones cut between
    // characters (40 x "あ", 3 bytes each: 32 of them fit in 96 bytes).
    hello.name = "Ćwiek Ölçer 東京 Борис";
    bytes = lobby::Encode(hello);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->name == hello.name);
    hello.name.clear();
    for (int i = 0; i < 40; i++) hello.name += "\xE3\x81\x82";
    bytes = lobby::Encode(hello);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->name.size() == 96 && back->name == hello.name.substr(0, 96));
    hello.name = std::string(95, 'a') + "\xE3\x81\x82"; // the last character would straddle the limit
    bytes = lobby::Encode(hello);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->name == std::string(95, 'a'));

    // A spectator's match list: every kept match whole, in one message under
    // the state chunk size (what stays under the transport's datagram).
    lobby::Message list;
    list.type = lobby::Type::Matches;
    for (size_t i = 0; i < lobby::kKeptMatches; i++) {
        lobby::MatchEntry e;
        e.id = 0x1000 + i;
        e.setup.players[0].character = PackCode("RMK");
        e.setup.players[1].character = PackCode("KEN");
        e.setup.players[1].color = 9;
        e.setup.stage = static_cast<uint16_t>(i);
        e.setup.seed = 0xABCDEF0000 + i;
        e.frames = 9000 + static_cast<uint32_t>(i);
        e.finished = i + 1 < lobby::kKeptMatches;
        e.winner = static_cast<int8_t>(i % 2);
        list.matches.push_back(e);
    }
    bytes = lobby::Encode(list);
    CHECK(bytes.size() < lobby::kStateChunk + 64);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->type == lobby::Type::Matches && back->matches == list.matches);

    // The session's state: seats with names, picks and flags.
    lobby::Message state;
    state.type = lobby::Type::SessionState;
    state.time = 123456;
    state.echo = 777;
    state.echoAge = 12;
    state.seats[0] = {"Host", true, true, {PackCode("Z37"), 2, 14, 1, 0, 5}, true, true, lobby::Choice::Rematch, 3};
    state.seats[1] = {"Joiner", true, false, {}, false, true, lobby::Choice::CharacterSelect, 1, 3, 59, lobby::Link::WiFi};
    state.phase = lobby::SessionPhase::PostMatch;
    state.played = 4;
    state.spectators = 2;
    state.matchId = 0xFEED;
    state.stage = 12;
    state.stageLocked = true;
    state.seats[1].screen = 2;
    state.seats[1].row = 4;
    state.seats[1].hover = 0x4B454E;
    auto sb = lobby::Encode(state);
    back = lobby::Decode(sb.data(), sb.size());
    CHECK(back && back->time == 123456 && back->echo == 777 && back->echoAge == 12);
    CHECK(back && back->seats[0].name == "Host" && back->seats[0].pick == state.seats[0].pick &&
          back->seats[0].ready && back->seats[0].choice == lobby::Choice::Rematch && back->seats[0].wins == 3);
    CHECK(back && back->seats[1].name == "Joiner" && !back->seats[1].hasPick && back->seats[1].preloaded &&
          back->seats[1].choice == lobby::Choice::CharacterSelect);
    // Each player's own input delay, frame rate and link type (the HUD).
    CHECK(back && back->seats[1].inputDelay == 3 && back->seats[1].fps == 59 && back->seats[1].link == lobby::Link::WiFi &&
          back->seats[0].link == lobby::Link::Unknown);
    CHECK(back && back->phase == lobby::SessionPhase::PostMatch && back->played == 4 && back->spectators == 2 &&
          back->matchId == 0xFEED && back->stage == 12 && back->stageLocked);
    CHECK(back && back->seats[1].screen == 2 && back->seats[1].row == 4 && back->seats[1].hover == 0x4B454E &&
          back->seats[0].screen == 0);

    // Spectator inputs: a full batch fits a Weyvelength datagram.
    lobby::Message inputs;
    inputs.type = lobby::Type::Inputs;
    inputs.matchId = 5;
    inputs.first = 600;
    for (size_t i = 0; i < lobby::kInputsPerMessage; i++) {
        inputs.frames.push_back({{{PackedInput(i), PackedInput(i * 3)}}, uint32_t(i * 7)});
    }
    auto ib = lobby::Encode(inputs);
    CHECK(ib.size() + 1 <= 1024);
    back = lobby::Decode(ib.data(), ib.size());
    CHECK(back && back->first == 600 && back->frames.size() == lobby::kInputsPerMessage &&
          back->frames[59].inputs.p[1] == 177 && back->frames[59].checksum == 413);

    lobby::Message welcome;
    welcome.type = lobby::Type::Welcome;
    welcome.matchId = 99;
    welcome.setup.players[0] = {PackCode("RYU"), 0, 2, 0, 1, 0};
    welcome.setup.players[1] = {PackCode("CMY"), 0, 1, 1, 0, 3, 0, 60};
    welcome.setup.stage = 7;
    welcome.setup.seed = 12345;
    bytes = lobby::Encode(welcome);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->matchId == 99 && back->setup == welcome.setup);
    CHECK(UnpackCode(back->setup.players[0].character) == "RYU");
    CHECK(back->setup.players[0].version == -1 && back->setup.players[1].version == 60);

    lobby::Message bye;
    bye.type = lobby::Type::Bye;
    bye.text = "the other player's game lacks Urien's final version (60)";
    bytes = lobby::Encode(bye);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->type == lobby::Type::Bye && back->text == bye.text);

    lobby::Message loaded;
    loaded.type = lobby::Type::Loaded;
    loaded.matchId = 99;
    loaded.stateChecksum = 0xABCD1234;
    loaded.reconciled = true;
    auto lb = lobby::Encode(loaded);
    back = lobby::Decode(lb.data(), lb.size());
    CHECK(back && back->stateChecksum == 0xABCD1234 && back->reconciled);

    // The frozen-state reconciliation messages.
    lobby::Message hashes;
    hashes.type = lobby::Type::StateHashes;
    hashes.matchId = 99;
    hashes.first = 100;
    hashes.total = 536;
    hashes.hashes = {1, 2, 0xFFFFFFFFFFFFFFFFull};
    auto hb = lobby::Encode(hashes);
    back = lobby::Decode(hb.data(), hb.size());
    CHECK(back && back->type == lobby::Type::StateHashes && back->first == 100 && back->total == 536 &&
          back->hashes == hashes.hashes);

    lobby::Message request;
    request.type = lobby::Type::StateRequest;
    request.regions = {3, 400};
    auto rb = lobby::Encode(request);
    back = lobby::Decode(rb.data(), rb.size());
    CHECK(back && back->regions == request.regions);

    lobby::Message data;
    data.type = lobby::Type::StateData;
    data.region = 400;
    data.offset = 1024;
    data.bytes.assign(lobby::kStateChunk, 0x5A);
    auto db = lobby::Encode(data);
    CHECK(db.size() + 1 <= 1024); // with the mux's channel byte
    back = lobby::Decode(db.data(), db.size());
    CHECK(back && back->region == 400 && back->offset == 1024 && back->bytes == data.bytes);
    CHECK(!lobby::Decode(db.data(), db.size() - 1)); // truncated

    bytes[0] ^= 1;
    CHECK(!lobby::Decode(bytes.data(), bytes.size()));
    CHECK(!lobby::Decode(bytes.data(), 5));
}

void TestMuxAndConditions()
{
    LoopbackNetwork net(3, {});
    Mux a(net.Endpoint(1)), b(net.Endpoint(2));
    const uint8_t x[] = {1, 2, 3};
    a.Channel(7).Send(2, x, 3);
    a.Channel(8).Send(2, x, 2);
    net.Tick();
    Packet p;
    CHECK(b.Channel(8).Receive(p) && p.data.size() == 2 && p.from == 1);
    CHECK(b.Channel(7).Receive(p) && p.data.size() == 3 && p.data[2] == 3);
    CHECK(!b.Channel(7).Receive(p));

    // 40 ms of added latency: nothing arrives at once, everything after.
    ConditionedTransport slow(net.Endpoint(3), 9);
    slow.SetConditions({40, 0, 0.0});
    for (int i = 0; i < 10; i++) slow.Send(4, x, 3);
    net.Tick();
    int got = 0;
    while (net.Endpoint(4).Receive(p)) got++;
    CHECK(got == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    slow.Receive(p); // flushes due packets
    net.Tick();
    while (net.Endpoint(4).Receive(p)) got++;
    CHECK(got == 10);
}

void TestWrongBuildIsRefused()
{
    MockMatchOptions mo;
    mo.seed = 16;
    mo.renderedFrames = 200;
    auto r = RunMockMatch(mo);
    r.recording[0].gameBuild = "some-other-build";
    const auto v = Verify(r.recording[0], MockBug::None, 0, 0);
    CHECK(v.status == VerifyResult::Status::Unsupported);
}

struct Test {
    const char* name;
    void (*fn)();
};

} // namespace

// A spectator fed the host's confirmed frames (out of order, in batches)
// plays the same match, and notices a frame whose checksum differs.
void TestSpectator()
{
    MockMatchOptions mo;
    mo.seed = 21;
    mo.renderedFrames = 1500;
    // No loss: the handshake's resends are timed in real milliseconds, and
    // the loopback match runs far faster than that.
    mo.link = {4, 2, 0.0};
    const auto r = RunMockMatch(mo);
    CHECK(r.started);
    const ReplayFile& host = r.recording[0];
    const int frames = host.finalThrough + 1;
    std::printf("    host recording: %d frames confirmed of %d\n", frames, host.FrameCount());
    CHECK(frames > 1000);

    for (int corrupt : {-1, 700}) {
        MockGame game;
        SpectateConfig sc;
        sc.setup = host.setup;
        sc.delayFrames = 10;
        SpectatorPlayback spec(game, sc);
        std::string error;
        CHECK(spec.Start(&error));
        // Batches of 60 in reverse order, each sent twice.
        for (int first = (frames - 1) / 60 * 60; first >= 0; first -= 60) {
            std::vector<FrameInputs> in;
            std::vector<uint32_t> sums;
            for (int f = first; f < std::min(frames, first + 60); f++) {
                in.push_back(host.inputs[f]);
                sums.push_back(host.FrameChecksum(f) ^ (f == corrupt ? 1u : 0u));
            }
            spec.Add(first, in, sums);
            spec.Add(first, in, sums);
        }
        CHECK(spec.NextWanted() == frames);
        for (int i = 0; i < frames * 2 && spec.NextFrame() < frames; i++) spec.Update();
        CHECK(spec.NextFrame() == frames);
        CHECK(spec.DesyncFrame() == corrupt);
        const ReplayFile rec = spec.Recording();
        CHECK(rec.FrameCount() == frames && rec.FrameChecksum(frames - 1) == host.FrameChecksum(frames - 1));
    }
}

void TestUpdateInfo()
{
    JsonValue j;
    std::string error;
    CHECK(ParseJson(R"({"a": [1, 2.5, -3e2], "b": "x\u00e9\n", "c": {"d": true, "e": null}})", &j, &error));
    CHECK(j.Find("a") && j.Find("a")->array.size() == 3 && j.Find("a")->array[2].number == -300);
    CHECK(j.Str("b") == "x\xC3\xA9\n" && j.Find("c")->Find("d")->boolean);
    CHECK(!ParseJson(R"({"a": [1, 2})", &j, &error) && !ParseJson(R"({"a": 1} x)", &j, &error));

    const char* doc = R"({"product": "SFVR", "version": "0.2.0", "published": "2026-11-02", "notes": "Fixes.",
        "files": [{"path": "sfv_rollback.dll", "size": 3, "url": "releases/0.2.0/sfv_rollback.dll",
                   "sha256": "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"}]})";
    UpdateInfo info;
    CHECK(ParseUpdateInfo(doc, &info, &error));
    CHECK(info.version == "0.2.0" && info.files.size() == 1 && info.files[0].size == 3);
    // SHA-256 of "abc", lowercased on reading.
    const uint8_t abc[] = {'a', 'b', 'c'};
    CHECK(Sha256Hex(abc, 3) == info.files[0].sha256);
    std::string bad = doc;
    bad.replace(bad.find("sfv_rollback.dll"), 16, "..\\\\evil.dll");
    CHECK(!ParseUpdateInfo(bad, &info, &error));

    CHECK(SafeUpdatePath("sfv_rollback.dll") && SafeUpdatePath("sfv_noise.txt") && SafeUpdatePath("xinput1_3.dll"));
    CHECK(!SafeUpdatePath("sfv_rollback.ini") && !SafeUpdatePath("../x.dll") && !SafeUpdatePath("a\\b.dll") &&
          !SafeUpdatePath("C:x.dll") && !SafeUpdatePath("x.exe") && !SafeUpdatePath(""));
    // The fonts folder: .ttc only, and no other folder or way out of it.
    CHECK(SafeUpdatePath("SFVR/fonts/NotoSansCJK-Medium.ttc") && SafeUpdatePath("SFVR/fonts/x.TTC"));
    CHECK(!SafeUpdatePath("NotoSansCJK-Medium.ttc") && !SafeUpdatePath("SFVR/fonts/x.dll") &&
          !SafeUpdatePath("SFVR/fonts/../x.ttc") && !SafeUpdatePath("SFVR/fonts/a/x.ttc") &&
          !SafeUpdatePath("SFVR/fonts/a\\x.ttc") && !SafeUpdatePath("SFVR/fonts/") && !SafeUpdatePath("SFVR/fonts/.ttc") &&
          !SafeUpdatePath("SFVR/logs/x.ttc") && !SafeUpdatePath("sfvr/fonts/x.ttc") && !SafeUpdatePath("SFVR\\fonts\\x.ttc"));

    CHECK(CompareVersions("0.1.0-prototype", "0.1.0") < 0 && CompareVersions("0.1.0", "0.1.1") < 0);
    CHECK(CompareVersions("0.10.0", "0.9.9") > 0 && CompareVersions("0.2", "0.2.0") == 0);
    CHECK(CompareVersions("0.2.6.1", "0.2.6") > 0 && CompareVersions("0.2.6.1", "0.2.7") < 0);
    CHECK(CompareVersions("0.1.1-test", "0.1.0-prototype") > 0 && CompareVersions("1.0.0-a", "1.0.0-b") < 0);
    // Branch builds: the suffix's parts as semver orders them.
    CHECK(CompareVersions("0.3.0-rooms.9", "0.3.0-rooms.10") < 0 && CompareVersions("0.3.0-rooms.10", "0.3.0-rooms.2") > 0);
    CHECK(CompareVersions("0.3.0-rooms.2", "0.3.0-rooms.2") == 0 && CompareVersions("0.3.0-rooms.6", "0.3.0") < 0);
    CHECK(CompareVersions("0.3.0-rooms", "0.3.0-rooms.1") < 0 && CompareVersions("0.3.0-1", "0.3.0-rooms") < 0);
    CHECK(CompareVersions("0.3.0-rooms.1", "0.2.4") > 0 && CompareVersions("0.2.5", "0.3.0-rooms.1") < 0);

    CHECK(ResolveUrl("https://sfvr.example/update.json", "releases/a.dll") == "https://sfvr.example/releases/a.dll");
    CHECK(ResolveUrl("https://sfvr.example/x/update.json", "/a.dll") == "https://sfvr.example/a.dll");
    CHECK(ResolveUrl("https://sfvr.example/update.json", "http://other/a.dll") == "http://other/a.dll");
    CHECK(Base64Decode("aGVsbG8=") == std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));
}

void TestShownSetup()
{
    // Ryu's costume 0 has colors 0..9 and EX colors 100 and 101; Eleven
    // plays a fighter in costume 12 (two colors); Gill's costume 3 has one.
    LookRules rules;
    rules.isEleven = [](const PlayerSetup& p) { return p.costume == 12; };
    rules.elevenOtherColor = 1;
    rules.colorsOf = [](const PlayerSetup& p) {
        if (p.character == PackCode("GLL") && p.costume == 3) return std::vector<int>{0};
        if (p.costume == 12) return std::vector<int>{0, 1};
        std::vector<int> ids;
        for (int i = 0; i < 10; i++) ids.push_back(i);
        ids.push_back(100);
        ids.push_back(101);
        return ids;
    };
    MatchSetup s;
    s.players[0].character = s.players[1].character = PackCode("RYU");
    // The same look: each viewer sees the other in the next color.
    MatchSetup p1 = ShownSetup(s, 0, rules), p2 = ShownSetup(s, 1, rules);
    CHECK(p1.players[0].color == 0 && p1.players[1].color == 1);
    CHECK(p2.players[0].color == 1 && p2.players[1].color == 0);
    // Only colors change.
    p1.players[1].color = 0;
    CHECK(p1 == s);
    // EX colors in list order, wrapping at the end.
    s.players[0].color = s.players[1].color = 9;
    CHECK(ShownSetup(s, 0, rules).players[1].color == 100);
    s.players[0].color = s.players[1].color = 101;
    CHECK(ShownSetup(s, 0, rules).players[1].color == 0 && ShownSetup(s, 1, rules).players[0].color == 0);
    // Different looks: nothing changes, for either viewer.
    s.players[1].color = 3;
    CHECK(ShownSetup(s, 0, rules) == s && ShownSetup(s, 1, rules) == s);
    s.players[1].costume = 1;
    s.players[1].color = 101;
    CHECK(ShownSetup(s, 0, rules) == s);
    // A one-color costume stays as it is.
    MatchSetup g;
    g.players[0].character = g.players[1].character = PackCode("GLL");
    g.players[0].costume = g.players[1].costume = 3;
    CHECK(ShownSetup(g, 0, rules) == g && ShownSetup(g, 1, rules) == g);
    // Two Elevens, even as different fighters: the other one in bronze.
    MatchSetup e;
    e.players[0].character = PackCode("KEN");
    e.players[1].character = PackCode("Z21");
    e.players[0].costume = e.players[1].costume = 12;
    CHECK(ShownSetup(e, 0, rules).players[1].color == 1 && ShownSetup(e, 0, rules).players[0].color == 0);
    CHECK(ShownSetup(e, 1, rules).players[0].color == 1 && ShownSetup(e, 1, rules).players[1].color == 0);
    // One Eleven against the same fighter in another costume: no change.
    e.players[1].character = PackCode("KEN");
    e.players[1].costume = 0;
    CHECK(ShownSetup(e, 0, rules) == e);

    // What the two sides loaded, with the recolor taken back, compares equal;
    // a real difference still shows.
    MatchSetup same;
    same.players[0].character = same.players[1].character = PackCode("CNL");
    const MatchSetup host = ShownSetup(same, 0, rules), joiner = ShownSetup(same, 1, rules);
    CHECK(!(host == joiner));
    CHECK(RealPicks(host, same, 0, rules) == RealPicks(joiner, same, 1, rules));
    MatchSetup wrong = joiner;
    wrong.players[1].vskill = 1;
    CHECK(!(RealPicks(host, same, 0, rules) == RealPicks(wrong, same, 1, rules)));
    // A side that loaded another color than its view (not a recolor) still differs.
    MatchSetup odd = joiner;
    odd.players[0].color = 7;
    CHECK(!(RealPicks(host, same, 0, rules) == RealPicks(odd, same, 1, rules)));
}

void TestRulesFormat()
{
    // The defaults (the Table rules screen's), and a round trip of every kind.
    Rules r;
    CHECK(FormatRules(r) == "r1 ft2 bo3 t99 ws3");
    Rules back;
    std::string error;
    CHECK(ParseRules(FormatRules(r), &back, &error) && back == r);
    r = {20, 5, 30, Rotation::LoserStays, 0};
    CHECK(FormatRules(r) == "r1 ft20 bo5 t30 ls0" && ParseRules(FormatRules(r), &back, &error) && back == r);
    r = {1, 1, 60, Rotation::RoundRobin, 0};
    CHECK(FormatRules(r) == "r1 ft1 bo1 t60 rr" && ParseRules(FormatRules(r), &back, &error) && back == r);
    // Fields in any order; values the game does not allow, a missing or
    // repeated field, and another version are refused.
    CHECK(ParseRules("r1 t60 ws5 ft4 bo1", &back, &error) && back.firstTo == 4 && back.timer == 60 && back.staysLimit == 5);
    for (const char* bad : {"", "x", "r1", "r2 ft2 bo3 t99 ws3", "r1 ft0 bo3 t99 ws3", "r1 ft21 bo3 t99 ws3", "r1 ft2 bo2 t99 ws3",
                            "r1 ft2 bo3 t0 ws3", "r1 ft2 bo3 t-1 ws3", "r1 ft2 bo3 t99 ws4", "r1 ft2 bo3 t99", "r1 ft2 ft3 bo3 t99 ws3",
                            "r1 ft2 bo3 t99 ws3 rr", "r1 ft2 bo3 t99 ws3 stage1", "r1 ftx bo3 t99 ws3"}) {
        CHECK(!ParseRules(bad, &back, &error));
    }
    CHECK(!ParseRules("r2 ft2 bo3 t99 ws3", &back, &error) && error.find("version 2") != std::string::npos);
    CHECK(FormatRules(Rules{}).size() < 512);
    CHECK(DescribeRules(Rules{}) == "First to 2 \xC2\xB7 winner stays, up to 3 \xC2\xB7 99 seconds");
    CHECK(DescribeRules(Rules{3, 1, 30, Rotation::RoundRobin, 0}) == "First to 3 \xC2\xB7 round robin \xC2\xB7 30 seconds \xC2\xB7 one round");
}

void TestMatchTicket()
{
    // Direct Match: the rules it always had, the first joiner plays, a vote.
    const MatchTicket host = DirectMatchHost(), join = DirectMatchJoin("ABC123", false);
    CHECK(host.host && !join.host && join.roomCode == "ABC123" && !host.seatToken && host.after == AfterMatch::Vote);
    MatchSetup s;
    ApplyRulesToSetup(host.rules, &s);
    CHECK(s.roundsToWin == 2 && s.timerSeconds == 99);
    ApplyRulesToSetup(Rules{3, 5, 30, Rotation::RoundRobin, 0}, &s);
    CHECK(s.roundsToWin == 3 && s.timerSeconds == 30);
    ApplyRulesToSetup(Rules{3, 1, 60, Rotation::WinnerStays, 3}, &s);
    CHECK(s.roundsToWin == 1 && s.timerSeconds == 60);
    // A set: over once a player has firstTo games.
    const Rules ft3{3, 3, 99, Rotation::WinnerStays, 3};
    const uint16_t going[2] = {2, 2}, p2[2] = {1, 3}, p1[2] = {3, 0};
    CHECK(SetWinner(ft3, going) == -1 && SetWinner(ft3, p2) == 1 && SetWinner(ft3, p1) == 0);
    CHECK(DescribeTicket(host) == "host, first joiner plays, r1 ft2 bo3 t99 ws3, vote after each match");
}

void TestKeeperProtocol()
{
    using namespace sfv::keeper;
    // Requests: a room's settings round trip; bad ones are refused.
    const Request room = RoomRequest(7, RoomSettings{8, false, 11});
    CHECK(FormatRequest(room) == "7:room:8:0:11");
    Request back;
    RoomSettings s;
    CHECK(ParseRequest("7:room:8:0:11", &back) && back.seq == 7 && back.kind == "room" && back.args.size() == 3);
    CHECK(ParseRoomSettings(back, &s) && s.tables == 8 && !s.isPublic && s.preset == 11);
    CHECK(ParseRequest("1:peek", &back) && back.kind == "peek" && back.args.empty());
    for (const char* bad : {"", "room", "0:room", "-1:room", "x:room", "5:", "99999999999:room"}) CHECK(!ParseRequest(bad, &back));
    for (const char* bad : {"1:room:0:1:0", "1:room:9:1:0", "1:room:4:2:0", "1:room:4:1:12", "1:room:4:1", "1:room:4:1:x", "1:peek:4:1:0"}) {
        CHECK(ParseRequest(bad, &back) && !ParseRoomSettings(back, &s));
    }
    CHECK(std::string(PresetName(0)) == "Casual sets" && std::string(PresetName(11)) == "Friends only" &&
          std::string(PresetName(12)) == "Room");
    // Answers: round trip, a newline in the text cannot add a field, other
    // datagrams are not answers.
    const std::string a = FormatAnswer({12, "room", true, "ABC234"});
    Answer ans;
    CHECK(ParseAnswer(reinterpret_cast<const uint8_t*>(a.data()), a.size(), &ans) && ans.seq == 12 && ans.kind == "room" &&
          ans.ok && ans.text == "ABC234");
    const std::string no = FormatAnswer({3, "room", false, "too\nmany rooms"});
    CHECK(ParseAnswer(reinterpret_cast<const uint8_t*>(no.data()), no.size(), &ans) && !ans.ok && ans.text == "too many rooms");
    const uint8_t session[] = {0x53, 0x46, 0x56, 0x01, 0x02};
    CHECK(!ParseAnswer(session, sizeof(session), &ans));
    // Base64, and the proof's text.
    const uint8_t bytes[] = {0, 1, 2, 250, 251, 252, 253};
    for (size_t n = 0; n <= sizeof(bytes); n++) {
        std::vector<uint8_t> out;
        CHECK(Base64Decode(Base64Encode(bytes, n), &out) && out == std::vector<uint8_t>(bytes, bytes + n));
    }
    std::vector<uint8_t> junk;
    CHECK(!Base64Decode("ab!d", &junk) && !Base64Decode("abc", &junk) && !Base64Decode("ab=c", &junk));
    CHECK(ProofMessage("ABC234", 77) == "sfvr-keeper 1 ABC234 77");
    // The public list: sorted (open seats, then queues only, then full; the
    // most members first), packed whole into values of at most 512 bytes,
    // and read back, separators in names dropped.
    std::vector<ListedRoom> rooms;
    for (int i = 0; i < 60; i++) {
        ListedRoom r;
        r.code = "R" + std::to_string(10000 + i);
        r.preset = i % kPresetCount;
        r.owner = "Player\t" + std::to_string(i) + "\n,\xE3\x81\x82";
        r.members = i % 17;
        r.tables = 1 + i % 8;
        r.openSeats = i % 3 == 0 ? 0 : 2;
        r.rules = "r1 ft2 bo3 t99 ws3";
        r.faces = {"RYU", "KEN", "Z21", "Z39", "CNL", "NSH", "ZGF"};
        rooms.push_back(r);
    }
    SortRoomList(&rooms);
    CHECK(rooms.front().openSeats > 0 && rooms.front().members < kRoomCap && rooms.back().members == kRoomCap);
    for (size_t i = 1; i < rooms.size(); i++) {
        auto group = [](const ListedRoom& r) { return r.members >= kRoomCap ? 2 : r.openSeats > 0 ? 0 : 1; };
        CHECK(group(rooms[i - 1]) < group(rooms[i]) || (group(rooms[i - 1]) == group(rooms[i]) && rooms[i - 1].members >= rooms[i].members));
    }
    auto pack = [](const std::vector<ListedRoom>& list) {
        std::map<std::string, std::string> d;
        const std::vector<std::string> values = PackRoomList(list);
        CHECK(values.size() <= kListKeys);
        for (size_t i = 0; i < values.size(); i++) {
            CHECK(values[i].size() <= kMaxValue);
            d[ListKey(static_cast<int>(i))] = values[i];
        }
        return d;
    };
    std::map<std::string, std::string> data = pack(rooms);
    const std::vector<ListedRoom> read = UnpackRoomList(data);
    CHECK(read.size() == rooms.size());
    // Long names: the 16 values fill, the rest is left out (48 or more fit).
    std::vector<ListedRoom> many = rooms;
    for (int i = 0; i < 4; i++) many.insert(many.end(), rooms.begin(), rooms.end());
    for (ListedRoom& r : many) r.owner = std::string(96, 'n');
    const std::vector<ListedRoom> some = UnpackRoomList(pack(many));
    CHECK(some.size() >= 48 && some.size() < many.size() && some.front().code == many.front().code &&
          some.front().owner == many.front().owner);
    for (size_t i = 0; i < read.size(); i++) {
        CHECK(read[i].code == rooms[i].code && read[i].members == rooms[i].members && read[i].faces.size() == 6 &&
              read[i].owner == "Player" + std::to_string(std::stoi(rooms[i].code.substr(1)) - 10000) + "\xE3\x81\x82");
    }
    CHECK(PackRoomList({}).empty());
    data[ListKey(3)] = "not\ta room";
    CHECK(UnpackRoomList(data).size() < read.size());
    // Chat: three at once, then one a second; 200 characters (not bytes);
    // refusals count as strikes for 10 s.
    ChatLimiter chat;
    CHECK(chat.Allow(0, "a") && chat.Allow(0, "b") && chat.Allow(10, "c") && !chat.Allow(20, "d"));
    CHECK(!chat.Allow(500, "e") && chat.Allow(1100, "f") && !chat.Allow(1200, "g"));
    CHECK(chat.Strikes(1200) == 3 && chat.Strikes(10600) == 1 && chat.Strikes(11300) == 0);
    std::string longest;
    for (int i = 0; i < 200; i++) longest += "\xE3\x81\x82";
    CHECK(Utf8Length(longest) == 200 && chat.Allow(5000, longest) && !chat.Allow(9000, longest + "a") && !chat.Allow(12000, ""));
    // The keeper's proof: signed with a new key, checked as the mod does; a
    // proof for another member id or room fails. The key file round trips.
    KeyPair key;
    CHECK(GenerateKey(&key));
    KeyPair readKey;
    CHECK(KeyFromText(KeyToText(key) + "\n", &readKey) && std::memcmp(&readKey, &key, sizeof(key)) == 0 &&
          !KeyFromText("12", &readKey));
    const std::string proof = ProofMessage("ABC234", 77);
    uint8_t signature[64];
    CHECK(Sign(key, reinterpret_cast<const uint8_t*>(proof.data()), proof.size(), signature));
    std::vector<uint8_t> sig;
    CHECK(Base64Decode(Base64Encode(signature, 64), &sig) && sig.size() == 64);
    CHECK(VerifySignature(reinterpret_cast<const uint8_t*>(proof.data()), proof.size(), sig.data(), sig.size(), key.pub));
    for (const std::string& other : {ProofMessage("ABC234", 78), ProofMessage("ABC235", 77)}) {
        CHECK(!VerifySignature(reinterpret_cast<const uint8_t*>(other.data()), other.size(), sig.data(), sig.size(), key.pub));
    }
    // A room's detail: 8 tables and 16 players with the longest names come
    // back whole over several parts, each fitting a datagram with its answer;
    // a part out of range or a broken record is refused.
    RoomDetail detail;
    for (int t = 0; t < 8; t++) detail.tables.push_back("r1 ft2 bo3 t99 ws3");
    for (int i = 0; i < 16; i++) detail.people.push_back({std::string(96, static_cast<char>('a' + i)), i % 2 ? "RYU" : "Z41"});
    detail.people[3].name = "bad\x1Ename\x1F";
    const std::vector<std::string> parts = PackRoomDetail(detail);
    CHECK(parts.size() >= 2);
    std::vector<std::string> bodies;
    for (size_t i = 0; i < parts.size(); i++) {
        CHECK(FormatAnswer({99, "peek", true, parts[i]}).size() <= 1024);
        int part = 0, of = 0;
        std::string body;
        CHECK(ReadDetailPart(parts[i], &part, &of, &body) && part == static_cast<int>(i + 1) && of == static_cast<int>(parts.size()));
        bodies.push_back(body);
    }
    RoomDetail back2;
    back2.code = "ABC234";
    CHECK(JoinRoomDetail(bodies, &back2) && back2.code == "ABC234" && back2.tables == detail.tables && back2.people.size() == 16);
    CHECK(back2.people[3].name == "badname" && back2.people[0] == detail.people[0] && back2.people[15].chara == "RYU");
    RoomDetail empty;
    const std::vector<std::string> none = PackRoomDetail(empty);
    int part = 0, of = 0;
    std::string body;
    CHECK(none.size() == 1 && ReadDetailPart(none[0], &part, &of, &body) && body.empty() && JoinRoomDetail({body}, &empty));
    CHECK(!ReadDetailPart("3/2\x1Dtx", &part, &of, &body) && !ReadDetailPart("1/2tx", &part, &of, &body) &&
          !JoinRoomDetail({"xjunk"}, &empty) && !JoinRoomDetail({"pRYU"}, &empty));
    CHECK(FormatRequest(PeekRequest(5, "ABC234")) == "5:peek:ABC234");
    // The name filter: folded (case, digits and symbols as letters, the rest
    // dropped), a word anywhere in the name.
    const std::vector<std::string> words = {FoldName("badword"), FoldName("")};
    CHECK(FoldName("B4.dw0rd-$!") == "badwords" && FoldName("\xE3\x81\x82") == "");
    CHECK(NameRejected("xX_B4D w0rd_Xx", words) && NameRejected("BADWORD", words) && !NameRejected("Bad Wolf", words) &&
          !NameRejected("", words));
    using Numbers = std::map<uint32_t, int>;
    const Numbers numbers{{12, 1}, {34, 3}}, onlyFive{{5, 2}};
    CHECK(FormatMask(numbers) == "12=1,34=3" && ParseMask("12=1,34=3") == numbers && ParseMask("").empty() &&
          ParseMask("x=1,5=2,6,7=x") == onlyFive);
    // Ping datagrams round trip; other datagrams are not pings.
    uint8_t ping[kPingSize];
    FormatPing(true, 0x0123456789ABCDEFull, ping);
    bool pong = false;
    uint64_t clock = 0;
    CHECK(ParsePing(ping, sizeof(ping), &pong, &clock) && pong && clock == 0x0123456789ABCDEFull);
    CHECK(!ParsePing(ping, sizeof(ping) - 1, &pong, &clock) && !ParsePing(reinterpret_cast<const uint8_t*>(a.data()), kPingSize, &pong, &clock));
    // The next owner: the longest present, never the leaver or the keeper.
    CHECK(NextOwner({5, 9, 3}, 5, 1) == 9 && NextOwner({1, 5, 9}, 9, 1) == 5 && NextOwner({5}, 5, 1) == 0 && NextOwner({}, 0, 1) == 0);
}

void TestTexturePackage()
{
    // A package as the paks hold one: a name table entry first (the same
    // string, no header), then the platform data with two mips, the first's
    // bytes at the end of the file, the second's inline.
    std::vector<uint8_t> pkg;
    auto i32 = [&](int32_t v) { pkg.insert(pkg.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
    auto i64 = [&](int64_t v) { pkg.insert(pkg.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 8); };
    auto str = [&](const char* s) {
        i32(static_cast<int32_t>(std::strlen(s) + 1));
        pkg.insert(pkg.end(), s, s + std::strlen(s) + 1);
    };
    i32(0x9E2A83C1);
    for (int i = 0; i < 4; i++) i32(-7);
    str("PF_B8G8R8A8");
    i32(0x1234);
    i32(4); i32(2); i32(1);
    str("PF_B8G8R8A8");
    i32(0); i32(2);
    // Each mip: bCooked, the bulk data header (offsets from where the bulk
    // data starts, at the end), the inline bytes if any, the mip's size.
    i32(1); i32(0x1); i32(32); i32(32);
    const size_t offsetAt = pkg.size();
    i64(0);
    i32(4); i32(2);
    i32(1); i32(0x40); i32(8); i32(8); i64(0);
    for (int i = 0; i < 2; i++) { pkg.push_back(10); pkg.push_back(20); pkg.push_back(30); pkg.push_back(255); }
    i32(2); i32(1);
    const int64_t end = static_cast<int64_t>(pkg.size());
    // Mip 0: red opaque, blue clear, alternating.
    for (int i = 0; i < 8; i++) {
        const bool red = i % 2 == 0;
        pkg.push_back(red ? 0 : 255); pkg.push_back(0); pkg.push_back(red ? 255 : 0); pkg.push_back(red ? 255 : 0);
    }
    i32(0x9E2A83C1); // the package tag that closes the file
    TexPackage p;
    std::string error;
    CHECK(ParseTexturePackage(pkg.data(), pkg.size(), &p, &error));
    CHECK(p.format == TexFormat::Bgra8 && p.width == 4 && p.height == 2 && p.mips.size() == 2);
    CHECK(p.mips.size() == 2 && p.mips[0].offset == static_cast<size_t>(end) && p.mips[1].width == 2 && p.mips[1].size == 8);
    CHECK(PickMip(p, 0) == &p.mips[0] && PickMip(p, 2) == &p.mips[1] && PickMip(p, 3) == &p.mips[0] && PickMip(p, 1) == &p.mips[1]);
    TexImage image;
    CHECK(MipImage(pkg.data(), pkg.size(), p, p.mips[0], 2, &image, &error));
    // 4x2 into 2x1: each pixel is a red and a clear blue one: red, half alpha.
    CHECK(image.width == 2 && image.height == 1 && image.bytes.size() == 8);
    CHECK(image.bytes.size() == 8 && image.bytes[0] == 0 && image.bytes[2] == 255 && image.bytes[3] == 128);
    CHECK(MipImage(pkg.data(), pkg.size(), p, p.mips[1], 0, &image, &error) && image.width == 2 && image.bytes[2] == 30);
    // Broken ones are refused: a cut file, a mip size that does not halve.
    CHECK(!ParseTexturePackage(pkg.data(), static_cast<size_t>(end) + 4, &p, &error));
    // Without the tag the data ends the file.
    CHECK(ParseTexturePackage(pkg.data(), pkg.size() - 4, &p, &error) && p.mips.size() == 2 && p.mips[0].offset == static_cast<size_t>(end));
    std::vector<uint8_t> bad = pkg;
    const int32_t three = 3;
    std::memcpy(bad.data() + offsetAt + 8 + 8 + 4 + 20 + 8, &three, 4); // the second mip's width
    CHECK(!ParseTexturePackage(bad.data(), bad.size(), &p, &error));
    CHECK(TexBytes(TexFormat::Bc1, 6, 6) == 4 * 8 && TexBytes(TexFormat::Bc3, 1, 1) == 16);
    // BC3, as the pad glyphs are: mips below 4x4 stored as 4x4, all inline.
    pkg.clear();
    i32(0x9E2A83C1);
    i32(8); i32(8); i32(1);
    str("PF_DXT5");
    i32(0); i32(3);
    for (const int side : {8, 4, 4}) {
        const int bytes = side == 8 ? 64 : 16;
        i32(1); i32(0x40); i32(bytes); i32(bytes); i64(0);
        pkg.insert(pkg.end(), static_cast<size_t>(bytes), static_cast<uint8_t>(side));
        i32(side); i32(side);
    }
    i32(0x9E2A83C1);
    CHECK(ParseTexturePackage(pkg.data(), pkg.size(), &p, &error) && p.format == TexFormat::Bc3 && p.mips.size() == 3);
    CHECK(MipImage(pkg.data(), pkg.size(), p, *PickMip(p, 0), 4, &image, &error) && image.format == TexFormat::Bc3 &&
          image.width == 8 && image.bytes.size() == 64 && image.bytes[0] == 8);
}

void TestUpdateSignature()
{
    // A key pair as the release script makes one, through the same API.
    BCRYPT_KEY_HANDLE key = nullptr;
    CHECK(BCRYPT_SUCCESS(BCryptGenerateKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, &key, 256, 0)));
    CHECK(BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0)));
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG size = 0;
    CHECK(BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &size, 0)));
    uint8_t pub[64];
    std::memcpy(pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);

    const std::string text = R"({"product": "SFVR", "version": "9.9.9"})";
    uint8_t digest[32];
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)text.data(), (ULONG)text.size(), digest, 32);
    uint8_t sig[64];
    CHECK(BCRYPT_SUCCESS(BCryptSignHash(key, nullptr, digest, 32, sig, 64, &size, 0)) && size == 64);
    BCryptDestroyKey(key);

    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    CHECK(VerifySignature(bytes, text.size(), sig, 64, pub));
    std::string tampered = text;
    tampered[tampered.size() - 3] = '8';
    CHECK(!VerifySignature(reinterpret_cast<const uint8_t*>(tampered.data()), tampered.size(), sig, 64, pub));
    uint8_t otherSig[64];
    std::memcpy(otherSig, sig, 64);
    otherSig[5] ^= 1;
    CHECK(!VerifySignature(bytes, text.size(), otherSig, 64, pub));
    const uint8_t noKey[64] = {};
    CHECK(!VerifySignature(bytes, text.size(), sig, 64, noKey));
}

int main()
{
    const Test tests[] = {
        {"hashes", TestHashes},
        {"input delay suggestion", TestInputDelaySuggestion},
        {"ignore mask", TestIgnoreMask},
        {"skipped saves", TestSkippedSaves},
        {"replay file round trip", TestReplayRoundTrip},
        {"compact recording", TestCompactRecording},
        {"clean match", TestCleanMatch},
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
        {"update info", TestUpdateInfo},
        {"update signature", TestUpdateSignature},
        {"texture package", TestTexturePackage},
        {"shown setup", TestShownSetup},
        {"rules format", TestRulesFormat},
        {"match ticket", TestMatchTicket},
        {"keeper protocol", TestKeeperProtocol},
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
