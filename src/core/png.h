#pragma once
// A PNG of an RGB image, for screenshots the mod takes itself (the `shot`
// command: the test agents on Windows and SteamOS, where no outside tool can
// capture the game in Steam's Game Mode). The pixel data goes in stored
// (uncompressed) deflate blocks: larger files, no compressor needed.

#include <cstdint>
#include <vector>

namespace sfv {

// rgb: width * height * 3 bytes, rows top to bottom.
std::vector<uint8_t> EncodePng(int width, int height, const uint8_t* rgb);

} // namespace sfv
