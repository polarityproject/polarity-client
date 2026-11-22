// Updates, migration, shown setups, pick checks, rules, stages and tickets.

#include "test_support.h"

void TestUpdateInfo() {
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
    // "more_files": after "files", marked; updaters before 0.3.0 read only
    // "files" (and refuse a font path there).
    std::string more = doc;
    more.insert(more.rfind('}'), R"(, "more_files": [{"path": "SFVR/fonts/x.ttc", "size": 3, "url": "releases/0.2.0/x.ttc",
        "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}])");
    CHECK(ParseUpdateInfo(more, &info, &error));
    CHECK(info.files.size() == 2 && !info.files[0].more && info.files[1].more && info.files[1].path == "SFVR/fonts/x.ttc");
    std::string moreBad = more;
    moreBad.replace(moreBad.find("SFVR/fonts/x.ttc"), 16, "SFVR/fonts/x.exe");
    CHECK(!ParseUpdateInfo(moreBad, &info, &error));

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

    // Polarity's update info, and where the fonts land from either site.
    std::string polarity = more;
    polarity.replace(polarity.find("\"SFVR\""), 6, "\"Polarity\"");
    polarity.replace(polarity.find("SFVR/fonts/x.ttc"), 16, "Polarity/fonts/x.ttc");
    CHECK(ParseUpdateInfo(polarity, &info, &error) && info.product == "Polarity" && info.files[1].path == "Polarity/fonts/x.ttc");
    std::string other = doc;
    other.replace(other.find("\"SFVR\""), 6, "\"Other\"");
    CHECK(!ParseUpdateInfo(other, &info, &error));
    CHECK(SafeUpdatePath("Polarity/fonts/x.ttc") && !SafeUpdatePath("Polarity/fonts/x.dll") && !SafeUpdatePath("Polarity/logs/x.ttc"));
    CHECK(UpdateFileOnDisk("SFVR/fonts/x.ttc") == "Polarity\\fonts\\x.ttc" && UpdateFileOnDisk("Polarity/fonts/x.ttc") == "Polarity\\fonts\\x.ttc");
    CHECK(UpdateFileOnDisk("polarity.dll") == "polarity.dll");

    CHECK(ResolveUrl("https://sfvr.example/update.json", "releases/a.dll") == "https://sfvr.example/releases/a.dll");
    CHECK(ResolveUrl("https://sfvr.example/x/update.json", "/a.dll") == "https://sfvr.example/a.dll");
    CHECK(ResolveUrl("https://sfvr.example/update.json", "http://other/a.dll") == "http://other/a.dll");
    CHECK(Base64Decode("aGVsbG8=") == std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));
}

