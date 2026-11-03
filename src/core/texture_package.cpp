#include "core/texture_package.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace sfv {

namespace {

// FByteBulkData flags (UE4's EBulkDataFlags).
constexpr uint32_t kPayloadAtEndOfFile = 0x1;
constexpr uint32_t kCompressedZlib = 0x2;
constexpr uint32_t kUnused = 0x20;
constexpr uint32_t kSeparateFile = 0x100;
constexpr uint32_t kSize64Bit = 0x2000;
// PACKAGE_FILE_TAG: a package's first and last four bytes.
constexpr uint8_t kPackageTag[4] = {0xC1, 0x83, 0x2A, 0x9E};

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t at;
    bool ok = true;

    template <typename T> T Get()
    {
        T v{};
        if (at + sizeof(T) > size) {
            ok = false;
            return v;
        }
        std::memcpy(&v, data + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
};

TexFormat FormatOf(const std::string& name)
{
    if (name == "PF_B8G8R8A8") return TexFormat::Bgra8;
    if (name == "PF_DXT1") return TexFormat::Bc1;
    if (name == "PF_DXT5") return TexFormat::Bc3;
    if (name == "PF_BC5") return TexFormat::Bc5;
    if (name == "PF_A2B10G10R10") return TexFormat::Rgb10A2;
    return TexFormat::Unknown;
}

// The mips after the platform data's header at `at`: each a bCooked int32
// (when `cookedFlag`), the bulk data, then its sizes and `zFields` more
// int32s. False when they do not add up. The offsets of data at the end of
// the file count from where the package's bulk data starts, which is the
// end of the file less all of it.
bool ReadMips(const uint8_t* data, size_t size, size_t at, TexPackage* p, bool cookedFlag, int zFields, std::string* error)
{
    Reader r{data, size, at};
    const int32_t firstMip = r.Get<int32_t>();
    const int32_t count = r.Get<int32_t>();
    if (!r.ok || firstMip < 0 || firstMip > 15 || count < 1 || count > 16) {
        *error = "no mip list";
        return false;
    }
    p->mips.clear();
    std::vector<size_t> atEndMips; // indexes into p->mips with offsets from the bulk data's start
    uint64_t bulkEnd = 0;
    uint32_t lastW = 0, lastH = 0;
    for (int32_t m = 0; m < count; m++) {
        if (cookedFlag && r.Get<int32_t>() != 1) {
            *error = "a mip is not cooked";
            return false;
        }
        const uint32_t flags = r.Get<uint32_t>();
        r.Get<int32_t>(); // element count
        const int64_t onDisk = flags & kSize64Bit ? r.Get<int64_t>() : r.Get<int32_t>();
        const int64_t offset = r.Get<int64_t>();
        if (!r.ok || onDisk < 0 || static_cast<uint64_t>(onDisk) > size) {
            *error = "a mip's bulk data header runs past the file";
            return false;
        }
        const bool unused = (flags & kUnused) != 0;
        const bool atEnd = (flags & kPayloadAtEndOfFile) != 0;
        size_t mipAt = 0;
        if (!unused && !atEnd) {
            if (flags & kSeparateFile) {
                *error = "mip data in a separate file";
                return false;
            }
            mipAt = r.at;
            r.at += static_cast<size_t>(onDisk);
            if (r.at > size) {
                *error = "inline mip data runs past the file";
                return false;
            }
        }
        const int32_t w = r.Get<int32_t>(), h = r.Get<int32_t>();
        for (int z = 0; z < zFields; z++) r.Get<int32_t>();
        if (!r.ok || w < 1 || h < 1 || w > 16384 || h > 16384) {
            *error = "a mip's size is not one";
            return false;
        }
        // Each mip halves the last (down to 1; block formats stop at one 4x4
        // block: the pad glyphs' 2x2 and 1x1 mips say 4x4).
        const uint32_t floor = p->format == TexFormat::Bgra8 || p->format == TexFormat::Rgb10A2 ? 1u : 4u;
        auto halves = [&](uint32_t now, uint32_t last) { return now == std::max(1u, last / 2) || (now == floor && last == floor); };
        if (m > 0 && (!halves(static_cast<uint32_t>(w), lastW) || !halves(static_cast<uint32_t>(h), lastH))) {
            *error = "the mip sizes do not halve";
            return false;
        }
        lastW = static_cast<uint32_t>(w);
        lastH = static_cast<uint32_t>(h);
        if (unused || onDisk == 0) continue;
        if (flags & kCompressedZlib) continue; // not read: the next mip may be plain
        if (atEnd) {
            if (offset < 0 || static_cast<uint64_t>(offset) + static_cast<uint64_t>(onDisk) > size) {
                *error = "a mip's data at the end of the file runs past it";
                return false;
            }
            mipAt = static_cast<size_t>(offset);
            bulkEnd = std::max(bulkEnd, static_cast<uint64_t>(offset) + static_cast<uint64_t>(onDisk));
            atEndMips.push_back(p->mips.size());
        }
        if (static_cast<size_t>(onDisk) != TexBytes(p->format, lastW, lastH)) {
            *error = "a mip's size on disk does not match its format";
            return false;
        }
        p->mips.push_back({lastW, lastH, mipAt, static_cast<size_t>(onDisk)});
    }
    if (p->mips.empty()) {
        *error = "no mip has its data in the package";
        return false;
    }
    // The bulk data ends the file, before the package tag that closes it: its
    // start is what is left before it, and it starts past the platform data.
    const bool tagged = size >= 4 && std::memcmp(data + size - 4, kPackageTag, 4) == 0;
    const size_t end = size - (tagged ? 4 : 0);
    if (bulkEnd > end) {
        *error = "the mips' data at the end of the file runs past it";
        return false;
    }
    const size_t bulkStart = end - static_cast<size_t>(bulkEnd);
    if (!atEndMips.empty() && bulkStart < r.at) {
        *error = "the mips' data at the end of the file overlaps the texture";
        return false;
    }
    for (size_t i : atEndMips) p->mips[i].offset += bulkStart;
    return true;
}

} // namespace

const char* TexFormatName(TexFormat f)
{
    switch (f) {
    case TexFormat::Bgra8: return "BGRA8";
    case TexFormat::Bc1: return "BC1";
    case TexFormat::Bc3: return "BC3";
    case TexFormat::Bc5: return "BC5";
    case TexFormat::Rgb10A2: return "RGB10A2";
    default: return "unknown";
    }
}

size_t TexBytes(TexFormat f, uint32_t width, uint32_t height)
{
    const size_t blocks = size_t((width + 3) / 4) * ((height + 3) / 4);
    switch (f) {
    case TexFormat::Bgra8:
    case TexFormat::Rgb10A2: return size_t(width) * height * 4;
    case TexFormat::Bc1: return blocks * 8;
    case TexFormat::Bc3:
    case TexFormat::Bc5: return blocks * 16;
    default: return 0;
    }
}

bool ParseTexturePackage(const uint8_t* data, size_t size, TexPackage* out, std::string* error)
{
    std::string why = "no texture platform data";
    // Every "PF_" that is an FString with a plausible header before it (the
    // name table has the same strings, without one).
    for (size_t at = 16; at + 3 <= size; at++) {
        if (data[at] != 'P' || data[at + 1] != 'F' || data[at + 2] != '_') continue;
        int32_t length = 0;
        std::memcpy(&length, data + at - 4, 4);
        if (length < 5 || length > 40 || at + static_cast<size_t>(length) > size || data[at + length - 1] != 0) continue;
        const std::string name(reinterpret_cast<const char*>(data + at), static_cast<size_t>(length - 1));
        int32_t head[3];
        std::memcpy(head, data + at - 16, 12);
        const int32_t w = head[0], h = head[1], slices = head[2] & 0x3FFFFFFF;
        if (w < 1 || h < 1 || w > 16384 || h > 16384 || slices != 1) continue;
        TexPackage p;
        p.formatName = name;
        p.format = FormatOf(name);
        p.width = static_cast<uint32_t>(w);
        p.height = static_cast<uint32_t>(h);
        if (p.format == TexFormat::Unknown) {
            why = "pixel format " + name + " is not read";
            continue;
        }
        // The game's own layout first (a bCooked flag, no SizeZ), then the others.
        for (const auto [cooked, zFields] : {std::pair{true, 0}, {true, 1}, {false, 0}, {false, 1}}) {
            std::string mipWhy;
            if (ReadMips(data, size, at + static_cast<size_t>(length), &p, cooked, zFields, &mipWhy)) {
                *out = std::move(p);
                return true;
            }
            if (cooked && zFields == 0) why = mipWhy;
        }
    }
    if (error) *error = why;
    return false;
}

const TexMip* PickMip(const TexPackage& package, uint32_t maxSide)
{
    if (package.mips.empty()) return nullptr;
    const TexMip* best = &package.mips.front();
    if (maxSide == 0) return best;
    for (const TexMip& m : package.mips) {
        if (std::max(m.width, m.height) >= maxSide) best = &m;
    }
    return best;
}

std::vector<uint8_t> ShrinkBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight)
{
    std::vector<uint8_t> out(size_t(outWidth) * outHeight * 4);
    for (uint32_t oy = 0; oy < outHeight; oy++) {
        const uint32_t y0 = static_cast<uint32_t>(uint64_t(oy) * height / outHeight);
        const uint32_t y1 = std::max(y0 + 1, static_cast<uint32_t>(uint64_t(oy + 1) * height / outHeight));
        for (uint32_t ox = 0; ox < outWidth; ox++) {
            const uint32_t x0 = static_cast<uint32_t>(uint64_t(ox) * width / outWidth);
            const uint32_t x1 = std::max(x0 + 1, static_cast<uint32_t>(uint64_t(ox + 1) * width / outWidth));
            uint64_t b = 0, g = 0, r = 0, a = 0, n = 0;
            for (uint32_t y = y0; y < y1; y++) {
                const uint8_t* p = bgra + (size_t(y) * width + x0) * 4;
                for (uint32_t x = x0; x < x1; x++, p += 4) {
                    b += uint64_t(p[0]) * p[3];
                    g += uint64_t(p[1]) * p[3];
                    r += uint64_t(p[2]) * p[3];
                    a += p[3];
                    n++;
                }
            }
            uint8_t* q = out.data() + (size_t(oy) * outWidth + ox) * 4;
            if (a) {
                q[0] = static_cast<uint8_t>((b + a / 2) / a);
                q[1] = static_cast<uint8_t>((g + a / 2) / a);
                q[2] = static_cast<uint8_t>((r + a / 2) / a);
            }
            else {
                q[0] = q[1] = q[2] = 0;
            }
            q[3] = static_cast<uint8_t>((a + n / 2) / n);
        }
    }
    return out;
}

