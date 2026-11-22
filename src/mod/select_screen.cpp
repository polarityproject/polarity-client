// The select screen: the grid, the screen for a lobby's state, the live screen.

#include "mod/select_internal.h"

#include "mod/log.h"
#include "mod/pak_textures.h"
#include "mod/textures.h"
#include "mod/ui_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace sfv::mod {

using namespace selectui;
using namespace ui;

namespace {

void DrawPrompts(ImDrawList* d, const SelectFonts& f, const std::vector<Prompt>& prompts) { ui::DrawPrompts(d, f.text, prompts); }

SelectData g_data;
const char* const kTopLeft[] = {"RYU", "CNL", "RMK", "RSD", "NSH", "Z21", "Z23", "Z28", "Z32", "Z36", "Z39"};
const char* const kTopRight[] = {"KEN", "CMY", "BLR", "NCL", "GUL", "Z20", "Z25", "Z29", "Z33", "Z37", "Z41"};
const char* const kBottomLeft[] = {"ALX", "KRN", "ZGF", "LAR", "IBK", "JRI", "Z22", "Z26", "Z30", "Z34", "Z38", "Z42"};
const char* const kBottomRight[] = {"VEG", "BRD", "DSM", "FAN", "BSN", "URN", "Z24", "Z27", "Z31", "Z35", "Z40", "ELV"};

} // namespace

namespace selectui {

int CharaAt(const SelectData& data, const std::string& code) {
    for (size_t i = 0; i < data.charas.size(); i++) {
        if (data.charas[i].code == code) return static_cast<int>(i);
    }
    return -1;
}

void BuildGrid(const SelectData& data) {
    g_gridFor = &data;
    auto quad = [](Slot& t, float x, float y, bool right) {
        t.right = right;
        if (right) {
            t.q[0] = ImVec2(x + kSlant, y);
            t.q[1] = ImVec2(x + kSlant + kTopW, y);
            t.q[2] = ImVec2(x + kTopW, y + kTH);
            t.q[3] = ImVec2(x, y + kTH);
        } else {
            t.q[0] = ImVec2(x, y);
            t.q[1] = ImVec2(x + kTopW, y);
            t.q[2] = ImVec2(x + kTopW + kSlant, y + kTH);
            t.q[3] = ImVec2(x + kSlant, y + kTH);
        }
    };
    auto half = [&](std::vector<Slot>& row, const char* const* codes, size_t n, int r, bool right) {
        const float y = r ? kY1 : kY0;
        std::vector<Slot> slots;
        for (size_t j = 0; j < n; j++) {
            Slot t;
            t.chara = CharaAt(data, codes[j]);
            const float x = right ? kC + kHalf - kK * (y - kY0) + j * kPitch - kSlant : kC - kHalf + kK * (y - kY0) - j * kPitch - kTopW;
            quad(t, x, y, right);
            slots.push_back(t);
        }
        if (!right) std::reverse(slots.begin(), slots.end());
        row.insert(row.end(), slots.begin(), slots.end());
    };
    for (auto& row : g_rows) row.clear();
    half(g_rows[0], kTopLeft, std::size(kTopLeft), 0, false);
    Slot random;
    random.random = true;
    const float ry = kY0 + 40, rb = kY1 + kTH - 6;
    const float xt = kC - kHalf + 5 + kK * (ry - kY0), xb = kC - kHalf + 5 + kK * (rb - kY0);
    random.q[0] = ImVec2(xt, ry);
    random.q[1] = ImVec2(2 * kC - xt, ry);
    random.q[2] = ImVec2(2 * kC - xb, rb);
    random.q[3] = ImVec2(xb, rb);
    g_rows[0].push_back(random);
    half(g_rows[0], kTopRight, std::size(kTopRight), 0, true);
    half(g_rows[1], kBottomLeft, std::size(kBottomLeft), 1, false);
    g_rows[1].push_back(random);
    half(g_rows[1], kBottomRight, std::size(kBottomRight), 1, true);
}

} // namespace selectui

