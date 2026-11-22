// The select screen's preview with mock players (the Debug tab).

#include "mod/select_internal.h"

#include "core/text.h"
#include "core/game.h"

#include "mod/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <random>

namespace sfv::mod {

using namespace selectui;
using namespace ui;

namespace {

std::atomic<bool> g_previewOpen{false};
bool g_previewFresh = false;
int g_previewPhase = 0;
SelectData g_preview;

struct Mock {
    int stage = 0, setting = 0;
    bool locked = false;
    int chara[2] = {0, 0};
    int values[2][kSelectRows] = {{0, 0, 0, 0, 0, 0, 2}, {1, 0, 0, 1, 0, 0, 3}};
    bool ready[2] = {false, true};
    int shown = -1;
    float inPhase = 0, nextAct = 0;
    uint32_t roll = 1;
} g_mock;

int MockPhase() {
    if (!g_mock.locked) return 0;
    if (g_mock.ready[0]) return 3;
    return g_ui.loadout ? 2 : 1;
}

void SetMockPhase(int phase) {
    g_mock.locked = phase > 0;
    g_ui.loadout = phase >= 2;
    g_ui.wasLocked = g_mock.locked;
    g_mock.ready[0] = phase == 3;
    g_mock.ready[1] = !g_preview.spectator || phase == 3;
}

int MockCount(const SelectChara& c, int row, int costume) {
    switch (row) {
        case kRowCostume: return std::max<int>(1, static_cast<int>(c.costumes.size()));
        case kRowColor: {
            const size_t k = std::min<size_t>(costume, c.swatches.empty() ? 0 : c.swatches.size() - 1);
            return c.swatches.empty() || c.swatches[k].empty() ? 1 : static_cast<int>(c.swatches[k].size());
        }
        case kRowVariant: return 1;
        case kRowVSkill: return std::max<int>(2, static_cast<int>(c.vskills.size()));
        case kRowVTrigger: return std::max<int>(2, static_cast<int>(c.vtriggers.size()));
        case kRowVComment: return 1 + static_cast<int>(c.vcomments.size());
        default: return 11;
    }
}

SelectSide MockSide(int i) {
    SelectSide s;
    s.present = true;
    s.player = g_preview.spectator ? (i ? "Mock opponent" : "Mock host") : i ? "Mock opponent"
                                                                             : (g_preview.you.empty() ? "You" : g_preview.you);
    s.link = i ? 2 : 1;
    s.chara = g_mock.chara[i];
    s.ready = g_mock.ready[i];
    const SelectChara& c = g_preview.charas[std::clamp(s.chara, 0, static_cast<int>(g_preview.charas.size()) - 1)];
    const int* v = g_mock.values[i];
    const bool eleven = c.code == "ELV";
    auto roman = [](int n) { return n == 1 ? std::string("I") : n == 2 ? std::string("II")
                                                                       : std::to_string(n); };
    for (int r = 0; r < kSelectRows; r++) s.rows[r].fixed = eleven && r < kRowDelay;
    if (eleven) {
        for (int r = 0; r < kRowDelay; r++) s.rows[r].value = TR(Str::common_random);
    } else {
        s.rows[kRowCostume].value = c.costumes.empty() ? std::string(TR(Str::common_default)) : c.costumes[v[kRowCostume] % c.costumes.size()];
        const size_t ci = std::min<size_t>(v[kRowCostume], c.swatches.empty() ? 0 : c.swatches.size() - 1);
        const int colors = MockCount(c, kRowColor, v[kRowCostume]);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d", v[kRowColor] % colors + 1);
        s.rows[kRowColor].value = buf;
        if (!c.swatches.empty() && !c.swatches[ci].empty()) {
            s.rows[kRowColor].swatch1 = c.swatches[ci][v[kRowColor] % colors].first;
            s.rows[kRowColor].swatch2 = c.swatches[ci][v[kRowColor] % colors].second;
        }
        s.rows[kRowVariant].value = TR(Str::select_normal);
        s.rows[kRowVariant].fixed = true;
        for (int k = 0; k < 2; k++) {
            const auto& names = k ? c.vtriggers : c.vskills;
            const int n = v[kRowVSkill + k] % std::max<int>(2, static_cast<int>(names.size()));
            s.rows[kRowVSkill + k].value = roman(n + 1) + (n < static_cast<int>(names.size()) && !names[n].empty() ? "  " + names[n] : "");
        }
        const int comment = v[kRowVComment] % (1 + static_cast<int>(c.vcomments.size()));
        s.rows[kRowVComment].value = comment == kCommentAuto ? std::string(TR(Str::select_auto)) : std::to_string(comment);
    }
    s.rows[kRowDelay].value = TRF(v[kRowDelay] == 1 ? Str::common_frame : Str::common_frames, v[kRowDelay]);
    const char* hints[kSelectRows] = {"", "", "", "", "", "", "Suggested for a 38 ms ping: 2 frames. Lower feels sharper but rolls back more."};
    const int comment = v[kRowVComment] % (1 + static_cast<int>(c.vcomments.size()));
    const std::string commentLine = eleven                    ? std::string(TR(Str::select_hint_eleven))
                                    : comment == kCommentAuto ? std::string(TR(Str::select_hint_comment_auto))
                                                              : TRF(Str::select_hint_comment, c.vcomments[comment - 1].c_str());
    for (int r = 0; r < kSelectRows; r++) s.rows[r].hint = r == kRowVComment || eleven ? commentLine : hints[r];
    return s;
}

void MockTick(float dt) {
    const int phase = MockPhase();
    if (g_mock.shown != phase) {
        g_mock.shown = phase;
        g_mock.inPhase = 0;
        g_mock.nextAct = 0.6f;
    }
    g_mock.inPhase += dt;
    if (phase == 3 || g_mock.inPhase < g_mock.nextAct) return;
    auto next = [] {
        g_mock.roll = g_mock.roll * 1664525u + 1013904223u;
        return g_mock.roll >> 8;
    };
    const int stages = static_cast<int>(g_preview.stages.size());
    if (phase == 0) {
        g_mock.stage = (g_mock.stage + 1) % (stages + 1);
        g_mock.setting = 0;
        g_mock.nextAct += 0.55f;
        return;
    }
    if (phase == 1) {
        std::vector<int> picks;
        for (const auto& row : g_rows) {
            for (const Slot& t : row) {
                if (t.chara >= 0) picks.push_back(t.chara);
            }
        }
        if (!picks.empty()) g_mock.chara[next() & 1] = picks[next() % picks.size()];
        g_mock.nextAct += 0.8f;
        return;
    }
    if (g_mock.inPhase > 3.0f) g_mock.ready[1] = true;
    if (g_mock.inPhase > 6.0f) g_mock.ready[0] = true;
    const int side = !g_mock.ready[0] && (g_mock.ready[1] || (next() & 1)) ? 0 : 1;
    if (!g_mock.ready[side]) {
        int* v = g_mock.values[side];
        const int row = static_cast<int>(next() % kRowVComment);
        const SelectChara& c = g_preview.charas[g_mock.chara[side]];
        v[row] = (v[row] + 1) % MockCount(c, row, v[kRowCostume]);
        if (row == kRowCostume) v[kRowColor] = 0;
    }
    g_mock.nextAct += 0.7f;
}

void PreviewReset() {
    g_mock = Mock{};
    g_ui = Ui{};
    BuildGrid(g_preview);
    g_mock.chara[0] = std::max(0, CharaAt(g_preview, "RYU"));
    g_mock.chara[1] = std::max(0, CharaAt(g_preview, "CNL"));
    int first = -1;
    for (size_t i = 0; i < g_preview.stages.size(); i++) {
        if (g_preview.stages[i].locked) continue;
        if (first < 0) first = static_cast<int>(i);
        if (g_preview.stages[i].code == "TAI") g_mock.stage = static_cast<int>(i);
    }
    if (g_mock.stage < 0 || g_mock.stage >= static_cast<int>(g_preview.stages.size()) || g_preview.stages[g_mock.stage].locked) {
        g_mock.stage = std::max(0, first);
    }
    SetMockPhase(g_previewPhase);
}

void DrawPreviewTabs(ImDrawList* d, const SelectFonts& f) {
    const char* const labels[] = {"Stage select", "Character", "Loadout", "Ready", "Close preview"};
    float x = 24;
    const int phase = MockPhase();
    for (int i = 0; i < 5; i++) {
        const float w = TextW(f.text, 15, labels[i]) + 28;
        const bool on = i < 4 && phase == i;
        const ImVec2 a = g_v.P(x, 16), b = g_v.P(x + w, 50);
        d->AddRectFilled(a, b, on ? Rgba(233, 196, 106) : Rgba(0, 0, 0, 150), g_v.L(17));
        d->AddRect(a, b, on ? Rgba(233, 196, 106) : Rgba(93, 90, 82), g_v.L(17));
        Text(d, f.text, g_v.L(15), g_v.P(x + w / 2, 24), on ? Rgba(21, 19, 13) : Rgba(244, 241, 232), labels[i], 0.5f);
        if (Clicked() && MouseIn(a, b)) {
            if (i == 4) g_previewOpen = false;
            else SetMockPhase(i);
        }
        x += w + 8;
    }
}

} // namespace

void SelectOpenPreview(SelectData data, int phase) {
    {
        std::lock_guard lock(g_dataMutex);
        g_preview = std::move(data);
        g_previewPhase = std::clamp(phase, 0, 3);
    }
    g_previewFresh = true;
    g_previewOpen = true;
    Log("select preview: %zu characters, %zu stages%s", g_preview.charas.size(), g_preview.stages.size(),
        g_preview.spectator ? ", watching" : "");
}

bool SelectPreviewOpen() { return g_previewOpen; }

void SelectDrawPreview(const SelectFonts& f) {
    if (!g_previewOpen || !f.title || !f.name || !f.text) return;
    std::lock_guard lock(g_dataMutex);
    if (g_preview.charas.empty()) {
        g_previewOpen = false;
        return;
    }
    g_draw = &g_preview;
    if (g_previewFresh) {
        g_previewFresh = false;
        PreviewReset();
    }
    if (g_gridFor != &g_preview) BuildGrid(g_preview);
    BeginScreen();
    if (g_preview.spectator) MockTick(ImGui::GetIO().DeltaTime);
    SelectLive live;
    live.you = g_preview.spectator ? -1 : 0;
    live.host = true;
    live.sides[0] = MockSide(0);
    live.sides[1] = MockSide(1);
    live.stage = g_mock.stage;
    live.setting = g_mock.setting;
    live.stageLocked = g_mock.locked;
    live.pingMs = g_preview.spectator ? 120 : 38;
    live.spectators = 3;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    SelectInput in;
    g_inputOn = true;
    Render(d, f, live, &in);
    DrawPreviewTabs(d, f);
    g_inputOn = false;
    d->PopClipRect();
    if (in.stage >= 0) g_mock.stage = in.stage, g_mock.setting = 0;
    if (in.setting >= 0) g_mock.setting = in.setting;
    if (in.lockStage) g_mock.locked = true;
    if (in.unlockStage) g_mock.locked = false;
    if (in.chara >= 0) g_mock.chara[0] = in.chara;
    if (in.randomPick) {
        static std::mt19937 rng{std::random_device{}()};
        g_mock.chara[0] = static_cast<int>(rng() % g_preview.charas.size());
        std::fill(std::begin(g_mock.values[0]), std::end(g_mock.values[0]), 0);
        g_mock.values[0][kRowDelay] = 2;
    }
    if (in.row >= 0) {
        const SelectChara& c = g_preview.charas[g_mock.chara[0]];
        int& v = g_mock.values[0][in.row];
        const int count = MockCount(c, in.row, g_mock.values[0][kRowCostume]);
        v = (v + in.delta + count) % count;
        if (in.row == kRowCostume) g_mock.values[0][kRowColor] = 0;
    }
    if (in.ready) g_mock.ready[0] = true;
    if (in.unready) g_mock.ready[0] = false;
    if (in.leave) g_previewOpen = false;
}

} // namespace sfv::mod
