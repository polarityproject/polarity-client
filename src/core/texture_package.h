#pragma once
// A cooked Texture2D package as the game's paks hold it (one .uasset, no
// .uexp; docs/custom-rooms-plan.md "Spike results"): finds the texture's
// platform data and picks a mip, without the engine. The platform data is
// the pixel format's name as an FString ("PF_DXT5") after SizeX, SizeY and
// the packed slice count, then the first mip's index, the mip count and each
// mip: a bulk data header (flags, element count, size on disk, offset), the
// bytes inline unless the flags put them at the end of the file, then the
// mip's SizeX and SizeY (and SizeZ in some engine versions; both read).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sfv {

enum class TexFormat : uint8_t {
    Unknown,
    Bgra8,      // PF_B8G8R8A8
    Bc1,        // PF_DXT1
    Bc3,        // PF_DXT5
    Bc5,        // PF_BC5
    Rgb10A2,    // PF_A2B10G10R10 (R. Mika's art): DXGI R10G10B10A2, red in the low bits
};

const char* TexFormatName(TexFormat f);
// Bytes of one width x height image in this format (BC: whole 4x4 blocks).
size_t TexBytes(TexFormat f, uint32_t width, uint32_t height);

struct TexMip {
    uint32_t width = 0, height = 0;
    size_t offset = 0, size = 0; // its bytes in the package
};

struct TexPackage {
    TexFormat format = TexFormat::Unknown;
    std::string formatName;      // as the package names it
    uint32_t width = 0, height = 0;
    std::vector<TexMip> mips;    // the ones with bytes in the package, largest first
};

// False (and why) for a package this does not understand: no platform data,
// mips whose sizes disagree, compressed bulk data, a format not listed.
bool ParseTexturePackage(const uint8_t* data, size_t size, TexPackage* out, std::string* error);

// The mip to draw at most `maxSide` pixels on its longer side: the smallest
// one that still covers it, else the largest (0: the largest). Null if none.
const TexMip* PickMip(const TexPackage& package, uint32_t maxSide);

// An image the overlay can upload: BGRA8 rows (pitch = width * 4) or BC
// blocks as stored.
struct TexImage {
    TexFormat format = TexFormat::Unknown;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bytes;
};

// The mip as an uploadable image, `maxSide` at most for the uncompressed
// formats (BGRA8; RGB10A2 becomes BGRA8): scaled down by averaging, with the
// colours weighted by alpha so cut-out edges stay clean. BC mips as they are.
bool MipImage(const uint8_t* data, size_t size, const TexPackage& package, const TexMip& mip, uint32_t maxSide,
              TexImage* out, std::string* error);

// BGRA8 scaled down to fit maxSide (alpha-weighted box filter). Exposed for tests.
std::vector<uint8_t> ShrinkBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight);

} // namespace sfv
