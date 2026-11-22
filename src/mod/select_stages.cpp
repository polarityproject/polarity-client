// The stage select and My character's Random stages list.

#include "mod/select_internal.h"

#include "core/text.h"
#include "core/random_stage.h"
#include "mod/textures.h"

#include <algorithm>
#include <cstdio>

namespace sfv::mod {

using namespace selectui;
using namespace ui;

namespace {

struct ListVariant {
    std::string code, name;
    bool locked = false;
};

struct ListStage {
    std::string name;
    std::vector<ListVariant> variants;
    bool locked = true;
};

std::vector<ListStage> RandomList() {
    std::vector<ListStage> list;
    if (!g_draw) return list;
    for (const SelectStage& s : g_draw->stages) {
        if (IsNeverRandomStage(s.code)) continue;
        ListStage l;
        l.name = s.name;
        l.variants.push_back({s.code, s.name, s.locked});
        for (const SelectStage& k : s.settings) {
            if (!IsNeverRandomStage(k.code)) l.variants.push_back({k.code, k.name, k.locked});
        }
        for (const ListVariant& v : l.variants) l.locked = l.locked && v.locked;
        list.push_back(std::move(l));
    }
    return list;
}

bool IsOff(const std::vector<std::string>& off, const std::string& code) { return std::find(off.begin(), off.end(), code) != off.end(); }

std::pair<int, int> VariantsOn(const ListStage& l, const std::vector<std::string>& off) {
    int on = 0, owned = 0;
    for (const ListVariant& v : l.variants) {
        if (v.locked) continue;
        owned++;
        if (!IsOff(off, v.code)) on++;
    }
    return {on, owned};
}

std::pair<int, int> StagesOn(const std::vector<ListStage>& list, const std::vector<std::string>& off) {
    int on = 0, owned = 0;
    for (const ListStage& l : list) {
        const auto [a, b] = VariantsOn(l, off);
        on += a;
        owned += b;
    }
    return {on, owned};
}

std::vector<std::string> OwnedCodes(const ListStage& l) {
    std::vector<std::string> codes;
    for (const ListVariant& v : l.variants) {
        if (!v.locked) codes.push_back(v.code);
    }
    return codes;
}

bool SetVariants(const std::vector<ListStage>& list, std::vector<std::string>* off, const std::vector<std::string>& codes, bool on) {
    std::vector<std::string> next = *off;
    for (const std::string& c : codes) {
        next.erase(std::remove(next.begin(), next.end(), c), next.end());
        if (!on) next.push_back(c);
    }
    if (StagesOn(list, next).first == 0) return false;
    *off = ParseStageMask(FormatStageMask(next));
    return true;
}

void ListNote(std::string text) {
    g_ui.listNote = std::move(text);
    g_ui.listNoteUntil = ImGui::GetTime() + 3.5;
}

void CheckBox(ImDrawList* d, float x, float y, int on, int of) {
    const ImVec2 a = g_v.P(x, y), b = g_v.P(x + 24, y + 24);
    if (on <= 0) {
        d->AddRectFilled(a, b, Rgba(0, 0, 0, 120));
        d->AddRect(a, b, Rgba(236, 228, 206, 150), 0, 0, std::max(1.0f, g_v.L(1.5f)));
        return;
    }
    d->AddRectFilled(a, b, look::kGold);
    const float t = std::max(1.5f, g_v.L(3));
    if (on < of) {
        d->AddLine(g_v.P(x + 6, y + 12), g_v.P(x + 18, y + 12), look::kInk, t);
        return;
    }
    const ImVec2 tick[3] = {g_v.P(x + 5.5f, y + 12.5f), g_v.P(x + 10, y + 17), g_v.P(x + 18.5f, y + 7)};
    d->AddPolyline(tick, 3, look::kInk, 0, t);
}

void StagePicture(ImDrawList* d, const std::string& code, float x0, float y0, float x1, float y1, bool dim) {
    const TextureView* t = TextureSized(StageTex(code, "StageSelectBG"), g_v.L(x1 - x0));
    if (!t) {
        d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(20, 22, 26));
        return;
    }
    const float aspect = t->height ? static_cast<float>(t->width) / t->height : 4.0f;
    const float visible = std::min(1.0f, ((x1 - x0) / (y1 - y0)) / aspect);
    Image(d, t, g_v.P(x0, y0), g_v.P(x1, y1), ImVec2(0.5f - visible / 2, 0), ImVec2(0.5f + visible / 2, 1), dim ? Rgba(95, 95, 95) : IM_COL32_WHITE);
}

int MoveInGrid(int at, int n, int cols) {
    const int row = at / cols, col = at % cols, rowLen = std::min(cols, n - row * cols);
    if (Left()) return row * cols + (col + rowLen - 1) % rowLen;
    if (Right()) return row * cols + (col + 1) % rowLen;
    if (Up()) {
        int to = at - cols;
        if (to < 0) {
            to = ((n - 1) / cols) * cols + col;
            if (to >= n) to -= cols;
        }
        return to;
    }
    if (Down()) return at + cols < n ? at + cols : col;
    return at;
}

void DrawVariantList(ImDrawList* d, const SelectFonts& f, const std::vector<ListStage>& list, std::vector<std::string>* off, bool* changed) {
    const ListStage& l = list[g_ui.variantsOf];
    const int n = static_cast<int>(l.variants.size());
    ImFont* bold = f.bold ? f.bold : f.text;
    ImFont* medium = f.title ? f.title : f.text;
    g_ui.variantCursor = std::clamp(g_ui.variantCursor, 0, n - 1);
    constexpr float kRowH = 72, kGap = 8, kPic = 144;
    const auto [on, owned] = VariantsOn(l, *off);
    ui::ModalSpec m;
    m.w = 660;
    m.h = 44 + 20 + n * kRowH + (n - 1) * kGap + 24 + 48;
    m.x0 = (1600 - m.w) / 2;
    m.y0 = (900 - m.h) / 2;
    m.kicker = Upper(l.name);
    m.aside = TRF(Str::select_settings_on, on, owned);
    const bool noted = !g_ui.listNote.empty() && ImGui::GetTime() < g_ui.listNoteUntil;
    m.hint = noted ? g_ui.listNote : std::string(TR(Str::select_settings_hint));
    m.hintCol = noted ? look::kGold : look::kSoft;
    m.prompts = {{"button_a", TR(Str::select_on_off)}, {"button_y", TR(on == owned ? Str::select_all_off : Str::select_all_on)}, {"button_b", TR(Str::common_prompt_back)}};
    const float top = ui::Modal(d, bold, f.text, m, nullptr) + 20;
    const float x0 = m.x0 + 28, x1 = m.x0 + m.w - 28;
    for (int k = 0; k < n; k++) {
        const ListVariant& v = l.variants[k];
        const float y = top + k * (kRowH + kGap);
        const bool vOn = !v.locked && !IsOff(*off, v.code);
        ui::Control(d, x0, y, x1, y + kRowH, false);
        StagePicture(d, v.code, x0 + 1, y + 1, x0 + 1 + kPic, y + kRowH - 1, !vOn);
        const float tx = x0 + kPic + 16, room = x1 - 52 - tx;
        Text(d, bold, g_v.L(17), g_v.P(tx, y + 11), vOn ? look::kGold : look::kMuted, TRF(Str::choices_setting_n, k + 1));
        Text(d, medium, g_v.L(18), g_v.P(tx, y + 37), vOn ? look::kCream : look::kMuted, Ellipsize(medium, 18, v.name, room));
        if (v.locked) {
            d->AddRectFilled(g_v.P(x0, y), g_v.P(x1, y + kRowH), Rgba(0, 0, 0, 110));
            Text(d, f.text, g_v.L(15), g_v.P(x1 - 18, y + 27), look::kMuted, TR(Str::select_not_owned_tile), 1.0f);
        } else CheckBox(d, x1 - 40, y + (kRowH - 24) / 2, vOn ? 1 : 0, 1);
        if (k == g_ui.variantCursor) ui::Ring(d, x0, y, x1, y + kRowH);
        if (Clicked() && MouseIn(g_v.P(x0, y), g_v.P(x1, y + kRowH))) {
            g_ui.variantCursor = k;
            ui::ClickPad(ImGuiKey_GamepadFaceDown);
        }
    }
    if (Up()) g_ui.variantCursor = (g_ui.variantCursor + n - 1) % n;
    if (Down()) g_ui.variantCursor = (g_ui.variantCursor + 1) % n;
    const ListVariant& cur = l.variants[g_ui.variantCursor];
    if (Confirm()) {
        if (cur.locked) ListNote(TRF(Str::select_not_owned_dot, cur.name.c_str()));
        else if (SetVariants(list, off, {cur.code}, IsOff(*off, cur.code))) *changed = true;
        else ListNote(TR(Str::select_keep_one));
    }
    if (NextSetting()) {
        if (SetVariants(list, off, OwnedCodes(l), on != owned)) *changed = true;
        else ListNote(TR(Str::select_keep_one));
    }
    if (Back()) g_ui.variantsOf = -1;
}

} // namespace

