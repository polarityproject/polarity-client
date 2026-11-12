#include "core/compress.h"

#include <windows.h>

#include <compressapi.h>

#include <algorithm>

namespace sfv {

namespace {

constexpr DWORD kAlgorithm = COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW;

// The codec's functions, looked up at run time rather than imported: Wine's
// cabinet.dll (Proton, the Steam Deck) had none of them until 2026, and an
// import Wine lacks becomes a stub that ends the process when called: Steam
// Deck players' games closed as their first match's recording was saved,
// after its results screen (0.2.6). Without them chunks stay uncompressed.
struct Codec {
    decltype(&::CreateCompressor) createCompressor = nullptr;
    decltype(&::Compress) compress = nullptr;
    decltype(&::CloseCompressor) closeCompressor = nullptr;
    decltype(&::CreateDecompressor) createDecompressor = nullptr;
    decltype(&::Decompress) decompress = nullptr;
    decltype(&::CloseDecompressor) closeDecompressor = nullptr;
    bool canCompress = false, canDecompress = false;
};

const Codec& Cabinet()
{
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

bool DecompressXpressHuffman(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out)
{
    // [MS-XCA] 2.2.4: blocks of 65536 output bytes, each after a table of
    // 512 4-bit code lengths (256 bytes, the low nibble the even symbol);
    // then a bit stream read 16 bits (little endian) at a time, most
    // significant bit first, with the bytes of long match lengths in between.
    // Symbols under 256 are bytes; the others matches: length in the low 4
    // bits (+3; 15 means more in the bytes that follow), the offset's bit
    // count in the next 4 (the offset is that many bits plus 1 << count).
    constexpr int kMaxBits = 15;
    // What would not get smaller Windows stores as it is (as its own
    // decoder reads it: the same size in as out).
    if (size == rawSize) {
        out->assign(data, data + size);
        return true;
    }
    std::vector<uint8_t> buf(rawSize);
    std::vector<uint16_t> table(1u << kMaxBits);
    uint8_t lengths[512];
    size_t in = 0, at = 0;
    auto read16 = [&](size_t p) -> uint32_t { return p + 1 < size ? data[p] | data[p + 1] << 8 : 0; };
    while (at < rawSize) {
        if (in + 256 + 4 > size) return false;
        for (int i = 0; i < 256; i++) {
            lengths[2 * i] = data[in + i] & 15;
            lengths[2 * i + 1] = data[in + i] >> 4;
        }
        // The canonical code: shorter codes first, then by symbol; each code
        // fills the table entries its bits prefix.
        size_t entry = 0;
        for (int bits = 1; bits <= kMaxBits; bits++) {
            for (int sym = 0; sym < 512; sym++) {
                if (lengths[sym] != bits) continue;
                const size_t count = size_t{1} << (kMaxBits - bits);
                if (entry + count > table.size()) return false;
                std::fill(table.begin() + entry, table.begin() + entry + count, static_cast<uint16_t>(sym));
                entry += count;
            }
        }
        if (entry != table.size()) return false;
        in += 256;
        uint32_t next = read16(in) << 16 | read16(in + 2);
        in += 4;
        int extra = 16;
        auto take = [&](int bits) {
            next = bits >= 32 ? 0 : next << bits;
            extra -= bits;
            if (extra < 0) {
                next |= read16(in) << -extra;
                extra += 16;
                in += 2;
            }
        };
        const size_t blockEnd = at + 65536;
        while (at < blockEnd && at < rawSize) {
            int sym = table[next >> (32 - kMaxBits)];
            take(lengths[sym]);
            if (sym < 256) {
                buf[at++] = static_cast<uint8_t>(sym);
                continue;
            }
            sym -= 256;
            size_t length = sym & 15;
            const int offsetBits = sym >> 4;
            if (length == 15) {
                if (in >= size) return false;
                length = data[in++];
                if (length == 255) {
                    if (in + 2 > size) return false;
                    length = read16(in);
                    in += 2;
                    if (length == 0) {
                        if (in + 4 > size) return false;
                        length = read16(in) | read16(in + 2) << 16;
                        in += 4;
                    }
                    if (length < 15) return false;
                    length -= 15;
                }
                length += 15;
            }
            length += 3;
            const size_t offset = (offsetBits ? next >> (32 - offsetBits) : 0) + (size_t{1} << offsetBits);
            take(offsetBits);
            if (offset > at || length > rawSize - at) return false;
            for (size_t i = 0; i < length; i++, at++) buf[at] = buf[at - offset];
        }
    }
    *out = std::move(buf);
    return true;
}

bool Compress(const uint8_t* data, size_t size, std::vector<uint8_t>* out)
{
    const Codec& api = Cabinet();
    COMPRESSOR_HANDLE c = nullptr;
    if (!api.canCompress || !api.createCompressor(kAlgorithm, nullptr, &c) || !c) return false;
    // XPRESS Huffman never grows its input by more than a little.
    std::vector<uint8_t> buf(size + size / 8 + 1024);
    SIZE_T written = 0;
    const bool ok = api.compress(c, data, size, buf.data(), buf.size(), &written) != FALSE;
    api.closeCompressor(c);
    if (!ok) return false;
    buf.resize(written);
    *out = std::move(buf);
    return true;
}

bool Decompress(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out)
{
    // Windows' own codec where it has one; else (Proton's cabinet.dll) the
    // decoder here: recordings made on Windows, and the benchmark's round,
    // read on a Steam Deck too.
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
