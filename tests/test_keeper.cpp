// The spectator relay, the keeper connector and the keeper protocol.

#include "test_support.h"

// The keeper's relay (core/spectate_relay.h) between a scripted host and
// viewers: Hello, the host's Loaded for each game, the stream, and the Bye
// once a viewer has seen the set. No game state passes through it.
void TestSpectateRelay() {
    {
        lobby::Message a;
        a.type = lobby::Type::InputsAck;
        a.matchId = 9;
        a.first = 300;
        a.playedTo = 250;
        a.decided = true;
        const auto bytes = lobby::Encode(a);
        const auto back = lobby::Decode(bytes.data(), bytes.size());
        CHECK(back && back->first == 300 && back->playedTo == 250 && back->decided);
        // A build before the relay: no played or decided.
        const auto old = lobby::Decode(bytes.data(), bytes.size() - 5);
        CHECK(old && old->first == 300 && old->playedTo == 0 && !old->decided);
    }
    using lobby::Message;
    using lobby::Type;

    struct Sent {
        uint64_t to;
        Message m;
    };

    std::vector<Sent> up, down;
    RelayConfig cfg;
    cfg.maxViewers = 3;
    cfg.firstTo = 2;
    SpectateRelay relay(cfg, [&](uint64_t to, const Message& m) { up.push_back({to, m}); }, [&](uint64_t to, const Message& m) { down.push_back({to, m}); });
    auto upOf = [&](Type t) {
        std::vector<Message> out;
        for (const Sent& s : up) {
            if (s.m.type == t) out.push_back(s.m);
        }
        return out;
    };
    auto downOf = [&](uint64_t to, Type t) {
        std::vector<Message> out;
        for (const Sent& s : down) {
            if (s.to == to && s.m.type == t) out.push_back(s.m);
        }
        return out;
    };
    int64_t now = 1000;
    auto step = [&](int64_t ms) {
        for (int64_t t = 0; t < ms; t += 10) relay.Step(now += 10);
    };
    const uint64_t kHost = 7, kBuild = 0xABCDu, kGame1 = 100, kGame2 = 200;
    relay.SetHost(kHost, kBuild);
    step(20);
    CHECK(upOf(Type::Hello).size() == 1 && upOf(Type::Hello)[0].build == kBuild && upOf(Type::Hello)[0].role == lobby::Role::Spectator);
    Message accept;
    accept.type = Type::Accept;
    relay.FromHost(accept, now);
    Message state;
    state.type = Type::SessionState;
    state.seats[0].name = "Host";
    state.matchId = kGame1;
    state.phase = lobby::SessionPhase::Match;
    relay.FromHost(state, now);
    Message welcome;
    welcome.type = Type::Welcome;
    welcome.matchId = kGame1;
    relay.FromHost(welcome, now);
    up.clear();
    step(20);
    // A Loaded for the game, until the host's comes back; then the stream.
    auto loads = upOf(Type::Loaded);
    CHECK(loads.size() == 1 && loads[0].matchId == kGame1);
    Message hostLoaded;
    hostLoaded.type = Type::Loaded;
    hostLoaded.matchId = kGame1;
    hostLoaded.stateChecksum = 0x1234;
    relay.FromHost(hostLoaded, now);
    up.clear();
    step(120);
    CHECK(upOf(Type::Loaded).empty());
    CHECK(!upOf(Type::InputsAck).empty() && upOf(Type::InputsAck)[0].matchId == kGame1 && upOf(Type::InputsAck)[0].first == 0);
    Message inputs;
    inputs.type = Type::Inputs;
    inputs.matchId = kGame1;
    // Out of order: 60..119 first, then 0..59.
    inputs.first = 60;
    inputs.frames.resize(60);
    for (size_t i = 0; i < 60; i++) inputs.frames[i].checksum = static_cast<uint32_t>(60 + i);
    relay.FromHost(inputs, now);
    inputs.first = 0;
    for (size_t i = 0; i < 60; i++) inputs.frames[i].checksum = static_cast<uint32_t>(i);
    relay.FromHost(inputs, now);
    up.clear();
    step(120);
    CHECK(!upOf(Type::InputsAck).empty() && upOf(Type::InputsAck).back().first == 120);

    // Viewers: a wrong build is turned away; others watch.
    Message hello;
    hello.type = Type::Hello;
    hello.role = lobby::Role::Spectator;
    hello.build = 0x9999;
    relay.FromViewer(50, hello, now);
    CHECK(downOf(50, Type::Reject).size() == 1 && downOf(50, Type::Reject)[0].reason == lobby::RejectReason::BuildMismatch);
    hello.build = kBuild;
    hello.name = "A";
    relay.FromViewer(50, hello, now);
    CHECK(downOf(50, Type::Accept).size() == 1 && downOf(50, Type::Accept)[0].name == "Host");
    hello.name = "B";
    relay.FromViewer(51, hello, now);
    Message vLoaded;
    vLoaded.type = Type::Loaded;
    vLoaded.matchId = kGame1;
    vLoaded.stateChecksum = 0x1234;
    relay.FromViewer(50, vLoaded, now);
    CHECK(!downOf(50, Type::Loaded).empty() && downOf(50, Type::Loaded)[0].stateChecksum == 0x1234);
    // B's state differs: it gets only the host's checksum (its own driver
    // ends its session), never the host's state.
    vLoaded.stateChecksum = 0x5555;
    relay.FromViewer(51, vLoaded, now);
    CHECK(!downOf(51, Type::Loaded).empty() && downOf(51, Type::Loaded)[0].stateChecksum == 0x1234);
    Message ack;
    ack.type = Type::InputsAck;
    ack.matchId = kGame1;
    ack.first = 0;
    relay.FromViewer(50, ack, now);
    down.clear();
    step(300);
    size_t streamed = 0;
    bool inOrder = true;
    for (const Message& m : downOf(50, Type::Inputs)) {
        for (size_t i = 0; i < m.frames.size(); i++) inOrder &= m.frames[i].checksum == m.first + i;
        streamed += m.frames.size();
    }
    CHECK(streamed == 120 && inOrder);
    CHECK(!downOf(51, Type::SessionState).empty() && downOf(51, Type::SessionState)[0].spectators == 2);

    // A fourth viewer: the relay is full (3). C, the third, gets the set's
    // games with its Accept, and the live game's Welcome only after them.
    hello.name = "C";
    down.clear();
    relay.FromViewer(52, hello, now);
    CHECK(!downOf(52, Type::Matches).empty() && downOf(52, Type::Matches)[0].matches.size() == 1);
    step(300);
    CHECK(downOf(52, Type::Welcome).empty());
    hello.name = "D";
    relay.FromViewer(53, hello, now);
    CHECK(downOf(53, Type::Reject).size() == 1 && downOf(53, Type::Reject)[0].reason == lobby::RejectReason::Busy);

    // Game 1 done, game 2 starts: once game 1 is complete in the host's list
    // the relay moves on to game 2.
    Message list;
    list.type = Type::Matches;
    list.sliceOf = 1;
    lobby::MatchEntry e1;
    e1.id = kGame1;
    e1.frames = 120;
    e1.finished = true;
    e1.winner = 0;
    list.matches = {e1};
    relay.FromHost(list, now);
    welcome.matchId = kGame2;
    relay.FromHost(welcome, now);
    // The host's list (game 1 only) comes again after game 2's Welcome:
    // viewers still see game 1 first.
    relay.FromHost(list, now);
    down.clear();
    step(1100);
    {
        const auto lists = downOf(50, Type::Matches);
        CHECK(!lists.empty() && lists.back().matches.size() == 2 && lists.back().matches[0].id == kGame1 &&
              lists.back().matches[1].id == kGame2);
    }
    up.clear();
    step(600);
    loads = upOf(Type::Loaded);
    CHECK(!loads.empty() && loads.back().matchId == kGame2);
    hostLoaded.matchId = kGame2;
    relay.FromHost(hostLoaded, now);
    step(120);
    inputs.matchId = kGame2;
    inputs.first = 0;
    relay.FromHost(inputs, now);
    // Character select after game 2 began... (the players' vote): A is on
    // game 2 still playing it, B is on game 1. Neither leaves its game;
    // A goes to the select screen once its battle is at the result, and
    // stays there while the players are on it.
    {
        auto phaseOf = [&](uint64_t to) {
            const auto states = downOf(to, Type::SessionState);
            return states.empty() ? lobby::SessionPhase::Lobby : states.back().phase;
        };
        Message aLoaded = vLoaded;
        aLoaded.matchId = kGame2;
        aLoaded.stateChecksum = 0x1234;
        relay.FromViewer(50, aLoaded, now);
        Message aAck;
        aAck.type = Type::InputsAck;
        aAck.matchId = kGame2;
        aAck.first = 60;
        aAck.playedTo = 30;
        relay.FromViewer(50, aAck, now);
        Message select = state;
        select.matchId = kGame2;
        select.phase = lobby::SessionPhase::Lobby;
        relay.FromHost(select, now);
        down.clear();
        step(300);
        CHECK(phaseOf(50) == lobby::SessionPhase::Match);
        CHECK(phaseOf(51) == lobby::SessionPhase::Match);
        aAck.playedTo = 50;
        aAck.decided = true;
        relay.FromViewer(50, aAck, now);
        down.clear();
        step(300);
        CHECK(phaseOf(50) == lobby::SessionPhase::Lobby);
        CHECK(phaseOf(51) == lobby::SessionPhase::Match);
        // On the select screen its acks no longer say decided: still there.
        aAck.decided = false;
        relay.FromViewer(50, aAck, now);
        down.clear();
        step(300);
        CHECK(phaseOf(50) == lobby::SessionPhase::Lobby);
        // The players start the next game: the latch goes.
        select.phase = lobby::SessionPhase::Match;
        relay.FromHost(select, now);
        down.clear();
        step(300);
        CHECK(phaseOf(50) == lobby::SessionPhase::Match);
        // Back to where the test was: A watching game 2 from frame 0.
        vLoaded.matchId = kGame1;
    }
    // P1 wins game 2: the set (first to 2) is decided.
    state.seats[0].wins = 2;
    state.matchId = kGame2;
    relay.FromHost(state, now);
    step(300);
    CHECK(relay.Stats().decided);
    // The same two play on (keeper again): the score starts over and the
    // set end is to come again; then back to where the test was.
    {
        Message next = state;
        next.seats[0].wins = 0;
        relay.SetFirstTo(3);
        relay.FromHost(next, now);
        CHECK(!relay.Stats().decided);
        next.seats[0].wins = 3;
        relay.FromHost(next, now);
        CHECK(relay.Stats().decided);
    }

    // The host leaves. A on game 1 through its end is not done (game 2 was
    // played); on game 2 through its end it is: Bye. B, still on game 1, is not.
    Message bye;
    bye.type = Type::Bye;
    bye.text = "P1 won the set 2-0";
    relay.FromHost(bye, now);
    ack.first = 120;
    relay.FromViewer(50, ack, now);
    down.clear();
    step(300);
    CHECK(downOf(50, Type::Bye).empty());
    vLoaded.matchId = kGame2;
    vLoaded.stateChecksum = 0x1234;
    relay.FromViewer(50, vLoaded, now);
    ack.matchId = kGame2;
    ack.first = 0;
    relay.FromViewer(50, ack, now);
    step(300);
    CHECK(downOf(50, Type::Bye).empty());
    // All of game 2 in hand is not done: A is still playing it.
    ack.first = 60;
    relay.FromViewer(50, ack, now);
    step(600);
    CHECK(downOf(50, Type::Bye).empty());
    // Its battle at the result: done.
    ack.playedTo = 40;
    ack.decided = true;
    relay.FromViewer(50, ack, now);
    step(1600);
    auto byes = downOf(50, Type::Bye);
    CHECK(!byes.empty() && byes[0].text == "P1 won the set 2-0");
    CHECK(downOf(51, Type::Bye).empty());
    CHECK(relay.Stats().viewers == 2 && !relay.Done(now));
    // After the host left, nothing goes up.
    up.clear();
    step(500);
    CHECK(up.empty());
    relay.ViewerGone(51);
    relay.ViewerGone(52);
    step(3100);
    CHECK(relay.Done(now));
}

