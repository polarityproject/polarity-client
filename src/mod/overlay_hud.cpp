#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

void AddToast(const std::string& text, ImVec4 color, int seconds) {
    if (!g_inMatch) UiSoundPlay(color.x > 0.9f && color.y < 0.5f ? UiSound::NoticeError : UiSound::Notice);
    if (g_toasts.size() >= 5) g_toasts.erase(g_toasts.begin());
    g_toasts.push_back({text, color, GetTickCount64() + seconds * 1000ull, GetTickCount64()});
}

void ToastMigrated() {
    if (!g_migrated || !OnMenuScreen()) return;
    g_migrated = false;
    AddToast("SFVR is now Polarity. Your settings, replays and logs are in the Polarity folder.", kGold, 12);
}

void ToastReports() {
    static bool noticed = false;
    if (!noticed && OnMenuScreen()) {
        noticed = true;
        if (!g_panelPath.empty() && NetOptions().uploadReports &&
            !GetPrivateProfileIntW(L"report", L"notice_shown", 0, g_panelPath.c_str())) {
            WritePrivateProfileStringW(L"report", L"notice_shown", L"1", g_panelPath.c_str());
            AddToast("Polarity sends desync and crash reports to help fix bugs. You can turn this off in Polarity settings.", kGold, 15);
        }
    }
    static int toasted = 0;
    const int sent = ReportsSent();
    if (sent == toasted) return;
    toasted = sent;
    AddToast("Bug report sent", kDim, 6);
}

void ToastUpdate() {
    static std::string toasted;
    const UpdateView u = UpdateViewNow();
    if (u.state != UpdateView::State::Available || u.latest == toasted) return;
    if (HubOpen() && !RoomsScreenOpen()) return;
    toasted = u.latest;
    const bool inRoom = RoomsScreenOpen() && RoomsSnapshot().state == RoomsView::State::Room;
    Log("overlay: Polarity %s is out: a toast (%s)", u.latest.c_str(), inRoom ? "in a room" : "at the game's menus");
    AddToast("Polarity " + u.latest + " is out. " + (inRoom ? "Leave the room to update." : "Press Back + Start to update."), kGold, 10);
}

void ToastChanges(const NetView& now) {
    static NetView before;
    static bool have = false;
    if (!have) {
        before = now;
        have = true;
        return;
    }
    using P = NetView::Phase;
    if (now.phase == P::Hosting && before.phase != P::Hosting && !g_visible && !now.setPlay) {
        AddToast("Hosting. Code " + now.code + " copied", kGreen, 10);
    }
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat &a = before.seats[i], &b = now.seats[i];
        if (i == now.you) continue;
        if (now.host && b.present && !a.present && !b.name.empty() && !now.setPlay) AddToast(b.name + " joined");
        if (now.phase == P::PostMatch && before.phase == P::PostMatch && b.choice != a.choice &&
            b.choice != lobby::Choice::None) {
            AddToast(b.name + (b.choice == lobby::Choice::Rematch ? " wants a rematch" : " wants character select"));
        }
    }
    if (now.spectator && now.spectators != before.spectators && now.phase != P::Idle && now.phase != P::Ended) {
        AddToast(now.spectators > before.spectators ? "A spectator joined (" + std::to_string(now.spectators) + " watching)"
                                                    : std::to_string(now.spectators) + " watching",
                 kDim);
    }
    if (now.phase == P::Ended && before.phase != P::Ended && !now.result.empty() && !now.setPlay) AddToast("Session over: " + now.result, kRed, 6);
    before = now;
}

Notice DesyncNotice(const NetView& v) {
    return {"DESYNC AT FRAME " + std::to_string(v.desyncFrame), v.desyncReport.empty() ? "saving the recording..." : "recording saved",
            IM_COL32(255, 107, 94, 255), 1, 99};
}

