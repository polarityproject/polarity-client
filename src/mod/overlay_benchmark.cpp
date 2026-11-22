#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

void OpenBenchmarkModal(bool result) {
    g_benchChoice = 0;
    if (result) {
        g_benchShown = BenchmarkLast();
        g_benchModal = g_benchShown.valid ? BenchModal::Result : BenchModal::None;
    } else {
        g_benchModal = BenchModal::Confirm;
    }
}

void StartBenchmarkRun() {
    g_benchModal = BenchModal::Loading;
    g_benchLaunch = {};
    g_benchLaunch.active = true;
    g_benchLaunch.since = g_benchLaunch.stepSince = ImGui::GetTime();
}

void StepBenchmarkLaunch() {
    BenchLaunch& l = g_benchLaunch;
    const double now = ImGui::GetTime();
    if (l.step == 0) {
        if (l.drawn < 1) return;
        if (!BenchmarkStart(BuildVersion())) {
            g_benchModal = BenchModal::None;
            l = {};
            AddToast("The benchmark can't start now.", kRed, 4);
            return;
        }
        l.switchesAt = FlowBattleSceneSwitches();
        l.step = 1;
        l.stepSince = now;
    }
    if (l.step == 1 && FlowBattleSceneSwitches() != l.switchesAt) {
        l.step = 2;
        l.stepSince = l.blackFrom = now;
    }
    if (l.step == 1 && now - l.since > kBenchGiveUp) {
        Log("benchmark: no battle scene %.0f s after Run: stopping it", kBenchGiveUp);
        ReplayApplyControl(ReplayControl::Leave);
        g_benchModal = BenchModal::None;
        l = {};
        AddToast("The benchmark didn't start. Try again.", kRed, 5);
        return;
    }
    if (l.step == 2 && g_benchModal == BenchModal::Loading && now - l.blackFrom >= kBenchToBlack) {
        g_benchModal = BenchModal::None;
        g_benchReturn = true;
        SettingsScreenHide();
        HubHide();
    }
    const ReplayView v = ReplayViewNow();
    const bool running = v.active && v.benchmark && v.frame > 0;
    if (l.step == 2 && g_benchModal == BenchModal::None && (running || now - l.stepSince > kBenchGiveUp)) {
        if (!running) Log("benchmark: the round has not started %.0f s after its battle scene: showing it", kBenchGiveUp);
        l.step = 3;
        l.revealFrom = now;
    }
    if (l.step == 3 && now - l.revealFrom >= kBenchReveal) l = {};
    if (l.step == 2 && g_benchModal == BenchModal::None && !v.active) l = {};
}

void DrawBenchmarkLaunch(const HubFonts& f) {
    using namespace ui;
    BenchLaunch& l = g_benchLaunch;
    const double now = ImGui::GetTime();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    double black = l.blackFrom < 0 ? 0 : std::min(1.0, (now - l.blackFrom) / kBenchToBlack);
    float alpha = 1;
    if (l.step == 3) {
        const double out = std::min(1.0, (now - l.revealFrom) / kBenchReveal);
        black *= 1 - out;
        alpha = static_cast<float>(std::max(0.0, 1 - out * 2));
    }
    if (black > 0) d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(black * 255)));
    if (alpha > 0) {
        const int firstVertex = d->VtxBuffer.Size;
        g_inputOn = false;
        ModalSpec m;
        m.x0 = 480, m.y0 = 270, m.w = 640, m.h = 290;
        m.kicker = "BENCHMARK";
        m.hint = "It starts by itself.";
        int clicked = -1;
        float y = Modal(d, bold, f.body, m, &clicked);
        Text(d, f.display, g_v.L(28), g_v.P(m.x0 + 28, y), look::kCream, "Getting the round ready");
        y += 62;
        const char* const kSteps[] = {"Reading the round", "Loading the characters and the stage", "Starting the round"};
        for (int i = 0; i < 3; i++) {
            const int state = i < l.step ? 2 : i == l.step ? 1
                                                           : 0;
            const ImVec2 c = g_v.P(m.x0 + 40, y + 11);
            const ImU32 ring = state == 2 ? look::kOk : state == 1 ? look::kRing
                                                                   : look::kEdgeControl;
            d->AddCircle(c, g_v.L(12), ring, 24, g_v.L(2));
            if (state == 2) {
                const ImVec2 tick[] = {g_v.P(m.x0 + 34, y + 11), g_v.P(m.x0 + 38.5f, y + 16), g_v.P(m.x0 + 46.5f, y + 6)};
                d->AddPolyline(tick, 3, look::kOk, 0, g_v.L(2.2f));
            } else if (state == 1) {
                const float turn = static_cast<float>(now * 4.0);
                d->AddCircleFilled(g_v.P(m.x0 + 40 + 7 * std::cos(turn), y + 11 + 7 * std::sin(turn)), g_v.L(2.6f), look::kRing);
            }
            std::string text = kSteps[i];
            if (state == 1 && now - l.stepSince >= 3) text += " \xC2\xB7 " + std::to_string(static_cast<int>(now - l.stepSince)) + " s";
            Text(d, f.body, g_v.L(17), g_v.P(m.x0 + 66, y), state == 0 ? look::kMuted : look::kCream, text);
            y += 36;
        }
        if (alpha < 1) {
            for (int i = firstVertex; i < d->VtxBuffer.Size; i++) {
                ImU32& col = d->VtxBuffer[i].col;
                const ImU32 a = static_cast<ImU32>(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
                col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
            }
        }
    }
    l.drawn++;
    StepBenchmarkLaunch();
}

