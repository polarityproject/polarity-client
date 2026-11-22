// XPRESS Huffman: Windows' codec looked up at run time, and a decoder of the mod's own (docs/desync-replays.md).

#include "core/compress.h"

#include <windows.h>

#include <compressapi.h>

#include <algorithm>

namespace sfv {

namespace {

constexpr DWORD kAlgorithm = COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW;
constexpr size_t kGrowthShare = 8, kGrowthSlack = 1024;

struct Codec {
    decltype(&::CreateCompressor) createCompressor = nullptr;
    decltype(&::Compress) compress = nullptr;
    decltype(&::CloseCompressor) closeCompressor = nullptr;
    decltype(&::CreateDecompressor) createDecompressor = nullptr;
    decltype(&::Decompress) decompress = nullptr;
    decltype(&::CloseDecompressor) closeDecompressor = nullptr;
    bool canCompress = false, canDecompress = false;
};

const Codec& Cabinet() {
    static const Codec codec = [] {
        Codec c;
        HMODULE m = LoadLibraryW(L"cabinet.dll");
        if (!m) return c;
        auto get = [m](auto* fn, const char* name) { *fn = reinterpret_cast<std::remove_pointer_t<decltype(fn)>>(GetProcAddress(m, name)); };
        get(&c.createCompressor, "CreateCompressor");
        get(&c.compress, "Compress");
        get(&c.closeCompressor, "CloseCompressor");
        get(&c.createDecompressor, "CreateDecompressor");
        get(&c.decompress, "Decompress");
        get(&c.closeDecompressor, "CloseDecompressor");
        c.canCompress = c.createCompressor && c.compress && c.closeCompressor;
        c.canDecompress = c.createDecompressor && c.decompress && c.closeDecompressor;
        return c;
    }();
    return codec;
}

} // namespace

bool DecompressXpressHuffman(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out) {
    constexpr int kMaxBits = 15;
    constexpr int kSymbols = 512, kLiterals = 256, kTableBytes = kSymbols / 2;
    constexpr size_t kBlockOutput = 65536;
    constexpr int kNibble = 15, kNibbleBits = 4, kWordBits = 16, kStreamBits = 32;
    constexpr size_t kByteLength = 15, kWordLength = 255, kMinMatch = 3;
    if (rawSize > kMaxRawSize) return false;
    if (size == rawSize) {
        out->assign(data, data + size);
        return true;
    }
    std::vector<uint8_t> buf(rawSize);
    std::vector<uint16_t> table(1u << kMaxBits);
    uint8_t lengths[kSymbols];
    size_t in = 0, at = 0;
    auto read16 = [&](size_t p) -> uint32_t { return p + 1 < size ? data[p] | data[p + 1] << 8 : 0; };
    while (at < rawSize) {
        if (in + kTableBytes + 4 > size) return false;
        for (int i = 0; i < kTableBytes; i++) {
            lengths[2 * i] = data[in + i] & kNibble;
            lengths[2 * i + 1] = data[in + i] >> kNibbleBits;
        }
        size_t entry = 0;
        for (int bits = 1; bits <= kMaxBits; bits++) {
            for (int sym = 0; sym < kSymbols; sym++) {
                if (lengths[sym] != bits) continue;
                const size_t count = size_t{1} << (kMaxBits - bits);
                if (entry + count > table.size()) return false;
                std::fill(table.begin() + entry, table.begin() + entry + count, static_cast<uint16_t>(sym));
                entry += count;
            }
        }
        if (entry != table.size()) return false;
        in += kTableBytes;
        uint32_t next = read16(in) << kWordBits | read16(in + 2);
        in += 4;
        int extra = kWordBits;
        auto take = [&](int bits) {
            next = bits >= kStreamBits ? 0 : next << bits;
            extra -= bits;
            if (extra < 0) {
                next |= read16(in) << -extra;
                extra += kWordBits;
                in += 2;
            }
        };
        const size_t blockEnd = at + kBlockOutput;
        while (at < blockEnd && at < rawSize) {
            int sym = table[next >> (kStreamBits - kMaxBits)];
            take(lengths[sym]);
            if (sym < kLiterals) {
                buf[at++] = static_cast<uint8_t>(sym);
                continue;
            }
            sym -= kLiterals;
            size_t length = sym & kNibble;
            const int offsetBits = sym >> kNibbleBits;
            if (length == kByteLength) {
                if (in >= size) return false;
                length = data[in++];
                if (length == kWordLength) {
                    if (in + 2 > size) return false;
                    length = read16(in);
                    in += 2;
                    if (length == 0) {
                        if (in + 4 > size) return false;
                        length = read16(in) | read16(in + 2) << kWordBits;
                        in += 4;
                    }
                    if (length < kByteLength) return false;
                    length -= kByteLength;
                }
                length += kByteLength;
            }
            length += kMinMatch;
            const size_t offset = (offsetBits ? next >> (kStreamBits - offsetBits) : 0) + (size_t{1} << offsetBits);
            take(offsetBits);
            if (offset > at || length > rawSize - at) return false;
            for (size_t i = 0; i < length; i++, at++) buf[at] = buf[at - offset];
        }
    }
    *out = std::move(buf);
    return true;
}

bool Compress(const uint8_t* data, size_t size, std::vector<uint8_t>* out) {
    const Codec& api = Cabinet();
    COMPRESSOR_HANDLE c = nullptr;
    if (!api.canCompress || !api.createCompressor(kAlgorithm, nullptr, &c) || !c) return false;
    std::vector<uint8_t> buf(size + size / kGrowthShare + kGrowthSlack);
    SIZE_T written = 0;
    const bool ok = api.compress(c, data, size, buf.data(), buf.size(), &written) != FALSE;
    api.closeCompressor(c);
    if (!ok) return false;
    buf.resize(written);
    *out = std::move(buf);
    return true;
}

bool Decompress(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out) {
    if (rawSize > kMaxRawSize) return false;
    const Codec& api = Cabinet();
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!api.canDecompress || !api.createDecompressor(kAlgorithm, nullptr, &d) || !d) return DecompressXpressHuffman(data, size, rawSize, out);
    std::vector<uint8_t> buf(rawSize);
    SIZE_T written = 0;
    const bool ok = api.decompress(d, data, size, buf.data(), buf.size(), &written) != FALSE && written == rawSize;
    api.closeDecompressor(d);
    if (!ok) return DecompressXpressHuffman(data, size, rawSize, out);
    *out = std::move(buf);
    return true;
}

} // namespace sfv