namespace {

bool CursorTo(int chara) {
    for (int r = 0; r < 2; r++) {
        for (int x = 0; x < static_cast<int>(g_rows[r].size()); x++) {
            if (g_rows[r][x].chara == chara && chara >= 0) {
                g_ui.cursorRow = r;
                g_ui.cursorX = x;
                return true;
            }
        }
    }
    return false;
}

void MoveCursor(int dr, int dx) {
    if (dx) {
        const int n = static_cast<int>(g_rows[g_ui.cursorRow].size());
        g_ui.cursorX = (g_ui.cursorX + dx + n) % n;
    }
    if (dr) {
        const ImVec2* q = g_rows[g_ui.cursorRow][g_ui.cursorX].q;
        const float x = (q[0].x + q[2].x) / 2;
        const int to = 1 - g_ui.cursorRow;
        int best = 0;
        float bestD = 1e9f;
        for (int i = 0; i < static_cast<int>(g_rows[to].size()); i++) {
            const ImVec2* o = g_rows[to][i].q;
            const float dist = std::fabs((o[0].x + o[2].x) / 2 - x);
            if (dist < bestD) {
                bestD = dist;
                best = i;
            }
        }
        g_ui.cursorRow = to;
        g_ui.cursorX = best;
    }
}

} // namespace

