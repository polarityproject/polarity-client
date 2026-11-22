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
    Set(shared.view.keeper && !shared.view.lobby.empty() ? State::Browsing : State::Finding);
    nextDesksMs = 0;
}

void Client::Listings() {
    const int64_t now = NowMs();
    if (listed != Listed::None) {
        if (now - listedAt < kListAnswerMs) return;
        listed = Listed::None;
    }
    if (now - listedAt < kListGapMs) return;
    if (roomsWanted && wantDesk && shared.view.keeper) {
        roomsWanted = false;
        listed = Listed::Rooms;
        listedAt = now;
        svc->ListRooms({{keeper::kListingKind, keeper::ListingKindValue(lobby::kProtocol, MyVersion())}});
        return;
    }
    if (allListAt >= 0 && now >= allListAt) {
        allListAt = -1;
        if (shared.view.state == State::NoLobby) {
            listed = Listed::AllBuilds;
            listedAt = now;
            svc->ListRooms({{keeper::kKind, keeper::kKindLobby}});
            return;
        }
    }
    if (now < nextDesksMs) return;
    listed = Listed::Desks;
    listedAt = now;
    nextDesksMs = now + (shared.view.state == State::Room ? kDesksRoomMs : wantDesk ? kDesksScreenMs
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
    const bool atDeskOrRoom = shared.view.state == State::Lobby || shared.view.state == State::Room || shared.view.state == State::Creating ||
                              shared.view.state == State::Joining;
    if (!any) {
        if (!saidNoLobby) Log("rooms: no desk for build %u listed", lobby::kProtocol);
        saidNoLobby = true;
        shared.view.summary = {};
        if (!atDeskOrRoom) {
            shared.view.keeper = false;
            shared.view.publicRooms.clear();
            shared.view.lobby.clear();
            Set(State::NoLobby);
            allListAt = NowMs() + kListGapMs;
        }
        return;
    }
    if (saidNoLobby) Log("rooms: desk listed (revision %llu, %d public rooms)", static_cast<unsigned long long>(top.rev), top.publicRooms);
    saidNoLobby = false;
    shared.view.otherBuild.clear();
    shared.view.summary = top.For(MyVersion());
    if (!atDeskOrRoom) {
        shared.view.keeper = true;
        if (!deskJoining) shared.view.lobby = best;
        if (shared.view.state == State::Finding || shared.view.state == State::NoLobby) Set(State::Browsing);
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
    if (!shared.leftRoom.empty()) {
        const auto left = std::find_if(list.begin(), list.end(), [&](const keeper::ListedRoom& r) { return r.code == shared.leftRoom; });
        if (left == list.end() || NowMs() - shared.leftAtMs > kLeftHideMs) shared.leftRoom.clear();
        else if (left->members <= 1) list.erase(left);
    }
    keeper::SortRoomList(&list);
    shared.view.publicRooms = std::move(list);
    roomsRev = shared.view.summary.rev;
}

void Client::Peeks() {
    if (shared.view.state != State::Lobby || !shared.view.keeper || shared.peekWanted.empty()) {
        if (shared.peekWanted.empty()) shared.view.peek = keeper::RoomDetail{};
        return;
    }
    if (shared.peekWanted != shared.view.peek.code) shared.view.peek = keeper::RoomDetail{};
    const int64_t since = NowMs() - peekAskedMs;
    if (since < kPeekGapMs) return;
    if (shared.peekWanted == peekCode) {
        if (peekSeq && since < kPeekEveryMs * 2) return;
        if (since < kPeekEveryMs) return;
    }
    if (peekCode != shared.peekWanted) Log("rooms: peek %s", shared.peekWanted.c_str());
    peekCode = shared.peekWanted;
    peekSeq = ++seq;
    peekAskedMs = NowMs();
    peekParts.clear();
    svc->SetMemberData(keeper::kRequest, keeper::FormatRequest(keeper::PeekRequest(peekSeq, peekCode)));
}

} // namespace rooms

} // namespace sfv::mod
