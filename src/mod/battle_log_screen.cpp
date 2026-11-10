#include "mod/battle_log_screen.h"
#include "mod/frame_meter.h"
#include "mod/hitbox_display.h"

#include "core/build_id.h"
#include "core/battle_log.h"
#include "core/display_name.h"
#include "mod/files.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/replay_driver.h"
#include "mod/ui_kit.h"
#include "mod/ui_sound.h"

#include <imgui.h>
#include <imgui_internal.h> // ImTextCharToUtf8

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace sfv::mod {
namespace {

using namespace ui;
using battlelog::Entry;

// ---- the files, read on a worker -------------------------------------------

std::mutex g_dataMutex;
std::vector<Entry> g_entries;     // all files' headers, as read
std::map<std::string, Entry> g_cache; // by path: kept while size and time are the same
std::atomic<bool> g_reading{false};
std::atomic<bool> g_read{false};  // read at least once
std::atomic<int> g_generation{0}; // bumped when a read finishes

void ReadFolder()
{
    if (g_reading.exchange(true)) return;
    std::thread([] {
        ThreadGuard running;
        const std::wstring dir = ReplaysDir();
        std::vector<Entry> found;
        std::map<std::string, Entry> cache;
        {
            std::lock_guard lock(g_dataMutex);
            cache = g_cache;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.sfvr").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::string path = Utf8Path(dir + L"\\" + fd.cFileName);
                const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                const int64_t modified = (static_cast<int64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                auto it = cache.find(path);
                if (it != cache.end() && it->second.size == size && it->second.modified == modified) {
                    found.push_back(it->second);
                    continue;
                }
                Entry e;
                e.path = path;
                e.size = size;
                e.modified = modified;
                std::string error;
                if (!LoadReplayHeader(path, &e.header, &error)) continue;
                found.push_back(std::move(e));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        {
            std::lock_guard lock(g_dataMutex);
            g_cache.clear();
            for (const Entry& e : found) g_cache[e.path] = e;
            g_entries = std::move(found);
        }
        Log("battle log: %zu files read in %s", g_cache.size(), Utf8Path(dir).c_str());
        g_read = true;
        g_generation++;
        g_reading = false;
    }).detach();
}

// ---- the screen's state ------------------------------------------------------

std::atomic<bool> g_open{false};
bool g_bySets = false;           // the view: false the timeline (the default), true by set
int g_focus = 0;                 // the focused row (selectable rows only)
float g_scroll = 0;              // the list's scroll, design units
uint64_t g_expanded = 0;         // the set shown open (Sets view)
battlelog::Filter g_filter;
enum class LogModal { None, Characters, Opponent };
LogModal g_modal = LogModal::None;
int g_modalSide = 0;             // characters: 0 the player's list, 1 the opponent's
int g_modalRow[2] = {0, 0};
int g_modalScroll[2] = {0, 0};
std::string g_search;            // opponent: typed
bool g_typing = false;
int g_seenGeneration = -1;
std::vector<Entry> g_merged;     // the screen's copy (Merge)

constexpr float kListX0 = 48, kListX1 = 868, kListY0 = 142, kListY1 = 832;
constexpr float kDetailX0 = 892, kDetailX1 = 1552;
// The rows: 10 inside the header strip (kListY0 + 32) and the pane's 2 px
// border on every side but the scroll bar's.
constexpr float kRowsTop = kListY0 + 42, kRowsBottom = kListY1 - 12;

const ImU32 kCream = look::kCream;
const ImU32 kMuted = look::kMuted;
const ImU32 kSoft = look::kSoft;
const ImU32 kGold = look::kGold;

std::string Upper2(const std::string& s) { return Upper(core::DisplayName(s)); }

std::tm LocalTime(uint64_t unix)
{
    const std::time_t t = static_cast<std::time_t>(unix);
    std::tm tm{};
    localtime_s(&tm, &t);
    return tm;
}

std::string Clock(uint64_t unix)
{
    const std::tm tm = LocalTime(unix);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return b;
}

const char* const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
const char* const kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// "TODAY · NOV 8", "YESTERDAY · NOV 7", "WED · NOV 5".
std::string DayLabel(uint64_t unix)
{
    const std::tm tm = LocalTime(unix);
    const std::tm now = LocalTime(static_cast<uint64_t>(std::time(nullptr)));
    const std::tm yesterday = LocalTime(static_cast<uint64_t>(std::time(nullptr) - 86400));
    std::string head = kDays[tm.tm_wday];
    if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday) head = "TODAY";
    else if (tm.tm_year == yesterday.tm_year && tm.tm_yday == yesterday.tm_yday) head = "YESTERDAY";
    char b[48];
    std::snprintf(b, sizeof(b), "%s \xC2\xB7 %s %d", head.c_str(), kMonths[tm.tm_mon], tm.tm_mday);
    return b;
}

int DayKey(uint64_t unix)
{
    const std::tm tm = LocalTime(unix);
    return tm.tm_year * 400 + tm.tm_yday;
}

// The list's rows.
enum class RowKind { Day, Match, SetHeader, Game };
struct Row {
    RowKind kind;
    const Entry* entry = nullptr;        // Match, Game: the match; SetHeader: its last game
    const battlelog::Set* set = nullptr; // SetHeader, Game
    std::string day;
    float y = 0, h = 0;
    int game = 0; // Game: its number in the set
};

float RowHeight(RowKind k) { return k == RowKind::Day ? 30 : k == RowKind::SetHeader ? 86 : k == RowKind::Game ? 54 : 60; }

// A character's face (the select grid's icon), mirrored for the right side.
void Face(ImDrawList* d, uint32_t chara, float x, float y, float s, bool mirror)
{
    d->AddRectFilled(g_v.P(x, y), g_v.P(x + s, y + s), Rgba(20, 22, 27));
    if (chara) {
        // The character select's grid icon (116 x 144), its top square: the
        // HUD portrait (FaceIconLeft) is tinted blue for the fight.
        if (const TextureView* t = TextureSized(CharaTex(UnpackCode(chara), "CharaSelectIconLeft"), g_v.L(s))) {
            const float v1 = 116.0f / 144.0f;
            Image(d, t, g_v.P(x, y), g_v.P(x + s, y + s), mirror ? ImVec2(1, 0) : ImVec2(0, 0), mirror ? ImVec2(0, v1) : ImVec2(1, v1),
                  FadeIn(t));
        }
    }
    d->AddRect(g_v.P(x, y), g_v.P(x + s, y + s), Rgba(236, 228, 206, 77), 0, 0, std::max(1.0f, g_v.L(1)));
}

// Text cut with an ellipsis to `room` design units.
std::string Fit(ImFont* f, float size, const std::string& t, float room)
{
    if (TextW(f, size, t) <= room) return t;
    std::string s = t;
    while (!s.empty() && TextW(f, size, s + "...") > room) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    }
    return s + "...";
}

// A match's line: [face] NAME  2 - 1  NAME [face] ... time. Its own side's
// name first? No: P1 on the left, as the game shows matches.
void MatchLine(ImDrawList* d, const HubFonts& f, const Entry& e, float x0, float x1, float y, float h, bool on, float face, const std::string& lead,
               const std::string& right)
{
    ImFont* bold = f.bold ? f.bold : f.strong;
    const ImU32 fg = on ? look::kInk : kCream, dim = on ? look::kInkSoft : kMuted;
    const MatchSummary& s = e.header.summary;
    float x = x0;
    if (!lead.empty()) {
        Text(d, bold, g_v.L(16), g_v.P(x, y + h / 2 - 10), dim, lead);
        x += 34;
    }
    const float mid = (x + face + x1 - 70 - face) / 2;
    const float cy = y + (h - face) / 2;
    Face(d, e.header.setup.players[0].character, x, cy, face, false);
    const float nameRoom = mid - 60 - (x + face + 10) - 8;
    Text(d, bold, g_v.L(19), g_v.P(x + face + 10, y + h / 2 - 12), fg, Fit(bold, 19, Upper2(battlelog::NameOf(e, 0)), nameRoom));
    // The round wins, the winner's bright.
    if (s.present) {
        const int w0 = s.roundWins[0], w1 = s.roundWins[1];
        Text(d, f.display, g_v.L(22), g_v.P(mid - 20, y + h / 2 - 14), w0 > w1 ? fg : dim, std::to_string(w0), 1);
        Text(d, f.display, g_v.L(22), g_v.P(mid, y + h / 2 - 14), dim, "-", 0.5f);
        Text(d, f.display, g_v.L(22), g_v.P(mid + 20, y + h / 2 - 14), w1 > w0 ? fg : dim, std::to_string(w1), 0);
    }
    const float right1 = x1 - 70;
    Face(d, e.header.setup.players[1].character, right1 - face, cy, face, true);
    Text(d, bold, g_v.L(19), g_v.P(right1 - face - 10, y + h / 2 - 12), fg, Fit(bold, 19, Upper2(battlelog::NameOf(e, 1)), nameRoom), 1);
    Text(d, f.body, g_v.L(16), g_v.P(x1, y + h / 2 - 10), dim, right, 1);
}

std::string CharaLabelOf(const BattleLogLive& live, uint32_t c)
{
    if (!c) return "Any";
    const std::string n = live.charaName ? live.charaName(c) : std::string();
    return n.empty() ? UnpackCode(c) : n;
}

} // namespace

void OpenBattleLog()
{
    g_open = true;
    g_modal = LogModal::None;
    g_typing = false;
    ReadFolder();
    Log("battle log: open");
}

void CloseBattleLog()
{
    g_open = false;
    g_typing = false;
}

bool BattleLogOpen() { return g_open; }

void RefreshBattleLog() { ReadFolder(); }

std::string BattleLogInfo()
{
    if (!g_read) {
        static bool asked = false;
        if (!asked) {
            asked = true;
            ReadFolder();
        }
        return "";
    }
    std::lock_guard lock(g_dataMutex);
    std::map<uint64_t, int> ids;
    size_t n = 0;
    for (const Entry& e : g_entries) {
        if (e.header.matchId && ids[e.header.matchId]++) continue;
        n++;
    }
    return n == 0 ? "No matches yet" : std::to_string(n) + (n == 1 ? " match" : " matches");
}

void DrawBattleLog(const HubFonts& f, const BattleLogLive& live, bool takeInput, BattleLogInput* in)
{
    *in = BattleLogInput{};
    BeginScreen();
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    Backdrop(d);
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput && !g_typing;
    ImFont* bold = f.bold ? f.bold : f.strong;

    // The data: merged when a read finished.
    if (g_seenGeneration != g_generation) {
        g_seenGeneration = g_generation;
        std::vector<Entry> all;
        {
            std::lock_guard lock(g_dataMutex);
            all = g_entries;
        }
        g_merged = battlelog::Merge(std::move(all), live.myName);
    }
    std::vector<const Entry*> shown;
    for (const Entry& e : g_merged) {
        if (battlelog::Passes(e, g_filter, live.myName)) shown.push_back(&e);
    }
    const std::vector<battlelog::Set> sets = battlelog::GroupSets(shown);

    // The rows.
    std::vector<Row> rows;
    int lastDay = -1;
    auto day = [&](uint64_t unix) {
        const int k = DayKey(unix);
        if (k == lastDay) return;
        lastDay = k;
        Row r{RowKind::Day};
        r.day = DayLabel(unix);
        rows.push_back(r);
    };
    if (!g_bySets) {
        for (const Entry* e : shown) {
            day(e->header.createdUnix);
            Row r{RowKind::Match, e};
            rows.push_back(r);
        }
    }
    else {
        for (const battlelog::Set& s : sets) {
            const Entry* last = s.games.back();
            day(last->header.createdUnix);
            if (s.games.size() == 1 && !s.key) {
                Row r{RowKind::Match, last};
                rows.push_back(r);
                continue;
            }
            Row h{RowKind::SetHeader, last, &s};
            rows.push_back(h);
            if (g_expanded == s.key) {
                int n = 0;
                for (const Entry* g : s.games) {
                    Row r{RowKind::Game, g, &s};
                    r.game = g->header.summary.setGame ? g->header.summary.setGame : ++n;
                    rows.push_back(r);
                }
            }
        }
    }
    float y = 0;
    std::vector<int> selectable;
    for (size_t i = 0; i < rows.size(); i++) {
        // The first day is the list's header strip's (topDay): no row of
        // its own, so the first match sits as far from the strip as from
        // the pane's sides.
        const bool headerDay = i == 0 && rows[i].kind == RowKind::Day;
        rows[i].h = headerDay ? 0 : RowHeight(rows[i].kind);
        rows[i].y = y;
        y += headerDay ? 0 : rows[i].h + (rows[i].kind == RowKind::Day ? 2 : 6);
        if (rows[i].kind != RowKind::Day) selectable.push_back(static_cast<int>(i));
    }
    const float total = y;
    g_focus = selectable.empty() ? 0 : std::clamp(g_focus, 0, static_cast<int>(selectable.size()) - 1);
    const Row* focused = selectable.empty() ? nullptr : &rows[selectable[g_focus]];

    // ---- input (the list; the modals take it below) ----
    if (g_modal == LogModal::None && takeInput) {
        SetUiSoundSet(UiSoundSet::Menu);
        if (!selectable.empty()) {
            if (Up()) g_focus = std::max(0, g_focus - 1);
            if (Down()) g_focus = std::min(static_cast<int>(selectable.size()) - 1, g_focus + 1);
        }
        if (Pressed({ImGuiKey_GamepadL1}, false) || Pressed({ImGuiKey_GamepadR1}, false)) {
            PlayUi(UiSound::Step);
            g_bySets = !g_bySets;
            g_focus = 0;
            g_scroll = 0;
        }
        if (Pressed({ImGuiKey_GamepadL2}, false)) {
            PlayUi(UiSound::Decide);
            g_modal = LogModal::Characters;
            g_modalSide = 0;
        }
        if (Pressed({ImGuiKey_GamepadR2}, false)) {
            PlayUi(UiSound::Decide);
            g_modal = LogModal::Opponent;
            g_modalRow[0] = 0;
            g_modalScroll[0] = 0;
        }
        if (Pressed({ImGuiKey_GamepadFaceUp}, false)) {
            PlayUi(UiSound::Decide);
            EnsureDirectory(ReplaysDir());
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + ReplaysDir() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        }
        if (focused && Pressed({ImGuiKey_GamepadFaceLeft}, false) && g_bySets && focused->set) {
            PlayUi(UiSound::Decide);
            g_expanded = g_expanded == focused->set->key ? 0 : focused->set->key;
        }
        if (focused && Confirm()) {
            if (focused->kind == RowKind::SetHeader) {
                g_expanded = focused->set->key;
                g_focus++;
            }
            else if (focused->entry) {
                const Entry& e = *focused->entry;
                if (!ReplayCompatible(e.header.gameBuild, live.buildId)) {
                    PlayUi(UiSound::Error);
                }
                else {
                    in->watch = e.path;
                    in->watchTitle = Upper2(battlelog::NameOf(e, 0)) + " vs " + Upper2(battlelog::NameOf(e, 1));
                    if (e.header.summary.setGame && focused->set && focused->set->games.size() > 1) {
                        in->watchTitle += " \xC2\xB7 game " + std::to_string(e.header.summary.setGame) + " of " +
                                          std::to_string(focused->set->games.size());
                    }
                }
            }
        }
        if (Back()) in->back = true;
        // Rebuilt next frame with the focus where it went.
        focused = selectable.empty() ? nullptr : &rows[selectable[std::clamp(g_focus, 0, static_cast<int>(selectable.size()) - 1)]];
    }

    // ---- the top ----
    StageText(d, f.display, 40, 48, 22, kCream, "BATTLE LOG");
    {
        size_t matches = shown.size();
        const std::string info = std::to_string(matches) + (matches == 1 ? " match" : " matches") + " \xC2\xB7 " + std::to_string(sets.size()) +
                                 (sets.size() == 1 ? " set" : " sets");
        StageText(d, bold, 20, 1552, 34, kCream, info, 1);
    }
    // The filters (LT, RT) and the view (LB / RB).
    // On the stage, not in a pane: each on a pane's glass first, as opaque
    // as the panels below (the control alone was too see-through).
    auto filterButton = [&](float x0, float x1, const char* glyph, const std::string& label, const std::string& value) {
        Pane(d, x0, 84, x1, 128);
        Control(d, x0, 84, x1, 128, false);
        Glyph(d, glyph, g_v.P(x0 + 10, 92), g_v.P(x0 + 38, 120));
        Text(d, bold, g_v.L(18), g_v.P(x0 + 48, 94), kMuted, label);
        const float vx = x1 - 34;
        Text(d, bold, g_v.L(18), g_v.P(vx, 94), kCream, Fit(bold, 18, value, x1 - x0 - 190), 1);
        d->AddTriangleFilled(g_v.P(x1 - 24, 101), g_v.P(x1 - 12, 101), g_v.P(x1 - 18, 109), kGold);
    };
    filterButton(48, 452, "button_lt", "Characters", CharaLabelOf(live, g_filter.mine) + " vs " + CharaLabelOf(live, g_filter.theirs));
    filterButton(464, 868, "button_rt", "Opponent", g_filter.opponent.empty() ? std::string("Any") : Upper2(g_filter.opponent));
    {
        const float w0 = TextW(bold, 17, "Timeline") + 28, w1 = TextW(bold, 17, "Sets") + 28;
        const float x1 = 1552, x0 = x1 - (6 + 36 + 6 + w0 + 6 + w1 + 6 + 36 + 6);
        Pane(d, x0, 84, x1, 128);
        Control(d, x0, 84, x1, 128, false);
        Glyph(d, "button_lb", g_v.P(x0 + 6, 92), g_v.P(x0 + 42, 120));
        float tx = x0 + 48;
        for (int i = 0; i < 2; i++) {
            const bool sel = (i == 1) == g_bySets;
            const float w = i == 0 ? w0 : w1;
            if (sel) d->AddRectFilled(g_v.P(tx, 90), g_v.P(tx + w, 122), Rgba(236, 228, 206, 56));
            Text(d, bold, g_v.L(17), g_v.P(tx + w / 2, 96), sel ? kCream : kMuted, i == 0 ? "Timeline" : "Sets", 0.5f);
            tx += w + 6;
        }
        Glyph(d, "button_rb", g_v.P(x1 - 42, 92), g_v.P(x1 - 6, 120));
    }

    // ---- the list ----
    Pane(d, kListX0, kListY0, kListX1, kListY1);
    std::string topDay = rows.empty() ? std::string() : rows.front().kind == RowKind::Day ? rows.front().day : "";
    // Scroll: the focused row in view.
    const float view = kRowsBottom - kRowsTop;
    if (focused) {
        if (focused->y < g_scroll) g_scroll = focused->y - (focused == &rows[selectable.front()] ? 40 : 0);
        if (focused->y + focused->h > g_scroll + view) g_scroll = focused->y + focused->h - view;
    }
    g_scroll = std::clamp(g_scroll, 0.0f, std::max(0.0f, total - view));
    for (const Row& r : rows) {
        if (r.kind == RowKind::Day && r.y <= g_scroll + 1) topDay = r.day;
    }
    Strip(d, kListX0 + 2, kListY0 + 2, kListX1 - 2, kListY0 + 32, false);
    Text(d, bold, g_v.L(16), g_v.P(kListX0 + 20, kListY0 + 8), kGold, topDay.empty() ? std::string(g_bySets ? "SETS" : "MATCHES") : topDay);
    Text(d, f.body, g_v.L(16), g_v.P(kListX1 - 20, kListY0 + 8), kSoft,
         g_bySets ? std::to_string(sets.size()) + (sets.size() == 1 ? " set" : " sets") : std::to_string(shown.size()) + (shown.size() == 1 ? " match" : " matches"), 1);
    d->PushClipRect(g_v.P(kListX0 + 2, kRowsTop), g_v.P(kListX1 - 2, kRowsBottom), true);
    const float lx0 = kListX0 + 12, lx1 = kListX1 - 18;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        const float ry = kRowsTop + r.y - g_scroll;
        if (ry > kRowsBottom || ry + r.h < kRowsTop) continue;
        const bool on = focused == &r;
        switch (r.kind) {
        case RowKind::Day:
            if (r.y > 1) Text(d, bold, g_v.L(16), g_v.P(lx0 + 6, ry + 8), kGold, r.day);
            break;
        case RowKind::Match:
            Control(d, lx0, ry, lx1, ry + r.h, on);
            MatchLine(d, f, *r.entry, lx0 + 10, lx1 - 12, ry, r.h, on, 44, "", Clock(r.entry->header.createdUnix));
            break;
        case RowKind::Game:
            Control(d, lx0 + 22, ry, lx1, ry + r.h, on);
            MatchLine(d, f, *r.entry, lx0 + 32, lx1 - 12, ry, r.h, on, 40, "G" + std::to_string(r.game), Clock(r.entry->header.createdUnix));
            break;
        case RowKind::SetHeader: {
            const battlelog::Set& s = *r.set;
            d->AddRectFilled(g_v.P(lx0, ry), g_v.P(lx1, ry + r.h), on ? kGold : Rgba(0, 0, 0, 82));
            d->AddRect(g_v.P(lx0, ry), g_v.P(lx1, ry + r.h), on ? kGold : Rgba(236, 228, 206, 77), 0, 0, std::max(1.0f, g_v.L(1)));
            const ImU32 fg = on ? look::kInk : kCream, dim = on ? look::kInkSoft : kMuted, kick = on ? look::kInk : kGold;
            const bool open = g_expanded == s.key;
            if (open) d->AddTriangleFilled(g_v.P(lx0 + 12, ry + 13), g_v.P(lx0 + 24, ry + 13), g_v.P(lx0 + 18, ry + 21), kick);
            else d->AddTriangleFilled(g_v.P(lx0 + 14, ry + 11), g_v.P(lx0 + 22, ry + 17), g_v.P(lx0 + 14, ry + 23), kick);
            const std::string rule = s.rule.empty() ? "DIRECT MATCH" : Upper(s.rule);
            const std::string info = rule + " \xC2\xB7 " + std::to_string(s.games.size()) + (s.games.size() == 1 ? " GAME" : " GAMES") +
                                     " \xC2\xB7 " + Clock(s.games.front()->header.createdUnix);
            Text(d, bold, g_v.L(15), g_v.P(lx0 + 34, ry + 9), kick, info);
            // WON / LOST by the player's side of the last game.
            const int me = battlelog::MySide(*s.games.back(), live.myName);
            const bool won = s.wins[me] > s.wins[1 - me], lost = s.wins[me] < s.wins[1 - me];
            if (won || lost) Text(d, bold, g_v.L(15), g_v.P(lx1 - 14, ry + 9), on ? look::kInk : won ? look::kOk : kMuted, won ? "WON" : "LOST", 1);
            // The two players, the set's score between them.
            const Entry& e = *s.games.back();
            const float fy = ry + 34, face = 44, mid = (lx0 + 34 + face + lx1 - 14 - face) / 2;
            Face(d, e.header.setup.players[0].character, lx0 + 34, fy, face, false);
            Text(d, bold, g_v.L(20), g_v.P(lx0 + 34 + face + 10, fy + 10), fg, Fit(bold, 20, Upper2(battlelog::NameOf(e, 0)), mid - 70 - (lx0 + 34 + face + 10)));
            Text(d, f.display, g_v.L(26), g_v.P(mid, fy + 6), fg, std::to_string(s.wins[0]) + " - " + std::to_string(s.wins[1]), 0.5f);
            Face(d, e.header.setup.players[1].character, lx1 - 14 - face, fy, face, true);
            Text(d, bold, g_v.L(20), g_v.P(lx1 - 14 - face - 10, fy + 10), fg, Fit(bold, 20, Upper2(battlelog::NameOf(e, 1)), mid - 70 - (lx0 + 34 + face + 10)), 1);
            (void)dim;
            break;
        }
        }
    }
    d->PopClipRect();
    if (total > view) ScrollBar(d, kListX1 - 8, kRowsTop + 4, kRowsBottom - 4, g_scroll, view, total, false);
    if (rows.empty()) {
        const std::string msg = !g_read ? "Reading your matches..."
                                : g_merged.empty() ? "No matches yet. Your online matches are kept here."
                                                   : "No matches with these filters.";
        Text(d, bold, g_v.L(20), g_v.P((kListX0 + kListX1) / 2, 420), kSoft, msg, 0.5f);
    }

    // ---- the details ----
    const Entry* de = focused ? focused->entry : nullptr;
    Pane(d, kDetailX0, kListY0, kDetailX1, kListY1);
    if (de) {
        const MatchSummary& s = de->header.summary;
        std::string kicker;
        const std::tm tm = LocalTime(de->header.createdUnix);
        char when[32];
        std::snprintf(when, sizeof(when), "%s %d, %s", kMonths[tm.tm_mon], tm.tm_mday, Clock(de->header.createdUnix).c_str());
        if (s.setGame && focused->set && focused->set->games.size() > 1) {
            kicker = "GAME " + std::to_string(s.setGame) + " OF " + std::to_string(focused->set->games.size()) + " \xC2\xB7 " +
                     (s.setRule.empty() ? std::string("DIRECT MATCH") : Upper(s.setRule)) + " \xC2\xB7 " + when;
        }
        else {
            kicker = std::string("MATCH \xC2\xB7 ") + when;
        }
        Strip(d, kDetailX0 + 2, kListY0 + 2, kDetailX1 - 2, kListY0 + 46, false);
        Text(d, bold, g_v.L(16), g_v.P(kDetailX0 + 24, kListY0 + 14), kGold, kicker);
        // The two fighters, the round score between them.
        const float ay0 = kListY0 + 46, ay1 = ay0 + 230;
        d->PushClipRect(g_v.P(kDetailX0 + 2, ay0), g_v.P(kDetailX1 - 2, ay1), true);
        static const FadeStop kLeft[] = {{0, 1}, {0.55f, 1}, {0.92f, 0}, {1, 0}};
        for (int p = 0; p < 2; p++) {
            const uint32_t c = de->header.setup.players[p].character;
            if (!c) continue;
            if (const TextureView* t = TextureSized(CharaTex(UnpackCode(c), "PartCharacter"), g_v.L(380))) {
                const float x = p == 0 ? kDetailX0 - 40 : kDetailX1 + 40 - 380;
                if (p == 0) ImageFadeX(d, t, g_v.P(x, ay0 - 30), g_v.P(x + 380, ay0 + 350), ImVec2(0, 0), ImVec2(1, 1), kLeft, 4, false, FadeIn(t));
                else {
                    static const FadeStop kRight[] = {{0, 0}, {0.08f, 0}, {0.45f, 1}, {1, 1}};
                    ImageFadeX(d, t, g_v.P(x, ay0 - 30), g_v.P(x + 380, ay0 + 350), ImVec2(0, 0), ImVec2(1, 1), kRight, 4, true, FadeIn(t));
                }
            }
        }
        d->PopClipRect();
        if (s.present) {
            StageText(d, f.display, 56, (kDetailX0 + kDetailX1) / 2, ay0 + 84,
                      kCream, std::to_string(s.roundWins[0]) + " - " + std::to_string(s.roundWins[1]), 0.5f);
        }
        for (int p = 0; p < 2; p++) {
            const float x = p == 0 ? kDetailX0 + 24 : kDetailX1 - 24;
            const float align = p == 0 ? 0.0f : 1.0f;
            StageText(d, f.body, 16, x, ay1 - 52, kSoft, CharaLabelOf(live, de->header.setup.players[p].character), align);
            StageText(d, bold, 22, x, ay1 - 32, kCream, Fit(bold, 22, Upper2(battlelog::NameOf(*de, p)), 290), align);
        }
        d->AddLine(g_v.P(kDetailX0 + 2, ay1), g_v.P(kDetailX1 - 2, ay1), Rgba(236, 228, 206, 36), std::max(1.0f, g_v.L(1)));
        float ry = ay1 + 16;
        // Round winners come from a decided match only (a match left early
        // keeps its rounds' frames for playback, not who won them).
        if (!s.rounds.empty() && s.ending == 0) {
            Text(d, bold, g_v.L(16), g_v.P(kDetailX0 + 24, ry), kGold, "ROUNDS");
            ry += 28;
            for (size_t i = 0; i < s.rounds.size() && i < 7; i++) {
                const int w = s.rounds[i].winner;
                Text(d, f.display, g_v.L(18), g_v.P(kDetailX0 + 24, ry + 10), kMuted, std::to_string(i + 1));
                if (w >= 0) {
                    Face(d, de->header.setup.players[w].character, kDetailX0 + 60, ry + 4, 32, w == 1);
                    Text(d, bold, g_v.L(19), g_v.P(kDetailX0 + 104, ry + 9), kCream, Upper2(battlelog::NameOf(*de, w)));
                }
                else {
                    Text(d, bold, g_v.L(19), g_v.P(kDetailX0 + 60, ry + 9), kSoft, "Draw");
                }
                d->AddLine(g_v.P(kDetailX0 + 24, ry + 41), g_v.P(kDetailX1 - 24, ry + 41), Rgba(236, 228, 206, 36), std::max(1.0f, g_v.L(1)));
                ry += 42;
            }
            ry += 10;
        }
        Text(d, bold, g_v.L(16), g_v.P(kDetailX0 + 24, ry), kGold, "MATCH");
        ry += 28;
        std::string line;
        const std::string stage = live.stageName ? live.stageName(de->header.setup.stage) : std::string();
        if (!stage.empty()) line += stage + " \xC2\xB7 ";
        line += std::to_string(de->header.setup.timerSeconds) + " seconds";
        if (s.seconds) {
            line += " \xC2\xB7 " + std::to_string(s.seconds / 60) + " min " + std::to_string(s.seconds % 60) + " s";
        }
        if (s.present && s.ending == 1) line += " \xC2\xB7 not finished";
        Text(d, bold, g_v.L(19), g_v.P(kDetailX0 + 24, ry), kSoft, Fit(bold, 19, line, kDetailX1 - kDetailX0 - 48));
        ry += 32;
        const bool playable = ReplayCompatible(de->header.gameBuild, live.buildId);
        const std::string ver = "SFVR " + de->header.modVersion;
        if (playable) {
            d->AddLine(g_v.P(kDetailX0 + 26, ry + 12), g_v.P(kDetailX0 + 31, ry + 17), look::kOk, g_v.L(2.4f));
            d->AddLine(g_v.P(kDetailX0 + 31, ry + 17), g_v.P(kDetailX0 + 40, ry + 6), look::kOk, g_v.L(2.4f));
            Text(d, bold, g_v.L(19), g_v.P(kDetailX0 + 50, ry), kSoft, ver);
        }
        else {
            Text(d, bold, g_v.L(19), g_v.P(kDetailX0 + 24, ry), kMuted, Fit(bold, 19, "Made with " + ver + ": can't be played on this version", kDetailX1 - kDetailX0 - 48));
        }
    }

    // ---- the prompt line ----
    std::string hint;
    PromptList prompts;
    if (focused && focused->entry && focused->kind != RowKind::SetHeader) {
        const Entry& e = *focused->entry;
        const int me = battlelog::MySide(e, live.myName);
        hint = (e.header.summary.setGame && focused->set && focused->set->games.size() > 1)
                   ? "Game " + std::to_string(e.header.summary.setGame) + " of " + std::to_string(focused->set->games.size()) + " against " +
                         core::DisplayName(battlelog::NameOf(e, 1 - me)) + "."
                   : "Against " + core::DisplayName(battlelog::NameOf(e, 1 - me)) + ".";
        if (ReplayCompatible(e.header.gameBuild, live.buildId)) prompts.push_back({"button_a", "Watch"});
    }
    else if (focused && focused->kind == RowKind::SetHeader) {
        prompts.push_back({"button_a", "Open"});
    }
    if (g_bySets && focused && focused->set) prompts.push_back({"button_x", g_expanded == focused->set->key ? "Close set" : "Open set"});
    prompts.push_back({"button_y", "Open folder"});
    prompts.push_back({"button_b", "Back"});
    if (live.launching) {
        hint = "Loading the match...";
        prompts.clear();
    }
    int clicked = -1;
    PromptLine(d, f.body, bold, "", kGold, g_modal == LogModal::None ? hint : std::string(), g_modal == LogModal::None ? prompts : PromptList{}, &clicked);
    if (clicked >= 0 && g_modal == LogModal::None) ClickPad(PadKeyOfGlyph(prompts[clicked].first));

    // ---- the filters ----
    if (g_modal == LogModal::Characters) {
        const auto mine = battlelog::Characters(g_merged, live.myName, false);
        const auto theirs = battlelog::Characters(g_merged, live.myName, true);
        const std::vector<std::pair<uint32_t, int>>* lists[2] = {&mine, &theirs};
        const int count[2] = {static_cast<int>(mine.size()) + 1, static_cast<int>(theirs.size()) + 1};
        ModalSpec m;
        m.x0 = 340, m.y0 = 96, m.w = 920, m.h = 680;
        m.kicker = "FILTER";
        const int side = g_modalSide;
        const int row = std::clamp(g_modalRow[side], 0, count[side] - 1);
        const uint32_t onCode = row == 0 ? 0 : (*lists[side])[row - 1].first;
        const int onCount = row == 0 ? static_cast<int>(g_merged.size()) : (*lists[side])[row - 1].second;
        m.hint = row == 0 ? "Any character." : std::to_string(onCount) + (onCount == 1 ? " match " : " matches ") + (side == 0 ? "as " : "against ") +
                                                  CharaLabelOf(live, onCode) + ".";
        m.prompts = {{"button_a", "Choose"}, {"button_y", "Any"}, {"button_b", "Back"}};
        int mclicked = -1;
        float by = Modal(d, bold, f.body, m, &mclicked);
        Text(d, f.display, g_v.L(28), g_v.P(m.x0 + 28, by), kCream, "Characters");
        by += 50;
        constexpr int kVisible = 9;
        for (int s = 0; s < 2; s++) {
            const float x0 = m.x0 + 28 + s * 444, x1 = x0 + 420;
            d->AddRectFilled(g_v.P(x0, by), g_v.P(x1, by + 30), Rgba(0, 0, 0, 82));
            Text(d, bold, g_v.L(15), g_v.P(x0 + 12, by + 6), kGold, s == 0 ? "YOUR CHARACTER" : "OPPONENT'S CHARACTER");
            Text(d, f.body, g_v.L(15), g_v.P(x1 - 12, by + 6), kMuted,
                 std::to_string(count[s] - 1) + (s == 0 ? " played" : " faced"), 1);
            int& scroll = g_modalScroll[s];
            const int r = std::clamp(g_modalRow[s], 0, count[s] - 1);
            if (r < scroll) scroll = r;
            if (r >= scroll + kVisible) scroll = r - kVisible + 1;
            for (int i = scroll; i < count[s] && i < scroll + kVisible; i++) {
                const float ry = by + 30 + (i - scroll) * 50;
                const bool on = s == side && i == r;
                const uint32_t code = i == 0 ? 0 : (*lists[s])[i - 1].first;
                const int n = i == 0 ? static_cast<int>(g_merged.size()) : (*lists[s])[i - 1].second;
                if (on) d->AddRectFilled(g_v.P(x0, ry), g_v.P(x1, ry + 50), kGold);
                d->AddLine(g_v.P(x0, ry + 49.5f), g_v.P(x1, ry + 49.5f), Rgba(236, 228, 206, 36), std::max(1.0f, g_v.L(1)));
                if (code) Face(d, code, x0 + 8, ry + 7, 36, false);
                Text(d, bold, g_v.L(19), g_v.P(x0 + 56, ry + 13), on ? look::kInk : kCream, CharaLabelOf(live, code));
                Text(d, bold, g_v.L(17), g_v.P(x1 - 40, ry + 14), on ? look::kInkSoft : kMuted, std::to_string(n), 1);
                const uint32_t current = s == 0 ? g_filter.mine : g_filter.theirs;
                if (current == code) {
                    d->AddLine(g_v.P(x1 - 28, ry + 25), g_v.P(x1 - 22, ry + 31), look::kOk, g_v.L(2.4f));
                    d->AddLine(g_v.P(x1 - 22, ry + 31), g_v.P(x1 - 12, ry + 19), look::kOk, g_v.L(2.4f));
                }
            }
            if (count[s] > kVisible) ScrollBar(d, x1 - 4, by + 34, by + 30 + kVisible * 50 - 4, static_cast<float>(scroll), kVisible, static_cast<float>(count[s]), false);
        }
        if (mclicked >= 0) ClickPad(PadKeyOfGlyph(m.prompts[mclicked].first));
        if (takeInput) {
            if (Left() || Right()) g_modalSide = 1 - g_modalSide;
            if (Up()) g_modalRow[side] = std::max(0, row - 1);
            if (Down()) g_modalRow[side] = std::min(count[side] - 1, row + 1);
            if (Confirm()) {
                (side == 0 ? g_filter.mine : g_filter.theirs) = onCode;
                g_focus = 0;
                g_scroll = 0;
            }
            if (Pressed({ImGuiKey_GamepadFaceUp}, false)) {
                PlayUi(UiSound::Decide);
                g_filter.mine = g_filter.theirs = 0;
                g_focus = 0;
            }
            if (Back()) g_modal = LogModal::None;
        }
    }
    else if (g_modal == LogModal::Opponent) {
        const auto all = battlelog::Opponents(g_merged, live.myName);
        std::vector<std::pair<std::string, int>> list;
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        for (const auto& o : all) {
            if (g_search.empty() || lower(core::DisplayName(o.first)).find(lower(g_search)) != std::string::npos) list.push_back(o);
        }
        ModalSpec m;
        m.x0 = 440, m.y0 = 120, m.w = 720, m.h = 640;
        m.kicker = "FILTER";
        int& row = g_modalRow[0];
        row = list.empty() ? 0 : std::clamp(row, 0, static_cast<int>(list.size()) - 1);
        m.hint = g_typing ? "Type a name. Enter to keep it." : list.empty() ? "No opponent with that name."
                 : std::to_string(list[row].second) + (list[row].second == 1 ? " match against " : " matches against ") +
                       core::DisplayName(list[row].first) + ".";
        m.prompts = g_typing ? PromptList{} : PromptList{{"button_a", "Choose"}, {"button_x", "Search"}, {"button_y", "Any"}, {"button_b", "Back"}};
        int mclicked = -1;
        float by = Modal(d, bold, f.body, m, &mclicked);
        Text(d, f.display, g_v.L(28), g_v.P(m.x0 + 28, by), kCream, "Opponent");
        by += 50;
        // The search field.
        const float fx0 = m.x0 + 28, fx1 = m.x0 + m.w - 28;
        d->AddRectFilled(g_v.P(fx0, by), g_v.P(fx1, by + 50), Rgba(0, 0, 0, 90));
        d->AddRect(g_v.P(fx0, by), g_v.P(fx1, by + 50), g_typing ? look::kRing : Rgba(236, 228, 206, 77), 0, 0, g_v.L(g_typing ? 2 : 1));
        const std::string field = g_search.empty() && !g_typing ? std::string("Search names") : g_search;
        Text(d, bold, g_v.L(20), g_v.P(fx0 + 16, by + 12), g_search.empty() && !g_typing ? kMuted : kCream, field);
        if (g_typing && std::fmod(ImGui::GetTime(), 1.0) < 0.5) {
            const float cx = fx0 + 16 + TextW(bold, 20, g_search) + 2;
            d->AddLine(g_v.P(cx, by + 12), g_v.P(cx, by + 38), kCream, g_v.L(2));
        }
        Text(d, f.body, g_v.L(16), g_v.P(fx1 - 14, by + 15), kMuted,
             std::to_string(list.size()) + " of " + std::to_string(all.size()), 1);
        by += 64;
        constexpr int kVisible = 7;
        int& scroll = g_modalScroll[0];
        if (row < scroll) scroll = row;
        if (row >= scroll + kVisible) scroll = row - kVisible + 1;
        for (int i = scroll; i < static_cast<int>(list.size()) && i < scroll + kVisible; i++) {
            const float ry = by + (i - scroll) * 56;
            const bool on = i == row && !g_typing;
            if (on) d->AddRectFilled(g_v.P(fx0, ry), g_v.P(fx1, ry + 56), kGold);
            d->AddLine(g_v.P(fx0, ry + 55.5f), g_v.P(fx1, ry + 55.5f), Rgba(236, 228, 206, 36), std::max(1.0f, g_v.L(1)));
            Text(d, bold, g_v.L(20), g_v.P(fx0 + 16, ry + 15), on ? look::kInk : kCream, Fit(bold, 20, Upper2(list[i].first), 420));
            Text(d, bold, g_v.L(17), g_v.P(fx1 - 16, ry + 17), on ? look::kInkSoft : kMuted,
                 std::to_string(list[i].second) + (list[i].second == 1 ? " match" : " matches"), 1);
        }
        if (mclicked >= 0) ClickPad(PadKeyOfGlyph(m.prompts[mclicked].first));
        if (takeInput && g_typing) {
            g_textEntry = true;
            for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
                if (c < 32 || c == 127) continue;
                char utf8[5] = {};
                ImTextCharToUtf8(utf8, c);
                if (g_search.size() < 40) g_search += utf8;
                row = 0;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !g_search.empty()) {
                size_t n = g_search.size() - 1;
                while (n > 0 && (static_cast<unsigned char>(g_search[n]) & 0xC0) == 0x80) n--;
                g_search.erase(n);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                PlayUi(UiSound::Decide);
                g_typing = false;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                PlayUi(UiSound::Cancel);
                g_typing = false;
                g_search.clear();
            }
        }
        else if (takeInput) {
            if (!list.empty() && Up()) row = std::max(0, row - 1);
            if (!list.empty() && Down()) row = std::min(static_cast<int>(list.size()) - 1, row + 1);
            if (Pressed({ImGuiKey_GamepadFaceLeft}, false)) {
                PlayUi(UiSound::Decide);
                g_typing = true;
            }
            if (Confirm() && !list.empty()) {
                g_filter.opponent = list[row].first;
                g_modal = LogModal::None;
                g_focus = 0;
                g_scroll = 0;
            }
            if (Pressed({ImGuiKey_GamepadFaceUp}, false)) {
                PlayUi(UiSound::Decide);
                g_filter.opponent.clear();
                g_search.clear();
                g_modal = LogModal::None;
                g_focus = 0;
            }
            if (Back()) g_modal = LogModal::None;
        }
    }
    g_inputOn = false;
    d->PopClipRect();
}