void DrawNotices(bool fight, const NetView& v) {
    const ULONGLONG now = GetTickCount64();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [&](const Toast& t) { return t.until <= now; }),
                   g_toasts.end());
    std::vector<Notice> notices;
    for (const Toast& t : g_toasts) {
        ImVec4 c = t.color;
        c.w = 1;
        notices.push_back({"", t.text, ImGui::ColorConvertFloat4ToU32(c), (now - t.at) / 1000.0f, (t.until - now) / 1000.0f});
    }
    if (fight && v.desyncFrame >= 0) notices.push_back(DesyncNotice(v));
    else if (g_preview.desync) notices.push_back(DesyncNotice(PreviewView(NetView::Phase::Running)));
    if (notices.empty()) return;
    if (fight) DrawFightNotes(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, notices);
    else DrawToasts(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, notices, HubOpen());
}

void DrawPlayerBars(const NetView& v) {
    static ULONGLONG overAt = 0;
    float alpha = 1.0f;
    if (v.phase == NetView::Phase::PostMatch) {
        if (!overAt) overAt = GetTickCount64();
        alpha = 1.0f - static_cast<float>(GetTickCount64() - overAt) / kBarsFadeMs;
        if (alpha <= 0) return;
    } else {
        overAt = 0;
    }
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    auto color = [alpha](ImVec4 c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a * alpha)); };
    ImFont* big = g_fontHeading;
    ImFont* text = g_fontSmall;
    const ImVec4 detail = Rgb(214, 211, 200);
    const float bigSize = big->LegacySize, textSize = text->LegacySize;
    const float rule = std::max(1.0f, 1.5f * g_uiScale);
    const float pad = 10 * g_uiScale, gap = 8 * g_uiScale;
    const float top = 0, bottom = bigSize + 6 * g_uiScale + rule;
    const unsigned ping = v.avgPingMs > 0 ? static_cast<unsigned>(v.avgPingMs + 0.5f) : v.pingMs;
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat& s = v.seats[i];
        const bool left = i == 0;
        const float x0 = left ? screen.x * 0.085f : screen.x * 0.60f;
        const float x1 = left ? screen.x * 0.40f : screen.x * 0.915f;
        const ImU32 dark = color(Rgb(10, 10, 9), 0.82f), clear = color(Rgb(10, 10, 9), 0.0f);
        d->AddRectFilledMultiColor(ImVec2(x0, top), ImVec2(x1, bottom), left ? dark : clear, left ? clear : dark,
                                   left ? clear : dark, left ? dark : clear);
        const ImU32 gold = color(kGoldLine), goldClear = color(kGoldLine, 0.0f);
        d->AddRectFilledMultiColor(ImVec2(x0, bottom - rule), ImVec2(x1, bottom), left ? gold : goldClear, left ? goldClear : gold,
                                   left ? goldClear : gold, left ? gold : goldClear);
        enum class Kind { Text,
                          Link,
                          Relay };

        struct Part {
            std::string text;
            ImFont* font;
            ImU32 color;
            Kind kind = Kind::Text;
        };

        std::vector<Part> parts;
        std::string name = s.name.empty() ? std::string("Player") : s.name;
        const std::string you = v.you == i ? " (you)" : "";
        parts.push_back({name + you, big, color(Rgb(242, 240, 230))});
        parts.push_back({"(" + std::to_string(s.wins) + "/" + std::to_string(v.played) + ")", big, color(detail)});
        if (s.link != lobby::Link::Unknown) parts.push_back({"", text, color(detail), Kind::Link});
        const bool relayed = v.linkPath == NetView::Path::Relayed;
        if (v.spectator) {
            if (i == 1) {
                if (relayed) parts.push_back({"", text, color(PathColor(v)), Kind::Relay});
                parts.push_back({"watching, " + std::to_string(v.behind) + " in hand", text, color(detail)});
            }
        } else if (v.you != i) {
            char line[48];
            if (v.syncing) {
                parts.push_back({"connecting...", text, color(detail)});
            } else {
                if (relayed) parts.push_back({"", text, color(PathColor(v)), Kind::Relay});
                std::snprintf(line, sizeof(line), "%ums \xC2\xB1%.0f", ping, v.jitterMs);
                parts.push_back({line, text, color(PingColorVec(ping, static_cast<unsigned>(v.jitterMs + 0.5f)))});
            }
            if (s.fps) parts.push_back({std::to_string(SmoothedFps(i, s.fps)) + " fps", g_fontTiny, color(detail)});
            parts.push_back({std::to_string(v.recentRollback) + " RBF", g_fontTiny, color(detail)});
        }
        auto width = [textSize](const Part& part) {
            return part.kind != Kind::Text ? textSize : part.font->CalcTextSizeA(part.font->LegacySize, FLT_MAX, 0, part.text.c_str()).x;
        };
        auto total = [&] {
            float sum = -gap;
            for (const Part& part : parts) sum += width(part) + gap;
            return sum;
        };
        while (!name.empty() && total() > x1 - x0 - 2 * pad) {
            while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
            if (!name.empty()) name.pop_back();
            parts[0].text = name + "..." + you;
        }
        d->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
        const float mid = top + (bottom - rule - top) * 0.5f;
        float x = left ? x0 + pad : x1 - pad;
        for (const Part& part : parts) {
            const float size = part.font->LegacySize;
            const float w = width(part);
            const float at = left ? x : x - w;
            const ImVec2 iconAt(at, mid - textSize * 0.5f);
            if (part.kind == Kind::Link) DrawLinkIcon(d, iconAt, textSize, s.link, part.color);
            else if (part.kind == Kind::Relay) DrawRelayIcon(d, iconAt, textSize, part.color);
            else d->AddText(part.font, size, ImVec2(at, mid - size * 0.5f), part.color, part.text.c_str());
            x += left ? w + gap : -(w + gap);
        }
        d->PopClipRect();
    }
}

