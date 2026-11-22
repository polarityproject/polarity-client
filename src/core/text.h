#pragma once
// Player-facing text from the strings table (data/strings.tsv, docs/strings.md).
// TR(Str::key) gives the key's text in the current language, or the English
// when that language has none. Strings with printf conversions keep the
// English ones in the same order in every language (polarity_strings checks).

#include "strings_table.h"

#include <cstdarg>
#include <cstddef>
#include <string>

namespace sfv {

const char* TR(Str key);

// TR(key) as a printf format with the arguments after it.
std::string TRF(Str key, ...);

// The key's text in a given language column (English where that column is
// empty), and that text as a printf format with the arguments in args. Log
// lines that say what a screen says use column 0.
const char* TextIn(size_t language, Str key);
std::string TextFormatV(size_t language, Str key, va_list args);
std::string TextFormat(size_t language, Str key, ...);

// The language by its column in the table (0: English); out of range: English.
void TextSetLanguage(size_t language);
size_t TextLanguage();

} // namespace sfv
