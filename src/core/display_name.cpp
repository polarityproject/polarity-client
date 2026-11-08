#include "core/display_name.h"

#include <cstdint>

namespace sfv::core {

namespace {

// The next code point of UTF-8 text from `i` (advanced past it); a malformed
// byte reads as U+FFFD, one byte.
uint32_t Next(const std::string& s, size_t& i)
{
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
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

void Append(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool Stripped(uint32_t cp)
{
    return (cp >= 0x2300 && cp <= 0x23FF) ||   // Miscellaneous Technical (watch, hourglass...)
           (cp >= 0x2600 && cp <= 0x27BF) ||   // Miscellaneous Symbols, Dingbats
           (cp >= 0x2B00 && cp <= 0x2BFF) ||   // Miscellaneous Symbols and Arrows (stars, squares)
           cp == 0x200D || cp == 0x20E3 ||     // zero-width joiner, keycap
           (cp >= 0xFE00 && cp <= 0xFE0F) ||   // variation selectors
           (cp >= 0x10000 && cp <= 0x1FFFF) || // plane 1: emoji, pictographs, historic scripts
           (cp >= 0xE0000 && cp <= 0xE007F);   // tags (flag sequences)
}

// A mathematical alphanumeric (U+1D400..U+1D7FF) as a plain letter or digit,
// or 0. The letters run in 52-letter styles (A-Z then a-z) from U+1D400 to
// U+1D6A3; the digits in 10-digit styles from U+1D7CE.
uint32_t PlainLetter(uint32_t cp)
{
    if (cp >= 0x1D400 && cp <= 0x1D6A3) {
        const uint32_t k = (cp - 0x1D400) % 52;
        return k < 26 ? 'A' + k : 'a' + (k - 26);
    }
    if (cp >= 0x1D7CE && cp <= 0x1D7FF) return '0' + (cp - 0x1D7CE) % 10;
    return 0;
}

} // namespace

std::string DisplayName(const std::string& name)
{
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