namespace selectui {

void Render(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in) {
    g_myCharacter = live.myCharacter;
    if (g_gridFor != g_draw) BuildGrid(*g_draw);
    const bool watching = live.you < 0;
    const SelectSide* me = watching ? nullptr : &live.sides[live.you];
    if (live.stageLocked && !g_ui.wasLocked && !(live.myCharacter && g_ui.cameFromStage)) {
        g_ui.loadout = false;
        g_ui.onRandom = false;
        g_ui.lastChara = Ui::kNoChara;
    }
    if (!live.stageLocked) g_ui.cameFromStage = true;
    else if (g_ui.wasLocked) g_ui.cameFromStage = false;
    g_ui.wasLocked = live.stageLocked;
    if (!live.myCharacter || live.stageLocked) g_ui.randomList = false;
    const bool dialog = g_ui.leaving || g_ui.randomList, inputOn = g_inputOn;
    if (dialog) g_inputOn = false;

    std::vector<Prompt> prompts;
    if (!live.stageLocked) {
        DrawBackdrop(d, live.stage, live.setting, Rgba(70, 69, 67));
        d->AddRectFilledMultiColor(g_v.P(0, 0), g_v.P(1600, 450), Rgba(6, 8, 11, 90), Rgba(6, 8, 11, 90), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0));
        d->AddRectFilledMultiColor(g_v.P(0, 450), g_v.P(1600, 900), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 170), Rgba(6, 8, 11, 170));
        DrawStageSelect(d, f, live, in);
        for (const auto& row : g_rows) {
            for (const Slot& t : row) {
                if (const SelectChara* c = Chara(t.chara)) Texture(CharaTex(c->code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"));
            }
        }
        for (const SelectSide& s : live.sides) {
            if (const SelectChara* c = s.present ? Chara(s.chara) : nullptr) Texture(CharaTex(c->code, "PartCharacter"));
        }
        for (const char* icon : {"button_a", "button_b", "button_y"}) Texture(PadTex(icon));
        const SelectStage* stage = Shown(live.stage, 0);
        if (live.host && !watching && stage && !stage->settings.empty()) prompts.push_back({"button_y", "Setting", 3});
        const bool onRandomStage = live.myCharacter && (in->stage >= 0 ? in->stage : live.stage) >= static_cast<int>(g_draw->stages.size());
        if (onRandomStage) prompts.push_back({"button_x", "Random stages", 2});
        if (live.host && !watching) prompts.push_back({"button_a", "Confirm stage", 0});
        prompts.push_back({"button_b", live.myCharacter ? "Back" : "Leave", 1});
        if (onRandomStage && Pressed({ImGuiKey_GamepadFaceLeft}, false)) {
            g_ui.randomList = true;
            g_ui.variantsOf = -1;
            g_ui.listNote.clear();
        }
        if (Back()) {
            if (live.myCharacter) in->closeStage = true;
            else OpenLeave();
        }
    } else {
        const bool ready = me && me->ready;
        const bool grid = me && !ready && !g_ui.loadout;
        const bool loadout = me && !ready && g_ui.loadout;
        if (me && me->chara != g_ui.lastChara) {
            g_ui.onRandom = !CursorTo(me->chara) && g_ui.onRandom;
            g_ui.lastChara = me->chara;
        }
        if (grid) {
            if (Left()) MoveCursor(0, -1);
            if (Right()) MoveCursor(0, 1);
            if (Up() || Down()) MoveCursor(1, 0);
            g_ui.onRandom = g_rows[g_ui.cursorRow][g_ui.cursorX].random;
        }
        const int hover = g_ui.onRandom ? -1 : g_rows[g_ui.cursorRow][g_ui.cursorX].chara;
        bool onGrid[2], onRandom[2], showLoadout[2];
        int shown[2], lit[2];
        for (int i = 0; i < 2; i++) {
            const SelectSide& s = live.sides[i];
            const bool mine = live.you == i;
            onGrid[i] = mine ? grid : s.present && !s.ready && s.screen == 1;
            onRandom[i] = onGrid[i] && (mine ? g_ui.onRandom : s.hoverRandom);
            shown[i] = !s.present ? -1 : onGrid[i] ? (onRandom[i] ? -1 : mine ? hover
                                                                              : s.hover)
                                                   : s.chara;
            showLoadout[i] = s.present && s.chara >= 0 &&
                             (mine ? !grid : s.ready || s.screen == 2 || (s.screen == 0 && (watching || !grid)));
            lit[i] = mine ? (loadout ? g_ui.row : -1) : !s.ready && s.screen == 2 ? s.row
                                                                                  : -1;
        }
        in->screen = grid ? 1 : me ? 2
                                   : 0;
        in->hover = hover;
        in->hoverRandom = g_ui.onRandom;
        in->screenRow = g_ui.row;
        if (loadout) {
            if (Up()) g_ui.row = (g_ui.row + kSelectRows - 1) % kSelectRows;
            if (Down()) g_ui.row = (g_ui.row + 1) % kSelectRows;
            if (!me->rows[g_ui.row].fixed) {
                if (Left()) in->row = g_ui.row, in->delta = -1;
                if (Right()) in->row = g_ui.row, in->delta = 1;
            }
        }

        DrawBackdrop(d, live.stage, live.setting, Rgba(118, 116, 112));
        d->AddRectFilledMultiColor(g_v.P(0, 0), g_v.P(1600, 300), Rgba(6, 8, 11, 120), Rgba(6, 8, 11, 120), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0));
        DrawRender(d, shown[0], false);
        DrawRender(d, shown[1], true);
        d->AddRectFilledMultiColor(g_v.P(0, 500), g_v.P(1600, 640), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 230), Rgba(6, 8, 11, 230));
        d->AddRectFilled(g_v.P(0, 640), g_v.P(1600, 900), Rgba(6, 8, 11, 200));
        for (int i = 0; i < 2; i++) {
            if (showLoadout[i]) DrawLoadout(d, f, i == 1, live.sides[i], lit[i], loadout && live.you == i, in);
        }
        for (int i = 0; i < 2; i++) {
            if (live.myCharacter && i == 1) continue;
            DrawPlayerLine(d, f, live, i);
            const SelectChara* c = Chara(shown[i]);
            const std::string name = onRandom[i] ? "RANDOM" : c ? Upper(c->name)
                                                                : "";
            Text(d, f.name, g_v.L(74), g_v.P(i ? 1482.0f : 118.0f, 496), Rgba(255, 255, 255), name, i ? 1.0f : 0.0f, true);
        }
        Text(d, f.name, g_v.L(42), g_v.P(800, 502), Rgba(255, 255, 255), live.myCharacter ? "MY CHARACTER" : "CHARACTER SELECT", 0.5f, true);
        if (live.myCharacter) {
            DrawCard(d, f, live.card);
            const std::string& note = loadout ? me->rows[g_ui.row].hint : live.note;
            Text(d, f.text, g_v.L(19), g_v.P(48, 860), Rgba(236, 232, 220), note);
        } else Infinity(d, g_v.P(800, 578), g_v.L(56), Rgba(242, 201, 76));
        for (int r = 0; r < 2; r++) {
            for (int x = 0; x < static_cast<int>(g_rows[r].size()); x++) {
                const Slot& t = g_rows[r][x];
                if (r == 1 && t.random) continue;
                bool tag[2];
                for (int i = 0; i < 2; i++) tag[i] = t.random ? onRandom[i] : t.chara >= 0 && t.chara == shown[i];
                DrawTile(d, t, tag[0], tag[1], f);
                if (grid && Clicked() && MouseInQuad(t.q)) {
                    if (g_ui.cursorRow == r && g_ui.cursorX == x) ui::ClickPad(ImGuiKey_GamepadFaceDown);
                    g_ui.cursorRow = r;
                    g_ui.cursorX = x;
                    g_ui.onRandom = t.random;
                }
            }
        }
        if (grid && live.myCharacter) {
            prompts.push_back({"button_a", "Select", 0});
            prompts.push_back({"button_x", "Preferred stage", 2});
            prompts.push_back({"button_y", "Random", 3});
            prompts.push_back({"button_b", live.keepLabel.empty() ? "Back" : live.keepLabel, 1});
        } else if (grid) {
            prompts.push_back({"button_a", "Select", 0});
            prompts.push_back({"button_b", live.host ? "Stage" : "Leave", 1});
        } else if (watching) {
            prompts.push_back({"button_b", "Leave", 1});
        } else if (loadout && live.myCharacter) {
            prompts.push_back({"button_a", "Confirm", 0});
            prompts.push_back({"button_x", "Preferred stage", 2});
            prompts.push_back({"button_b", "Characters", 1});
        } else if (loadout) {
            prompts.push_back({"button_a", "Ready", 0});
            prompts.push_back({"button_b", "Back", 1});
        }
        const SelectChara* under = !g_ui.onRandom && hover >= 0 ? Chara(hover) : nullptr;
        if (grid && under && under->locked) StatusPill(d, f, ("You don't own " + under->name).c_str());
        if (ready && live.myCharacter) {
            prompts.push_back({"button_a", "Back to the room", 0});
            prompts.push_back({"button_b", "Change again", 1});
        } else if (ready) {
            if (live.bothReady) {
                StatusPill(d, f, live.loading ? kLoadingText : "Both ready. Starting...");
                prompts.push_back({"button_b", "Leave", 1});
            } else {
                StatusPill(d, f, "Ready. Waiting for your opponent...");
                prompts.push_back({"button_b", "Not ready", 1});
            }
        }
        const bool a = Confirm();
        if (grid && a && under && under->locked) {
        } else if (grid && a) {
            if (g_ui.onRandom) in->randomPick = true;
            else if (hover >= 0) in->chara = hover;
            g_ui.loadout = true;
            g_ui.onRandom = false;
            g_ui.lastChara = Ui::kNoChara;
            g_ui.row = 0;
        } else if (loadout && a) {
            in->ready = true;
            PlayUi(UiSound::ReadyOn);
        } else if (ready && a && live.myCharacter) {
            in->done = true;
        }
        if ((grid || loadout) && live.myCharacter && Pressed({ImGuiKey_GamepadFaceLeft}, false)) in->openStage = true;
        if (grid && live.myCharacter && Pressed({ImGuiKey_GamepadFaceUp}, false)) {
            in->randomPick = true;
            g_ui.loadout = true;
            g_ui.onRandom = false;
            g_ui.lastChara = Ui::kNoChara;
            g_ui.row = 0;
        }
        if (Back()) {
            if (grid && live.myCharacter) in->cancel = true;
            else if (ready && !live.bothReady) in->unready = true, PlayUi(UiSound::ReadyOff);
            else if (loadout) g_ui.loadout = false;
            else if (grid && live.host) in->unlockStage = true;
            else OpenLeave();
        }
    }
    if (watching) {
        const char* text = "WATCHING";
        char detail[64];
        if (live.pingMs) std::snprintf(detail, sizeof(detail), "%u ms to the host  \xC2\xB7  %d watching", live.pingMs, std::max(1, live.spectators));
        else std::snprintf(detail, sizeof(detail), "%d watching", std::max(1, live.spectators));
        const float tw = TextW(f.text, 19, text), dw = TextW(f.text, 18, detail);
        const float w = 24 + tw + 18 + 30 + dw + 24;
        const float bx = 800 - w / 2;
        Pill(d, g_v.P(bx, 16), g_v.P(bx + w, 52));
        Text(d, f.text, g_v.L(19), g_v.P(bx + 24, 22), Rgba(242, 201, 76), text);
        const float ix = bx + 24 + tw + 18;
        if (live.relayed) IconCloud(d, g_v.P(ix + 10, 34), g_v.s, Rgba(233, 196, 106));
        else IconLink(d, g_v.P(ix + 10, 34), g_v.s, Rgba(217, 212, 198));
        Text(d, f.text, g_v.L(18), g_v.P(ix + 28, 23), Rgba(236, 232, 220), detail);
    }
    if (watching && live.loading) StatusPill(d, f, kLoadingText);
    g_inputOn = inputOn;
    if (g_ui.leaving) DrawLeaveDialog(d, f, live, dialog, in);
    else if (g_ui.randomList) DrawRandomList(d, f, live, in);
    else DrawPrompts(d, f, prompts);
}

} // namespace selectui

