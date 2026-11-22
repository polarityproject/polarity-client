// Player-facing text: the strings table's current language (docs/strings.md).

#include "core/text.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace sfv {

namespace {

std::atomic<size_t> g_language{0};

} // namespace

const char* TR(Str key) {
    const size_t k = static_cast<size_t>(key);
    if (k >= kStringCount) return "";
    const char* text = kStrings[g_language.load(std::memory_order_relaxed)][k];
    return *text ? text : kStrings[0][k];
}

std::string TRF(Str key, ...) {
    const char* format = TR(key);
    va_list args;
    va_start(args, key);
    va_list again;
    va_copy(again, args);
    const int n = std::vsnprintf(nullptr, 0, format, args);
    va_end(args);
    std::string out;
    if (n > 0) {
        std::vector<char> buf(static_cast<size_t>(n) + 1);
        std::vsnprintf(buf.data(), buf.size(), format, again);
        out.assign(buf.data(), static_cast<size_t>(n));
    }
    va_end(again);
    return out;
}

void TextSetLanguage(size_t language) { g_language = language < kLanguageCount ? language : 0; }

size_t TextLanguage() { return g_language; }

} // namespace sfv