// The keeper's connects off its loop (keeper/connector.h), with fake clients:
// each request comes back once (a client or a failure), a cancelled one never
// does and its client is freed, retired clients are freed, and whatever is
// left at the end is freed too.
void TestKeeperConnector() {
    struct Fake {
        int n = 0;
    };

    std::atomic<int> made{0}, freed{0}, calls{0};
    auto make = [&]() -> Fake* {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (++calls % 3 == 0) return nullptr; // every third connect fails
        made++;
        return new Fake{made};
    };
    auto destroy = [&](Fake* f) {
        freed++;
        delete f;
    };
    {
        sfv::keeper::Connector<Fake> c(make, destroy, 2);
        std::vector<uint64_t> tickets;
        for (int i = 0; i < 6; i++) tickets.push_back(c.Request());
        CHECK(tickets[0] != 0 && tickets[1] != tickets[0]);
        c.Cancel(tickets[5]); // still queued behind the others: never runs
        std::vector<Fake*> got;
        int over = 0;
        for (int wait = 0; wait < 300 && over < 5; wait++) {
            for (int i = 0; i < 5; i++) {
                Fake* f = nullptr;
                if (tickets[i] && c.Take(tickets[i], &f)) {
                    tickets[i] = 0;
                    over++;
                    if (f) got.push_back(f);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Fake* none = nullptr;
        CHECK(over == 5 && !c.Take(tickets[5], &none) && c.Pending() == 0);
        CHECK(static_cast<int>(got.size()) == made.load() && calls.load() == 5);
        for (Fake* f : got) c.Retire(f);
        for (int wait = 0; wait < 200 && freed.load() < made.load(); wait++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(freed.load() == made.load());
        // Cancelled while it runs: its client is freed when it comes.
        const uint64_t late = c.Request();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        c.Cancel(late);
        for (int wait = 0; wait < 200 && calls.load() < 6; wait++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK(freed.load() == made.load() && c.Pending() == 0);
        // Left for the end: done and never taken.
        c.Request();
        c.Request();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(freed.load() == made.load());
}

void TestKeeperProtocol() {
    using namespace sfv::keeper;
    // Requests: a room's settings round trip; bad ones are refused.
    const Request room = RoomRequest(7, RoomSettings{8, false, RoomTitle{3, {"Z30", "CNL"}}});
    CHECK(FormatRequest(room) == "7:room:8:0:3,Z30,CNL");
    Request back;
    RoomSettings s;
    CHECK(ParseRequest("7:room:8:0:3,Z30,CNL", &back) && back.seq == 7 && back.kind == "room" && back.args.size() == 3);
    CHECK(ParseRoomSettings(back, &s) && s.tables == 8 && !s.isPublic && s.title == (RoomTitle{3, {"Z30", "CNL"}}));
    CHECK(ParseRequest("2:room:4:1:10", &back) && ParseRoomSettings(back, &s) && s.title.id == 10 && s.title.chara[0].empty());
    CHECK(ParseRequest("1:peek", &back) && back.kind == "peek" && back.args.empty());
    for (const char* bad : {"", "room", "0:room", "-1:room", "x:room", "5:", "99999999999:room"}) CHECK(!ParseRequest(bad, &back));
    // Bad settings, and titles the keeper does not offer or that name the
    // wrong characters: refused.
    for (const char* bad : {"1:room:0:1:0", "1:room:9:1:0", "1:room:4:2:0", "1:room:4:1", "1:room:4:1:x", "1:peek:4:1:0", "1:room:4:1:1",
                            "1:room:4:1:2", "1:room:4:1:4", "1:room:4:1:19", "1:room:4:1:-1", "1:room:4:1:3", "1:room:4:1:3,RYU",
                            "1:room:4:1:3,RYU,XXX", "1:room:4:1:3,RYU,KEN,CNL", "1:room:4:1:5,RYU", "1:room:4:1:3,ELV,RYU", "1:room:4:1:3,ryu,KEN"}) {
        CHECK(ParseRequest(bad, &back) && !ParseRoomSettings(back, &s));
    }
    // Titles: the 16 offered (0, 3, 5-18), the keeper's English text with the
    // characters' names, the roster.
    CHECK(TitleId(0) == 0 && TitleId(1) == 3 && TitleId(2) == 5 && TitleId(15) == 18 && TitleId(16) == 0 && TitleIndex(4) == -1);
    CHECK(TitleText(RoomTitle{3, {"Z30", "CNL"}}) == "G user looking for Chun-Li!" && TitleText(RoomTitle{10}) == "Beginners Welcome!" &&
          TitleText(RoomTitle{}).empty() && TitleText(RoomTitle{5}) == "Hardcore match! Main character only!");
    CHECK(CharacterIndex("RYU") == 0 && std::string(CharacterCode(kCharacterCount - 1)) == "Z42" && std::string(CharacterName("VEG")) == "M. Bison" &&
          std::string(CharacterName("ELV")).empty() && CharacterIndex("NDK") == -1);
    for (int i = 0; i < kCharacterCount; i++) CHECK(CharacterIndex(CharacterCode(i)) == i && std::string(CharacterName(CharacterCode(i))).size() > 0);
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
        r.title.id = TitleId(i % kTitleCount);
        if (TitleCharacters(r.title.id)) r.title.chara[0] = CharacterCode(i % kCharacterCount), r.title.chara[1] = CharacterCode((i + 7) % kCharacterCount);
        r.owner = "Player\t" + std::to_string(i) + "\n,\xE3\x81\x82";
        r.members = i % 17;
        r.tables = 1 + i % 8;
        r.openSeats = i % 3 == 0 ? 0 : 2;
        r.rules = "r1 ft2 bo3 t99 ws3";
        r.faces = {"RYU", "KEN", "Z21", "Z39", "CNL", "NSH", "ZGF"};
        r.ownerCountry = i % 4 == 0 ? "" : i % 4 == 1 ? "DE"
                                       : i % 4 == 2   ? "jp"
                                                      : "USA"; // only two capitals go through
        rooms.push_back(r);
    }
    SortRoomList(&rooms);
    CHECK(rooms.front().openSeats > 0 && rooms.front().members < kRoomCap && rooms.back().members == kRoomCap);
    for (size_t i = 1; i < rooms.size(); i++) {
        auto group = [](const ListedRoom& r) { return r.members >= kRoomCap ? 2 : r.openSeats > 0 ? 0
                                                                                                  : 1; };
        CHECK(group(rooms[i - 1]) < group(rooms[i]) || (group(rooms[i - 1]) == group(rooms[i]) && rooms[i - 1].members >= rooms[i].members));
    }
    // The public list in the server's room listing: each room's slots fit
    // Weyvelength's 128 bytes and read back (separators in names dropped);
    // the keeper's signature holds for its room only.
    std::vector<ListedRoom> read;
    for (const ListedRoom& r : rooms) {
        const Listing l = RoomListing(r, 16);
        CHECK(l.size() == 3 && l.at(kListingKind) == "c16");
        for (const auto& [k, v] : l) CHECK(v.size() <= kListingValueMax);
        ListedRoom parsed;
        CHECK(ParseRoomListing(r.code, l, 16, &parsed));
        CHECK(!ParseRoomListing(r.code, l, 17, &parsed) && !ParseRoomListing("", l, 16, &parsed));
        read.push_back(parsed);
    }
    {
        // A room's version goes in its kind: players list their own
        // version's ("c16/0.3.0"); one of no version reads as "".
        ListedRoom r = rooms[0];
        r.version = "0.3.0";
        const Listing l = RoomListing(r, 16);
        CHECK(l.at(kListingKind) == "c16/0.3.0" && ListingKindValue(16, "0.3.0") == "c16/0.3.0");
        ListedRoom parsed;
        CHECK(ParseRoomListing(r.code, l, 16, &parsed) && parsed.version == "0.3.0");
        CHECK(!ParseRoomListing(r.code, l, 17, &parsed));
        CHECK(ParseRoomListing(rooms[0].code, RoomListing(rooms[0], 16), 16, &parsed) && parsed.version.empty());
        // The version is signed with the rest: another one breaks the signature's message.
        Listing other = l;
        other[kListingKind] = "c16/0.3.1";
        CHECK(ListingMessage(r.code, other) != ListingMessage(r.code, l));
    }
    for (size_t i = 0; i < read.size(); i++) {
        CHECK(read[i].code == rooms[i].code && read[i].title == rooms[i].title && read[i].members == rooms[i].members &&
              read[i].tables == rooms[i].tables && read[i].openSeats == rooms[i].openSeats && read[i].rules == rooms[i].rules &&
              read[i].faces.size() == 6 &&
              read[i].owner == "Player" + std::to_string(std::stoi(rooms[i].code.substr(1)) - 10000) + "\xE3\x81\x82" &&
              read[i].ownerCountry == (rooms[i].ownerCountry == "DE" ? "DE" : ""));
    }
    {
        // A name with a bar and the longest one: cut at a whole character.
        ListedRoom r = rooms[1];
        r.owner = "a|b" + std::string(200, 'x') + "\xE3\x81\x82";
        Listing l = RoomListing(r, 16);
        ListedRoom parsed;
        CHECK(l.at(kListingOwner).size() <= kListingValueMax && ParseRoomListing(r.code, l, 16, &parsed) && parsed.owner.rfind("a|b", 0) == 0);
        // Signed by the keeper; checked against the room it was signed for.
        KeyPair key;
        CHECK(GenerateKey(&key));
        const std::string message = ListingMessage(r.code, l);
        uint8_t sig[64];
        CHECK(Sign(key, reinterpret_cast<const uint8_t*>(message.data()), message.size(), sig));
        l[kListingSig] = Base64Encode(sig, sizeof(sig));
        CHECK(l.at(kListingSig).size() <= kListingValueMax);
        auto verify = [&](std::string_view code, const Listing& listing) {
            const std::string m = ListingMessage(code, listing);
            std::vector<uint8_t> got;
            return Base64Decode(listing.at(kListingSig), &got) && got.size() == 64 &&
                   VerifySignature(reinterpret_cast<const uint8_t*>(m.data()), m.size(), got.data(), got.size(), key.pub);
        };
        CHECK(verify(r.code, l));
        CHECK(!verify("OTHER1", l)); // copied onto another room
        Listing forged = l;
        forged[kListingRoom] = "0|1|1|2|r1 ft2|RYU"; // changed after signing
        CHECK(!verify(r.code, forged));
        // Junk slots.
        Listing bad = l;
        bad[kListingRoom] = "0|1|1|2|r1";
        CHECK(!ParseRoomListing(r.code, bad, 16, &parsed));
        bad = l;
        bad[kListingOwner] = "DE no bar";
        CHECK(!ParseRoomListing(r.code, bad, 16, &parsed));
    }
    // The desk's summary.
    {
        DeskSummary d{1234567890123ull, 40, 160}, summary;
        CHECK(FormatDeskSummary(d) == "1234567890123|40|160" && ParseDeskSummary(FormatDeskSummary(d), &summary) && summary == d);
        CHECK(!ParseDeskSummary("x|1|2", &summary) && !ParseDeskSummary("1|2", &summary) && !ParseDeskSummary("", &summary));
        // Per version: each player's own counts.
        DeskSummary v{9, 5, 20, {{"0.3.0", 3, 12}, {"0.2.9", 2, 8}}};
        CHECK(FormatDeskSummary(v) == "9|5|20|0.3.0:3:12,0.2.9:2:8" && ParseDeskSummary(FormatDeskSummary(v), &summary) && summary == v);
        CHECK(summary.For("0.3.0").publicRooms == 3 && summary.For("0.3.0").players == 12 && summary.For("0.3.0").rev == 9);
        CHECK(summary.For("0.4.0").publicRooms == 0 && d.For("0.3.0") == d); // none of its own; a desk of no versions: the totals
        CHECK(!ParseDeskSummary("9|5|20|0.3.0:x:1", &summary));
        Listing desk{{kBuild, "16"}, {kCount, "3"}, {kDeskSummary, "7|1|2"}};
        CHECK(DeskMessage("ABC234", desk) == "sfvr-desk 1 ABC234\n16\n3\n7|1|2");
    }
    CHECK(sfv::FlagCount() == 194 && sfv::FlagAsset("DE") == "DEU" && sfv::FlagAsset("SZ") == "SWZ" && sfv::FlagAsset("ZW") == "ZWE" &&
          sfv::FlagAsset("HK").empty() && sfv::FlagAsset("de").empty() && sfv::FlagAsset("").empty());
    CHECK(CleanCountry("GB") == "GB" && CleanCountry("gb").empty() && CleanCountry("G").empty() && CleanCountry("G\x1F").empty());
    // The rooms API's JSON: the three public totals, every room (not only what
    // fits the lobby's keys), names escaped, broken UTF-8 replaced.
    {
        RoomTotals totals;
        totals.online = 12;
        totals.inPublicRooms = 3;
        totals.publicRooms = 1;
        CHECK(FormatRoomListJson({}, totals, 1700000000123) ==
              "{\"version\":1,\"updated\":1700000000123,\"totals\":{\"online\":12,\"in_public_rooms\":3,\"public_rooms\":1},"
              "\"rooms\":[]}");
        ListedRoom r;
        r.code = "ABC123";
        r.title = RoomTitle{10};
        r.owner = "a\"b\\c\n\xE3\x81\x82\xFF\xC0\xAFz";
        r.members = 3;
        r.tables = 2;
        r.openSeats = 1;
        r.rules = "r1 ft2 bo3 t99 ws3";
        r.faces = {"RYU", "KEN"};
        // Every member with a flag and a face, or null for either.
        r.players = {{"Laptop", "DE", "RYU"}, {"Player 2", "", "KEN"}, {"Deck", "JP", ""}};
        const std::string one = FormatRoomListJson({r}, totals, 1);
        CHECK(one.find("{\"code\":\"ABC123\",\"title\":\"Beginners Welcome!\",\"title_id\":10,\"title_characters\":[],\"owner\":\"a\\\"b\\\\c\\u000a\xE3\x81\x82"
                       "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDz\",\"client_version\":null,\"sfvr_version\":null,\"members\":3,\"capacity\":16,\"tables\":2,\"open_seats\":1,"
                       "\"rules\":\"r1 ft2 bo3 t99 ws3\",\"faces\":[\"RYU\",\"KEN\"],\"players\":["
                       "{\"name\":\"Laptop\",\"country\":\"DE\",\"face\":\"RYU\"},{\"name\":\"Player 2\",\"country\":null,\"face\":\"KEN\"},"
                       "{\"name\":\"Deck\",\"country\":\"JP\",\"face\":null}],\"table_list\":[]}") != std::string::npos);
        // Every table: its rules, state, seats (null open) and score.
        ListedRoom::TableEntry te;
        te.rules = "r1 ft3 bo3 t99 rr";
        te.state = "playing";
        te.seat[0] = {"Laptop", "DE", "RYU"};
        te.wins[0] = 1;
        r.tableList = {te, ListedRoom::TableEntry{"r1 ft2 bo3 t99 ws3", "open"}};
        CHECK(FormatRoomListJson({r}, totals, 1).find("\"table_list\":[{\"table\":1,\"rules\":\"r1 ft3 bo3 t99 rr\",\"state\":\"playing\","
                                                      "\"seats\":[{\"name\":\"Laptop\",\"country\":\"DE\",\"face\":\"RYU\"},null],\"score\":[1,0]},"
                                                      "{\"table\":2,\"rules\":\"r1 ft2 bo3 t99 ws3\",\"state\":\"open\",\"seats\":[null,null],\"score\":[0,0]}]}") !=
              std::string::npos);
        r.tableList.clear();
        // A title naming characters, and none (null: a page shows the owner's).
        r.title = RoomTitle{3, {"Z30", "CNL"}};
        CHECK(FormatRoomListJson({r}, totals, 1).find("\"title\":\"G user looking for Chun-Li!\",\"title_id\":3,\"title_characters\":[\"Z30\",\"CNL\"]") !=
              std::string::npos);
        r.title = RoomTitle{};
        CHECK(FormatRoomListJson({r}, totals, 1).find("\"title\":null,\"title_id\":0,\"title_characters\":[]") != std::string::npos);
        // The room's SFVR version (players list and join only their own's).
        r.version = "0.3.0";
        CHECK(FormatRoomListJson({r}, totals, 1).find("\"client_version\":\"0.3.0\",\"sfvr_version\":\"0.3.0\",\"members\":3") != std::string::npos);
        r.version.clear();
        std::vector<ListedRoom> many;
        for (int i = 0; i < 5; i++) many.insert(many.end(), rooms.begin(), rooms.end());
        const std::string all = FormatRoomListJson(many, totals, 1);
        CHECK(std::count(all.begin(), all.end(), '{') == static_cast<std::ptrdiff_t>(many.size()) + 2);
    }
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
    for (int i = 0; i < 16; i++) detail.people[i].country = i % 3 == 0 ? "" : i % 3 == 1 ? "FR"
                                                                                         : "x\x1F";
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
    CHECK(back2.people[1] == detail.people[1] && back2.people[1].country == "FR" && back2.people[2].country.empty() &&
          back2.people[2].name == detail.people[2].name);
    RoomDetail empty;
    const std::vector<std::string> none = PackRoomDetail(empty);
    int part = 0, of = 0;
    std::string body;
    CHECK(none.size() == 1 && ReadDetailPart(none[0], &part, &of, &body) && body.empty() && JoinRoomDetail({body}, &empty));
    CHECK(!ReadDetailPart("3/2\x1Dtx", &part, &of, &body) && !ReadDetailPart("1/2tx", &part, &of, &body) &&
          !JoinRoomDetail({"xjunk"}, &empty) && !JoinRoomDetail({"pRYU"}, &empty) &&
          !JoinRoomDetail({"pRYU\x1Fname"}, &empty));
    CHECK(FormatRequest(PeekRequest(5, "ABC234")) == "5:peek:ABC234");
    // The name filter: folded (case, digits and symbols as letters, the rest
    // dropped), a word anywhere in the name.
    const std::vector<std::string> words = {FoldName("badword"), FoldName("")};
    CHECK(FoldName("B4.dw0rd-$!") == "badwords" && FoldName("\xE3\x81\x82") == "");
    CHECK(NameRejected("xX_B4D w0rd_Xx", words) && NameRejected("BADWORD", words) && !NameRejected("Bad Wolf", words) &&
          !NameRejected("", words));
    // The word file: comments and blanks skipped, folded, prefixes kept,
    // phrases run together, duplicates and entries of nothing dropped.
    const std::vector<std::string> list = ParseWordFile("# a comment\n\nBadWord\n  bad \r\nDirty*\nrotten egg\nmud*\n\xE3\x81\x82\nbad\n");
    CHECK((list == std::vector<std::string>{"badword", "bad", "dirty*", "rotten_egg", "mud*"}));
    CHECK(WordEntry("Gr8*") == "gr*" && WordEntry(" * ").empty());
    // Names: 5 letters or more anywhere, shorter entries as a whole word.
    CHECK(!NameRejected("Sinbad", list) && NameRejected("the_bad_one", list) && NameRejected("B4D", list) &&
          !NameRejected("Baddie", list) && NameRejected("xxDIRTYxx", list) && NameRejected("Mudkip", list) &&
          !NameRejected("Kmud", list) && NameRejected("RottenEgg99", list) && !NameRejected("\xE3\x81\x82", list));
    // Chat: whole words, starred character for character; phrases; prefixes.
    CHECK(CensorChat("that is BAD, really b4d!", list) == "that is ***, really ***!");
    CHECK(CensorChat("Sinbad badword", list) == "Sinbad *******");
    CHECK(CensorChat("so dirtyyy", list) == "so *******" && CensorChat("so undirty", list) == "so undirty");
    CHECK(CensorChat("a rotten egg here", list) == "a ****** *** here" && CensorChat("rotten apples", list) == "rotten apples");
    CHECK(CensorChat("so bad\xE2\x80\x94", list) == "so ****"); // a dash that is part of the word: one star
    CHECK(CensorChat("b.a.d and b a d pass", list) == "b.a.d and b a d pass");
    CHECK(CensorChat("an al pass", ParseWordFile("anal")) == "an al pass"); // words never run together but a phrase's
    CHECK(CensorChat("all fine", {}) == "all fine" && CensorChat("", list).empty());
    // In room data: values of at most 512 bytes, round trip, the hash.
    std::vector<std::string> many;
    for (int i = 0; i < 400; i++) many.push_back("word" + std::string(1, static_cast<char>('a' + i % 26)) + std::to_string(i));
    const std::vector<std::string> packed = PackWordList(many);
    CHECK(packed.size() > 1 && static_cast<int>(packed.size()) <= kWordListKeys);
    for (const std::string& v : packed) CHECK(v.size() <= 512);
    CHECK(UnpackWordList(packed) == many && PackWordList({}).empty() && UnpackWordList({}).empty());
    CHECK(WordListHash(many) != WordListHash(list) && WordListHash(list) == WordListHash(ParseWordFile("badword\nbad\ndirty*\nrotten egg\nmud*")) &&
          WordListHash(list).size() == 16 && WordListKey(3) == "wl3");
    CHECK(FormatMask({{12, 4821}}) == "12=4821" && ParseMask("12=4821").at(12) == 4821);
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
    // Tables: the state round trips; seats fill from the queue.
    TableState ts;
    ts.phase = TablePhase::Playing;
    ts.seat[0] = 11;
    ts.seat[1] = 22;
    ts.ready[1] = true;
    ts.wins[0] = 1;
    ts.code = "ABC234";
    ts.stayer = 11;
    ts.streak = 2;
    ts.lastWinner = 11;
    ts.lastLoser = 33;
    ts.lastScore[0] = 2;
    ts.call = 41;
    ts.callSeconds = 15;
    TableState tsBack;
    CHECK(ParseTable(FormatTable(ts), &tsBack) && tsBack == ts);
    CHECK(ParseTable(FormatTable(TableState{}), &tsBack) && tsBack == TableState{});
    // The relay room (16th field); a keeper before it wrote 15.
    TableState tw = ts;
    tw.watch = "RLY123";
    CHECK(ParseTable(FormatTable(tw), &tsBack) && tsBack == tw && tsBack.watch == "RLY123");
    // The again mark (17th field); keepers before it wrote 16 or 15.
    tw.again = true;
    CHECK(ParseTable(FormatTable(tw), &tsBack) && tsBack == tw && tsBack.again);
    {
        std::string old16 = FormatTable(ts);
        old16.resize(old16.rfind('|'));
        CHECK(ParseTable(old16, &tsBack) && tsBack == ts && !tsBack.again);
        std::string old15 = old16;
        old15.resize(old15.rfind('|'));
        CHECK(ParseTable(old15, &tsBack) && tsBack == ts && tsBack.watch.empty());
    }
    for (const char* bad : {"", "x|0|0|0|0|0||0|0|0|0|0|0", "o|0|0|0|0|0||0|0|0|0|0", "o|-1|0|0|0|0||0|0|0|0|0|0"}) CHECK(!ParseTable(bad, &tsBack));
    CHECK(FormatIds({5, 7}) == "5,7" && ParseIds("5,7") == std::vector<uint32_t>({5, 7}) && ParseIds("").empty());
    std::vector<uint32_t> q = {1, 2, 3};
    TableState t;
    FillSeats(&t, &q);
    CHECK(t.phase == TablePhase::Call && t.seat[0] == 1 && t.seat[1] == 2 && q == std::vector<uint32_t>({3}));
    // Winner stays, up to 2 sets in a row: 1 beats 2 (stays, 2 to the back),
    // 3 sits; 1 beats 3 (2 sets: goes too, after 3), 2 and 3 sit.
    Rules ws2{2, 3, 99, Rotation::WinnerStays, 2};
    const uint8_t twoOne[2] = {2, 1}, oneTwo[2] = {1, 2};
    ApplySetResult(ws2, 0, twoOne, &t, &q);
    CHECK(t.phase == TablePhase::After && t.seat[0] == 1 && t.seat[1] == 0 && t.stayer == 1 && t.streak == 1 &&
          t.lastWinner == 1 && t.lastLoser == 2 && t.lastScore[0] == 2 && t.lastScore[1] == 1 && q == std::vector<uint32_t>({3, 2}));
    FillSeats(&t, &q);
    CHECK(t.phase == TablePhase::Call && t.seat[0] == 1 && t.seat[1] == 3 && q == std::vector<uint32_t>({2}));
    ApplySetResult(ws2, 0, twoOne, &t, &q);
    CHECK(t.seat[0] == 0 && t.seat[1] == 0 && t.stayer == 0 && q == std::vector<uint32_t>({2, 3, 1}));
    FillSeats(&t, &q);
    CHECK(t.seat[0] == 2 && t.seat[1] == 3 && q == std::vector<uint32_t>({1}));
    // P2 wins: the winner keeps seat 2; a new stayer starts at 1 set.
    ApplySetResult(ws2, 1, oneTwo, &t, &q);
    CHECK(t.seat[0] == 0 && t.seat[1] == 3 && t.stayer == 3 && t.streak == 1 && q == std::vector<uint32_t>({1, 2}));
    // Loser stays (no limit); round robin: both to the back, the loser first.
    Rules ls{2, 3, 99, Rotation::LoserStays, 0}, rr{2, 3, 99, Rotation::RoundRobin, 0};
    TableState l;
    l.seat[0] = 1;
    l.seat[1] = 2;
    std::vector<uint32_t> lq;
    for (int i = 0; i < 5; i++) {
        ApplySetResult(ls, 0, twoOne, &l, &lq);
        FillSeats(&l, &lq);
    }
    CHECK(l.seat[1] == 2 && l.seat[0] == 1 && l.stayer == 2 && l.streak == 5 && lq.empty());
    TableState r;
    r.seat[0] = 1;
    r.seat[1] = 2;
    std::vector<uint32_t> rq = {3};
    ApplySetResult(rr, 0, twoOne, &r, &rq);
    CHECK(r.seat[0] == 0 && r.seat[1] == 0 && r.stayer == 0 && rq == std::vector<uint32_t>({3, 2, 1}));
    // Three players taking turns whoever wins: 3 vs 2 (2 played the last set
    // too: they wait longest, whatever the result), then 1 vs 3, 2 vs 1.
    FillSeats(&r, &rq);
    CHECK(r.seat[0] == 3 && r.seat[1] == 2 && rq == std::vector<uint32_t>({1}));
    ApplySetResult(rr, 1, oneTwo, &r, &rq); // 2 wins
    CHECK(rq == std::vector<uint32_t>({1, 3, 2}));
    FillSeats(&r, &rq);
    CHECK(r.seat[0] == 1 && r.seat[1] == 3);
    ApplySetResult(rr, 0, twoOne, &r, &rq); // 1 wins; 3 played two in a row
    CHECK(rq == std::vector<uint32_t>({2, 1, 3}));
    // Leaving the table: seat or queue.
    TableState lt;
    lt.seat[0] = 4;
    std::vector<uint32_t> ltq = {5, 6};
    CHECK(LeaveTable(4, &lt, &ltq) && !lt.seat[0] && LeaveTable(6, &lt, &ltq) && ltq == std::vector<uint32_t>({5}) &&
          !LeaveTable(9, &lt, &ltq));
    // Tickets.
    TableTicket host{true, 3, "", 0x0123456789abcdefull, "r1 ft2 bo3 t99 ws3"}, join{false, 3, "XYZ234", 42, "r1 ft1 bo1 t30 rr"};
    TableTicket tb;
    CHECK(FormatTicket(host) == "host|3|0123456789abcdef|r1 ft2 bo3 t99 ws3");
    CHECK(ParseTicket(FormatTicket(host), &tb) && tb.host && tb.table == 3 && tb.token == host.token && tb.rules == host.rules);
    CHECK(ParseTicket(FormatTicket(join), &tb) && !tb.host && tb.code == "XYZ234" && tb.token == 42);
    for (const char* bad : {"", "host|3|12|r1 ft2 bo3 t99 ws3", "host|9|0123456789abcdef|r1 ft2 bo3 t99 ws3",
                            "join|1|0123456789abcdef|r1 ft2 bo3 t99 ws3", "host|1|0123456789abcdef|r9"}) {
        CHECK(!ParseTicket(bad, &tb));
    }
    // The next owner: the longest present, never the leaver or the keeper.
    CHECK(NextOwner({5, 9, 3}, 5, 1) == 9 && NextOwner({1, 5, 9}, 9, 1) == 5 && NextOwner({5}, 5, 1) == 0 && NextOwner({}, 0, 1) == 0);
}