const char* VoteText(lobby::Choice c) {
    switch (c) {
        case lobby::Choice::Rematch: return "Rematch";
        case lobby::Choice::CharacterSelect: return "Character select";
        case lobby::Choice::None: break;
    }
    return "Choosing...";
}

void DrawSides(const NetView& v, const bool (&show)[kNumPlayers]) {
    static ULONGLONG shownAt = 0;
    static int shownFor = -1;
    const int key = v.played * 2 + (v.phase == NetView::Phase::Lobby ? 1 : 0);
    if (shownFor != key) {
        shownFor = key;
        shownAt = GetTickCount64();
        ResetVote();
    }
    const bool armed = GetTickCount64() - shownAt >= 750;
    const int setWinner = !v.setPlay ? -1 : v.seats[0].wins >= v.firstTo ? 0
                                        : v.seats[1].wins >= v.firstTo   ? 1
                                                                         : -1;
    const RoomsView rooms = v.setPlay && !g_previewing ? RoomsSnapshot() : RoomsView{};
    VoteSide sides[kNumPlayers];
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat& s = v.seats[i];
        VoteSide& o = sides[i];
        o.shown = show[i];
        o.mine = v.you == i;
        o.slot = i;
        o.name = s.name;
        for (const RoomMember& m : rooms.members) {
            if (m.name == s.name) o.country = m.country;
        }
        o.wins = s.wins;
        o.firstTo = v.setPlay ? v.firstTo : 0;
        const bool voting = setWinner < 0 || v.againVote;
        if (setWinner >= 0) {
            o.status = setWinner == i ? "Wins the set" : "Set over";
            o.statusKind = setWinner == i ? 1 : 2;
            const std::string next = v.againVote ? std::string() : RoomsAfterSet(i, setWinner);
            if (!next.empty()) o.lines.push_back(next);
        }
        if (voting) {
            if (setWinner < 0) {
                o.status = VoteText(s.choice);
                o.statusKind = s.choice == lobby::Choice::None ? 0 : 1;
            }
            if (v.againVote && o.mine) o.lines.push_back("No one's waiting, so you can keep playing.");
            o.choosing = o.mine;
            o.chosen = s.choice == lobby::Choice::Rematch ? 0 : s.choice == lobby::Choice::CharacterSelect ? 1
                                                                                                           : -1;
        }
        char line[160];
        if (o.mine) {
            o.delay = g_choices.delay;
            const int fixed = NetOptions().delaySetting, suggested = v.SuggestedDelay();
            if (v.spectator) {
            } else if (fixed >= 0) std::snprintf(line, sizeof(line), "Delay %d, from Polarity settings", fixed), o.suggestion = line;
            else if (suggested >= 0) std::snprintf(line, sizeof(line), "Suggested delay %d (ping %u ms)", suggested, v.link.pingMs), o.suggestion = line;
            if (voting && v.voteSecondsLeft >= 0 && s.choice == lobby::Choice::None) {
                const bool neither = v.seats[1 - i].choice == lobby::Choice::None;
                std::snprintf(line, sizeof(line),
                              !v.setPlay || !v.againVote ? "Rematch in %d s"
                              : neither                  ? "Back to the room in %d s"
                                                         : "Next set in %d s",
                              v.voteSecondsLeft);
                o.hint = line;
            } else o.hint = !v.setPlay ? "Leave ends the session." : v.againVote ? "Leave returns to the room."
                                                                                 : "Leave ends the set here.";
        } else {
            o.delay = s.inputDelay;
            o.hint = "Input delay " + std::to_string(s.inputDelay) + (s.inputDelay == 1 ? " frame" : " frames");
        }
    }
    VoteInput in;
    DrawVote(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, sides, armed, !g_previewing, &in);
    if (in.choose == 0) Act([] { NetChoose(lobby::Choice::Rematch); });
    else if (in.choose == 1) Act([] { NetChoose(lobby::Choice::CharacterSelect); });
    else if (in.choose == 2) Act([] { NetLeave(); });
    if (in.delayStep) g_choices.delay = std::clamp(g_choices.delay + in.delayStep, 0, kMaxDelayFrames);
    if (v.you >= 0 && v.you < kNumPlayers && sides[v.you].choosing) SyncChoices(v);
}

