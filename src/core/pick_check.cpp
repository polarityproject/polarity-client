// The check on another PC's picks: characters, costumes, colors, V-Skills and stages.

#include "core/pick_check.h"

#include <algorithm>

namespace sfv {

namespace {

std::string Printable(const std::string& code) {
    std::string out = code;
    for (char& c : out) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '?';
    }
    return out.empty() ? "(none)" : out;
}

PickVerdict WithPrefix(const std::string& prefix, PickVerdict v) {
    if (v.kind == PickVerdict::Kind::Bad) v.why = prefix + v.why;
    return v;
}

} // namespace

PickVerdict PickCheck(const PlayerSetup& pick, const PickTables& tables) {
    const std::string code = UnpackCode(pick.character);
    const int index = tables.charaIndex(code);
    if (index < 0 || !tables.isFighter(index)) return PickVerdict::Bad("no fighter " + Printable(code));
    const std::string costume = code + " costume " + std::to_string(pick.costume);
    if (pick.costume >= tables.costumeCount(index)) return PickVerdict::Bad("no " + costume);
    const bool eleven = tables.isEleven(pick);
    if (!eleven && tables.colorCount(index, pick.costume) <= 0) return PickVerdict::Bad(costume + " is not offered");
    const std::vector<int> colors = eleven ? std::vector<int>{0, 1} : tables.colorIds(index, pick.costume);
    if (std::find(colors.begin(), colors.end(), pick.color) == colors.end()) return PickVerdict::Bad(costume + " has no color id " + std::to_string(pick.color));
    const auto skills = [&](bool vtrigger) {
        const size_t n = tables.skillCount(code, vtrigger);
        return n ? n : 2;
    };
    if (pick.vskill >= skills(false)) return PickVerdict::Bad(code + " has no V-Skill " + std::to_string(pick.vskill + 1));
    if (pick.vtrigger >= skills(true)) return PickVerdict::Bad(code + " has no V-Trigger " + std::to_string(pick.vtrigger + 1));
    bool later = false;
    if (pick.variation) {
        const std::optional<uint8_t> variants = tables.variants(index, pick.costume);
        if (!variants) later = true;
        else if ((pick.variation & *variants) != pick.variation) return PickVerdict::Bad(costume + " has no variant " + std::to_string(pick.variation));
    }
    if (pick.comment) {
        const std::optional<size_t> comments = tables.commentCount(code);
        if (!comments) later = true;
        else if (pick.comment > *comments) return PickVerdict::Bad(code + " has no V-Comment " + std::to_string(pick.comment));
    }
    if (later) return {PickVerdict::Kind::NotYet, {}};
    return {};
}

PickVerdict SetupCheck(const MatchSetup& setup, const PickTables& tables) {
    PickVerdict later;
    for (int p = 0; p < kNumPlayers; p++) {
        const PickVerdict v = WithPrefix("P" + std::to_string(p + 1) + ": ", PickCheck(setup.players[p], tables));
        if (v.kind == PickVerdict::Kind::Bad) return v;
        if (v.kind == PickVerdict::Kind::NotYet) later = v;
    }
    if (setup.stage == lobby::kRandomStage) return PickVerdict::Bad("no stage " + std::to_string(setup.stage));
    const PickVerdict stage = StageRuleCheck(setup.stage, tables);
    return stage.Fine() ? later : stage;
}

PickVerdict StageRuleCheck(uint16_t stage, const PickTables& tables) {
    if (stage == lobby::kAnyStage || stage == lobby::kRandomStage || tables.isPlayableStage(stage)) return {};
    return PickVerdict::Bad("no stage " + std::to_string(stage));
}

PickVerdict MessageCheck(const lobby::Message& m, const PickTables& tables) {
    const auto seat = [&](const lobby::Seat& s) { return s.hasPick || s.pick.character ? PickCheck(s.pick, tables) : PickVerdict{}; };
    PickVerdict later;
    const auto bad = [&later](const PickVerdict& v) {
        if (v.kind == PickVerdict::Kind::NotYet) later = v;
        return v.kind == PickVerdict::Kind::Bad;
    };
    switch (m.type) {
        case lobby::Type::PlayerState: return seat(m.seat);
        case lobby::Type::SessionState:
            for (int i = 0; i < kNumPlayers; i++) {
                const PickVerdict v = WithPrefix("P" + std::to_string(i + 1) + ": ", seat(m.seats[i]));
                if (bad(v)) return v;
            }
            if (const PickVerdict v = StageRuleCheck(m.stage, tables); bad(v)) return v;
            return later;
        case lobby::Type::Welcome:
        case lobby::Type::Loaded: return SetupCheck(m.setup, tables);
        case lobby::Type::Matches:
            for (const lobby::MatchEntry& e : m.matches) {
                if (const PickVerdict v = SetupCheck(e.setup, tables); bad(v)) return v;
            }
            return later;
        default: return {};
    }
}

} // namespace sfv
