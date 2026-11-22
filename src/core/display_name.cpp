// Display names: emoji and pictographs out, mathematical letters as plain ones (docs/rooms-client.md).

#include "core/display_name.h"

#include <cstdint>
#include <algorithm>
#include <iterator>

namespace sfv::core {

namespace {

uint32_t Next(const std::string& s, size_t& i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1
                           : (c & 0xF0) == 0xE0   ? 2
                           : (c & 0xF8) == 0xF0   ? 3
                                                  : -1;
    if (extra < 0 || i + extra >= s.size() + (extra ? 0 : 1)) {
        i++;
        return 0xFFFD;
    }
    uint32_t cp = extra ? c & (0x3F >> extra) : c;
    for (int k = 1; k <= extra; k++) {
        if ((b(i + k) & 0xC0) != 0x80) {
            i++;
            return 0xFFFD;
        }
        cp = cp << 6 | (b(i + k) & 0x3F);
    }
    i += extra + 1;
    return cp;
}

void Append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct CodeRange {
    uint32_t first, last;
};

constexpr CodeRange kStrippedRanges[] = {
    {0x2300, 0x23FF},
    {0x2600, 0x27BF},
    {0x2B00, 0x2BFF},
    {0x200D, 0x200D},
    {0x20E3, 0x20E3},
    {0xFE00, 0xFE0F},
    {0x10000, 0x1FFFF},
    {0xE0000, 0xE007F},
};

constexpr uint32_t kMathLetters = 0x1D400, kMathLettersEnd = 0x1D6A3;
constexpr uint32_t kMathDigits = 0x1D7CE, kMathDigitsEnd = 0x1D7FF;
constexpr uint32_t kLettersPerStyle = 52, kDigitsPerStyle = 10, kAlphabet = 26;

bool Stripped(uint32_t cp) {
    return std::any_of(std::begin(kStrippedRanges), std::end(kStrippedRanges),
                       [cp](const CodeRange& r) { return cp >= r.first && cp <= r.last; });
}

uint32_t PlainLetter(uint32_t cp) {
    if (cp >= kMathLetters && cp <= kMathLettersEnd) {
        const uint32_t k = (cp - kMathLetters) % kLettersPerStyle;
        return k < kAlphabet ? 'A' + k : 'a' + (k - kAlphabet);
    }
    if (cp >= kMathDigits && cp <= kMathDigitsEnd) return '0' + (cp - kMathDigits) % kDigitsPerStyle;
    return 0;
}

} // namespace

std::string DisplayName(const std::string& name) {
    std::string out;
    bool space = false;
    for (size_t i = 0; i < name.size();) {
        uint32_t cp = Next(name, i);
        if (const uint32_t plain = PlainLetter(cp)) cp = plain;
        else if (Stripped(cp)) continue;
        if (cp == ' ' || cp == '\t') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        Append(out, cp);
    }
    if (out.empty() && !name.empty()) return "Player";
    return out;
}

} // namespace sfv::core