bool MipImage(const uint8_t* data, size_t size, const TexPackage& package, const TexMip& mip, uint32_t maxSide,
              TexImage* out, std::string* error)
{
    if (mip.offset + mip.size > size) {
        if (error) *error = "the mip is not in the data";
        return false;
    }
    const uint8_t* bytes = data + mip.offset;
    TexImage image;
    image.width = mip.width;
    image.height = mip.height;
    if (package.format != TexFormat::Bgra8 && package.format != TexFormat::Rgb10A2) {
        image.format = package.format;
        image.bytes.assign(bytes, bytes + mip.size);
        *out = std::move(image);
        return true;
    }
    image.format = TexFormat::Bgra8;
    std::vector<uint8_t> bgra;
    if (package.format == TexFormat::Rgb10A2) {
        bgra.resize(mip.size);
        for (size_t i = 0; i + 4 <= mip.size; i += 4) {
            uint32_t v;
            std::memcpy(&v, bytes + i, 4);
            bgra[i + 0] = static_cast<uint8_t>((v >> 20 & 0x3FF) >> 2);
            bgra[i + 1] = static_cast<uint8_t>((v >> 10 & 0x3FF) >> 2);
            bgra[i + 2] = static_cast<uint8_t>((v & 0x3FF) >> 2);
            bgra[i + 3] = static_cast<uint8_t>((v >> 30) * 85);
        }
        bytes = bgra.data();
    }
    const uint32_t side = std::max(mip.width, mip.height);
    if (maxSide && side > maxSide) {
        const uint32_t w = std::max(1u, static_cast<uint32_t>((uint64_t(mip.width) * maxSide + side / 2) / side));
        const uint32_t h = std::max(1u, static_cast<uint32_t>((uint64_t(mip.height) * maxSide + side / 2) / side));
        image.bytes = ShrinkBgra(bytes, mip.width, mip.height, w, h);
        image.width = w;
        image.height = h;
    }
    else {
        image.bytes.assign(bytes, bytes + mip.size);
    }
    *out = std::move(image);
    return true;
}

} // namespace sfv
