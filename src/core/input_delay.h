#pragma once
// The input delay suggested for a link: how it is measured and worked out.
// docs/input-delay.md explains the numbers and where they came from.
//
//   late  = ceil((ping / 2 + min(jitter, 17)) * 60 / 1000)   frames
//   delay = clamp(late - 2, 0, 3)
//
// "late" is how many frames after its own the other player's input arrives;
// delay hides that many frames, rollback covers the rest. Aiming at two
// frames of rollback keeps the delay at 0 for nearby players, as the game's
// own netcode had it (no delay, rollback only), and the cap keeps it at 3 for
// far ones.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfv::core {

// Round trips of the session's heartbeat (every 250 ms) kept for the
// suggestion: the last 10 seconds.
constexpr size_t kLinkSamples = 40;
// Samples needed before a suggestion is made (2 seconds of the lobby).
constexpr size_t kSuggestSamples = 8;
// One step between two samples counts at most this much toward the jitter,
// so a lone spike cannot stand for the whole link.
constexpr unsigned kJitterStepCapMs = 50;
// Jitter counted at most: one frame. Above it the link has spikes, which a
// rollback absorbs better than frames of delay for the whole match.
constexpr unsigned kJitterCapMs = 17;
// Frames of rollback the suggestion leaves to prediction at a steady ping.
constexpr int kRollbackFramesAimed = 2;
// Jitter assumed where only a ping is known (a room member's): the testers'
// median (2026-11-04 reports).
constexpr unsigned kTypicalJitterMs = 8;
constexpr int kMaxSuggestedDelay = 3;

struct LinkStats {
    unsigned pingMs = 0;   // median round trip
    unsigned jitterMs = 0; // mean change between consecutive round trips
    size_t samples = 0;
};

// The median round trip and the jitter (the mean absolute change between
// consecutive samples, GekkoNet's measure) of round trips in arrival order.
inline LinkStats MeasureLink(const std::vector<uint16_t>& rtts)
{
    LinkStats s;
    s.samples = rtts.size();
    if (rtts.empty()) return s;
    std::vector<uint16_t> sorted = rtts;
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    s.pingMs = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2] + 1u) / 2;
    if (n >= 2) {
        unsigned sum = 0;
        for (size_t i = 1; i < n; i++) {
            const int step = static_cast<int>(rtts[i]) - static_cast<int>(rtts[i - 1]);
            sum += std::min(static_cast<unsigned>(step < 0 ? -step : step), kJitterStepCapMs);
        }
        s.jitterMs = (sum + static_cast<unsigned>(n - 1) / 2) / static_cast<unsigned>(n - 1);
    }
    return s;
}

// Frames after its own that the other player's input arrives: the one-way
// trip plus the jitter counted, in 60 fps frames, rounded up.
inline int FramesLate(unsigned pingMs, unsigned jitterMs)
{
    const unsigned jitter = std::min(jitterMs, kJitterCapMs);
    // (ping / 2 + jitter) * 60 / 1000 = 3 * (ping + 2 * jitter) / 100
    return static_cast<int>((3u * (pingMs + 2u * jitter) + 99u) / 100u);
}

inline int SuggestedDelay(unsigned pingMs, unsigned jitterMs)
{
    return std::clamp(FramesLate(pingMs, jitterMs) - kRollbackFramesAimed, 0, kMaxSuggestedDelay);
}

// A link's tier, for the color its ping shows in (user, 2026-11-08): the
// delay suggested for it (0 to 3), or 4 when even 3 frames leave more than
// kRollbackFramesAimed frames to rollback (late above 5 frames).
constexpr int kPingTierHeavy = 4;
inline int PingTier(unsigned pingMs, unsigned jitterMs)
{
    const int late = FramesLate(pingMs, jitterMs);
    const int delay = std::clamp(late - kRollbackFramesAimed, 0, kMaxSuggestedDelay);
    return late - delay > kRollbackFramesAimed ? kPingTierHeavy : delay;
}

} // namespace sfv::core