// A round jump's fade (DrawReplayStripe): out over kFadeOut, the jump asked
// once black, black until the replay has taken it and its seek (or restart)
// is done, then in over kFadeIn on the round's call as it plays.
enum class JumpFade { None, Out, Black, In };
struct JumpState {
    JumpFade phase = JumpFade::None;
    int dir = 0;
    double since = 0;
    uint32_t handledAt = 0; // ReplayView::handled once the jump is taken
};
JumpState g_jump;
constexpr double kFadeOut = 0.25, kFadeIn = 0.35, kJumpGiveUp = 30;

void RoundJumpFade(ImDrawList* d, const ReplayView& v)
{
    const double now = ImGui::GetTime();
    float alpha = 0;
    switch (g_jump.phase) {
    case JumpFade::None: return;
    case JumpFade::Out:
        alpha = static_cast<float>((now - g_jump.since) / kFadeOut);
        if (alpha >= 1) {
            alpha = 1;
            // dir 0: a fade the replay itself asked for (a takeover's end):
            // no command, black until it is done seeking.
            g_jump.handledAt = g_jump.dir != 0 ? ControlReplay(ReplayControl::Round, g_jump.dir) : v.handled;
            g_jump.phase = g_jump.handledAt || g_jump.dir == 0 ? JumpFade::Black : JumpFade::In;
            g_jump.since = now;
        }
        break;
    case JumpFade::Black:
        alpha = 1;
        if ((v.handled >= g_jump.handledAt && !v.seeking) || now - g_jump.since > kJumpGiveUp) {
            g_jump.phase = JumpFade::In;
            g_jump.since = now;
        }
        break;
    case JumpFade::In:
        alpha = 1 - static_cast<float>((now - g_jump.since) / kFadeIn);
        if (alpha <= 0) {
            g_jump = {};
            return;
        }
        break;
    }
    // Over the game's picture and HUD; the stripe draws on top.
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(0, 0, 0, static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f))));
}

