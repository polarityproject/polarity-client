// A spectator's stream: frames in hand, running dry and the slow motion that refills them (docs/netplay.md).

#include "core/spectate.h"

#include "core/recorder.h"
#include "core/state.h"

#include <algorithm>
#include <ctime>

namespace sfv {

namespace {

constexpr int kRunsLogged = 10, kNotesLogged = 40;
constexpr int kRestartPart = 4, kVeryLowPart = 8, kLowPart = 4, kFullSpeedPart = 2;
constexpr int kHoldEveryVeryLow = 3, kHoldEveryLow = 6;

} // namespace

SpectatorPlayback::SpectatorPlayback(IGame& game, SpectateConfig config) : game_(game), config_(std::move(config)) {}

void SpectatorPlayback::Log(const std::string& msg) const {
    if (config_.log) config_.log(msg);
}

bool SpectatorPlayback::Start(std::string* error) {
    if (!game_.BeginMatch(config_.setup)) {
        if (error) *error = "the game cannot start the match setup";
        return false;
    }
    const auto& regions = game_.Regions();
    if (regions.empty()) {
        if (error) *error = "the game reports no rollback state";
        return false;
    }
    recording_ = ReplayFile{};
    recording_.modVersion = config_.modVersion;
    recording_.gameBuild = game_.BuildId();
    recording_.matchId = config_.matchId;
    recording_.localPlayer = -1;
    recording_.createdUnix = static_cast<uint64_t>(std::time(nullptr));
    recording_.setup = config_.setup;
    recording_.regions = DescribeRegions(regions);
    recording_.initialHashes.resize(regions.size());
    HashRegions(regions, recording_.initialHashes.data());
    hashes_.assign(regions.size(), 0);
    Log("spectating match, delay " + std::to_string(config_.delayFrames) + " frames");
    return true;
}

void SpectatorPlayback::Add(int first, const std::vector<FrameInputs>& inputs, const std::vector<uint32_t>& checksums) {
    for (size_t i = 0; i < inputs.size(); i++) {
        const int frame = first + static_cast<int>(i);
        if (frame < received_) continue;
        pending_[frame] = {inputs[i], i < checksums.size() ? checksums[i] : 0};
    }
    const int before = received_;
    for (auto it = pending_.find(received_); it != pending_.end(); it = pending_.find(received_)) {
        inputs_.push_back(it->second.first);
        checksums_.push_back(it->second.second);
        pending_.erase(it);
        received_++;
    }
    if (received_ != before) {
        const auto now = Clock::now();
        if (started_ || stalls_) {
            const int gap = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastArrival_).count());
            longestGapMs_ = std::max(longestGapMs_, gap);
        }
        lastArrival_ = now;
    }
}

void SpectatorPlayback::Play() {
    const int frame = next_;
    game_.Advance(inputs_[frame], false);
    const auto& regions = game_.Regions();
    HashRegions(regions, hashes_.data());
    recording_.SetFrame(frame, inputs_[frame], hashes_.data());
    const uint32_t expected = checksums_[frame];
    bool differs = false;
    if (expected != 0 && !game_.OutcomeDecided()) {
        const uint32_t mine = FoldChecksum(hashes_.data(), hashes_.size());
        differs = mine != expected;
        if (differs && desyncCount_++ == 0) {
            desyncFrame_ = frame;
            recording_.events.push_back({frame, ReplayEventType::Desync, mine, expected});
            Log("spectator desync at frame " + std::to_string(frame));
        }
    }
    if (differs && runStart_ < 0) {
        runStart_ = frame;
    } else if (!differs && runStart_ >= 0) {
        if (runsLogged_++ < kRunsLogged) {
            Log("checksums differed in frames " + std::to_string(runStart_) + "-" + std::to_string(frame - 1));
        }
        runStart_ = -1;
    }
    next_++;
}

void SpectatorPlayback::Update() {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    const int delay = std::max(1, config_.delayFrames);
    const int buffered = received_ - next_;
    const auto now = Clock::now();
    const auto msSince = [&](Clock::time_point t) { return static_cast<long long>(duration_cast<milliseconds>(now - t).count()); };
    const bool note = notesLogged_ < kNotesLogged;
    if (!started_) {
        const int need = stalls_ == 0 ? delay : std::max(1, delay / kRestartPart);
        if (buffered < need) return;
        started_ = true;
        if (stalls_ > 0 && note) {
            notesLogged_++;
            Log("resumed at frame " + std::to_string(next_) + " after " + std::to_string(msSince(stallFrom_)) + " ms dry, " +
                std::to_string(buffered) + " frames in hand");
        }
    }
    if (buffered <= 0) {
        stalls_++;
        started_ = false;
        holdEvery_ = 0;
        stallFrom_ = now;
        if (note) {
            notesLogged_++;
            Log("ran dry at frame " + std::to_string(next_) + ": the last frames came " + std::to_string(msSince(lastArrival_)) +
                " ms ago; waiting for " + std::to_string(std::max(1, delay / kRestartPart)) + " in hand");
        }
        return;
    }
    const int was = holdEvery_;
    if (buffered < delay / kVeryLowPart) holdEvery_ = kHoldEveryVeryLow;
    else if (buffered < delay / kLowPart) holdEvery_ = holdEvery_ ? holdEvery_ : kHoldEveryLow;
    else if (buffered >= delay / kFullSpeedPart) holdEvery_ = 0;
    if (holdEvery_ && !was && note) {
        notesLogged_++;
        Log("low on frames at frame " + std::to_string(next_) + " (" + std::to_string(buffered) + " in hand, the last came " +
            std::to_string(msSince(lastArrival_)) + " ms ago): slowing a little");
    } else if (!holdEvery_ && was && note) {
        notesLogged_++;
        Log("full speed again at frame " + std::to_string(next_) + " (" + std::to_string(buffered) + " in hand, " + std::to_string(held_) +
            " frames held so far)");
    }
    updates_++;
    if (holdEvery_ && updates_ % holdEvery_ == 0) {
        held_++;
        return;
    }
    Play();
}

ReplayFile SpectatorPlayback::Recording() const {
    ReplayFile copy = recording_;
    copy.finalThrough = copy.FrameCount() - 1;
    return copy;
}

} // namespace sfv