BenchDataLine BenchmarkDataLine(const BenchmarkResult& b) {
    auto names = [](const std::string& codes) {
        std::vector<std::string> out;
        size_t from = 0;
        while (from < codes.size()) {
            const size_t comma = codes.find(',', from);
            const std::string code = codes.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
            from = comma == std::string::npos ? codes.size() : comma + 1;
            if (code.empty()) continue;
            const char* name = keeper::CharacterName(code);
            out.push_back(code == "CMN" ? "every character's shared data" : code == "?" ? "a mod that can't be read"
                                                                        : *name         ? name
                                                                                        : code);
        }
        std::string s;
        for (size_t i = 0; i < out.size(); i++) s += (i == 0 ? "" : i + 1 == out.size() ? " and "
                                                                                        : ", ") +
                                                     out[i];
        return s;
    };
    BenchDataLine l;
    if (b.DataChanged()) {
        l.kind = 2;
        l.head = "This round's battle data isn't the game's own: " + names(b.data.substr(8)) + ".";
        l.body = "A mod changes it. Matches with these characters fall out of sync against players without that mod.";
        return l;
    }
    if (b.ModsChangeBattle()) {
        const size_t colon = b.mods.find(':');
        l.kind = 1;
        l.head = "Mods change the battle data of " + (colon == std::string::npos ? std::string("some characters") : names(b.mods.substr(colon + 1))) + ".";
        l.body = "Matches with them fall out of sync against players who don't have the same mods.";
        return l;
    }
    if (b.mods.empty()) return l;
    l.kind = 0;
    if (b.mods == "none") l.head = "No mods are installed.";
    else {
        const int count = std::atoi(b.mods.c_str());
        l.head = std::to_string(count) + (count == 1 ? " mod is installed. It doesn't" : " mods are installed. None of them") + " change battle data.";
    }
    return l;
}

