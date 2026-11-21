#include "core/bad_pick.h"

#include <utility>

namespace sfv {

namespace {

constexpr uint16_t kNoStage = 0xFFF0;

void Spoil(BadPickField field, PlayerSetup& p) {
    switch (field) {
        case BadPickField::Character: p.character = PackCode("ZZZ"); break;
        case BadPickField::Costume: p.costume = 999; break;
        case BadPickField::Color: p.color = 99; break;
        case BadPickField::VSkill: p.vskill = 9; break;
        case BadPickField::VTrigger: p.vtrigger = 9; break;
        case BadPickField::Comment: p.comment = 99; break;
        case BadPickField::Variant: p.variation = 0x80; break;
        case BadPickField::Stage: break;
    }
}

void SpoilSetup(BadPickField field, MatchSetup& s) {
    if (field == BadPickField::Stage) s.stage = kNoStage;
    for (PlayerSetup& p : s.players) {
        if (p.character) Spoil(field, p);
    }
}

} // namespace

std::optional<BadPick> BadPickParse(const std::string& field, const std::string& message) {
    static const std::pair<const char*, BadPickField> kFields[] = {
        {"character", BadPickField::Character}, {"costume", BadPickField::Costume}, {"color", BadPickField::Color}, {"vskill", BadPickField::VSkill}, {"vtrigger", BadPickField::VTrigger}, {"comment", BadPickField::Comment}, {"variant", BadPickField::Variant}, {"stage", BadPickField::Stage}};
    static const std::pair<const char*, lobby::Type> kMessages[] = {
        {"state", lobby::Type::PlayerState}, {"session", lobby::Type::SessionState}, {"welcome", lobby::Type::Welcome}, {"loaded", lobby::Type::Loaded}, {"matches", lobby::Type::Matches}};
    BadPick bad;
    bool known = false;
    for (const auto& [name, value] : kFields) {
        if (field == name) bad.field = value, known = true;
    }
    if (!known) return std::nullopt;
    if (message.empty()) return bad;
    for (const auto& [name, value] : kMessages) {
        if (message == name) bad.only = value;
    }
    if (!bad.only) return std::nullopt;
    return bad;
}

void BadPickApply(const BadPick& bad, lobby::Message& m) {
    if (bad.only && m.type != *bad.only) return;
    switch (m.type) {
        case lobby::Type::PlayerState: Spoil(bad.field, m.seat.pick); break;
        case lobby::Type::SessionState:
            if (bad.field == BadPickField::Stage) m.stage = kNoStage;
            for (lobby::Seat& seat : m.seats) {
                if (seat.pick.character) Spoil(bad.field, seat.pick);
            }
            break;
        case lobby::Type::Welcome:
        case lobby::Type::Loaded: SpoilSetup(bad.field, m.setup); break;
        case lobby::Type::Matches:
            for (lobby::MatchEntry& e : m.matches) SpoilSetup(bad.field, e.setup);
            break;
        default: break;
    }
}

} // namespace sfv
