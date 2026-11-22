// polarity_strings: checks data/strings.tsv and writes the header TR() reads (docs/strings.md).

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Row {
    int line = 0;
    std::string key;
    std::vector<std::string> text;
};

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t tab = line.find('\t', from);
        out.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
        if (tab == std::string::npos) return out;
        from = tab + 1;
    }
}

bool Unescape(const std::string& in, std::string* out) {
    out->clear();
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '\\') {
            out->push_back(in[i]);
            continue;
        }
        if (++i == in.size()) return false;
        switch (in[i]) {
            case 'n': out->push_back('\n'); break;
            case 't': out->push_back('\t'); break;
            case '\\': out->push_back('\\'); break;
            default: return false;
        }
    }
    return true;
}

bool Conversions(const std::string& s, std::vector<std::string>* out) {
    out->clear();
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%') continue;
        if (i + 1 < s.size() && s[i + 1] == '%') {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < s.size() && std::string("-+#0").find(s[j]) != std::string::npos) j++;
        while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == '*')) j++;
        while (j < s.size() && std::string("hlLzjt").find(s[j]) != std::string::npos) j++;
        if (j == s.size() || std::string("diouxXeEfgGcsp").find(s[j]) == std::string::npos) return false;
        out->push_back(s.substr(i, j - i + 1));
        i = j;
    }
    return true;
}

bool ValidKey(const std::string& key) {
    if (key.empty() || !std::islower(static_cast<unsigned char>(key[0]))) return false;
    for (char c : key) {
        if (!std::islower(static_cast<unsigned char>(c)) && !std::isdigit(static_cast<unsigned char>(c)) && c != '_' && c != '.') return false;
    }
    return key.back() != '.' && key.find("..") == std::string::npos;
}

std::string Identifier(const std::string& key) {
    std::string out = key;
    for (char& c : out) {
        if (c == '.') c = '_';
    }
    return out;
}

std::string Literal(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c >= 0x7F) {
            char oct[8];
            std::snprintf(oct, sizeof(oct), "\\%03o", c);
            out += oct;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: polarity_strings <strings.tsv> <out.h>\n";
        return 2;
    }
    const std::string path = argv[1];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << path << ": cannot open\n";
        return 1;
    }
    int errors = 0;
    auto fail = [&](int line, const std::string& what) {
        std::cerr << path << "(" << line << "): " << what << "\n";
        errors++;
    };
    std::vector<std::string> languages;
    std::vector<Row> rows;
    std::set<std::string> keys, identifiers;
    std::string line;
    for (int number = 1; std::getline(in, line); number++) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (number == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> cells = SplitTabs(line);
        if (languages.empty()) {
            if (cells.size() < 2 || cells[0] != "key" || cells[1] != "en") {
                fail(number, "the first row must be the header: key, en, then the other languages");
                break;
            }
            languages.assign(cells.begin() + 1, cells.end());
            continue;
        }
        if (cells.size() > languages.size() + 1) fail(number, "more cells than languages");
        Row row;
        row.line = number;
        row.key = cells[0];
        if (!ValidKey(row.key)) fail(number, "key '" + row.key + "': lower case letters, digits, '_' and '.' only");
        if (!keys.insert(row.key).second) {
            fail(number, "key '" + row.key + "' twice");
        } else if (!identifiers.insert(Identifier(row.key)).second) {
            fail(number, "key '" + row.key + "' clashes with another once '.' is '_'");
        }
        for (size_t l = 0; l < languages.size(); l++) {
            std::string text;
            if (l + 1 < cells.size() && !Unescape(cells[l + 1], &text)) fail(number, languages[l] + ": a '\\' that is not \\n, \\t or \\\\");
            row.text.push_back(text);
        }
        if (row.text[0].empty()) fail(number, "key '" + row.key + "' has no English text");
        std::vector<std::string> english, other;
        if (!Conversions(row.text[0], &english)) fail(number, "en: a '%' that is not a printf conversion (write %% for a percent sign)");
        for (size_t l = 1; l < languages.size(); l++) {
            if (row.text[l].empty()) continue;
            if (!Conversions(row.text[l], &other) || other != english) {
                fail(number, languages[l] + ": printf conversions differ from the English ones, or are in another order");
            }
        }
        rows.push_back(std::move(row));
    }
    if (languages.empty()) fail(1, "no header row");
    if (errors) return 1;

    std::ostringstream h;
    h << "#pragma once\n// Generated from data/strings.tsv by polarity_strings (src/tools/strings_gen.cpp).\n\n";
    h << "#include <cstddef>\n\nnamespace sfv {\n\n";
    h << "enum class Str : unsigned int {\n";
    for (const Row& r : rows) h << "    " << Identifier(r.key) << ",\n";
    h << "};\n\n";
    h << "inline constexpr size_t kStringCount = " << rows.size() << ";\n";
    h << "inline constexpr size_t kLanguageCount = " << languages.size() << ";\n";
    h << "inline constexpr const char* kLanguageCodes[kLanguageCount] = {";
    for (size_t l = 0; l < languages.size(); l++) h << (l ? ", " : "") << Literal(languages[l]);
    h << "};\n\n";
    h << "inline constexpr const char* kStrings[kLanguageCount][kStringCount] = {\n";
    for (size_t l = 0; l < languages.size(); l++) {
        h << "    {\n";
        for (const Row& r : rows) h << "        " << Literal(r.text[l]) << ",\n";
        h << "    },\n";
    }
    h << "};\n\n} // namespace sfv\n";

    const std::string out = argv[2];
    std::string old;
    if (std::ifstream existing(out, std::ios::binary); existing) old.assign(std::istreambuf_iterator<char>(existing), {});
    if (old != h.str()) {
        std::ofstream o(out, std::ios::binary);
        o << h.str();
        if (!o) {
            std::cerr << out << ": cannot write\n";
            return 1;
        }
    }
    return 0;
}