void SelectSetData(SelectData data) {
    std::lock_guard lock(g_dataMutex);
    g_data = std::move(data);
    g_gridFor = nullptr;
}

void SelectReset() {
    std::lock_guard lock(g_dataMutex);
    g_ui = Ui{};
}

void SelectWarmTextures() {
    std::lock_guard lock(g_dataMutex);
    if (g_data.charas.empty()) return;
    if (g_gridFor != &g_data) BuildGrid(g_data);
    g_drawing = false;

    struct Restore {
        ~Restore() { g_drawing = true; }
    } restore;

    for (const char* name : {"StagePreviewWindowShadow", "StagePreviewWindow", "StageSelectCursor", "StageSelect_Random"}) {
        Texture(MenuTex(name));
    }
    for (const char* icon : {"button_a", "button_b", "button_y", "button_start"}) Texture(PadTex(icon));
    for (const SelectStage& s : g_data.stages) Texture(StageTex(s.code, "StageSelectIcon"));
    for (const auto& row : g_rows) {
        for (const Slot& t : row) {
            if (t.chara >= 0 && t.chara < static_cast<int>(g_data.charas.size())) {
                Texture(CharaTex(g_data.charas[t.chara].code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"));
            }
        }
    }
    for (const SelectStage& s : g_data.stages) Texture(StageTex(s.code, "StageSelectBG"));
    for (const SelectStage& s : g_data.stages) {
        for (const SelectStage& k : s.settings) {
            Texture(StageTex(k.code, "StageSelectIcon"));
            Texture(StageTex(k.code, "StageSelectBG"));
        }
    }
    for (const SelectChara& c : g_data.charas) Texture(CharaTex(c.code, "PartCharacter"));
}

void SelectWarmBootTextures(const SelectData& data, const std::string& chara, const std::string& stage) {
    std::vector<std::pair<std::string, uint32_t>> want;
    auto add = [&](std::string path) { want.emplace_back(std::move(path), 0); };
    for (const char* icon : {"button_a", "button_b", "button_x", "button_y", "button_start", "button_back", "button_lb", "button_rb"}) {
        add(PadTex(icon));
    }
    if (!chara.empty()) add(CharaTex(chara, "PartCharacter"));
    if (!stage.empty()) add(StageTex(stage, "StageSelectBG"));
    for (const char* name : {"StagePreviewWindowShadow", "StagePreviewWindow", "StageSelectCursor", "StageSelect_Random"}) add(MenuTex(name));
    auto onRight = [](const std::string& code) {
        auto in = [&](const auto& side) { return std::any_of(std::begin(side), std::end(side), [&](const char* c) { return code == c; }); };
        return in(kTopRight) || in(kBottomRight);
    };
    for (const SelectChara& c : data.charas) add(CharaTex(c.code, onRight(c.code) ? "CharaSelectIconRight" : "CharaSelectIconLeft"));
    for (const SelectStage& s : data.stages) add(StageTex(s.code, "StageSelectIcon"));
    for (const SelectStage& s : data.stages) add(StageTex(s.code, "StageSelectBG"));
    WarmPakTextures(want);
    Log("select screen: warming %zu textures (favorites %s, %s)", want.size(), chara.empty() ? "-" : chara.c_str(),
        stage.empty() ? "-" : stage.c_str());
}

void SelectDrawLive(const SelectFonts& f, const SelectLive& live, float alpha, bool takeInput, SelectInput* input) {
    *input = SelectInput{};
    if (!f.title || !f.name || !f.text || alpha <= 0) return;
    std::lock_guard lock(g_dataMutex);
    if (g_data.charas.empty()) return;
    g_draw = &g_data;
    BeginScreen();
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const int from = d->VtxBuffer.Size;
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    if (takeInput) SetUiSoundSet(live.stageLocked ? UiSoundSet::CharaSelect : UiSoundSet::Menu);
    Render(d, f, live, input);
    g_inputOn = false;
    d->PopClipRect();
    FadeFrom(d, from, alpha);
}

} // namespace sfv::mod
