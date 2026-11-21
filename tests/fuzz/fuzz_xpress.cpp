// libFuzzer target: the XPRESS Huffman decoder on any bytes (scripts/fuzz.ps1).
// The first 4 bytes are the size claimed before compression, as a recording's
// PACK chunk gives it, kept under 4 MB here so each run stays quick (the cap
// itself is a unit test).
#include "core/compress.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4) return 0;
    uint32_t rawSize = 0;
    std::memcpy(&rawSize, data, 4);
    rawSize %= 4u << 20;
    std::vector<uint8_t> out;
    if (sfv::DecompressXpressHuffman(data + 4, size - 4, rawSize, &out) && out.size() != rawSize) __debugbreak();
    return 0;
}
