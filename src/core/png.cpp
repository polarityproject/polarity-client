// A minimal PNG writer: RGB rows in stored zlib blocks.

#include "core/png.h"

#include "core/state.h"

#include <algorithm>
#include <cstring>

namespace sfv {

namespace {

constexpr uint8_t kBitDepth = 8, kColorRgb = 2;
constexpr size_t kRgbBytes = 3;
constexpr size_t kMaxStoredBlock = 65535;
constexpr uint8_t kZlibHeader[] = {0x78, 0x01};

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void Chunk(std::vector<uint8_t>& out, const char (&type)[5], const std::vector<uint8_t>& data) {
    Put32(out, static_cast<uint32_t>(data.size()));
    const size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Put32(out, Crc32(out.data() + at, out.size() - at));
}

} // namespace

std::vector<uint8_t> EncodePng(int width, int height, const uint8_t* rgb) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    Put32(ihdr, static_cast<uint32_t>(width));
    Put32(ihdr, static_cast<uint32_t>(height));
    ihdr.insert(ihdr.end(), {kBitDepth, kColorRgb, 0, 0, 0});
    Chunk(out, "IHDR", ihdr);

    const size_t stride = static_cast<size_t>(width) * kRgbBytes;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * height);
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + y * stride, rgb + (y + 1) * stride);
    }
    std::vector<uint8_t> z(std::begin(kZlibHeader), std::end(kZlibHeader));
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(kMaxStoredBlock, raw.size() - at);
        const bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
    } while (at < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    Put32(z, (b << 16) | a);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    return out;
}

} // namespace sfv