namespace {

// The move from SFVR's names (mod/migrate.h), on a folder laid out as an SFVR
// install that just took the Polarity update.
std::wstring MigrateTemp(const wchar_t* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"polarity-migrate-" + name + L"-" + std::to_wstring(GetCurrentProcessId());
    // A run before this one's (same pid is unlikely; clean anyway).
    SHFILEOPSTRUCTW op{};
    std::wstring from = dir + L'\0';
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void Put(const std::wstring& path, const std::string& text) {
    const size_t slash = path.find_last_of(L'\\');
    std::wstring parent = path.substr(0, slash);
    // Parents first (two levels are enough here).
    CreateDirectoryW(parent.substr(0, parent.find_last_of(L'\\')).c_str(), nullptr);
    CreateDirectoryW(parent.c_str(), nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD n = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
}

bool Has(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring IniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t v[256] = {};
    GetPrivateProfileStringW(section, key, L"(none)", v, 256, path.c_str());
    return v;
}

} // namespace

void TestMigrateFromSfvr() {
    using sfv::mod::MigrateFromSfvr;
    SetEnvironmentVariableW(L"POLARITY_LOADER", L"1"); // as Polarity's loader sets it
    {
        // The usual case: everything moves, SFVR's DLL goes.
        const std::wstring d = MigrateTemp(L"plain");
        Put(d + L"\\sfv_rollback.ini", "[mod]\r\nreport_dir=SFVR\\matches\r\n[update]\r\nurl=https://sfvr.pages.dev/update.json\r\n[weyve]\r\nname=POLARITYDEV\r\n");
        Put(d + L"\\sfv_rollback.1.ini", "[weyve]\r\nname=TWO\r\n");
        Put(d + L"\\sfv_rollback.panel.ini", "[report]\r\nnotice_shown=1\r\n");
        Put(d + L"\\sfv_rollback.panel.1.ini", "[report]\r\nnotice_shown=1\r\n");
        Put(d + L"\\SFVR\\logs\\sfvr.log", "SFVR log started 2026-11-10 12:00:00\n");
        Put(d + L"\\SFVR\\logs\\sfvr.1.log", "SFVR log started 2026-11-10 12:00:01\n");
        Put(d + L"\\SFVR\\logs\\sfvr_2026-11-09_10-00-00.log", "SFVR log started 2026-11-09 10:00:00\n");
        Put(d + L"\\SFVR\\matches\\a_p1.sfvr", "SFVR");
        Put(d + L"\\SFVR\\replays\\r.sfvr", "SFVR");
        Put(d + L"\\SFVR\\fonts\\NotoSansCJK-Medium.ttc", "font");
        Put(d + L"\\SFVR\\SFVR-logs-2026-11-09.zip", "zip");
        Put(d + L"\\polarity.dll", "new");
        Put(d + L"\\sfv_rollback.dll", "old");
        Put(d + L"\\sfv_rollback.dll.1.old", "older");
        Put(d + L"\\sfv_rollback.live.0.3.dll", "copy");
        Put(d + L"\\sfvr_fonts_license.txt", "license");
        Put(d + L"\\polarity_fonts_license.txt", "license");
        const auto r = MigrateFromSfvr(d);
        CHECK(r.moved && !r.waited);
        CHECK(Has(d + L"\\polarity.ini") && !Has(d + L"\\sfv_rollback.ini"));
        CHECK(IniValue(d + L"\\polarity.ini", L"update", L"url").empty() && IniValue(d + L"\\polarity.ini", L"mod", L"report_dir").empty());
        CHECK(IniValue(d + L"\\polarity.ini", L"weyve", L"name") == L"POLARITYDEV");
        CHECK(Has(d + L"\\polarity.1.ini") && Has(d + L"\\polarity.panel.ini") && Has(d + L"\\polarity.panel.1.ini"));
        CHECK(!Has(d + L"\\SFVR") && Has(d + L"\\Polarity\\matches\\a_p1.sfvr") && Has(d + L"\\Polarity\\replays\\r.sfvr"));
        CHECK(Has(d + L"\\Polarity\\fonts\\NotoSansCJK-Medium.ttc"));
        CHECK(Has(d + L"\\Polarity\\logs\\polarity.log") && Has(d + L"\\Polarity\\logs\\polarity.1.log") &&
              Has(d + L"\\Polarity\\logs\\polarity_2026-11-09_10-00-00.log") && !Has(d + L"\\Polarity\\logs\\sfvr.log"));
        CHECK(Has(d + L"\\Polarity\\Polarity-logs-2026-11-09.zip"));
        CHECK(!Has(d + L"\\sfv_rollback.dll") && !Has(d + L"\\sfv_rollback.dll.1.old") && !Has(d + L"\\sfv_rollback.live.0.3.dll"));
        CHECK(!Has(d + L"\\sfvr_fonts_license.txt") && Has(d + L"\\polarity_fonts_license.txt") && Has(d + L"\\polarity.dll"));
        // Again: nothing left to do.
        const auto again = MigrateFromSfvr(d);
        CHECK(!again.moved && again.lines.empty());
    }
    {
        // Both names already there: nothing is overwritten, the rest moves,
        // and SFVR\ keeps a note of what stayed. No polarity.dll: SFVR's stays.
        const std::wstring d = MigrateTemp(L"both");
        Put(d + L"\\sfv_rollback.ini", "[weyve]\r\nname=OLD\r\n");
        Put(d + L"\\polarity.ini", "[weyve]\r\nname=NEW\r\n");
        Put(d + L"\\sfv_rollback.dll", "old");
        Put(d + L"\\SFVR\\matches\\a.sfvr", "old a");
        Put(d + L"\\SFVR\\matches\\b.sfvr", "b");
        Put(d + L"\\SFVR\\replays\\r.sfvr", "r");
        Put(d + L"\\Polarity\\matches\\a.sfvr", "new a");
        // The same file in both (the font an update put in Polarity\fonts): no conflict.
        Put(d + L"\\SFVR\\fonts\\x.ttc", "font");
        Put(d + L"\\Polarity\\fonts\\x.ttc", "font");
        const auto r = MigrateFromSfvr(d);
        CHECK(r.moved);
        CHECK(Has(d + L"\\sfv_rollback.ini") && IniValue(d + L"\\polarity.ini", L"weyve", L"name") == L"NEW");
        CHECK(Has(d + L"\\sfv_rollback.dll"));
        CHECK(Has(d + L"\\Polarity\\matches\\b.sfvr") && Has(d + L"\\Polarity\\replays\\r.sfvr") && !Has(d + L"\\SFVR\\replays"));
        CHECK(Has(d + L"\\SFVR\\matches\\a.sfvr") && Has(d + L"\\SFVR\\Moved to Polarity.txt"));
        CHECK(!Has(d + L"\\SFVR\\fonts") && Has(d + L"\\Polarity\\fonts\\x.ttc"));
        // Once the player moves it away, the next start finishes.
        DeleteFileW((d + L"\\SFVR\\matches\\a.sfvr").c_str());
        const auto again = MigrateFromSfvr(d);
        CHECK(!Has(d + L"\\SFVR"));
        (void)again;
    }
    {
        // A file in use (Explorer, another program): the folder cannot be
        // renamed, everything else moves, and the next start takes the file.
        const std::wstring d = MigrateTemp(L"busy");
        Put(d + L"\\SFVR\\logs\\held.log", "held");
        Put(d + L"\\SFVR\\matches\\m.sfvr", "m");
        HANDLE held = CreateFileW((d + L"\\SFVR\\logs\\held.log").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(held != INVALID_HANDLE_VALUE);
        const auto r = MigrateFromSfvr(d);
        CHECK(r.moved && Has(d + L"\\Polarity\\matches\\m.sfvr") && Has(d + L"\\SFVR\\logs\\held.log"));
        CHECK(Has(d + L"\\SFVR\\Moved to Polarity.txt"));
        CloseHandle(held);
        const auto again = MigrateFromSfvr(d);
        CHECK(again.moved && Has(d + L"\\Polarity\\logs\\held.log") && !Has(d + L"\\SFVR"));
    }
    {
        // SFVR's loader (no POLARITY_LOADER): it loads sfv_rollback.dll only, so that stays.
        const std::wstring d = MigrateTemp(L"oldloader");
        Put(d + L"\\polarity.dll", "new");
        Put(d + L"\\sfv_rollback.dll", "old");
        SetEnvironmentVariableW(L"POLARITY_LOADER", nullptr);
        MigrateFromSfvr(d);
        SetEnvironmentVariableW(L"POLARITY_LOADER", L"1");
        CHECK(Has(d + L"\\sfv_rollback.dll"));
    }
    {
        // Nothing of SFVR's: nothing happens.
        const std::wstring d = MigrateTemp(L"fresh");
        Put(d + L"\\polarity.dll", "new");
        const auto r = MigrateFromSfvr(d);
        CHECK(!r.moved && r.lines.empty());
    }
}

void TestShownSetup() {
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

void TestPickCheck() {
    // Ryu (index 0) has 20 costumes: costume 0 offers colors 0..9 and EX
    // colors 100 and 101 and has a dress variant, costume 5 is not offered,
    // costume 15 is the silver model Eleven plays in. X01 (index 1) is a
    // helper. Ryu has three V-Comments; skill names are not loaded (two of
    // each). Stages 0..9 are playable.
    PickTables t;
    t.charaIndex = [](const std::string& code) { return code == "RYU" ? 0 : code == "X01" ? 1
                                                                                          : -1; };
    t.isFighter = [](int index) { return index == 0; };
    t.costumeCount = [](int) { return 20; };
    t.colorCount = [](int, int costume) { return costume == 5 || costume == 15 ? 0 : 10; };
    t.colorIds = [](int, int) {
        std::vector<int> ids;
        for (int i = 0; i < 10; i++) ids.push_back(i);
        ids.push_back(100);
        ids.push_back(101);
        return ids;
    };
    t.isEleven = [](const PlayerSetup& p) { return p.costume == 15; };
    t.skillCount = [](const std::string&, bool) { return size_t{0}; };
    bool commentsLoaded = true;
    t.commentCount = [&commentsLoaded](const std::string&) { return commentsLoaded ? std::optional<size_t>(3) : std::nullopt; };
    bool variantsKnown = true;
    t.variants = [&variantsKnown](int, int costume) { return variantsKnown ? std::optional<uint8_t>(static_cast<uint8_t>(costume == 0 ? 1 : 0)) : std::nullopt; };
    t.isPlayableStage = [](int stage) { return stage >= 0 && stage < 10; };
    PlayerSetup ryu;
    ryu.character = PackCode("RYU");
    const auto with = [&](auto change) {
        PlayerSetup p = ryu;
        change(p);
        return PickCheck(p, t).why;
    };
    CHECK(PickCheck(ryu, t).Fine());
    CHECK(with([](PlayerSetup& p) { p.color = 101; p.vskill = 1; p.vtrigger = 1; p.comment = 3; p.variation = 1; }).empty());
    CHECK(with([](PlayerSetup& p) { p.costume = 15; p.color = 1; }).empty());
    CHECK(with([](PlayerSetup& p) { p.character = 0; }) == "no fighter (none)");
    CHECK(with([](PlayerSetup& p) { p.character = PackCode("X01"); }) == "no fighter X01");
    CHECK(with([](PlayerSetup& p) { p.character = 0x00FF0A41; }) == "no fighter A??");
    CHECK(with([](PlayerSetup& p) { p.costume = 20; }) == "no RYU costume 20");
    CHECK(with([](PlayerSetup& p) { p.costume = 0xFFFF; }) == "no RYU costume 65535");
    CHECK(with([](PlayerSetup& p) { p.costume = 5; }) == "RYU costume 5 is not offered");
    CHECK(with([](PlayerSetup& p) { p.color = 10; }) == "RYU costume 0 has no color id 10");
    CHECK(with([](PlayerSetup& p) { p.color = 255; }) == "RYU costume 0 has no color id 255");
    CHECK(with([](PlayerSetup& p) { p.costume = 15; p.color = 2; }) == "RYU costume 15 has no color id 2");
    CHECK(with([](PlayerSetup& p) { p.vskill = 2; }) == "RYU has no V-Skill 3");
    CHECK(with([](PlayerSetup& p) { p.vtrigger = 255; }) == "RYU has no V-Trigger 256");
    CHECK(with([](PlayerSetup& p) { p.comment = 4; }) == "RYU has no V-Comment 4");
    CHECK(with([](PlayerSetup& p) { p.variation = 2; }) == "RYU costume 0 has no variant 2");
    CHECK(with([](PlayerSetup& p) { p.costume = 1; p.variation = 1; }) == "RYU costume 1 has no variant 1");
    // Setups: both picks, then the stage (never random: the one to play).
    MatchSetup s;
    s.players[0] = s.players[1] = ryu;
    s.stage = 9;
    CHECK(SetupCheck(s, t).Fine());
    s.stage = lobby::kAnyStage;
    CHECK(SetupCheck(s, t).Fine());
    s.stage = 10;
    CHECK(SetupCheck(s, t).why == "no stage 10");
    s.stage = lobby::kRandomStage;
    CHECK(SetupCheck(s, t).why == "no stage 65534");
    s.stage = 0;
    s.players[1].comment = 9;
    CHECK(SetupCheck(s, t).why == "P2: RYU has no V-Comment 9");
    // The host's stage rule may be random.
    CHECK(StageRuleCheck(lobby::kRandomStage, t).Fine() && StageRuleCheck(lobby::kAnyStage, t).Fine() && StageRuleCheck(3, t).Fine());
    CHECK(StageRuleCheck(10, t).why == "no stage 10");
    // badpick's values fail the check in each message that carries a pick.
    CHECK(!BadPickParse("nope", "") && !BadPickParse("color", "nope") && BadPickParse("color", "welcome"));
    for (const char* field : {"character", "costume", "color", "vskill", "vtrigger", "comment", "variant", "stage"}) {
        lobby::Message welcome;
        welcome.type = lobby::Type::Welcome;
        welcome.setup.players[0] = welcome.setup.players[1] = ryu;
        CHECK(MessageCheck(welcome, t).Fine());
        BadPickApply(*BadPickParse(field, ""), welcome);
        CHECK(MessageCheck(welcome, t).kind == PickVerdict::Kind::Bad);
        lobby::Message state;
        state.type = lobby::Type::SessionState;
        state.stage = 3;
        state.seats[0].pick = ryu;
        CHECK(MessageCheck(state, t).Fine());
        BadPickApply(*BadPickParse(field, "session"), state);
        CHECK(MessageCheck(state, t).kind == PickVerdict::Kind::Bad);
        lobby::Message matches;
        matches.type = lobby::Type::Matches;
        matches.matches.resize(3);
        for (lobby::MatchEntry& e : matches.matches) e.setup = welcome.setup;
        matches.matches[0].setup.players[0] = matches.matches[0].setup.players[1] = ryu;
        matches.matches[0].setup.stage = 0;
        CHECK(MessageCheck(matches, t).kind == PickVerdict::Kind::Bad);
    }
    // A V-Comment list still loading: a pick with Auto passes, another waits;
    // a bad value elsewhere in the pick is still refused.
    commentsLoaded = false;
    CHECK(with([](PlayerSetup&) {}).empty() && PickCheck(ryu, t).Fine());
    PlayerSetup commented = ryu;
    commented.comment = 2;
    CHECK(PickCheck(commented, t).kind == PickVerdict::Kind::NotYet);
    commented.color = 99;
    CHECK(PickCheck(commented, t).why == "RYU costume 0 has no color id 99");
    MatchSetup waiting;
    waiting.players[0] = ryu;
    waiting.players[1] = ryu;
    waiting.players[1].comment = 1;
    CHECK(SetupCheck(waiting, t).kind == PickVerdict::Kind::NotYet);
    waiting.stage = 10;
    CHECK(SetupCheck(waiting, t).why == "no stage 10");
    commentsLoaded = true;
    // Variants not asked yet: a pick without one passes, one with a variant
    // waits.
    variantsKnown = false;
    CHECK(PickCheck(ryu, t).Fine());
    PlayerSetup varied = ryu;
    varied.variation = 1;
    CHECK(PickCheck(varied, t).kind == PickVerdict::Kind::NotYet);
    variantsKnown = true;
    // Only the message asked for.
    lobby::Message beat;
    beat.type = lobby::Type::PlayerState;
    beat.seat.pick = ryu;
    BadPickApply(*BadPickParse("color", "welcome"), beat);
    CHECK(beat.seat.pick == ryu && MessageCheck(beat, t).Fine());
    // A seat with no pick yet, and messages without picks, pass.
    beat.seat.pick = {};
    CHECK(MessageCheck(beat, t).Fine());
    lobby::Message inputs;
    inputs.type = lobby::Type::Inputs;
    inputs.setup.stage = 10;
    CHECK(MessageCheck(inputs, t).Fine());
}

void TestRulesFormat() {
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

void TestRandomStage() {
    // Random never lands on Flamenco Tavern, Skies of Honor or Kanzuki Beach,
    // nor on a setting of theirs; every other stage still comes up.
    const std::vector<RandomStageChoice> stages = {
        {3, "CHN", ""},
        {14, "KPB", ""},
        {15, "DBI", ""},
        {17, "SX1", ""},
        {10, "TRN", ""},
        {67, "S25", "TRN"},
        {70, "X01", "SX1"},
    };
    std::map<int, int> seen;
    std::mt19937_64 rng{1234};
    for (int i = 0; i < 10000; i++) seen[PickRandomStage(stages, rng())]++;
    CHECK(seen.size() == 3 && seen.count(3) && seen.count(10) && seen.count(67));
    for (int never : {14, 15, 17, 70, -1}) CHECK(!seen.count(never));
    // The same roll, the same stage (the host draws once, and sends it).
    CHECK(PickRandomStage(stages, 7) == PickRandomStage(stages, 7));
    // Nothing left: -1 (the game keeps the stage it has prepared).
    CHECK(PickRandomStage({{14, "KPB", ""}, {15, "DBI", ""}}, 5) == -1);
    CHECK(PickRandomStage({}, 5) == -1);
    CHECK(IsNeverRandomStage("SX1") && IsNeverRandomStage("DBI") && IsNeverRandomStage("KPB") && !IsNeverRandomStage("") && !IsNeverRandomStage("CHN"));

    // A player's list leaves variants out by their own code: The Grid off
    // keeps The Grid Alternative.
    seen.clear();
    for (int i = 0; i < 2000; i++) seen[PickRandomStage(stages, rng(), {"CHN", "TRN"})]++;
    CHECK(seen.size() == 1 && seen.count(67));
    // Leaving everything out: every stage but the never-random ones again.
    seen.clear();
    for (int i = 0; i < 2000; i++) seen[PickRandomStage(stages, rng(), {"CHN", "TRN", "S25"})]++;
    CHECK(seen.size() == 3 && seen.count(3) && seen.count(10) && seen.count(67));
    // A never-random stage stays out whatever the list says.
    seen.clear();
    for (int i = 0; i < 2000; i++) seen[PickRandomStage(stages, rng(), {"CHN", "S25"})]++;
    CHECK(seen.size() == 1 && seen.count(10));

    // The ini's form: sorted, each once, junk dropped.
    CHECK(FormatStageMask({"S25", "CHN", "S25"}) == "CHN,S25");
    CHECK(FormatStageMask({}).empty());
    CHECK(ParseStageMask("chn, S25,,TRN") == (std::vector<std::string>{"CHN", "S25", "TRN"}));
    CHECK(ParseStageMask("CHNX,C,C-N,,  ").empty());
    CHECK(ParseStageMask(FormatStageMask({"TRN", "AFB", "SH4"})) == (std::vector<std::string>{"AFB", "SH4", "TRN"}));
}

void TestMatchTicket() {
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

void TestText() {
    CHECK(std::string(TR(Str::fight_leave_stay)) == "Stay");
    CHECK(TRF(Str::fight_vote_of_set, 3) == "of 3 to win the set");
    CHECK(TRF(Str::fight_vote_delay_frames, 12) == "12 frames");
    CHECK(std::string(TR(Str::fight_vote_p1_you)) == "P1 \xC2\xB7 YOU");
    TextSetLanguage(kLanguageCount + 4);
    CHECK(TextLanguage() == 0 && std::string(TR(Str::fight_leave_stay)) == "Stay");
}
