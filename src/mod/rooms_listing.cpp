// The desk and room listings, and peeks at a room's detail.

#include "mod/rooms_internal.h"

#include "core/display_name.h"

#include <algorithm>
#include <cstdlib>

namespace sfv::mod {

using namespace rooms;

namespace rooms {

void Client::ToBrowsing() {
    deskJoining = false;
    Set(view.keeper && !view.lobby.empty() ? State::Browsing : State::Finding);
    nextDesksMs = 0;
}

void Client::Listings() {
    const int64_t now = NowMs();
    if (listed != Listed::None) {
        if (now - listedAt < kListAnswerMs) return;
        listed = Listed::None;
    }
    if (now - listedAt < kListGapMs) return;
    if (roomsWanted && wantDesk && view.keeper) {
        roomsWanted = false;
        listed = Listed::Rooms;
        listedAt = now;
        svc->ListRooms({{keeper::kListingKind, keeper::ListingKindValue(lobby::kProtocol, MyVersion())}});
        return;
    }
    if (allListAt >= 0 && now >= allListAt) {
        allListAt = -1;
        if (view.state == State::NoLobby) {
            listed = Listed::AllBuilds;
            listedAt = now;
            svc->ListRooms({{keeper::kKind, keeper::kKindLobby}});
            return;
        }
    }
    if (now < nextDesksMs) return;
    listed = Listed::Desks;
    listedAt = now;
    nextDesksMs = now + (view.state == State::Room ? kDesksRoomMs : wantDesk ? kDesksScreenMs
                                                                             : kDesksHubMs);
    svc->ListRooms({{keeper::kKind, keeper::kKindLobby}, {keeper::kBuild, std::to_string(lobby::kProtocol)}});
}

void Client::OnDesks() {
    std::string best;
    int bestCount = -1;
    bool any = false;
    keeper::DeskSummary top;
    for (const WeyveListedRoom& r : svc->RoomList()) {
        if (!Signed(keeper::DeskMessage(r.code, r.listing), r.listing)) continue;
        const auto summary = r.listing.find(keeper::kDeskSummary);
        keeper::DeskSummary s;
        if (summary == r.listing.end() || !keeper::ParseDeskSummary(summary->second, &s)) continue;
        if (!any || s.rev > top.rev) top = s;
        any = true;
        const auto count = r.listing.find(keeper::kCount);
        const int n = count != r.listing.end() ? std::atoi(count->second.c_str()) : 0;
        if (!r.joinable || n >= keeper::kLobbyCap) continue;
        if (const auto stale = staleLobbies.find(r.code); stale != staleLobbies.end() && NowMs() < stale->second) continue;
        if (n > bestCount) {
            best = r.code;
            bestCount = n;
        }
    }
    const bool atDeskOrRoom = view.state == State::Lobby || view.state == State::Room || view.state == State::Creating ||
                              view.state == State::Joining;
    if (!any) {
        if (!saidNoLobby) Log("rooms: no desk for build %u listed", lobby::kProtocol);
        saidNoLobby = true;
        view.summary = {};
        if (!atDeskOrRoom) {
            view.keeper = false;
            view.publicRooms.clear();
            view.lobby.clear();
            Set(State::NoLobby);
            allListAt = NowMs() + kListGapMs;
        }
        return;
    }
    if (saidNoLobby) Log("rooms: desk listed (revision %llu, %d public rooms)", static_cast<unsigned long long>(top.rev), top.publicRooms);
    saidNoLobby = false;
    view.otherBuild.clear();
    view.summary = top.For(MyVersion());
    if (!atDeskOrRoom) {
        view.keeper = true;
        if (!deskJoining) view.lobby = best;
        if (view.state == State::Finding || view.state == State::NoLobby) Set(State::Browsing);
    }
    if (top.rev != roomsRev) roomsWanted = true;
}

void Client::OnRooms() {
    std::vector<keeper::ListedRoom> list;
    for (const WeyveListedRoom& r : svc->RoomList()) {
        keeper::ListedRoom l;
        if (!keeper::ParseRoomListing(r.code, r.listing, lobby::kProtocol, &l)) continue;
        if (l.version != MyVersion()) continue;
        if (!Signed(keeper::ListingMessage(r.code, r.listing), r.listing)) continue;
        l.owner = core::DisplayName(l.owner);
        for (auto& p : l.players) p.name = core::DisplayName(p.name);
        list.push_back(std::move(l));
    }
    if (!leftRoom.empty()) {
        const auto left = std::find_if(list.begin(), list.end(), [&](const keeper::ListedRoom& r) { return r.code == leftRoom; });
        if (left == list.end() || NowMs() - leftAtMs > kLeftHideMs) leftRoom.clear();
        else if (left->members <= 1) list.erase(left);
    }
    keeper::SortRoomList(&list);
    view.publicRooms = std::move(list);
    roomsRev = view.summary.rev;
}

void Client::Peeks() {
    if (view.state != State::Lobby || !view.keeper || peekWanted.empty()) {
        if (peekWanted.empty()) view.peek = keeper::RoomDetail{};
        return;
    }
    if (peekWanted != view.peek.code) view.peek = keeper::RoomDetail{};
    const int64_t since = NowMs() - peekAskedMs;
    if (since < kPeekGapMs) return;
    if (peekWanted == peekCode) {
        if (peekSeq && since < kPeekEveryMs * 2) return;
        if (since < kPeekEveryMs) return;
    }
    if (peekCode != peekWanted) Log("rooms: peek %s", peekWanted.c_str());
    peekCode = peekWanted;
    peekSeq = ++seq;
    peekAskedMs = NowMs();
    peekParts.clear();
    svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(keeper::PeekRequest(peekSeq, peekCode)));
}

} // namespace rooms

} // namespace sfv::mod
