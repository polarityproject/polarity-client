// The select screen's parts: backdrop, tiles, loadouts, player lines, card, leave dialog.

#include "mod/select_internal.h"

#include "mod/textures.h"

#include <algorithm>
#include <cstdio>

namespace sfv::mod {

using namespace selectui;
using namespace ui;

namespace {

const char* const kRowLabels[kSelectRows] = {"Costume", "Color", "Variant", "V-Skill", "V-Trigger", "V-Comment", "Input delay"};

} // namespace

namespace selectui {

const SelectChara* Chara(int i) {
    if (!g_draw || i < 0 || i >= static_cast<int>(g_draw->charas.size())) return nullptr;
    return &g_draw->charas[i];
}

const SelectStage* Shown(int stage, int setting) {
    if (!g_draw || stage < 0 || stage >= static_cast<int>(g_draw->stages.size())) return nullptr;
    const SelectStage& s = g_draw->stages[stage];
    return setting > 0 && setting <= static_cast<int>(s.settings.size()) ? &s.settings[setting - 1] : &s;
}

void DrawBackdrop(ImDrawList* d, int stage, int setting, ImU32 tint) {
    if (const SelectStage* s = Shown(stage, setting)) g_ui.lastStage = s->code;
    if (g_ui.lastStage.empty() && !g_draw->stages.empty()) g_ui.lastStage = g_draw->stages[0].code;
    if (g_ui.lastStage.empty()) return;
    static std::string shown, under;
    auto get = [](const std::string& path) {
        bool failed = false;
        const TextureView* t = BlurredStage(path, 6, 1.1f, &failed);
        return failed ? Texture(path) : t;
    };
    const std::string path = StageTex(g_ui.lastStage, "StageSelectBG");
    if (path != shown && get(path)) {
        under = shown;
        shown = path;
    }
    auto draw = [&](const TextureView* bg) {
        const float aspect = bg->height ? static_cast<float>(bg->width) / bg->height : 4.0f;
        const float visible = (1600.0f / 900.0f) / aspect;
        Image(d, bg, g_v.P(0, 0), g_v.P(1600, 900), ImVec2(0.5f - visible / 2, 0), ImVec2(0.5f + visible / 2, 1), tint);
    };
    const TextureView* top = shown.empty() ? nullptr : get(shown);
    if (!top) return;
    if (FadeIn(top) != IM_COL32_WHITE && !under.empty()) {
        if (const TextureView* below = get(under)) {
            const float aspect = below->height ? static_cast<float>(below->width) / below->height : 4.0f;
            const float visible = (1600.0f / 900.0f) / aspect;
            d->AddImage(ui::Id(below), g_v.P(0, 0), g_v.P(1600, 900), ImVec2(0.5f - visible / 2, 0), ImVec2(0.5f + visible / 2, 1), tint);
        }
    }
    draw(top);
    Grain(d);
}

void DrawLoadout(ImDrawList* d, const SelectFonts& f, bool right, const SelectSide& side, int highlight, bool interactive, SelectInput* in) {
    const float x0 = right ? 980.0f : 0.0f, top = 108, rowH = 42, gap = 4, w = 620;
    ImFont* font = f.title ? f.title : f.text;
    ImFont* bold = f.bold ? f.bold : font;
    for (int r = 0; r < kSelectRows; r++) {
        const SelectRow& row = side.rows[r];
        const float y = top + r * (rowH + gap);
        const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + rowH);
        const bool lit = r == highlight && interactive;
        if (lit) d->AddRectFilled(a, b, look::kGold);
        else {
            d->AddRectFilled(a, b, Rgba(8, 9, 11, 228));
            if (r == highlight) {
                const float ex = right ? x0 + w - 4 : x0;
                d->AddRectFilled(g_v.P(ex, y), g_v.P(ex + 4, y + rowH), look::kGold);
            }
            d->AddLine(ImVec2(a.x, b.y - 0.5f), ImVec2(b.x, b.y - 0.5f), look::kHairline, std::max(1.0f, g_v.L(1)));
        }
        const ImU32 ink = lit ? look::kInk : (row.fixed && interactive ? Rgba(244, 241, 232, 150) : look::kCream);
        ImFont* rf = lit ? bold : font;
        Text(d, rf, g_v.L(20), g_v.P(x0 + 44, y + 9), ink, kRowLabels[r]);
        const float mid = x0 + 190 + (w - 190) / 2;
        const float tw = TextW(rf, 20, row.value);
        const float sw = row.swatch1 ? 48.0f : 0.0f;
        const float tx = mid - (tw + sw) / 2;
        Text(d, rf, g_v.L(20), g_v.P(tx, y + 9), ink, row.value);
        if (row.swatch1) {
            const float sx = tx + tw + 10;
            d->AddRectFilled(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 31), FromArgb(row.swatch1));
            d->AddRect(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 31), Rgba(0, 0, 0));
            d->AddRectFilled(g_v.P(sx + 22, y + 11), g_v.P(sx + 40, y + 31), FromArgb(row.swatch2));
            d->AddRect(g_v.P(sx + 22, y + 11), g_v.P(sx + 40, y + 31), Rgba(0, 0, 0));
        }
        if (interactive && !row.fixed) {
            const ImU32 arrow = lit ? look::kInk : look::kGold;
            const float cy = y + rowH / 2, lx = x0 + 212, rx = x0 + w - 24;
            d->AddTriangleFilled(g_v.P(lx + 5, cy - 7), g_v.P(lx + 5, cy + 7), g_v.P(lx - 6, cy), arrow);
            d->AddTriangleFilled(g_v.P(rx - 5, cy - 7), g_v.P(rx + 6, cy), g_v.P(rx - 5, cy + 7), arrow);
            if (Clicked() && MouseIn(g_v.P(x0 + 190, y), g_v.P(x0 + 234, y + rowH))) {
                g_ui.row = r;
                in->row = r;
                in->delta = -1;
            } else if (Clicked() && MouseIn(g_v.P(x0 + w - 44, y), g_v.P(x0 + w, y + rowH))) {
                g_ui.row = r;
                in->row = r;
                in->delta = 1;
            }
        }
        if (interactive && Clicked() && MouseIn(a, b) && in->row < 0) g_ui.row = r;
    }
    const float y = top + kSelectRows * (rowH + gap);
    const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + 32);
    if (side.ready) {
        const ImU32 g1 = Rgba(217, 166, 50), g2 = Rgba(150, 108, 28, 230);
        d->AddRectFilledMultiColor(a, b, right ? g2 : g1, right ? g1 : g2, right ? g1 : g2, right ? g2 : g1);
        Text(d, f.name, g_v.L(20), g_v.P(x0 + 44, y + 4), Rgba(26, 20, 5), g_myCharacter ? "SAVED" : "READY");
    } else if (!g_myCharacter) {
        const std::string& hint = side.rows[interactive ? g_ui.row : kRowVComment].hint;
        if (!hint.empty()) {
            d->AddRectFilled(a, b, Rgba(8, 9, 11, 228));
            Text(d, f.text, g_v.L(17), g_v.P(x0 + 44, y + 6), look::kCream, Ellipsize(f.text, 17, hint, w - 64));
        }
    }
}