namespace selectui {

void DrawRandomList(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in) {
    const std::vector<ListStage> list = RandomList();
    const int n = static_cast<int>(list.size());
    if (n == 0) {
        g_ui.randomList = false;
        return;
    }
    std::vector<std::string> off = live.randomOff;
    bool changed = false;
    ImFont* bold = f.bold ? f.bold : f.text;
    ImFont* medium = f.title ? f.title : f.text;
    if (g_ui.variantsOf >= n) g_ui.variantsOf = -1;
    const bool sub = g_ui.variantsOf >= 0;
    const bool inputOn = g_inputOn;
    if (sub) g_inputOn = false;
    g_ui.listCursor = std::clamp(g_ui.listCursor, 0, n - 1);
    constexpr int kCols = 4;
    constexpr float kTileW = 350, kTileH = 58, kGapX = 12, kGapY = 8, kPic = 104;
    const int rows = (n + kCols - 1) / kCols;
    const auto [on, owned] = StagesOn(list, off);
    const ListStage& cur = list[g_ui.listCursor];
    ui::ModalSpec m;
    m.w = kCols * kTileW + (kCols - 1) * kGapX + 2 * 28;
    m.x0 = (1600 - m.w) / 2;
    const float gridY = 44 + 22;
    m.h = gridY + rows * kTileH + (rows - 1) * kGapY + 24 + 48;
    m.y0 = std::max(8.0f, (900 - m.h) / 2);
    m.kicker = TR(Str::select_random_stages);
    m.aside = TRF(Str::select_stages_on, on, owned);
    const bool noted = !g_ui.listNote.empty() && ImGui::GetTime() < g_ui.listNoteUntil && !sub;
    m.hint = noted ? g_ui.listNote : std::string(TR(Str::select_random_stages_hint));
    m.hintCol = noted ? look::kGold : look::kSoft;
    m.prompts = {{"button_a", TR(cur.variants.size() > 1 ? Str::select_settings : Str::select_on_off)}, {"button_y", TR(on == owned ? Str::select_all_off : Str::select_all_on)}, {"button_b", TR(Str::common_prompt_done)}};
    ui::Modal(d, bold, f.text, m, nullptr);
    const float gx = m.x0 + 28, gy = m.y0 + gridY;
    for (int i = 0; i < n; i++) {
        const ListStage& l = list[i];
        const float x = gx + (i % kCols) * (kTileW + kGapX), y = gy + (i / kCols) * (kTileH + kGapY);
        const auto [von, vowned] = VariantsOn(l, off);
        const bool lit = von > 0;
        ui::Control(d, x, y, x + kTileW, y + kTileH, false);
        StagePicture(d, l.variants[0].code, x + 1, y + 1, x + 1 + kPic, y + kTileH - 1, !lit);
        const float tx = x + kPic + 14, room = kTileW - kPic - 14 - 48;
        const bool second = l.variants.size() > 1 || l.locked;
        Text(d, medium, g_v.L(17), g_v.P(tx, y + (second ? 8 : 18)), lit ? look::kCream : look::kMuted, Ellipsize(medium, 17, l.name, room));
        if (l.locked) {
            d->AddRectFilled(g_v.P(x, y), g_v.P(x + kTileW, y + kTileH), Rgba(0, 0, 0, 110));
            Text(d, f.text, g_v.L(15), g_v.P(tx, y + 33), look::kMuted, TR(Str::select_not_owned_tile));
        } else {
            if (l.variants.size() > 1) {
                Text(d, f.text, g_v.L(15), g_v.P(tx, y + 33), lit ? look::kGold : look::kMuted,
                     TRF(Str::select_settings_count, von, vowned));
            }
            CheckBox(d, x + kTileW - 38, y + (kTileH - 24) / 2, von, vowned);
        }
        if (i == g_ui.listCursor) ui::Ring(d, x, y, x + kTileW, y + kTileH);
        if (Clicked() && MouseIn(g_v.P(x, y), g_v.P(x + kTileW, y + kTileH))) {
            g_ui.listCursor = i;
            ui::ClickPad(ImGuiKey_GamepadFaceDown);
        }
    }
    if (!sub) {
        g_ui.listCursor = MoveInGrid(g_ui.listCursor, n, kCols);
        const ListStage& at = list[g_ui.listCursor];
        if (Confirm()) {
            const auto [von, vowned] = VariantsOn(at, off);
            if (at.locked) ListNote(TRF(Str::select_not_owned_dot, at.name.c_str()));
            else if (at.variants.size() > 1) {
                g_ui.variantsOf = g_ui.listCursor;
                g_ui.variantCursor = 0;
            } else if (SetVariants(list, &off, OwnedCodes(at), von == 0)) changed = true;
            else ListNote(TR(Str::select_keep_one));
        }
        if (NextSetting()) {
            if (on != owned) {
                off.clear();
                changed = true;
            } else {
                int keep = g_ui.listCursor;
                for (int i = 0; list[keep].locked && i < n; i++) keep = i;
                std::vector<std::string> codes;
                for (int i = 0; i < n; i++) {
                    if (i == keep) continue;
                    for (const std::string& c : OwnedCodes(list[i])) codes.push_back(c);
                }
                if (SetVariants(list, &off, codes, false)) {
                    changed = true;
                    ListNote(TRF(Str::select_all_but, list[keep].name.c_str()));
                }
            }
        }
        if (Back()) g_ui.randomList = false;
    } else {
        g_inputOn = inputOn;
        DrawVariantList(d, f, list, &off, &changed);
    }
    g_inputOn = inputOn;
    if (changed) {
        in->randomOffChanged = true;
        in->randomOff = off;
    }
}

void DrawStageSelect(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in) {
    const int count = static_cast<int>(g_draw->stages.size());
    const bool picking = live.host && live.you >= 0;
    int stage = std::clamp(live.stage, 0, count);
    int setting = live.setting;
    auto lockedAt = [&](int i, int k) {
        const SelectStage* at = Shown(i, k);
        return at && at->locked;
    };
    auto step = [&](int from, int by) {
        int i = from;
        for (int tries = 0; tries <= count; tries++) {
            i = (i + by + count + 1) % (count + 1);
            if (!lockedAt(i, 0)) return i;
        }
        return from;
    };
    if (picking && Left()) in->stage = stage = step(stage, -1), setting = 0;
    if (picking && Right()) in->stage = stage = step(stage, 1), setting = 0;
    const int settings = stage < count ? static_cast<int>(g_draw->stages[stage].settings.size()) : 0;
    setting = std::clamp(setting, 0, settings);
    if (picking && settings) {
        const bool up = Up(), down = Down(), next = NextSetting();
        const int by = up ? -1 : (down || next) ? 1
                                                : 0;
        for (int tries = 0; by && tries <= settings; tries++) {
            setting = (setting + by + settings + 1) % (settings + 1);
            if (!lockedAt(stage, setting)) break;
        }
        if (by) in->setting = setting;
    }
    const SelectStage* shown = Shown(stage, setting);
    Text(d, f.name, g_v.L(42), g_v.P(800, 76), Rgba(255, 255, 255), TR(Str::select_stage_select), 0.5f, true);
    const float bx = 0, by = 132, bw = 1600, bh = 1600.0f * 534 / 1920;
    if (const TextureView* shadow = Texture(MenuTex("StagePreviewWindowShadow"))) Image(d, shadow, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const bool random = stage >= count;
    const std::string code = shown ? shown->code : "";
    if (const TextureView* bg = code.empty() ? nullptr : Texture(StageTex(code, "StageSelectBG"))) {
        const float my = by + bh * 11 / 534, mh = bh * 512 / 534;
        const float visible = (bw / mh) / (static_cast<float>(bg->width) / std::max(1u, bg->height));
        const float u0 = 0.5f - visible / 2, u1 = 0.5f + visible / 2;
        const int slices = 64;
        for (int i = 0; i < slices; i++) {
            auto edge = [&](float t) {
                const float c = 2 * t - 1;
                return (2 + 41 * (1 - c * c)) / 512.0f * mh;
            };
            const float t0 = static_cast<float>(i) / slices, t1 = static_cast<float>(i + 1) / slices;
            const float x0 = bx + bw * t0, x1 = bx + bw * t1;
            const float a0 = edge(t0), a1 = edge(t1);
            const ImVec2 q0 = g_v.P(x0, my + a0), q1 = g_v.P(x1, my + a1), q2 = g_v.P(x1, my + mh - a1), q3 = g_v.P(x0, my + mh - a0);
            const float ua = u0 + (u1 - u0) * t0, ub = u0 + (u1 - u0) * t1;
            d->AddImageQuad(Id(bg), q0, q1, q2, q3, ImVec2(ua, a0 / mh), ImVec2(ub, a1 / mh), ImVec2(ub, 1 - a1 / mh), ImVec2(ua, 1 - a0 / mh), FadeIn(bg));
        }
    }
    if (const TextureView* frame = Texture(MenuTex("StagePreviewWindow"))) Image(d, frame, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const std::string name = random ? std::string(TR(Str::select_random)) : shown ? shown->name
                                                                                  : std::string();
    Text(d, f.name, g_v.L(46), g_v.P(800, by + bh - 106), Rgba(255, 255, 255), name, 0.5f, true);
    if (random && live.myCharacter) {
        const auto [on, owned] = StagesOn(RandomList(), live.randomOff);
        const std::string line = on == owned ? TRF(Str::select_all_stages, owned) : TRF(Str::select_some_stages, on, owned);
        const float w = TextW(f.text, 16, line) + 36, y = by + bh - 146;
        Pill(d, g_v.P(800 - w / 2, y), g_v.P(800 + w / 2, y + 30));
        Text(d, f.text, g_v.L(16), g_v.P(800, y + 6), Rgba(233, 196, 106), line, 0.5f);
    }
    if (settings) {
        const std::string line = TRF(Str::select_setting_of, setting + 1, settings + 1);
        const float w = TextW(f.text, 16, line) + 36, y = by + bh - 146;
        Pill(d, g_v.P(800 - w / 2, y), g_v.P(800 + w / 2, y + 30));
        Text(d, f.text, g_v.L(16), g_v.P(800, y + 6), Rgba(233, 196, 106), line, 0.5f);
    }
    Infinity(d, g_v.P(800, by + bh + 30), g_v.L(56), Rgba(242, 201, 76));
    const float sw = 29, sh = 168, gap = 3;
    const float total = (count + 1) * sw + count * gap;
    const float sx = 800 - total / 2, sy = by + bh + 64;
    for (int i = 0; i <= count; i++) {
        const bool on = i == stage;
        const float x = sx + i * (sw + gap);
        const float lift = on ? 8.0f : 0.0f, grow = on ? 1.12f : 1.0f;
        const float cx = x + sw / 2, cy = sy + sh / 2 - lift;
        const ImVec2 a = g_v.P(cx - sw * grow / 2, cy - sh * grow / 2), b = g_v.P(cx + sw * grow / 2, cy + sh * grow / 2);
        const std::string icon = on && shown ? shown->code : i < count ? g_draw->stages[i].code
                                                                       : "";
        const TextureView* t = i < count ? Texture(StageTex(icon, "StageSelectIcon")) : Texture(MenuTex("StageSelect_Random"));
        if (t) Image(d, t, a, b);
        else d->AddRectFilled(a, b, Rgba(30, 32, 36));
        if (i < count && g_draw->stages[i].locked) d->AddRectFilled(a, b, Rgba(0, 0, 0, 170));
        if (on) {
            if (const TextureView* cursor = Texture(MenuTex("StageSelectCursor"))) Image(d, cursor, ImVec2(a.x - g_v.L(1), a.y - g_v.L(1)), ImVec2(b.x + g_v.L(1), b.y + g_v.L(1)));
        }
        if (picking && Clicked() && MouseIn(g_v.P(x, sy), g_v.P(x + sw, sy + sh)) && !lockedAt(i, 0)) in->stage = i;
    }
    Text(d, f.text, g_v.L(19), g_v.P(800, sy + sh + 16), Rgba(232, 228, 216),
         TR(picking ? Str::select_pick_stage : Str::select_p1_picking), 0.5f);
    if (picking && Confirm() && !lockedAt(stage, setting)) in->lockStage = true;
}

} // namespace selectui

} // namespace sfv::mod
