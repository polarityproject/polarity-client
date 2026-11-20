#pragma once
// Hashing and copy helpers over a game's state regions.

#include "core/game.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sfv {

// XXH64. Fast enough to hash several MB of battle state every frame.
uint64_t Hash64(const void* data, size_t len, uint64_t seed = 0);

// CRC-32 (IEEE), used for replay file integrity.
uint32_t Crc32(const void* data, size_t len, uint32_t crc = 0);

size_t StateSize(const std::vector<StateRegion>& regions);

// Concatenates every region into `dst` (StateSize bytes) and back.
void SaveRegions(const std::vector<StateRegion>& regions, uint8_t* dst);
void LoadRegions(const std::vector<StateRegion>& regions, const uint8_t* src);

// True when the 8-byte word holding byte `offset` is masked out.
inline bool IsIgnored(const std::vector<uint8_t>& ignore, size_t offset)
{
    const size_t word = offset / 8;
    return word / 8 < ignore.size() && (ignore[word / 8] >> (word % 8)) & 1;
}

// Values only meaningful inside this process (heap and image addresses)
// differ between machines even when the simulation agrees, so 8-byte words
// the test accepts hash as zero. The game adapter installs it; none by default.
using AddressTest = bool (*)(uint64_t value);
void SetAddressTest(AddressTest test);
bool IsProcessAddress(uint64_t value);

// One hash per region, over live memory or over a buffer laid out by SaveRegions.
// Words masked by the region's ignore bitmap, and addresses, hash as zero.
void HashRegions(const std::vector<StateRegion>& regions, uint64_t* out);
void HashSavedState(const std::vector<StateRegion>& regions, const uint8_t* state, uint64_t* out);

// The 32-bit checksum GekkoNet exchanges for desync detection, derived from the
// per-region hashes so the two always agree. Independent of region order.
uint32_t FoldChecksum(const uint64_t* regionHashes, size_t count);

// Taking another frozen state (the host's, or a recording's) at frame 0.
// The other side lists one hash per region; `other` holds the bytes of the
// regions it sent, by their index there.
struct OtherRegion {
    uint32_t index = 0;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
};

// An unmasked word that differed in a paired region. Pointer-like values
// (on either side) are never copied, so `copied` is false for them.
struct WordDifference {
    size_t region = 0; // this side's region
    size_t offset = 0;
    uint64_t here = 0, there = 0;
    bool copied = false;
};

struct TakenState {
    struct Region {
        size_t here = 0;    // this side's region
        uint32_t there = 0; // the other side's index
        size_t copied = 0;  // words copied into it
    };
    std::vector<Region> regions;               // paired regions, in the other side's order
    std::vector<OtherRegion> unpaired;         // sent regions with no same-size region left here
    std::vector<size_t> unmatched;             // regions here with no equal and no pair: they keep this side's bytes
    std::vector<WordDifference> differences;   // every differing unmasked word, copied or kept
    size_t wordsCopied = 0, pointersKept = 0, regionsChanged = 0;
};

// Copies `other` over the regions here that have no equal on the other side.
// Regions with an equal hash pair up first and are left alone; each remaining
// region the other side sent pairs with the most similar unpaired region of
// the same size here, and its differing words are copied, except masked words
// and pointer-like values. Bytes for regions that have an equal here are
// ignored. `hashes` are this side's, in region order.
TakenState TakeOtherState(const std::vector<StateRegion>& regions, const std::vector<uint64_t>& hashes,
                          const std::vector<uint64_t>& otherHashes, const std::vector<OtherRegion>& other);

// Log lines for a TakeOtherState result, `who` being "the host" or "the
// recording": each changed region, each unpaired region, and the differing
// words. The words show what differs at frame 0, for the work on making both
// sides start equal without a copy. A normal start has a few; the count is
// capped so a state that differs everywhere can't flood the log.
std::vector<std::string> DescribeTakenState(const std::vector<StateRegion>& regions, const TakenState& taken, const char* who);

} // namespace sfv
