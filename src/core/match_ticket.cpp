#include "core/match_ticket.h"

namespace sfv {

MatchTicket DirectMatchHost()
{
    MatchTicket t;
    t.host = true;
    return t;
}

MatchTicket DirectMatchJoin(const std::string& code, bool spectate)
{
    MatchTicket t;
    t.roomCode = code;
    t.spectate = spectate;
    return t;
}

void ApplyRulesToSetup(const Rules& rules, MatchSetup* setup)
{
    setup->roundsToWin = static_cast<uint8_t>((rules.bestOf + 1) / 2);
    setup->timerSeconds = rules.timer;
}

int SetWinner(const Rules& rules, const uint16_t (&wins)[kNumPlayers])
{
    for (int p = 0; p < kNumPlayers; p++) {
        if (wins[p] >= rules.firstTo) return p;
    }
    return -1;
}

std::string DescribeTicket(const MatchTicket& t)
{
    std::string out = t.host ? "host" : t.spectate ? "watch " + t.roomCode : "join " + t.roomCode;
    out += t.seatToken ? ", seat token" : ", first joiner plays";
    out += ", " + FormatRules(t.rules);
    out += t.after == AfterMatch::Set ? ", a set" : ", vote after each match";
    return out;
}

} // namespace sfv
