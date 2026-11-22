#pragma once
// Player-facing text from the strings table (data/strings.tsv, docs/strings.md).
// TR(Str::key) gives the key's text in the current language, or the English
// when that language has none. Strings with printf conversions keep the
// English ones in the same order in every language (polarity_strings checks).

#include "strings_table.h"

#include <cstddef>
#include <string>

namespace sfv {

const char* TR(Str key);

// TR(key) as a printf format with the arguments after it.
std::string TRF(Str key, ...);

// The language by its column in the table (0: English); out of range: English.
void TextSetLanguage(size_t language);
size_t TextLanguage();

} // namespace sfv