void DrawTile(ImDrawList* d, const Slot& t, bool p1, bool p2, const SelectFonts& f) {
    ImVec2 q[4];
    for (int i = 0; i < 4; i++) q[i] = g_v.P(t.q[i].x, t.q[i].y);
    const bool on = p1 || p2;
    if (t.random) {
        d->AddQuadFilled(q[0], q[1], q[2], q[3], on ? Rgba(240, 194, 74) : Rgba(157, 153, 143));
        ImVec2 in[4] = {ImVec2(q[0].x + g_v.L(2.5f), q[0].y + g_v.L(2)), ImVec2(q[1].x - g_v.L(2.5f), q[1].y + g_v.L(2)),
                        ImVec2(q[2].x - g_v.L(2), q[2].y - g_v.L(2)), ImVec2(q[3].x + g_v.L(2), q[3].y - g_v.L(2))};
        GradientQuad(d, in, Rgba(44, 44, 44), Rgba(6, 6, 6));
        Text(d, f.title, g_v.L(58), ImVec2((q[0].x + q[1].x) / 2, q[0].y + g_v.L(26)), Rgba(255, 255, 255), "?", 0.5f, true);
        return;
    }
    d->AddQuadFilled(q[0], q[1], q[2], q[3], on ? Rgba(240, 194, 74) : Rgba(157, 153, 143));
    const float k = kK, in = 2;
    ImVec2 iq[4];
    if (t.right) {
        iq[0] = g_v.P(t.q[0].x + in - k * in, t.q[0].y + in);
        iq[1] = g_v.P(t.q[1].x - in - k * in, t.q[1].y + in);
        iq[2] = g_v.P(t.q[2].x - in + k * in, t.q[2].y - in);
        iq[3] = g_v.P(t.q[3].x + in + k * in, t.q[3].y - in);
    } else {
        iq[0] = g_v.P(t.q[0].x + in + k * in, t.q[0].y + in);
        iq[1] = g_v.P(t.q[1].x - in + k * in, t.q[1].y + in);
        iq[2] = g_v.P(t.q[2].x - in - k * in, t.q[2].y - in);
        iq[3] = g_v.P(t.q[3].x + in - k * in, t.q[3].y - in);
    }
    d->AddQuadFilled(iq[0], iq[1], iq[2], iq[3], Rgba(20, 22, 26));
    if (const SelectChara* c = Chara(t.chara)) {
        if (const TextureView* tex = Texture(CharaTex(c->code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"))) {
            const ImVec2* art = t.right ? kArtRight : kArtLeft;
            auto uv = [&](int i) { return ImVec2(art[i].x / kIconW, art[i].y / kIconH); };
            const ImU32 tint = c->locked ? Rgba(70, 70, 70) : on ? IM_COL32_WHITE
                                                                 : Rgba(205, 205, 205);
            d->AddImageQuad(Id(tex), iq[0], iq[1], iq[2], iq[3], uv(0), uv(1), uv(2), uv(3), FadeIn(tex, tint));
        }
    }
    const float mid = 0.55f;
    ImVec2 lower[4] = {ImVec2(iq[0].x + (iq[3].x - iq[0].x) * mid, iq[0].y + (iq[3].y - iq[0].y) * mid),
                       ImVec2(iq[1].x + (iq[2].x - iq[1].x) * mid, iq[1].y + (iq[2].y - iq[1].y) * mid), iq[2], iq[3]};
    GradientQuad(d, lower, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, on ? 40 : 150));
    if (p1) {
        const ImVec2 at(iq[0].x + g_v.L(t.right ? kSlant - 6 : 2.0f), iq[0].y + g_v.L(2));
        d->AddRectFilled(at, ImVec2(at.x + g_v.L(20), at.y + g_v.L(15)), Rgba(47, 111, 228));
        Text(d, f.text, g_v.L(12), ImVec2(at.x + g_v.L(10), at.y + g_v.L(1)), Rgba(255, 255, 255), "1P", 0.5f);
    }
    if (p2) {
        const ImVec2 at(iq[2].x - g_v.L(t.right ? kSlant - 6 : 2.0f), iq[2].y - g_v.L(2));
        d->AddRectFilled(ImVec2(at.x - g_v.L(20), at.y - g_v.L(15)), at, Rgba(210, 59, 49));
        Text(d, f.text, g_v.L(12), ImVec2(at.x - g_v.L(10), at.y - g_v.L(14)), Rgba(255, 255, 255), "2P", 0.5f);
    }
}

void DrawPlayerLine(ImDrawList* d, const SelectFonts& f, const SelectLive& live, int i) {
    const float y = 470;
    const ImU32 white = Rgba(244, 241, 232), dim = Rgba(168, 162, 147), icon = Rgba(217, 212, 198);
    const bool left = i == 0;
    const SelectSide& s = live.sides[i];
    if (!s.present) {
        Text(d, f.text, g_v.L(16), g_v.P(left ? 120.0f : 1480.0f, y + 4), dim, "WAITING FOR A PLAYER...", left ? 0.0f : 1.0f);
        return;
    }
    const std::string player = Upper(s.player.empty() ? "Player" : s.player);
    const bool you = live.you == i;
    const bool link = live.you >= 0 && !you;
    char ping[32] = "";
    if (link && live.pingMs) std::snprintf(ping, sizeof(ping), "%u ms", live.pingMs);
    const float pw = (s.link ? 34.0f : 0.0f) + (link ? 34.0f : 0.0f) + (ping[0] ? TextW(f.text, 18, ping) + 18 : 0.0f);
    const float dir = left ? 1.0f : -1.0f;
    float x = left ? 120.0f : 1480.0f;
    Text(d, f.text, g_v.L(16), g_v.P(x, y + 4), white, player, left ? 0.0f : 1.0f);
    x += dir * (TextW(f.text, 16, player) + 10);
    if (you) {
        Text(d, f.text, g_v.L(16), g_v.P(x, y + 4), dim, "(you)", left ? 0.0f : 1.0f);
        x += dir * (TextW(f.text, 16, "(you)") + 10);
    }
    if (pw <= 0) return;
    const float px = left ? x : x - pw;
    Pill(d, g_v.P(px, y), g_v.P(px + pw, y + 26));
    float cx = px;
    if (s.link) {
        IconLinkType(d, g_v.P(cx + 17, y + 13), g_v.s, s.link, icon);
        cx += 34;
    }
    if (link) {
        if (live.relayed) IconCloud(d, g_v.P(cx + 17, y + 13), g_v.s, Rgba(233, 196, 106));
        else IconLink(d, g_v.P(cx + 17, y + 13), g_v.s, icon);
        cx += 34;
    }
    if (ping[0]) Text(d, f.text, g_v.L(18), g_v.P(cx + 4, y + 3), PingColor(live.pingMs), ping);
}

void DrawRender(ImDrawList* d, int chara, bool right) {
    const SelectChara* c = Chara(chara);
    if (!c) return;
    const TextureView* t = Texture(CharaTex(c->code, "PartCharacter"));
    if (!t) return;
    if (right) Image(d, t, g_v.P(1600 + 60 - 780, -70), g_v.P(1660, 710), ImVec2(1, 0), ImVec2(0, 1));
    else Image(d, t, g_v.P(-60, -70), g_v.P(720, 710));
}

void StatusPill(ImDrawList* d, const SelectFonts& f, const char* text) {
    const float w = TextW(f.text, 19, text) + 44;
    Pill(d, g_v.P(800 - w / 2, 856), g_v.P(800 + w / 2, 892));
    Text(d, f.text, g_v.L(19), g_v.P(800, 862), Rgba(233, 196, 106), text, 0.5f);
}

void OpenLeave() {
    g_ui.leaving = true;
    g_ui.leaveChosen = false;
}

void DrawLeaveDialog(ImDrawList* d, const SelectFonts& f, const SelectLive& live, bool takeInput, SelectInput* in) {
    const bool watching = live.you < 0;
    const char* title = watching ? "Stop watching?" : live.set ? "Leave the set?"
                                                               : "Leave the session?";
    const char* detail = watching   ? "You'll return to the main menu."
                         : live.set ? "The set ends here for both players."
                                    : "The session ends for both players.";
    ImFont* bold = f.bold ? f.bold : f.text;
    ui::ModalSpec m;
    m.w = 600;
    m.h = 44 + 22 + 34 + 12 + 24 + 24 + 48 + 26 + 46;
    m.x0 = 500;
    m.y0 = (844 - m.h) / 2;
    m.kicker = watching ? "STOP WATCHING" : "LEAVE";
    m.danger = !watching;
    m.prompts = {{"button_a", "Select"}, {"button_b", "Stay"}};
    int prompt = -1;
    float y = ui::Modal(d, bold, f.text, m, &prompt) + 22;
    Text(d, f.name, g_v.L(28), g_v.P(528, y), Rgba(244, 241, 232), title);
    y += 46;
    Text(d, f.text, g_v.L(19), g_v.P(528, y), look::kCream, detail);
    y += 24 + 24;
    int pressed = -1;
    if (ui::Button(d, bold, 528, y, 806, y + 48, watching ? "Stop watching" : "Leave", g_ui.leaveChosen, !watching)) pressed = 1;
    if (ui::Button(d, bold, 822, y, 1072, y + 48, "Stay", !g_ui.leaveChosen)) pressed = 0;
    if (!takeInput) return;
    if (Left() || Right()) g_ui.leaveChosen = !g_ui.leaveChosen;
    if (pressed >= 0) g_ui.leaveChosen = pressed == 1;
    if (Back()) g_ui.leaving = false;
    else if (Confirm() || pressed >= 0) {
        in->leave = g_ui.leaveChosen;
        g_ui.leaving = false;
    }
}

void DrawCard(ImDrawList* d, const SelectFonts& f, const SelectCard& c) {
    const float x0 = 980, y0 = 108, x1 = 1552, y1 = y0 + 48 + 66 + 20 + static_cast<float>(c.rows.size()) * 36 + 62;
    const ImU32 white = look::kCream, dim = look::kSoft, text = look::kCream, gold = look::kGold, green = Rgba(127, 216, 139);
    ImFont* medium = f.title ? f.title : f.text;
    ImFont* bold = f.bold ? f.bold : medium;
    d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(8, 9, 11, 228));
    d->AddRect(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(201, 168, 92, 150), 0, 0, std::max(1.0f, g_v.L(1)));
    Text(d, bold, g_v.L(16), g_v.P(x0 + 22, y0 + 18), gold, "CURRENT LOADOUT");
    const float bx0 = x0 + 22, by0 = y0 + 48, bx1 = x1 - 22, by1 = by0 + 66;
    d->AddRectFilled(g_v.P(bx0, by0), g_v.P(bx1, by1), Rgba(0, 0, 0, 120));
    float x = bx0 + 12;
    if (const TextureView* t = c.chara.empty() ? nullptr : Texture(CharaTex(c.chara, "CharaSelectIconLeft"))) {
        Image(d, t, g_v.P(x, by0 + 8), g_v.P(x + 40, by0 + 58), ImVec2(0.12f, 0.0f), ImVec2(0.88f, 0.95f));
    }
    x += 52;
    const std::string player = Upper(c.player.empty() ? "Player" : c.player);
    Text(d, bold, g_v.L(19), g_v.P(x, by0 + 21), white, player);
    x += TextW(bold, 19, player) + 6;
    Text(d, f.text, g_v.L(17), g_v.P(x, by0 + 22), dim, "(you)");
    x += TextW(f.text, 17, "(you)") + 14;
    Text(d, medium, g_v.L(18), g_v.P(x, by0 + 21), white, c.charaName);
    Text(d, medium, g_v.L(17), g_v.P(bx1 - 12, by0 + 22), gold, c.place, 1.0f);
    float y = by1 + 20;
    for (const SelectCard::Row& r : c.rows) {
        Text(d, f.text, g_v.L(18), g_v.P(x0 + 26, y), dim, r.label);
        const std::string value = Ellipsize(medium, 18, r.value, x1 - 22 - (x0 + 170) - (r.swatch1 ? 46 : 0));
        Text(d, medium, g_v.L(18), g_v.P(x0 + 170, y), white, value);
        if (r.swatch1) {
            const float sx = x0 + 170 + TextW(medium, 18, value) + 10;
            d->AddRectFilled(g_v.P(sx, y + 2), g_v.P(sx + 17, y + 19), FromArgb(r.swatch1));
            d->AddRectFilled(g_v.P(sx + 21, y + 2), g_v.P(sx + 38, y + 19), FromArgb(r.swatch2));
        }
        y += 36;
    }
    const float sy = y1 - 50;
    Text(d, medium, g_v.L(17), g_v.P(x0 + 22, sy), c.done ? green : text, c.status);
    Text(d, medium, g_v.L(17), g_v.P(x1 - 22, sy), c.done ? green : gold, c.statusRight, 1.0f);
    if (c.progress >= 0) {
        const float ly = y1 - 22, lx0 = x0 + 22, lx1 = x1 - 22;
        d->AddRectFilled(g_v.P(lx0, ly), g_v.P(lx1, ly + 4), Rgba(255, 255, 255, 40));
        const float p = std::clamp(c.progress, 0.0f, 1.0f);
        d->AddRectFilled(g_v.P(lx0, ly), g_v.P(lx0 + (lx1 - lx0) * p, ly + 4), c.done ? green : gold);
    }
}

} // namespace selectui

} // namespace sfv::mod
