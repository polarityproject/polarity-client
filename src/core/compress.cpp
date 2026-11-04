#include "core/compress.h"

#include <windows.h>

#include <compressapi.h>

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
    const Codec& api = Cabinet();
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!api.canDecompress || !api.createDecompressor(kAlgorithm, nullptr, &d) || !d) return false;
    std::vector<uint8_t> buf(rawSize);
    SIZE_T written = 0;
    const bool ok = api.decompress(d, data, size, buf.data(), buf.size(), &written) != FALSE && written == rawSize;
    api.closeDecompressor(d);
    if (!ok) return false;
    *out = std::move(buf);
    return true;
}

} // namespace sfv