void DrawReplayStripe(const HubFonts& f, bool takeInput)
{
    const ReplayView v = ReplayViewNow();
    if (!v.active) {
        g_jump = {}; // left mid-fade
        return;
    }
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    g_inputOn = takeInput;
    SetUiSoundSet(UiSoundSet::Menu);
    if (v.benchmark) {
        // The benchmark's: wait, keep the window in focus; B stops it.
        if (takeInput && Back()) ControlReplay(ReplayControl::Leave);
        g_inputOn = false;
        const float y0 = 856, y1 = 900;
        d->AddRectFilled(g_v.P(0, y0), g_v.P(1600, y1), Rgba(10, 12, 16, 184));
        d->AddLine(g_v.P(0, y0), g_v.P(1600, y0), Rgba(236, 228, 206, 115), std::max(1.0f, g_v.L(1)));
        if (v.frames > 0) d->AddRectFilled(g_v.P(0, y0 - 1), g_v.P(1600.0f * (v.frame + 1) / v.frames, y0 + 2), look::kGold);
        Text(d, bold, g_v.L(16), g_v.P(48, y0 + 13), look::kGold, "BENCHMARK");
        const int left = std::max(0, (v.frames - v.frame) / 60);
        char line[96];
        std::snprintf(line, sizeof(line), "Please wait, and keep the game window in focus \xC2\xB7 0:%02d left", left);
        Text(d, bold, g_v.L(18), g_v.P(48 + TextW(bold, 16, "BENCHMARK") + 18, y0 + 11), look::kCream, line);
        int clicked = -1;
        PromptRow(d, f.body, 17, {{"button_b", "Stop"}}, 1564, y0 + 8, 28, false, &clicked);
        if (clicked >= 0) ControlReplay(ReplayControl::Leave);
        return;
    }
    // Replay Takeover: the pad plays a side, so the stripe takes none of it
    // but Start (no use in a fight), which stops it; a big countdown first.
    if (v.takeover >= 0) {
        if (takeInput && Pressed({ImGuiKey_GamepadStart}, false)) {
            PlayUi(UiSound::Cancel);
            ControlReplay(ReplayControl::TakeoverStop);
        }
        g_inputOn = false;
        // The KO screen over: to black, back to the start point behind it,
        // the picture back there (RoundJumpFade with no jump of its own).
        if (v.takeoverFading && g_jump.phase == JumpFade::None) g_jump = {JumpFade::Out, 0, ImGui::GetTime(), 0};
        RoundJumpFade(d, v);
        if (v.takeoverCountdown > 0) {
            const int n = std::clamp(static_cast<int>(std::ceil(v.takeoverCountdown)), 1, 3);
            StageText(d, f.display, 220, 800, 300, look::kCream, std::to_string(n), 0.5f);
            StageText(d, bold, 26, 800, 560, look::kGold, v.takeover == 0 ? "YOU ARE PLAYER 1" : "YOU ARE PLAYER 2", 0.5f);
        }
        const float y0 = 856, y1 = 900;
        d->AddRectFilled(g_v.P(0, y0), g_v.P(1600, y1), Rgba(10, 12, 16, 184));
        d->AddLine(g_v.P(0, y0), g_v.P(1600, y0), Rgba(236, 228, 206, 115), std::max(1.0f, g_v.L(1)));
        if (v.roundProgress > 0) d->AddRectFilled(g_v.P(0, y0 - 1), g_v.P(1600 * v.roundProgress, y0 + 2), look::kGold);
        const char* state = v.takeover == 0 ? "CONTROL P1" : "CONTROL P2";
        Text(d, bold, g_v.L(16), g_v.P(48, y0 + 13), look::kGold, state);
        int clicked = -1;
        const PromptList prompts = {{"button_start", v.takeoverEnding ? "Back to the start" : "Stop"}};
        const float left = PromptRow(d, f.body, 17, prompts, 1564, y0 + 8, 28, false, &clicked);
        const float textX = 48 + TextW(bold, 16, state) + 18;
        const std::string line = v.takeoverCountdown > 0 ? "Get ready" : v.takeoverEnding ? "Round over: back to where you took control" : "Until the round's KO";
        Text(d, bold, g_v.L(18), g_v.P(textX, y0 + 11), look::kCream, Fit(bold, 18, v.title + " \xC2\xB7 " + line, left - 24 - textX));
        if (clicked >= 0) ControlReplay(ReplayControl::TakeoverStop);
        return;
    }
    // While a round jump fades, only B (leave) is taken.
    const bool fading = g_jump.phase != JumpFade::None;
    const bool canTakeOver = v.roundFrame >= 0 && !v.seeking && !fading;
    if (takeInput && fading && Back()) ControlReplay(ReplayControl::Leave);
    if (takeInput && !fading) {
        if (canTakeOver) {
            const int side = Pressed({ImGuiKey_GamepadFaceLeft}, false) ? 0 : Pressed({ImGuiKey_GamepadFaceUp}, false) ? 1 : -1;
            if (side >= 0) {
                PlayUi(UiSound::Decide);
                ControlReplay(ReplayControl::Takeover, side);
            }
        }
        if (Confirm()) ControlReplay(ReplayControl::Toggle);
        if (Pressed({ImGuiKey_GamepadBack}, false)) {
            PlayUi(UiSound::Decide);
            SetHitboxesInReplays(!HitboxesInReplays());
        }
        if (Pressed({ImGuiKey_GamepadStart}, false)) {
            PlayUi(UiSound::Decide);
            SetFrameMeterInReplays(!FrameMeterInReplays());
        }
        // A step back plays no sound, the menu's cue included.
        if (Pressed({ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}) && v.frame > v.backLimit) ControlReplay(ReplayControl::Step, -1);
        if (Pressed({ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}) && v.canStepForward) {
            PlayUi(UiSound::Cursor); // the directions' sound (ui_kit Left/Right)
            ControlReplay(ReplayControl::Step, 1);
        }
        // A round jump fades the picture out, jumps behind the black (a run
        // or the battle started again: nothing to watch) and fades back in
        // on the round's call (RoundJumpFade).
        if (g_jump.phase == JumpFade::None) {
            const int dir = Pressed({ImGuiKey_GamepadL1}, false) ? -1 : Pressed({ImGuiKey_GamepadR1}, false) ? 1 : 0;
            if (dir != 0) {
                PlayUi(UiSound::Step);
                g_jump = {JumpFade::Out, dir, ImGui::GetTime(), 0};
            }
        }
        static bool fast = false;
        const bool held = ImGui::IsKeyDown(ImGuiKey_GamepadR2);
        if (held != fast) {
            fast = held;
            ControlReplay(ReplayControl::Fast, held ? 4 : 0);
        }
        if (Back()) ControlReplay(ReplayControl::Leave);
    }
    g_inputOn = false;
    RoundJumpFade(d, v);
    // The stripe: 44 high along the bottom, glass over the fight.
    const float y0 = 856, y1 = 900;
    d->AddRectFilled(g_v.P(0, y0), g_v.P(1600, y1), Rgba(10, 12, 16, 184));
    d->AddLine(g_v.P(0, y0), g_v.P(1600, y0), Rgba(236, 228, 206, 115), std::max(1.0f, g_v.L(1)));
    // The round's progress: its action frames, from "FIGHT!" to the KO.
    if (v.roundProgress > 0) d->AddRectFilled(g_v.P(0, y0 - 1), g_v.P(1600 * v.roundProgress, y0 + 2), look::kGold);
    const bool action = v.roundFrame >= 0 && !fading; // a jump's black: no stale counter
    const char* state = v.seeking ? "SEEKING" : v.paused ? "PAUSED" : v.fast > 1 && action ? "FAST" : "PLAYING";
    Text(d, bold, g_v.L(16), g_v.P(48, y0 + 13), look::kGold, state);
    // The frame counter: per round, 0 on its first action frame, its last on
    // the KO's; none outside the action (intro, round call, KO, win poses).
    char frame[64] = "";
    if (action && v.roundFrames > 0) std::snprintf(frame, sizeof(frame), " \xC2\xB7 Frame %d / %d", v.roundFrame, v.roundFrames - 1);
    else if (action) std::snprintf(frame, sizeof(frame), " \xC2\xB7 Frame %d", v.roundFrame);
    PromptList prompts = {{"button_a", v.paused ? "Play" : "Pause"}, {"button_lb", "Round"}, {"button_rb", ""}, {"button_rt", "Hold to fast forward"},
                          {"button_back", HitboxesInReplays() ? "Hide hitboxes" : "Show hitboxes"},
                          {"button_start", FrameMeterInReplays() ? "Hide frame meter" : "Show frame meter"}, {"button_b", "Leave"}};
    // Replay Takeover, in a round's fight.
    if (canTakeOver) prompts.insert(prompts.begin() + 1, {{"button_x", "Control P1"}, {"button_y", "Control P2"}});
    int clicked = -1;
    const float left = PromptRow(d, f.body, 17, prompts, 1564, y0 + 8, 28, false, &clicked);
    // Frame steps: the d-pad's left and right, drawn as triangles; back dims
    // at the earliest frame a step reaches (ReplayView::backLimit).
    const float tx = left - 120;
    // The title gives way (cut short) before the frame counter does, so long
    // names never run into the controls.
    const float textX = 48 + TextW(bold, 16, state) + 18;
    const float titleRoom = std::max(60.0f, tx - 24 - textX - TextW(bold, 18, frame));
    Text(d, bold, g_v.L(18), g_v.P(textX, y0 + 11), look::kCream, Fit(bold, 18, v.title, titleRoom) + frame);
    const bool canBack = v.frame > v.backLimit && !v.seeking && !fading;
    d->AddTriangleFilled(g_v.P(tx, y0 + 22), g_v.P(tx + 12, y0 + 14), g_v.P(tx + 12, y0 + 30), canBack ? look::kCream : Rgba(236, 228, 206, 70));
    d->AddTriangleFilled(g_v.P(tx + 30, y0 + 14), g_v.P(tx + 42, y0 + 22), g_v.P(tx + 30, y0 + 30),
                         v.canStepForward && !fading ? look::kCream : Rgba(236, 228, 206, 70));
    Text(d, bold, g_v.L(17), g_v.P(tx + 52, y0 + 12), look::kCream, "Frame");
    if (clicked >= 0) ClickPad(PadKeyOfGlyph(prompts[clicked].first));
    // A replay that stopped matching its recording: what plays from there is
    // not the match as it was.
    if (v.outOfSync >= 0) {
        const std::string note = v.outOfSync > 0 ? "Out of sync with the recording from round " + std::to_string(v.outOfSync)
                                                 : "Out of sync with the recording";
        // Top centre, under the round timer: clear of the gauges and the frame meter.
        const float w = TextW(bold, 17, note) + 32, x = 800 - w / 2, top = 168;
        d->AddRectFilled(g_v.P(x, top), g_v.P(x + w, top + 34), Rgba(10, 12, 16, 200));
        d->AddRect(g_v.P(x, top), g_v.P(x + w, top + 34), Rgba(229, 67, 90, 200), 0, 0, std::max(1.0f, g_v.L(1.5f)));
        Text(d, bold, g_v.L(17), g_v.P(x + 16, top + 7), look::kCream, note);
    }
}

} // namespace sfv::mod