NetView PreviewView(NetView::Phase phase) {
    const Preview& p = g_preview;
    NetView v;
    v.phase = phase;
    v.you = p.you;
    v.host = p.you == 0;
    v.code = "K7Q2MX";
    v.played = 3;
    v.spectators = 1;
    v.linkPingMs = 38;
    v.link = {38, 4, core::kLinkSamples};
    v.stageRule = lobby::kRandomStage;
    v.overResults = true;
    v.setPlay = p.set;
    v.firstTo = 3;
    const NetplayOptions mine = ChosenOptions();
    for (int i = 0; i < kNumPlayers; i++) {
        lobby::Seat& s = v.seats[i];
        s.present = true;
        s.name = i == p.you && !mine.name.empty() ? mine.name : i == 0 ? "Player One"
                                                                       : "Player Two";
        s.hasPick = true;
        if (i == p.you && mine.hasPick) {
            s.pick = mine.pick;
        } else if (g_tables.charas.size() > 1) {
            const size_t other = g_choices.chara == 1 ? 0 : 1;
            const auto& costumes = g_tables.costumes[other];
            s.pick.character = PackCode(g_tables.charas[other].c_str());
            s.pick.costume = static_cast<uint16_t>(costumes.size() > 1 ? costumes[1] : costumes[0]);
            s.pick.color = 2;
        }
        s.ready = p.ready[i];
        s.preloaded = true;
        s.choice = p.choice[i] == 1 ? lobby::Choice::Rematch : p.choice[i] == 2 ? lobby::Choice::CharacterSelect
                                                                                : lobby::Choice::None;
        s.wins = static_cast<uint16_t>(i == 0 ? 2 : 1);
        s.inputDelay = static_cast<uint8_t>(i == p.you ? g_choices.delay : 1);
        s.fps = 60;
        s.link = i == 0 ? lobby::Link::Wired : lobby::Link::WiFi;
    }
    v.frame = 4210;
    v.pingMs = 41;
    v.avgPingMs = 40.6f;
    v.jitterMs = 3;
    v.framesAhead = 0.4f;
    v.rollbacks = 57;
    v.longestRollback = 4;
    v.inputDelay = g_choices.delay;
    v.predictionWindow = 8;
    v.linkPath = p.relayed ? NetView::Path::Relayed : NetView::Path::Direct;
    v.relayHost = "turn.cloudflare.com";
    if (p.desync) {
        v.desyncFrame = 3812;
        v.desyncReport = "desync-preview.txt";
    }
    return v;
}

} // namespace sfv::mod::overlay
