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
    Bgra8,   // PF_B8G8R8A8
    Bc1,     // PF_DXT1
    Bc3,     // PF_DXT5
    Bc5,     // PF_BC5
    Rgb10A2, // PF_A2B10G10R10 (R. Mika's art): DXGI R10G10B10A2, red in the low bits
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
    std::string formatName; // as the package names it
    uint32_t width = 0, height = 0;
    std::vector<TexMip> mips; // the ones with bytes in the package, largest first
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

// What is done to an image after it is read (the UI's glass and shadows):
// it is then always BGRA8, BC mips decoded first.
struct TexLook {
    float blur = 0;      // a gaussian's sigma, in the result's pixels (after the shrink)
    float saturate = 1;  // CSS saturate(): 1 as it is
    bool shadow = false; // black, keeping only the alpha: a shadow with the image's own outline
    uint32_t pad = 0;    // clear pixels added on every side (room for a shadow's blur)

    bool Any() const { return blur > 0 || saturate != 1 || shadow || pad > 0; }
};

// The mip as an uploadable image, `maxSide` at most for the uncompressed
// formats (BGRA8; RGB10A2 becomes BGRA8): scaled down by averaging, with the
// colours weighted by alpha so cut-out edges stay clean. BC mips as they are,
// unless `look` asks for something (then decoded, shrunk and done).
bool MipImage(const uint8_t* data, size_t size, const TexPackage& package, const TexMip& mip, uint32_t maxSide,
              TexImage* out, std::string* error, const TexLook& look = {});

// BC1 or BC3 blocks to BGRA8 rows (width x height; blocks cover whole 4x4s).
// False for another format or too few bytes.
bool DecodeBc(TexFormat format, const uint8_t* blocks, size_t size, uint32_t width, uint32_t height, std::vector<uint8_t>* bgra);
// BGRA8 in place: a gaussian blur (premultiplied, edges clamped); CSS saturate().
void BlurBgra(std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, float sigma);
void SaturateBgra(std::vector<uint8_t>& bgra, float amount);
// BGRA8 with `pad` clear pixels added on every side.
std::vector<uint8_t> PadBgra(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t pad);
// Monochrome noise, side x side BGRA8: each pixel white or black, its alpha
// how far from the middle; a tile of film grain (the same for a seed).
std::vector<uint8_t> NoiseBgra(uint32_t side, uint32_t seed);

// BGRA8 scaled down to fit maxSide (alpha-weighted box filter). Exposed for tests.
std::vector<uint8_t> ShrinkBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight);

} // namespace sfv
