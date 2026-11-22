#pragma once
// What the room screen's files share: the screen's state and its parts.

#include "mod/room_screen.h"

#include "core/keeper_protocol.h"
#include "core/rules.h"
#include "mod/rooms_screen.h"
#include "mod/ui_kit.h"

#include <imgui.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sfv::mod::roomui {

inline const ImU32 kGold = IM_COL32(233, 196, 106, 255);
inline const ImU32 kRing = IM_COL32(240, 194, 74, 255);
inline const ImU32 kCream = IM_COL32(244, 241, 232, 255);
inline const ImU32 kSoft = IM_COL32(228, 224, 212, 255);
inline const ImU32 kMuted = IM_COL32(198, 193, 180, 255);
inline const ImU32 kGreen = IM_COL32(127, 216, 139, 255);
inline const ImU32 kRed = IM_COL32(255, 107, 94, 255);

constexpr float kTablePitch = 190, kTableH = 182;
constexpr int kTablesInView = 4;
constexpr int kPlayersInView = 8;
constexpr double kCardMarginSeconds = 1.5;
constexpr int kKeeperCallSeconds = 21;
constexpr int kMaxCardDelay = 10;
constexpr int kCardVSkill = 0, kCardVTrigger = 1, kCardDelay = 2, kCardRows = 3;

enum class Modal { None,
                   Leaving,
                   Rules,
                   Remove };

struct State {
    int table = 0;
    float scroll = 0;
    Modal modal = Modal::None;
    bool typing = false;
    std::string text;
    float chatScroll = 0;
    uint32_t chatSeenFrom = 0;
    std::string chatSeenText;
    bool chatHeard = false;
    uint32_t chatHeardFrom = 0;
    std::string chatHeardText;
    int cardTick = -1;
    int choice = 0;
    int seatSide = 0;
    bool onPlayers = false;
    int player = 0;
    float playersFirst = 0;
    std::set<uint32_t> muted;
    uint32_t removing = 0;
    int cardTable = -1;
    uint32_t cardCall = 0;
    double cardSince = 0;
    int cardDelay = 2;
    bool cardReady = false;
    int cardRow = kCardVSkill;
    int cardVs = 1, cardVt = 1;
    int cardMouseRow = -1, cardMouseDir = 0;
    int oppVs = 0, oppVt = 0;
    double oppChangedAt = -10;
    int handoffTable = -1, handoffStep = -1;
    double handoffSince = 0;
    TableRulesEditor rules;
    std::vector<int> rulesPending;
};

inline State g;
inline TableHandoff g_handoff;
inline std::map<std::string, std::string> g_names;
inline std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> g_skillNames;
inline CardPick g_cardPick;

std::string SkillLabel(const std::string& code, bool trigger, int index);
int SkillCount(const std::string& code, bool trigger);
const RoomMember* Member(const RoomsView& v, uint32_t id);
std::string NameOf(const RoomsView& v, uint32_t id);
std::string Ordinal(int n);
std::vector<RoomMember> PlayerOrder(const RoomsView& v);

void Seat(ImDrawList* d, const HubFonts& f, float x, float y, const RoomMember* m, bool ready, bool sitHere, int seat, bool dim);
void Table(ImDrawList* d, const HubFonts& f, const RoomsView& v, float y, int index, bool on, bool dim);
void Players(ImDrawList* d, const HubFonts& f, const RoomsView& v);
void Chat(ImDrawList* d, const HubFonts& f, const RoomsView& v, bool* clickedBox, bool* clickedSend);
void Dialog(ImDrawList* d, const HubFonts& f, const std::string& kicker, const std::string& title, const std::string& text, bool danger,
            const char* yes, const char* no, const ui::PromptList& prompts, int* clicked, int* pressed);

std::string LeaveText(const RoomsView& v);
void Card(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t, const ui::PromptList& prompts, const std::string& hint, ImU32 hintCol, int* clicked);

struct HandoffStep {
    enum Progress { ToCome,
                    InProgress,
                    Done };

    std::string text;
    Progress state;
};

std::vector<HandoffStep> HandoffSteps(const HubFonts& f, const RoomsView& v, const keeper::TableState& s, const TableHandoff& h);
void HandoffModal(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t, const std::vector<HandoffStep>& steps, double stepSeconds,
                  const ui::PromptList& prompts, int* clicked);
void RulesModal(const HubFonts& f, const RoomsView& v, const ui::PromptList& prompts, int* clicked);
void Request(RoomScreenInput* in, const char* kind, std::vector<std::string> args = {});

} // namespace sfv::mod::roomui
