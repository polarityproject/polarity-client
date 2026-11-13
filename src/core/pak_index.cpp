#include "core/pak_index.h"

#include "core/file_open.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sfv {

namespace {

constexpr uint32_t kPakMagic = 0x5A6F12E1;
// The footer: magic, version, index offset, index size, index hash. The
// game's own paks have an encrypted-index flag byte before it; paks made with
// older tools (u4pak, UnrealPak before 4.16) end their index right at the
// magic, and read with the flag taken as given every mod pak of a 0.4.2 host
// was "index out of the file" (2026-11-13).
constexpr size_t kFooterSize = 44;
constexpr size_t kFlagSize = 1;

struct Reader {
    const uint8_t* p;
    size_t size, at = 0;
    bool ok = true;
    template <class T> T Get()
    {
        T v{};
        if (at + sizeof(T) > size) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    void Skip(size_t n)
    {
        if (at + n > size) ok = false;
        else at += n;
    }
    // An FString: its length with the terminator; negative, UTF-16.
    std::string String()
    {
        const int32_t n = Get<int32_t>();
        if (!ok || n == 0) return {};
        std::string s;
        if (n > 0) {
            if (static_cast<size_t>(n) > size - at) {
                ok = false;
                return {};
            }
            s.assign(reinterpret_cast<const char*>(p + at), static_cast<size_t>(n));
            at += static_cast<size_t>(n);
        }
        else {
            const size_t chars = static_cast<size_t>(-static_cast<int64_t>(n));
            if (chars > (size - at) / 2) {
                ok = false;
                return {};
            }
            for (size_t i = 0; i < chars; i++) {
                const uint16_t c = static_cast<uint16_t>(p[at + 2 * i] | (p[at + 2 * i + 1] << 8));
                s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
            at += chars * 2;
        }
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
};

// "../../../StreetFighterV/Content/" + "Chara/..." -> "StreetFighterV/Content/Chara/...".
std::string Join(const std::string& mount, const std::string& name)
{
    std::string m = mount;
    while (m.rfind("../", 0) == 0) m.erase(0, 3);
    std::string out = m + name;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

std::vector<std::string> Split(const std::string& path)
{
    std::vector<std::string> parts;
    size_t from = 0;
    while (from <= path.size()) {
        const size_t slash = path.find('/', from);
        parts.push_back(path.substr(from, slash == std::string::npos ? std::string::npos : slash - from));
        if (slash == std::string::npos) break;
        from = slash + 1;
    }
    return parts;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// "BAC_KEN.uasset" -> "BAC_KEN" (also .uexp).
std::string Stem(const std::string& file)
{
    const size_t dot = file.find('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

} // namespace

bool ParsePakIndex(const uint8_t* bytes, size_t size, uint64_t fileSize, uint64_t at, PakIndex* out, std::string* error)
{
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (fileSize < kFooterSize || at + size != fileSize || size < kFooterSize) return fail("too short for a pak");
    const uint8_t* footer = bytes + size - kFooterSize;
    uint32_t magic = 0, version = 0;
    uint64_t indexAt = 0, indexSize = 0;
    std::memcpy(&magic, footer, 4);
    std::memcpy(&version, footer + 4, 4);
    std::memcpy(&indexAt, footer + 8, 8);
    std::memcpy(&indexSize, footer + 16, 8);
    if (magic != kPakMagic) return fail("not a pak (no pak footer)");
    if (version < 1 || version > 3) return fail("pak version not read here");
    const uint64_t footerAt = fileSize - kFooterSize;
    if (indexAt < at || indexSize > footerAt || indexAt > footerAt - indexSize) return fail("index out of the file");
    // A byte between the index and the footer is the encrypted-index flag.
    if (indexAt + indexSize + kFlagSize <= footerAt && footer[-1] != 0) return fail("encrypted index");
    Reader r{bytes + (indexAt - at), static_cast<size_t>(indexSize)};
    PakIndex index;
    index.version = version;
    index.mount = r.String();
    const int32_t count = r.Get<int32_t>();
    if (!r.ok || count < 0 || count > 2000000) return fail("bad index");
    index.entries.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count && r.ok; i++) {
        PakEntry e;
        e.path = Join(index.mount, r.String());
        r.Get<int64_t>(); // offset
        e.size = static_cast<uint64_t>(r.Get<int64_t>());
        e.uncompressedSize = static_cast<uint64_t>(r.Get<int64_t>());
        e.compression = r.Get<uint32_t>();
        if (version == 1) r.Get<int64_t>(); // timestamp
        r.Skip(20);                         // hash
        if (version >= 3) {
            if (e.compression != 0) {
                const int32_t blocks = r.Get<int32_t>();
                if (blocks < 0 || blocks > 1000000) return fail("bad index");
                r.Skip(static_cast<size_t>(blocks) * 16);
            }
            e.encrypted = r.Get<uint8_t>() != 0;
            r.Get<uint32_t>(); // block size
        }
        if (r.ok) index.entries.push_back(std::move(e));
    }
    if (!r.ok) return fail("index cut short");
    *out = std::move(index);
    return true;
}

bool ReadPakIndex(const std::string& path, PakIndex* out, std::string* error)
{
    FILE* f = OpenFile(path, "rb");
    if (!f) {
        if (error) *error = "cannot open";
        return false;
    }
    _fseeki64(f, 0, SEEK_END);
    const uint64_t fileSize = static_cast<uint64_t>(_ftelli64(f));
    if (fileSize < kFooterSize) {
        std::fclose(f);
        if (error) *error = "too short for a pak";
        return false;
    }
    // The footer first, then the file from its index on.
    uint8_t footer[kFooterSize];
    _fseeki64(f, static_cast<int64_t>(fileSize - kFooterSize), SEEK_SET);
    const bool got = std::fread(footer, 1, kFooterSize, f) == kFooterSize;
    uint64_t indexAt = 0;
    std::memcpy(&indexAt, footer + 8, 8);
    if (!got || indexAt >= fileSize || fileSize - indexAt > (256ull << 20)) {
        std::fclose(f);
        if (error) *error = got ? "index out of the file" : "cannot read";
        return false;
    }
    std::vector<uint8_t> tail(static_cast<size_t>(fileSize - indexAt));
    _fseeki64(f, static_cast<int64_t>(indexAt), SEEK_SET);
    const bool read = std::fread(tail.data(), 1, tail.size(), f) == tail.size();
    std::fclose(f);
    if (!read) {
        if (error) *error = "cannot read";
        return false;
    }
    return ParsePakIndex(tail.data(), tail.size(), fileSize, indexAt, out, error);
}

BattleFile ClassifyGamePath(const std::string& path)
{
    BattleFile b;
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::vector<std::string> parts = Split(p);
    // .../Content/Chara/<C>/...
    size_t chara = parts.size();
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        if (parts[i] == "Content" && parts[i + 1] == "Chara") {
            chara = i + 2;
            break;
        }
    }
    if (chara + 1 >= parts.size()) return b;
    const std::string& code = parts[chara];
    const std::string file = Stem(parts.back());
    // Chara/<C>/BattleResource/[<v>/]<file>
    if (parts[chara + 1] == "BattleResource" && chara + 2 < parts.size()) {
        const bool versioned = chara + 3 < parts.size();
        const std::string version = versioned ? parts[chara + 2] : "";
        if (StartsWith(file, "BAI_")) b.kind = BattleFileKind::CpuAi;
        else if (StartsWith(file, "BAC_") || StartsWith(file, "BCM_") || StartsWith(file, "BCH_") ||
                 (StartsWith(file, "DA_") && file.find("_BattleResource") != std::string::npos)) {
            b.kind = BattleFileKind::MoveData;
        }
        if (b.kind != BattleFileKind::Other) {
            b.character = code;
            b.version = version;
        }
        return b;
    }
    // Chara/<C>/DataAsset/StageData/DA_<C>_Stage_<S>
    if (parts[chara + 1] == "DataAsset" && chara + 3 < parts.size() && parts[chara + 2] == "StageData" && StartsWith(file, "DA_") &&
        file.find("_Stage_") != std::string::npos) {
        b.kind = BattleFileKind::StageData;
        b.character = code;
    }
    return b;
}

std::string BattleDataKey(const std::string& packagePath)
{
    // /Game/Chara/KEN/BattleResource/061/BAC_KEN -> KEN/061/BAC_KEN
    const std::vector<std::string> parts = Split(packagePath);
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        if (parts[i] != "Chara") continue;
        if (i + 4 < parts.size() && parts[i + 2] == "BattleResource") return parts[i + 1] + "/" + parts[i + 3] + "/" + parts[i + 4];
        if (i + 3 < parts.size() && parts[i + 2] == "BattleResource") return parts[i + 1] + "/-/" + parts[i + 3];
    }
    return {};
}

} // namespace sfv
