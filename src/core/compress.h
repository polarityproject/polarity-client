#pragma once
// Byte compression for .sfvr chunks: Windows' own XPRESS Huffman codec
// (compressapi.h, in every Windows since 8), so the format needs no library.
// Raw mode: the caller keeps the uncompressed size. Where cabinet.dll lacks
// the codec (Wine before 2026, so Proton) Compress fails and chunks stay raw;
// Decompress falls back to a decoder of its own.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfv {

// False when the codec is unavailable or fails; `out` is then untouched.
bool Compress(const uint8_t* data, size_t size, std::vector<uint8_t>* out);
// `rawSize` is the size before compression; false unless exactly that many
// bytes come back. Without the codec (Proton) it decodes on its own.
bool Decompress(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out);
// The decoder on its own, for any Windows (or Wine): XPRESS Huffman in raw
// mode, as [MS-XCA] 2.2.4 describes it.
bool DecompressXpressHuffman(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out);

} // namespace sfv
