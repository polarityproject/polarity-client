// A pak's index: its footer, its entries, and which of them are battle or stage data (docs/research.md).

#include "core/pak_index.h"

#include "core/file_open.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sfv {

namespace {

constexpr uint32_t kPakMagic = 0x5A6F12E1;
constexpr size_t kFooterSize = 44;
constexpr size_t kFlagSize = 1;
constexpr size_t kFooterVersion = 4, kFooterIndexAt = 8, kFooterIndexSize = 16;
constexpr uint32_t kMinVersion = 1, kMaxVersion = 3, kVersionWithTimestamp = 1, kVersionWithBlocks = 3;
constexpr int32_t kMaxEntries = 2000000, kMaxBlocks = 1000000;
constexpr size_t kHashBytes = 20, kBlockBytes = 16;
constexpr uint64_t kMaxIndexBytes = 256ull << 20;

struct Reader {
    const uint8_t* p;
    size_t size, at = 0;
    bool ok = true;

    template <class T>
    T Get() {
        T v{};
        if (at + sizeof(T) > size) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p + at, sizeof(T));
        at += sizeof(T);
        return v;
    }

    void Skip(size_t n) {
        if (at + n > size) ok = false;
        else at += n;
    }

    std::string String() {
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
        } else {
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

std::string Join(const std::string& mount, const std::string& name) {
    std::string m = mount;
    while (m.rfind("../", 0) == 0) m.erase(0, 3);
    std::string out = m + name;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

std::vector<std::string> Split(const std::string& path) {
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

std::string Stem(const std::string& file) {
    const size_t dot = file.find('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

} // namespace

bool PakIndexParse(const uint8_t* bytes, size_t size, uint64_t fileSize, uint64_t at, PakIndex* out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (fileSize < kFooterSize || at + size != fileSize || size < kFooterSize) return fail("too short for a pak");
    const uint8_t* footer = bytes + size - kFooterSize;
    uint32_t magic = 0, version = 0;
    uint64_t indexAt = 0, indexSize = 0;
    std::memcpy(&magic, footer, 4);
    std::memcpy(&version, footer + kFooterVersion, 4);
    std::memcpy(&indexAt, footer + kFooterIndexAt, 8);
    std::memcpy(&indexSize, footer + kFooterIndexSize, 8);
    if (magic != kPakMagic) return fail("not a pak (no pak footer)");
    if (version < kMinVersion || version > kMaxVersion) return fail("pak version not read here");
    const uint64_t footerAt = fileSize - kFooterSize;
    if (indexAt < at || indexSize > footerAt || indexAt > footerAt - indexSize) return fail("index out of the file");
    if (indexAt + indexSize + kFlagSize <= footerAt && footer[-1] != 0) return fail("encrypted index");
    Reader r{bytes + (indexAt - at), static_cast<size_t>(indexSize)};
    PakIndex index;
    index.version = version;
    index.mount = r.String();
    const int32_t count = r.Get<int32_t>();
    if (!r.ok || count < 0 || count > kMaxEntries) return fail("bad index");
    index.entries.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count && r.ok; i++) {
        PakEntry e;
        e.path = Join(index.mount, r.String());
        r.Get<int64_t>();
        e.size = static_cast<uint64_t>(r.Get<int64_t>());
        e.uncompressedSize = static_cast<uint64_t>(r.Get<int64_t>());
        e.compression = r.Get<uint32_t>();
        if (version == kVersionWithTimestamp) r.Get<int64_t>();
        r.Skip(kHashBytes);
        if (version >= kVersionWithBlocks) {
            if (e.compression != 0) {
                const int32_t blocks = r.Get<int32_t>();
                if (blocks < 0 || blocks > kMaxBlocks) return fail("bad index");
                r.Skip(static_cast<size_t>(blocks) * kBlockBytes);
            }
            e.encrypted = r.Get<uint8_t>() != 0;
            r.Get<uint32_t>();
        }
        if (r.ok) index.entries.push_back(std::move(e));
    }
    if (!r.ok) return fail("index cut short");
    *out = std::move(index);
    return true;
}

bool PakIndexRead(const std::string& path, PakIndex* out, std::string* error) {
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
    uint8_t footer[kFooterSize];
    _fseeki64(f, static_cast<int64_t>(fileSize - kFooterSize), SEEK_SET);
    const bool got = std::fread(footer, 1, kFooterSize, f) == kFooterSize;
    uint64_t indexAt = 0;
    std::memcpy(&indexAt, footer + kFooterIndexAt, 8);
    if (!got || indexAt >= fileSize || fileSize - indexAt > kMaxIndexBytes) {
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
    return PakIndexParse(tail.data(), tail.size(), fileSize, indexAt, out, error);
}

BattleFile PakIndexClassifyPath(const std::string& path) {
    BattleFile b;
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::vector<std::string> parts = Split(p);
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
    if (parts[chara + 1] == "DataAsset" && chara + 3 < parts.size() && parts[chara + 2] == "StageData" && StartsWith(file, "DA_") &&
        file.find("_Stage_") != std::string::npos) {
        b.kind = BattleFileKind::StageData;
        b.character = code;
    }
    return b;
}

std::string PakIndexBattleDataKey(const std::string& packagePath) {
    const std::vector<std::string> parts = Split(packagePath);
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        if (parts[i] != "Chara") continue;
        if (i + 4 < parts.size() && parts[i + 2] == "BattleResource") return parts[i + 1] + "/" + parts[i + 3] + "/" + parts[i + 4];
        if (i + 3 < parts.size() && parts[i + 2] == "BattleResource") return parts[i + 1] + "/-/" + parts[i + 3];
    }
    return {};
}

} // namespace sfv
