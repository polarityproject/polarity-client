// Netplay sessions between MockGames, lobby messages and spectators.

#include "test_support.h"

void TestCleanMatch() {
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

void TestSetupMismatch() {
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

void TestStateOutsideSnapshot() {
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
        return idx; }(), "fighters"));
}

void TestMachineSpecificValue() {
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

void TestEffectUsesGameplayRng() {
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

namespace {

int RunStress(MockBug bug, int frames) {
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

} // namespace

void TestStressSession() {
    CHECK(RunStress(MockBug::None, 1200) == 0);
    CHECK(RunStress(MockBug::EffectUsesGameplayRng, 1200) > 0);
    CHECK(RunStress(MockBug::StateOutsideSnapshot, 3000) > 0);
}

void TestMatchCodes() {
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
        sloppy += char(std::tolower(static_cast<unsigned char>(c == '0' ? 'O' : c == '1' ? 'I'
                                                                                         : c)));
    }
    CHECK(DecodeMatchCode(sloppy, &back) && back == udp);
    std::string typo = u;
    typo[3] = typo[3] == 'A' ? 'B' : 'A';
    CHECK(!DecodeMatchCode(typo, &back));
    CHECK(!DecodeMatchCode("X-1234", &back));
}

void TestLobbyMessages() {
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
    for (size_t i = 0; i < lobby::kMatchesPerMessage; i++) {
        lobby::MatchEntry e;
        e.id = 0x1000 + i;
        e.setup.players[0].character = PackCode("RMK");
        e.setup.players[1].character = PackCode("KEN");
        e.setup.players[1].color = 9;
        e.setup.stage = static_cast<uint16_t>(i);
        e.setup.seed = 0xABCDEF0000 + i;
        e.frames = 9000 + static_cast<uint32_t>(i);
        e.finished = i + 1 < lobby::kMatchesPerMessage;
        e.winner = static_cast<int8_t>(i % 2);
        list.matches.push_back(e);
    }
    // A slice of the kept list (M5: 40 kept, 12 a message): one datagram.
    list.sliceAt = 24;
    list.sliceOf = 36;
    bytes = lobby::Encode(list);
    CHECK(bytes.size() + 1 <= 1024); // with the mux's channel byte
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->type == lobby::Type::Matches && back->matches == list.matches);
    CHECK(back && back->sliceAt == 24 && back->sliceOf == 36);
    // A message from a build before slices (no slice bytes): the whole list.
    std::vector<uint8_t> old(bytes.begin(), bytes.end() - 2);
    back = lobby::Decode(old.data(), old.size());
    CHECK(back && back->sliceAt == 0 && back->sliceOf == lobby::kMatchesPerMessage && back->matches.size() == lobby::kMatchesPerMessage);
    // A slice past its list is refused.
    list.sliceAt = 30;
    bytes = lobby::Encode(list);
    CHECK(!lobby::Decode(bytes.data(), bytes.size()));

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
    CHECK(back && back->type == lobby::Type::Bye && back->text == bye.text && !back->forfeit);
    // Leaving a table set mid-way: a forfeit.
    bye.forfeit = true;
    bytes = lobby::Encode(bye);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->forfeit);

    lobby::Message loaded;
    loaded.type = lobby::Type::Loaded;
    loaded.matchId = 99;
    loaded.stateChecksum = 0xABCD1234;
    auto lb = lobby::Encode(loaded);
    back = lobby::Decode(lb.data(), lb.size());
    CHECK(back && back->stateChecksum == 0xABCD1234);
    // Types 6 to 8 (the frozen-state copy before protocol 17) are refused.
    for (uint8_t gone = 6; gone <= 8; gone++) {
        lb[6] = gone; // after the magic and the protocol
        CHECK(!lobby::Decode(lb.data(), lb.size()));
    }

    bytes[0] ^= 1;
    CHECK(!lobby::Decode(bytes.data(), bytes.size()));
    CHECK(!lobby::Decode(bytes.data(), 5));
}

void TestMuxAndConditions() {
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

void TestWrongBuildIsRefused() {
    MockMatchOptions mo;
    mo.seed = 16;
    mo.renderedFrames = 200;
    auto r = RunMockMatch(mo);
    r.recording[0].gameBuild = "some-other-build";
    const auto v = Verify(r.recording[0], MockBug::None, 0, 0);
    CHECK(v.status == VerifyResult::Status::Unsupported);
}

// A spectator fed the host's confirmed frames (out of order, in batches)
// plays the same match, and notices a frame whose checksum differs.
void TestSpectator() {
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

// Watching with the stream falling behind: low on frames it holds some
// rendered frames (and plays the rest), dry it stalls, and it resumes with a
// quarter of the delay in hand, not all of it.
void TestSpectatorPacing() {
    MockGame game;
    SpectateConfig sc;
    sc.delayFrames = 20;
    SpectatorPlayback spec(game, sc);
    std::string error;
    CHECK(spec.Start(&error));
    std::vector<FrameInputs> in(1);
    int sent = 0;
    auto send = [&](int n) {
        for (int i = 0; i < n; i++, sent++) spec.Add(sent, in, {0});
    };
    send(19);
    spec.Update();
    CHECK(spec.NextFrame() == 0); // the first start waits for the whole delay
    send(1);
    for (int i = 0; i < 9; i++) spec.Update();
    CHECK(spec.NextFrame() == 9 && spec.Held() == 0); // 11 in hand: full speed
    for (int i = 0; i < 12; i++) spec.Update();
    CHECK(spec.Held() > 0 && spec.Stalls() == 0); // under a quarter of the delay: some frames held
    const int played = spec.NextFrame();
    for (int i = 0; i < 40 && spec.Stalls() == 0; i++) spec.Update();
    CHECK(spec.Stalls() == 1 && spec.NextFrame() == 20); // dry
    CHECK(played < 20);
    send(4);
    spec.Update();
    CHECK(spec.NextFrame() == 20); // 4 in hand: not yet
    send(1);
    for (int i = 0; i < 3; i++) spec.Update();
    CHECK(spec.NextFrame() > 20 && spec.Stalls() == 1); // 5 (a quarter of 20): playing again (slowly)
    send(60);
    for (int i = 0; i < 30; i++) spec.Update();
    const int heldBefore = spec.Held();
    for (int i = 0; i < 6; i++) spec.Update();
    CHECK(spec.Held() == heldBefore); // half the delay in hand again: full speed
}