void DrawBenchmarkModal(const HubFonts& f, bool takeInput) {
    using namespace ui;
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    g_inputOn = takeInput;
    UiSoundUseSet(UiSoundSet::Menu);
    int clicked = -1;
    if (g_benchModal == BenchModal::Confirm) {
        ModalSpec m;
        m.x0 = 480, m.y0 = 250, m.w = 640, m.h = 330;
        m.kicker = "BENCHMARK";
        m.prompts = {{"button_a", "Choose"}, {"button_b", "Back"}};
        float y = Modal(d, bold, f.body, m, &clicked);
        Text(d, f.display, g_v.L(28), g_v.P(m.x0 + 28, y), look::kCream, "Run the benchmark?");
        y += 50;
        ImGui::PushFont(f.body);
        const float wrap = g_v.L(m.w - 56);
        d->AddText(f.body, TextPx(19), g_v.P(m.x0 + 28, y), look::kCream,
                   "You watch one round played with a rollback on every frame, the most a match can have. It takes about a minute.",
                   nullptr, wrap);
        d->AddText(f.body, TextPx(19), g_v.P(m.x0 + 28, y + 62), look::kSoft, "Close other programs for a fair result.", nullptr, wrap);
        ImGui::PopFont();
        const float by = m.y0 + m.h - 46 - 22 - 48;
        const bool run = Button(d, bold, m.x0 + 28, by, m.x0 + m.w / 2 - 6, by + 48, "Run", g_benchChoice == 0);
        const bool later = Button(d, bold, m.x0 + m.w / 2 + 6, by, m.x0 + m.w - 28, by + 48, "Not now", g_benchChoice == 1);
        if (takeInput) {
            if (Left() || Right()) g_benchChoice = 1 - g_benchChoice;
            if ((Confirm() && g_benchChoice == 0) || run) StartBenchmarkRun();
            else if ((Confirm() && g_benchChoice == 1) || later || Back()) g_benchModal = BenchModal::None;
        }
    } else if (g_benchModal == BenchModal::Result) {
        const BenchmarkResult& b = g_benchShown;
        const bool pass = b.Pass();
        const BenchDataLine data = BenchmarkDataLine(b);
        ModalSpec m;
        m.x0 = 350, m.y0 = 120, m.w = 900, m.h = (b.matched ? (pass ? 470.0f : 510.0f) : 510.0f) + (data.kind < 0 ? 0.0f : data.body.empty() ? 44.0f
                                                                                                                                             : 74.0f);
        const std::time_t t = static_cast<std::time_t>(b.when);
        std::tm tm{};
        localtime_s(&tm, &t);
        static const char* const kMon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        char kick[64];
        std::snprintf(kick, sizeof(kick), "BENCHMARK \xC2\xB7 %s %d, %02d:%02d", kMon[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min);
        m.kicker = kick;
        m.aside = "Polarity " + b.version;
        m.danger = !b.matched;
        m.prompts = {{"button_a", "Choose"}, {"button_b", "Back"}};
        float y = Modal(d, bold, f.body, m, &clicked);
        const ImU32 verdict = pass ? look::kOk : !b.matched ? look::kDanger
                                                            : look::kGold;
        Text(d, f.display, g_v.L(64), g_v.P(m.x0 + 28, y - 4), verdict, pass ? "PASS" : "FAIL");
        const float vx = m.x0 + 28 + TextW(f.display, 64, pass ? "PASS" : "FAIL") + 28;
        char avg[48];
        std::snprintf(avg, sizeof(avg), "%.1f fps average", b.averageFps);
        Text(d, f.display, g_v.L(28), g_v.P(vx, y + 6), look::kCream, avg);
        Text(d, f.body, g_v.L(19), g_v.P(vx, y + 44), look::kSoft, "A pass needs 59 or more.");
        y += 92;
        char a[32], s2[32], c[48];
        std::snprintf(a, sizeof(a), "%.1f fps", b.lowFps);
        std::snprintf(s2, sizeof(s2), "%.1f ms", b.slowestMs);
        std::snprintf(c, sizeof(c), "%d of %d", b.slowFrames, b.frames);
        const std::pair<const char*, std::string> figures[] = {{"1% low", a}, {"Slowest frame", s2}, {"Frames over 20 ms", c}};
        const float fw = (m.w - 56 - 24) / 3;
        for (int i = 0; i < 3; i++) {
            const float x0 = m.x0 + 28 + i * (fw + 12);
            Control(d, x0, y, x0 + fw, y + 80, false);
            Text(d, bold, g_v.L(17), g_v.P(x0 + 18, y + 12), look::kMuted, figures[i].first);
            Text(d, f.display, g_v.L(26), g_v.P(x0 + 18, y + 38), look::kCream, figures[i].second);
        }
        y += 98;
        char times[128];
        std::snprintf(times, sizeof(times), "Each frame: rollbacks %.1f ms \xC2\xB7 presenting %.1f ms \xC2\xB7 %.1f ms in all", b.rollbackMs + b.gameMs,
                      b.drawMs, b.seconds * 1000.0 / std::max(1, b.frames));
        Text(d, bold, g_v.L(19), g_v.P(m.x0 + 28, y), look::kSoft, times);
        y += 40;
        d->AddLine(g_v.P(m.x0 + 28, y), g_v.P(m.x0 + m.w - 28, y), Rgba(236, 228, 206, 36), std::max(1.0f, g_v.L(1)));
        y += 14;
        if (b.matched) {
            d->AddLine(g_v.P(m.x0 + 30, y + 13), g_v.P(m.x0 + 36, y + 19), look::kOk, g_v.L(2.4f));
            d->AddLine(g_v.P(m.x0 + 36, y + 19), g_v.P(m.x0 + 46, y + 7), look::kOk, g_v.L(2.4f));
            Text(d, bold, g_v.L(19), g_v.P(m.x0 + 56, y), look::kCream, "The round played exactly as recorded.");
            if (!pass) {
                const bool rollbacks = b.rollbackMs + b.gameMs >= b.drawMs;
                Text(d, f.body, g_v.L(19), g_v.P(m.x0 + 56, y + 30), look::kSoft,
                     rollbacks ? "Rollbacks took the most time. With 1 or 2 frames of input delay, a match rolls back less."
                               : "Drawing the game took the most time. Lower graphics settings may help.");
            }
        } else {
            d->AddLine(g_v.P(m.x0 + 30, y + 7), g_v.P(m.x0 + 44, y + 21), look::kDanger, g_v.L(2.4f));
            d->AddLine(g_v.P(m.x0 + 44, y + 7), g_v.P(m.x0 + 30, y + 21), look::kDanger, g_v.L(2.4f));
            Text(d, bold, g_v.L(19), g_v.P(m.x0 + 56, y), look::kCream,
                 "The round played differently from frame " + std::to_string(b.firstDiffer) + ".");
            Text(d, f.body, g_v.L(19), g_v.P(m.x0 + 56, y + 30), look::kSoft,
                 data.kind == 2 ? "This PC would fall out of sync online."
                                : "This PC would fall out of sync online. Pack logs in Polarity settings and send them in.");
        }
        if (data.kind >= 0) {
            float dy = y + (b.matched && pass ? 44 : 74);
            const ImU32 mark = data.kind == 0 ? look::kOk : data.kind == 1 ? look::kGold
                                                                           : look::kDanger;
            if (data.kind == 0) {
                d->AddLine(g_v.P(m.x0 + 30, dy + 13), g_v.P(m.x0 + 36, dy + 19), mark, g_v.L(2.4f));
                d->AddLine(g_v.P(m.x0 + 36, dy + 19), g_v.P(m.x0 + 46, dy + 7), mark, g_v.L(2.4f));
            } else {
                d->AddTriangle(g_v.P(m.x0 + 38, dy + 4), g_v.P(m.x0 + 47, dy + 21), g_v.P(m.x0 + 29, dy + 21), mark, g_v.L(2.0f));
                d->AddLine(g_v.P(m.x0 + 38, dy + 10), g_v.P(m.x0 + 38, dy + 15), mark, g_v.L(2.0f));
            }
            Text(d, bold, g_v.L(19), g_v.P(m.x0 + 56, dy), look::kCream, data.head);
            if (!data.body.empty()) Text(d, f.body, g_v.L(19), g_v.P(m.x0 + 56, dy + 30), look::kSoft, data.body);
        }
        const float by = m.y0 + m.h - 46 - 22 - 48;
        const bool done = Button(d, bold, m.x0 + 28, by, m.x0 + m.w / 2 - 6, by + 48, "Done", g_benchChoice == 0);
        const bool again = Button(d, bold, m.x0 + m.w / 2 + 6, by, m.x0 + m.w - 28, by + 48, "Run again", g_benchChoice == 1);
        if (takeInput) {
            if (Left() || Right()) g_benchChoice = 1 - g_benchChoice;
            if ((Confirm() && g_benchChoice == 1) || again) StartBenchmarkRun();
            else if ((Confirm() && g_benchChoice == 0) || done || Back()) g_benchModal = BenchModal::None;
        }
    }
    if (clicked >= 0) ClickPad(PadKeyOfGlyph(clicked == 0 ? "button_a" : "button_b"));
    g_inputOn = false;
}

} // namespace sfv::mod::overlay
