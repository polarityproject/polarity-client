// Recordings, hashes and the small helpers they rest on.

#include "test_support.h"

void TestHashes() {
    CHECK(Hash64("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(Hash64("abc", 3) == 0x44BC2CF5AD770999ull);
    const char* longer = "Nobody inspects the spammish repetition";
    CHECK(Hash64(longer, std::strlen(longer)) == 0xFBCEA83C8A378BF1ull);
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);
}

void TestBuildIds() {
    const std::string live = "sfv-648AD3A4/adapter-12/sim-12/map-0000000000000000/noise-c2da3f81207455e0";
    // A recording from before the sim part: its adapter version stands in.
    const std::string old = "sfv-648AD3A4/adapter-12/map-0000000000000000/noise-1111111111111111";
    CHECK(BuildSimVersion(live) == 12);
    CHECK(BuildSimVersion(old) == 12);
    CHECK(BuildAdapterVersion(live) == 12);
    CHECK(BuildExe(live) == "sfv-648AD3A4");
    // Other checksum masks (noise, map): still plays.
    CHECK(ReplayCompatible(old, live));
    CHECK(ReplayCompatible("sfv-648AD3A4/adapter-13/sim-12/map-00000000000000ff/noise-22", live));
    // Another sim version, another exe, an older adapter: does not.
    CHECK(!ReplayCompatible("sfv-648AD3A4/adapter-12/sim-11/map-0/noise-0", live));
    CHECK(!ReplayCompatible("sfv-648AD3A5/adapter-12/sim-12/map-0/noise-0", live));
    CHECK(!ReplayCompatible("sfv-648AD3A4/adapter-10/map-0/noise-0", live));
    CHECK(!ReplayCompatible("mockfighter-1", live));
}

void TestPingTiers() {
    using core::PingTier;
    // The tier is the delay suggested, 0 to 3, then 4 once 3 frames leave
    // more than 2 to rollback (late above 5 frames: past 166 ms, no jitter).
    CHECK(PingTier(20, 0) == 0);
    CHECK(PingTier(66, 0) == 0);
    CHECK(PingTier(67, 0) == 1);
    CHECK(PingTier(101, 0) == 2);
    CHECK(PingTier(134, 0) == 3);
    CHECK(PingTier(166, 0) == 3);
    CHECK(PingTier(167, 0) == core::kPingTierHeavy);
    CHECK(PingTier(135, 17) == core::kPingTierHeavy);
}

void TestLoaderChoice() {
    using proxy::HasArgument;
    const wchar_t* flag = L"-sfve_loader=launcher";
    CHECK(HasArgument(L"\"C:\\Game\\StreetFighterV.exe\" -sfve_loader=launcher -sfve_mode=netplay", flag));
    CHECK(HasArgument(L"x.exe\t-sfve_loader=launcher", flag));
    CHECK(HasArgument(L"-sfve_loader=launcher", flag));
    // Only the whole argument, in its own case.
    CHECK(!HasArgument(L"x.exe -sfve_loader=launcher2", flag));
    CHECK(!HasArgument(L"x.exe x-sfve_loader=launcher", flag));
    CHECK(!HasArgument(L"x.exe -SFVE_LOADER=LAUNCHER", flag));
    CHECK(!HasArgument(L"x.exe -sfve_loader=launch", flag));
    CHECK(!HasArgument(L"", flag));
    CHECK(!HasArgument(nullptr, flag));

    wchar_t ini[MAX_PATH];
    GetTempPathW(MAX_PATH, ini);
    lstrcatW(ini, L"sfvtests_loader.ini");
    DeleteFileW(ini);
    CHECK(proxy::LoadsPolarity(ini)); // no file: the default
    WritePrivateProfileStringW(L"loader", L"load", L"none", ini);
    CHECK(!proxy::LoadsPolarity(ini));
    WritePrivateProfileStringW(L"loader", L"load", L"polarity", ini);
    CHECK(proxy::LoadsPolarity(ini));
    DeleteFileW(ini);
}

void TestDisplayName() {
    using core::DisplayName;
    CHECK(DisplayName("Ryu") == "Ryu");
    CHECK(DisplayName("") == "");
    // Emoji, their joiners and selectors go; the spaces they leave collapse.
    CHECK(DisplayName("\xF0\x9F\x94\xA5 Kira \xF0\x9F\x94\xA5") == "Kira");
    CHECK(DisplayName("Dad\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 Bod") == "Dad Bod");
    CHECK(DisplayName("\xE2\x9A\xA7 Eastwood \xE2\x9A\xA7") == "Eastwood");                       // U+26A7
    CHECK(DisplayName("\xF0\x90\x99\x9A sweetsakurastar \xF0\x90\x99\x9A") == "sweetsakurastar"); // U+1065A
    CHECK(DisplayName("A\xE2\x9D\xA4\xEF\xB8\x8F"
                      " B") == "A B"); // heart + VS16
    // Script and bold letters read plain; CJK and accents stay.
    CHECK(DisplayName("\xF0\x9D\x93\xA2\xF0\x9D\x93\xAA\xF0\x9D\x93\xB4\xF0\x9D\x93\xBE") == "Saku");
    CHECK(DisplayName("\xF0\x9D\x9F\x8F\xF0\x9D\x9F\x90") == "12");
    CHECK(DisplayName("\xE6\xA1\x9C Jos\xC3\xA9") == "\xE6\xA1\x9C Jos\xC3\xA9");
    // Nothing left: a placeholder, not an empty name.
    CHECK(DisplayName("\xF0\x9F\x98\x80\xF0\x9F\x98\x80") == "Player");
    // A cut-off sequence does not read past the end.
    CHECK(DisplayName(std::string("ab\xF0\x9F", 4)) == "ab\xEF\xBF\xBD\xEF\xBF\xBD");
}

void TestInputDelaySuggestion() {
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

void TestIgnoreMask() {
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
void TestSkippedSaves() {
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

// The Battle Log's summary chunk and list: the header alone reads it, a file
// without one still lists, both players' copies of a match list once (the
// player's own kept), sets group and score, filters pick.
void TestBattleLog() {
    auto make = [](uint64_t id, int local, uint64_t created, uint32_t c0, uint32_t c1, const char* n0, const char* n1, int winner,
                   uint64_t setKey, uint16_t game, uint8_t w0, uint8_t w1) {
        ReplayFile rf;
        rf.modVersion = "0.3.2";
        rf.gameBuild = "mockfighter-1";
        rf.matchId = id;
        rf.localPlayer = local;
        rf.createdUnix = created;
        rf.setup.players[0].character = c0;
        rf.setup.players[1].character = c1;
        rf.regions = {{"a", 16, {}}};
        rf.initialHashes = {1};
        for (int f = 0; f < 50; f++) {
            const uint64_t h[1] = {static_cast<uint64_t>(f)};
            rf.SetFrame(f, FrameInputs{}, h);
        }
        rf.finalThrough = 49;
        MatchSummary& s = rf.summary;
        s.present = true;
        s.names[0] = n0;
        s.names[1] = n1;
        s.winner = static_cast<int8_t>(winner);
        s.roundWins[winner & 1] = 2;
        s.roundWins[1 - (winner & 1)] = 1;
        s.rounds = {{1200, 0, 920}, {2400, 1, 1500}, {3600, static_cast<int8_t>(winner), 2700}};
        s.setKey = setKey;
        s.setGame = game;
        s.setRule = setKey ? "First to 3" : "";
        s.setWins[0] = w0;
        s.setWins[1] = w1;
        s.seconds = 171;
        return rf;
    };
    // The header: summary and setup without the inputs.
    const ReplayFile one = make(7, 0, 1000, 1, 2, "POLARITYDEV", "KANZUKI", 0, 7, 1, 1, 0);
    const std::vector<uint8_t> bytes = SerializeReplay(one, ReplayDetail::Compact);
    ReplayFile head;
    std::string error;
    CHECK(DeserializeReplayHeader(bytes.data(), std::min<size_t>(bytes.size(), 16384), &head, &error));
    CHECK(head.matchId == 7 && head.inputs.empty() && head.setup == one.setup);
    CHECK(head.summary.present && head.summary.names[1] == "KANZUKI" && head.summary.rounds.size() == 3);
    CHECK(head.summary.rounds[1].endFrame == 2400 && head.summary.rounds[1].winner == 1 && head.summary.setRule == "First to 3");
    CHECK(head.summary.setWins[0] == 1 && head.summary.seconds == 171 && head.summary.winner == 0);
    CHECK(head.summary.rounds[0].fightFrame == 920 && head.summary.rounds[2].fightFrame == 2700);
    // The rounds' action frames from a session's per-frame fight flags: on
    // after frame 3's tick, off after frame 7's (the KO); on again after 10,
    // still on at the end.
    {
        const std::vector<uint8_t> flags = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1};
        const std::vector<RoundResult> r = RoundsFromFrameFlags(flags);
        CHECK(r.size() == 2 && r[0].fightFrame == 4 && r[0].endFrame == 7 && r[1].fightFrame == 11 && r[1].endFrame == -1);
        CHECK(RoundsFromFrameFlags({}).empty());
    }
    ReplayFile full;
    CHECK(DeserializeReplay(bytes.data(), bytes.size(), &full, &error) && full.summary.setGame == 1 && full.FrameCount() == 50);
    // A file without a summary still has a header.
    ReplayFile old = one;
    old.summary = MatchSummary{};
    const std::vector<uint8_t> oldBytes = SerializeReplay(old);
    CHECK(DeserializeReplayHeader(oldBytes.data(), oldBytes.size(), &head, &error) && !head.summary.present && head.matchId == 7);

    using namespace battlelog;
    std::vector<Entry> entries;
    auto add = [&](const ReplayFile& rf, const char* path) {
        Entry e;
        e.path = path;
        e.header = rf;
        entries.push_back(std::move(e));
    };
    // A set of three against KANZUKI (match 7 also as their copy), a single
    // match against RED (where POLARITYDEV is P2), and one more set game later.
    add(make(7, 1, 1000, 1, 2, "POLARITYDEV", "KANZUKI", 0, 7, 1, 1, 0), "their-7");
    add(one, "mine-7");
    add(make(8, 0, 1100, 1, 2, "POLARITYDEV", "KANZUKI", 1, 7, 2, 1, 1), "mine-8");
    add(make(9, 1, 1200, 3, 4, "Red", "polaritydev", 1, 0, 0, 0, 0), "mine-9");
    add(make(10, 0, 1300, 1, 5, "POLARITYDEV", "KANZUKI", 0, 7, 3, 2, 1), "mine-10");
    const std::vector<Entry> merged = Merge(entries, "POLARITYDEV");
    CHECK(merged.size() == 4);
    CHECK(merged[0].header.matchId == 10 && merged[3].header.matchId == 7 && merged[3].path == "mine-7");
    CHECK(MySide(merged[1], "POLARITYDEV") == 1); // match 9: POLARITYDEV was P2 (any case)
    std::vector<const Entry*> all;
    for (const Entry& e : merged) all.push_back(&e);
    const std::vector<Set> sets = GroupSets(all);
    CHECK(sets.size() == 2);
    CHECK(sets[0].key == 7 && sets[0].games.size() == 3 && sets[0].games[0]->header.matchId == 7 && sets[0].rule == "First to 3");
    CHECK(sets[0].wins[0] == 2 && sets[0].wins[1] == 1);
    CHECK(sets[1].key == 0 && sets[1].games.size() == 1);
    // Filters: POLARITYDEV's character 1 (three matches), the opponent's 5 (one), RED by name.
    Filter f;
    f.mine = 1;
    CHECK(std::count_if(merged.begin(), merged.end(), [&](const Entry& e) { return Passes(e, f, "POLARITYDEV"); }) == 3);
    f = Filter{};
    f.theirs = 5;
    CHECK(std::count_if(merged.begin(), merged.end(), [&](const Entry& e) { return Passes(e, f, "POLARITYDEV"); }) == 1);
    f = Filter{};
    f.opponent = "red";
    CHECK(std::count_if(merged.begin(), merged.end(), [&](const Entry& e) { return Passes(e, f, "POLARITYDEV"); }) == 1);
    const auto mine = Characters(merged, "POLARITYDEV", false);
    CHECK(mine.size() == 2 && mine[0].first == 1 && mine[0].second == 3);
    const auto opponents = Opponents(merged, "POLARITYDEV");
    CHECK(opponents.size() == 2 && opponents[0].first == "KANZUKI" && opponents[0].second == 3);
}

void TestReplayRoundTrip() {
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

// Frames that ran on predicted inputs are hashed once confirmed, from their
// saves: the recordings come out as hashing every simulation makes them (delay
// 0, a link of 5 ticks each way: most frames run predicted and many run again), the peers
// agree, and each recording verifies against a straight run.
void TestHashOnConfirm() {
    MockMatchOptions mo;
    mo.seed = 11;
    mo.link = {4, 3, 0.05};
    mo.inputDelay = 0;
    mo.hashOnConfirm = false;
    const auto every = RunMockMatch(mo);
    PrintMatch(every);
    mo.hashOnConfirm = true;
    const auto once = RunMockMatch(mo);
    PrintMatch(once);
    CHECK(every.started && once.started);
    CHECK(FirstDesync(every) < 0 && FirstDesync(once) < 0);
    CHECK(once.rollbacks[0].max_frames >= 3 && once.rollbacks[1].max_frames >= 3);
    for (int p = 0; p < kNumPlayers; p++) {
        const ReplayFile& a = every.recording[p];
        const ReplayFile& b = once.recording[p];
        CHECK(b.finalThrough > 2500 && a.finalThrough == b.finalThrough);
        bool same = a.initialHashes == b.initialHashes;
        for (int f = 0; f <= b.finalThrough && same; f++) {
            same = a.inputs[f] == b.inputs[f] && a.FrameChecksum(f) == b.FrameChecksum(f) &&
                   std::memcmp(a.FrameHashes(f), b.FrameHashes(f), a.RegionCount() * 8) == 0;
        }
        CHECK(same);
        CHECK(once.saves[p].loads > 0 && once.saves[p].loadMismatches == 0);
        const auto straight = Verify(b, MockBug::None, 0, 0);
        CHECK(straight.status == VerifyResult::Status::Match);
    }
    const DiffReport d = DiffReplays(once.recording[0], once.recording[1]);
    CHECK(d.comparable && d.firstInputMismatch < 0 && d.firstStateMismatch < 0);
}

// The decoder for Proton (no XPRESS Huffman in its cabinet.dll) against
// Windows' own encoder: what Windows packs, it unpacks the same.
void TestXpressHuffmanDecoder() {
    std::vector<std::vector<uint8_t>> inputs;
    inputs.push_back(std::vector<uint8_t>(4096, 0)); // one long match
    std::vector<uint8_t> random(70000);
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (uint8_t& b : random) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        b = static_cast<uint8_t>(x);
    }
    inputs.push_back(random); // literals over a block's end
    std::vector<uint8_t> text;
    const char* words[] = {"round ", "frame ", "rollback ", "Polarity ", "hitstop ", "\n", "0123456789 "};
    for (int i = 0; text.size() < 300000; i++) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        const char* w = words[x % 7];
        text.insert(text.end(), w, w + std::strlen(w));
    }
    inputs.push_back(text); // five blocks, short and middling matches
    std::vector<uint8_t> state(400000, 0);
    for (size_t i = 0; i < state.size(); i += 64) state[i] = static_cast<uint8_t>(i / 64);
    for (size_t i = 150000; i < 150300; i++) state[i] = random[i - 150000];
    inputs.push_back(state); // game state: zeros, repeats, a long run (16-bit match lengths)
    std::vector<uint8_t> zeros(1 << 20, 0);
    inputs.push_back(zeros); // a run longer than 65535 (32-bit match length)
    int checked = 0;
    for (const std::vector<uint8_t>& in : inputs) {
        std::vector<uint8_t> packed, mine, theirs;
        if (!Compress(in.data(), in.size(), &packed)) {
            std::printf("    no Windows encoder here: skipped\n");
            return;
        }
        const bool decoded = DecompressXpressHuffman(packed.data(), packed.size(), in.size(), &mine);
        if (!decoded || mine != in) {
            size_t at = 0;
            while (at < mine.size() && at < in.size() && mine[at] == in[at]) at++;
            std::printf("    input %d (%zu bytes, %zu packed): decoded %d, first difference at %zu\n", checked, in.size(), packed.size(), decoded, at);
        }
        CHECK(decoded);
        CHECK(mine == in);
        CHECK(Decompress(packed.data(), packed.size(), in.size(), &theirs) && theirs == in);
        // Damaged input fails or differs, never overruns.
        std::vector<uint8_t> cut(packed.begin(), packed.begin() + packed.size() / 2), out;
        if (DecompressXpressHuffman(cut.data(), cut.size(), in.size(), &out)) CHECK(out.size() == in.size());
        checked++;
    }
    std::printf("    %d inputs decoded as Windows encoded them\n", checked);
}

void TestRecordingLimits() {
    // A size over the cap is refused before anything is allocated.
    const uint8_t some[300] = {};
    std::vector<uint8_t> out;
    CHECK(!Decompress(some, sizeof(some), kMaxRawSize + 1, &out) && !DecompressXpressHuffman(some, sizeof(some), 0xFFFFFFFFu, &out));
    // Files: "SFVR", the version, then chunks of a tag, a size and a payload.
    auto u32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    };
    auto chunk = [&](std::vector<uint8_t>& file, const char* tag, const std::vector<uint8_t>& payload) {
        file.insert(file.end(), tag, tag + 4);
        u32(file, static_cast<uint32_t>(payload.size()));
        file.insert(file.end(), payload.begin(), payload.end());
    };
    auto pack = [&](const char* inner, uint32_t rawSize, const std::vector<uint8_t>& packed) {
        std::vector<uint8_t> p(inner, inner + 4);
        u32(p, rawSize);
        p.insert(p.end(), packed.begin(), packed.end());
        return p;
    };
    std::vector<uint8_t> head{'S', 'F', 'V', 'R'};
    u32(head, kReplayFormatVersion);
    ReplayFile rf;
    std::string error;
    // A chunk that claims 128 MB, over the chunk cap; one that claims 4 GB,
    // over the file's.
    std::vector<uint8_t> big = head;
    chunk(big, "PACK", pack("INPT", 128u << 20, std::vector<uint8_t>(300)));
    CHECK(!DeserializeReplay(big.data(), big.size(), &rf, &error) && error == "cannot uncompress a chunk: file is corrupt");
    std::vector<uint8_t> huge = head;
    chunk(huge, "PACK", pack("INPT", 0xFFFFFFFFu, std::vector<uint8_t>(300)));
    CHECK(!DeserializeReplay(huge.data(), huge.size(), &rf, &error) && error == "file unpacks to too much: it is corrupt");
    // Chunks that unpack to more than 256 MB in all.
    std::vector<uint8_t> zeros(size_t{60} << 20, 0), packed;
    if (Compress(zeros.data(), zeros.size(), &packed)) {
        std::vector<uint8_t> bomb = head;
        for (int i = 0; i < 5; i++) chunk(bomb, "PACK", pack("XTRA", static_cast<uint32_t>(zeros.size()), packed));
        CHECK(bomb.size() < (size_t{1} << 20));
        CHECK(!DeserializeReplay(bomb.data(), bomb.size(), &rf, &error) && error == "file unpacks to too much: it is corrupt");
    } else {
        std::printf("    no Windows encoder here: the total skipped\n");
    }
    // A region hash table of 10,000 regions over 7,000 frames (70 million
    // hashes) from a file of a few hundred KB.
    std::vector<uint8_t> regions, inputs, windows;
    u32(regions, 10000);
    for (int i = 0; i < 10000; i++) {
        u32(regions, 0); // name
        u32(regions, 8); // size, 64-bit
        u32(regions, 0);
        u32(regions, 0); // mask
    }
    u32(inputs, 7000);
    inputs.resize(inputs.size() + 7000 * 8);
    u32(windows, 7000);
    u32(windows, 10000);
    u32(windows, 0);
    std::vector<uint8_t> table = head;
    chunk(table, "RGNS", regions);
    chunk(table, "INPT", inputs);
    chunk(table, "HWIN", windows);
    CHECK(!DeserializeReplay(table.data(), table.size(), &rf, &error) && error == "hash table too large");
}

void TestCompactRecording() {
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
