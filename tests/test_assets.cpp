// PNG, pak index, texture packages, key names and update signatures.

#include "test_support.h"

// The screenshot PNG (core/png.h): signature, IHDR, chunk CRCs, and the
// stored deflate blocks carrying every row (a 300x80 image is over 65535 bytes:
// two blocks).
void TestPng() {
    const int w = 300, h = 80;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < rgb.size(); i++) rgb[i] = static_cast<uint8_t>(i * 7);
    const std::vector<uint8_t> png = EncodePng(w, h, rgb.data());
    CHECK(png.size() > rgb.size() && std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    auto be32 = [&](size_t at) {
        return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | uint32_t(png[at + 3]);
    };
    size_t at = 8;
    std::vector<std::string> types;
    std::vector<uint8_t> idat;
    bool crcs = true;
    while (at + 12 <= png.size()) {
        const uint32_t len = be32(at);
        const std::string type(reinterpret_cast<const char*>(&png[at + 4]), 4);
        types.push_back(type);
        crcs &= Crc32(&png[at + 4], len + 4) == be32(at + 8 + len);
        if (type == "IHDR") CHECK(be32(at + 8) == uint32_t(w) && be32(at + 12) == uint32_t(h) && png[at + 16] == 8 && png[at + 17] == 2);
        if (type == "IDAT") idat.assign(png.begin() + at + 8, png.begin() + at + 8 + len);
        at += 12 + len;
    }
    CHECK(crcs && types == std::vector<std::string>({"IHDR", "IDAT", "IEND"}) && at == png.size());
    // Inflate the stored blocks back: the rows, each after filter byte 0.
    std::vector<uint8_t> raw;
    size_t p = 2;
    bool last = false;
    while (!last && p + 5 <= idat.size()) {
        last = idat[p] & 1;
        const size_t n = idat[p + 1] | (idat[p + 2] << 8);
        CHECK(((idat[p + 3] | (idat[p + 4] << 8)) ^ 0xFFFF) == n);
        raw.insert(raw.end(), idat.begin() + p + 5, idat.begin() + p + 5 + n);
        p += 5 + n;
    }
    bool rows = raw.size() == static_cast<size_t>(w * 3 + 1) * h;
    for (int y = 0; rows && y < h; y++) {
        rows = raw[y * (w * 3 + 1)] == 0 && std::memcmp(&raw[y * (w * 3 + 1) + 1], &rgb[y * w * 3], w * 3) == 0;
    }
    CHECK(last && rows);
}

// A version 3 pak's index (a mod's: unencrypted, two files, one compressed),
// read back, and the game's paths classified.
void TestPakIndex() {
    std::vector<uint8_t> pak(64, 0xAB); // file data before the index
    const uint64_t indexAt = pak.size();
    auto put = [&](const void* p, size_t n) { pak.insert(pak.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    auto put32 = [&](int32_t v) { put(&v, 4); };
    auto put64 = [&](int64_t v) { put(&v, 8); };
    auto str = [&](const std::string& t) {
        put32(static_cast<int32_t>(t.size() + 1));
        put(t.c_str(), t.size() + 1);
    };
    str("../../../StreetFighterV/Content/");
    put32(2);
    str("Chara/JRI/BattleResource/061/BAC_JRI.uasset");
    put64(0), put64(100), put64(300), put32(1);  // offset, size, uncompressed, zlib
    put(std::vector<uint8_t>(20, 0).data(), 20); // hash
    put32(1), put64(0), put64(100);              // one block
    uint8_t enc = 0;
    put(&enc, 1), put32(65536);
    str("Chara/JRI/Texture/T_JRI_00_COL.uasset");
    put64(100), put64(50), put64(50), put32(0);
    put(std::vector<uint8_t>(20, 0).data(), 20);
    put(&enc, 1), put32(0);
    const uint64_t indexSize = pak.size() - indexAt;
    uint8_t flag = 0;
    put(&flag, 1);
    put32(static_cast<int32_t>(0x5A6F12E1)), put32(3), put64(static_cast<int64_t>(indexAt)), put64(static_cast<int64_t>(indexSize));
    put(std::vector<uint8_t>(20, 0).data(), 20);

    PakIndex index;
    std::string error;
    CHECK(PakIndexParse(pak.data(), pak.size(), pak.size(), 0, &index, &error));
    CHECK(index.version == 3);
    CHECK(index.entries.size() == 2);
    if (index.entries.size() == 2) {
        CHECK(index.entries[0].path == "StreetFighterV/Content/Chara/JRI/BattleResource/061/BAC_JRI.uasset");
        CHECK(index.entries[0].uncompressedSize == 300 && index.entries[0].compression == 1);
        CHECK(index.entries[1].path == "StreetFighterV/Content/Chara/JRI/Texture/T_JRI_00_COL.uasset");
    }
    // From the index on only, as PakIndexRead reads it.
    PakIndex tail;
    CHECK(PakIndexParse(pak.data() + indexAt, pak.size() - indexAt, pak.size(), indexAt, &tail, &error) && tail.entries.size() == 2);
    // An encrypted index, a newer version, no pak at all.
    std::vector<uint8_t> bad = pak;
    bad[bad.size() - 45] = 1;
    CHECK(!PakIndexParse(bad.data(), bad.size(), bad.size(), 0, &index, &error) && error == "encrypted index");
    bad = pak;
    bad[bad.size() - 45 + 5] = 8;
    CHECK(!PakIndexParse(bad.data(), bad.size(), bad.size(), 0, &index, &error));
    std::vector<uint8_t> none(100, 0);
    CHECK(!PakIndexParse(none.data(), none.size(), none.size(), 0, &index, &error));
    // Older tools' paks: no flag byte, the footer right after the index.
    std::vector<uint8_t> old = pak;
    old.erase(old.end() - 45);
    CHECK(PakIndexParse(old.data(), old.size(), old.size(), 0, &index, &error) && index.entries.size() == 2);
    CHECK(PakIndexParse(old.data() + indexAt, old.size() - indexAt, old.size(), indexAt, &tail, &error) && tail.entries.size() == 2);
    // An index that runs into the footer.
    bad = old;
    const int64_t longer = static_cast<int64_t>(indexSize + 1);
    std::memcpy(bad.data() + bad.size() - 44 + 16, &longer, 8);
    CHECK(!PakIndexParse(bad.data(), bad.size(), bad.size(), 0, &index, &error) && error == "index out of the file");

    auto kind = [](const char* path) { return PakIndexClassifyPath(path).kind; };
    CHECK(kind("StreetFighterV/Content/Chara/KEN/BattleResource/061/BAC_KEN.uasset") == BattleFileKind::MoveData);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/BattleResource/061/BAC_KEN_eff.uexp") == BattleFileKind::MoveData);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/BattleResource/061/BCM_KEN.uasset") == BattleFileKind::MoveData);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/BattleResource/061/BCH_KEN.uasset") == BattleFileKind::MoveData);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/BattleResource/061/DA_KEN_BattleResource.uasset") == BattleFileKind::MoveData);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/BattleResource/061/BAI_KEN.uasset") == BattleFileKind::CpuAi);
    CHECK(kind("StreetFighterV/Content/Chara/CMN/BattleResource/BAC_CMN.uasset") == BattleFileKind::MoveData);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/DataAsset/StageData/DA_KEN_Stage_BRZ.uasset") == BattleFileKind::StageData);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/Animation/KEN_SPA_SYORYU.uasset") == BattleFileKind::Other);
    CHECK(kind("StreetFighterV/Content/Chara/KEN/Texture/T_KEN_00.uasset") == BattleFileKind::Other);
    CHECK(kind("StreetFighterV/Content/UI/Battle/BAC_FAKE.uasset") == BattleFileKind::Other);
    const BattleFile b = PakIndexClassifyPath(R"(StreetFighterV\Content\Chara\JRI\BattleResource\061\BAC_JRI.uasset)");
    CHECK(b.kind == BattleFileKind::MoveData && b.character == "JRI" && b.version == "061");
    CHECK(PakIndexBattleDataKey("/Game/Chara/KEN/BattleResource/061/BAC_KEN") == "KEN/061/BAC_KEN");
    CHECK(PakIndexBattleDataKey("/Game/Chara/CMN/BattleResource/005/BCM_CMN") == "CMN/005/BCM_CMN");
    CHECK(PakIndexBattleDataKey("/Game/Chara/KEN/Texture/T_KEN").empty());
}

void TestTexturePackage() {
    // A package as the paks hold one: a name table entry first (the same
    // string, no header), then the platform data with two mips, the first's
    // bytes at the end of the file, the second's inline.
    std::vector<uint8_t> pkg;
    auto i32 = [&](int32_t v) { pkg.insert(pkg.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
    auto i64 = [&](int64_t v) { pkg.insert(pkg.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 8); };
    auto str = [&](const char* s) {
        i32(static_cast<int32_t>(std::strlen(s) + 1));
        pkg.insert(pkg.end(), s, s + std::strlen(s) + 1);
    };
    i32(0x9E2A83C1);
    for (int i = 0; i < 4; i++) i32(-7);
    str("PF_B8G8R8A8");
    i32(0x1234);
    i32(4);
    i32(2);
    i32(1);
    str("PF_B8G8R8A8");
    i32(0);
    i32(2);
    // Each mip: bCooked, the bulk data header (offsets from where the bulk
    // data starts, at the end), the inline bytes if any, the mip's size.
    i32(1);
    i32(0x1);
    i32(32);
    i32(32);
    const size_t offsetAt = pkg.size();
    i64(0);
    i32(4);
    i32(2);
    i32(1);
    i32(0x40);
    i32(8);
    i32(8);
    i64(0);
    for (int i = 0; i < 2; i++) {
        pkg.push_back(10);
        pkg.push_back(20);
        pkg.push_back(30);
        pkg.push_back(255);
    }
    i32(2);
    i32(1);
    const int64_t end = static_cast<int64_t>(pkg.size());
    // Mip 0: red opaque, blue clear, alternating.
    for (int i = 0; i < 8; i++) {
        const bool red = i % 2 == 0;
        pkg.push_back(red ? 0 : 255);
        pkg.push_back(0);
        pkg.push_back(red ? 255 : 0);
        pkg.push_back(red ? 255 : 0);
    }
    i32(0x9E2A83C1); // the package tag that closes the file
    TexPackage p;
    std::string error;
    CHECK(ParseTexturePackage(pkg.data(), pkg.size(), &p, &error));
    CHECK(p.format == TexFormat::Bgra8 && p.width == 4 && p.height == 2 && p.mips.size() == 2);
    CHECK(p.mips.size() == 2 && p.mips[0].offset == static_cast<size_t>(end) && p.mips[1].width == 2 && p.mips[1].size == 8);
    CHECK(PickMip(p, 0) == &p.mips[0] && PickMip(p, 2) == &p.mips[1] && PickMip(p, 3) == &p.mips[0] && PickMip(p, 1) == &p.mips[1]);
    TexImage image;
    CHECK(MipImage(pkg.data(), pkg.size(), p, p.mips[0], 2, &image, &error));
    // 4x2 into 2x1: each pixel is a red and a clear blue one: red, half alpha.
    CHECK(image.width == 2 && image.height == 1 && image.bytes.size() == 8);
    CHECK(image.bytes.size() == 8 && image.bytes[0] == 0 && image.bytes[2] == 255 && image.bytes[3] == 128);
    CHECK(MipImage(pkg.data(), pkg.size(), p, p.mips[1], 0, &image, &error) && image.width == 2 && image.bytes[2] == 30);
    // Broken ones are refused: a cut file, a mip size that does not halve.
    CHECK(!ParseTexturePackage(pkg.data(), static_cast<size_t>(end) + 4, &p, &error));
    // Without the tag the data ends the file.
    CHECK(ParseTexturePackage(pkg.data(), pkg.size() - 4, &p, &error) && p.mips.size() == 2 && p.mips[0].offset == static_cast<size_t>(end));
    std::vector<uint8_t> bad = pkg;
    const int32_t three = 3;
    std::memcpy(bad.data() + offsetAt + 8 + 8 + 4 + 20 + 8, &three, 4); // the second mip's width
    CHECK(!ParseTexturePackage(bad.data(), bad.size(), &p, &error));
    CHECK(TexBytes(TexFormat::Bc1, 6, 6) == 4 * 8 && TexBytes(TexFormat::Bc3, 1, 1) == 16);
    // BC3, as the pad glyphs are: mips below 4x4 stored as 4x4, all inline.
    pkg.clear();
    i32(0x9E2A83C1);
    i32(8);
    i32(8);
    i32(1);
    str("PF_DXT5");
    i32(0);
    i32(3);
    for (const int side : {8, 4, 4}) {
        const int bytes = side == 8 ? 64 : 16;
        i32(1);
        i32(0x40);
        i32(bytes);
        i32(bytes);
        i64(0);
        pkg.insert(pkg.end(), static_cast<size_t>(bytes), static_cast<uint8_t>(side));
        i32(side);
        i32(side);
    }
    i32(0x9E2A83C1);
    CHECK(ParseTexturePackage(pkg.data(), pkg.size(), &p, &error) && p.format == TexFormat::Bc3 && p.mips.size() == 3);
    CHECK(MipImage(pkg.data(), pkg.size(), p, *PickMip(p, 0), 4, &image, &error) && image.format == TexFormat::Bc3 &&
          image.width == 8 && image.bytes.size() == 64 && image.bytes[0] == 8);
    // BC1: one block, white and black ends; indices 0,1,2,3 across the first row.
    const uint8_t bc1[8] = {0xFF, 0xFF, 0x00, 0x00, 0xE4, 0, 0, 0};
    std::vector<uint8_t> px;
    CHECK(DecodeBc(TexFormat::Bc1, bc1, 8, 4, 4, &px) && px.size() == 64);
    CHECK(px.size() == 64 && px[0] == 255 && px[3] == 255 && px[4] == 0 && px[8] == 170 && px[12] == 85 && px[16] == 255);
    // c0 <= c1: three colours and a clear one (index 3).
    const uint8_t bc1clear[8] = {0x00, 0x00, 0xFF, 0xFF, 0xC0, 0, 0, 0};
    CHECK(DecodeBc(TexFormat::Bc1, bc1clear, 8, 4, 4, &px) && px[12 + 3] == 0 && px[3] == 255);
    // BC3: alpha ends 255 and 0, the first pixel index 1 (0), the rest 0 (255).
    uint8_t bc3[16] = {255, 0, 1, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    CHECK(DecodeBc(TexFormat::Bc3, bc3, 16, 4, 4, &px) && px[3] == 0 && px[7] == 255 && px[0] == 255);
    CHECK(!DecodeBc(TexFormat::Bc5, bc3, 16, 4, 4, &px) && !DecodeBc(TexFormat::Bc1, bc1, 7, 4, 4, &px));
    // A look decodes BC: the 8x8 BC3 above, blurred, comes back BGRA8.
    TexLook look;
    look.blur = 1;
    CHECK(MipImage(pkg.data(), pkg.size(), p, *PickMip(p, 0), 0, &image, &error, look) && image.format == TexFormat::Bgra8 &&
          image.width == 8 && image.bytes.size() == 256);
    // Blur: a lone opaque pixel spreads, keeps its colour (premultiplied), and its total alpha.
    std::vector<uint8_t> dot(9 * 9 * 4, 0);
    dot[(4 * 9 + 4) * 4 + 2] = 200;
    dot[(4 * 9 + 4) * 4 + 3] = 255;
    BlurBgra(dot, 9, 9, 1);
    int total = 0;
    for (size_t i = 3; i < dot.size(); i += 4) total += dot[i];
    CHECK(dot[(4 * 9 + 4) * 4 + 3] < 255 && dot[(4 * 9 + 5) * 4 + 3] > 0 && dot[(4 * 9 + 5) * 4 + 2] == 200);
    CHECK(total > 240 && total < 270);
    // Saturate 0: grey; pad: clear round it; shadow: black, alpha kept.
    std::vector<uint8_t> red = {0, 0, 255, 255};
    SaturateBgra(red, 0);
    CHECK(red[0] == red[1] && red[1] == red[2] && red[2] == 54);
    const std::vector<uint8_t> padded = PadBgra({1, 2, 3, 4}, 1, 1, 1);
    CHECK(padded.size() == 36 && padded[3] == 0 && padded[16] == 1 && padded[19] == 4);
    // Noise: the same for a seed, white or black, about as much of each.
    const std::vector<uint8_t> n1 = NoiseBgra(64, 7), n2 = NoiseBgra(64, 7);
    int white = 0, black = 0;
    for (size_t i = 0; i < n1.size(); i += 4) (n1[i] ? white : black)++;
    CHECK(n1 == n2 && n1.size() == 64 * 64 * 4 && white > 1700 && black > 1700);
}

void TestKeyNames() {
    // The default assignment, slot by slot: W S D A B N G H K J Comma M Period Slash Enter Escape.
    CHECK(VirtualKeyOf("W") == 'W' && VirtualKeyOf("b") == 'B' && VirtualKeyOf("Comma") == 0xBC && VirtualKeyOf("Period") == 0xBE);
    CHECK(VirtualKeyOf("Slash") == 0xBF && VirtualKeyOf("Enter") == 0x0D && VirtualKeyOf("Escape") == 0x1B);
    CHECK(VirtualKeyOf("One") == '1' && VirtualKeyOf("NumPadNine") == 0x69 && VirtualKeyOf("F12") == 0x7B && VirtualKeyOf("F13") == 0);
    CHECK(VirtualKeyOf("LeftShift") == 0x10 && VirtualKeyOf("SpaceBar") == 0x20 && VirtualKeyOf("Nonsense") == 0 && VirtualKeyOf("") == 0);
    // Up, down, right, left; B (slot 4) is the pad's A, N its B, G X, H Y; Enter Start, Escape Back.
    CHECK(PadKeyOfSlot(0) == 0x1 && PadKeyOfSlot(2) == 0x8 && PadKeyOfSlot(3) == 0x4);
    CHECK(PadKeyOfSlot(4) == 0x40 && PadKeyOfSlot(5) == 0x80 && PadKeyOfSlot(6) == 0x10 && PadKeyOfSlot(7) == 0x20);
    CHECK(PadKeyOfSlot(14) == 0x4000 && PadKeyOfSlot(15) == 0x8000 && PadKeyOfSlot(16) == 0 && PadKeyOfSlot(-1) == 0);
}

void TestUpdateSignature() {
    // A key pair as the release script makes one, through the same API.
    BCRYPT_KEY_HANDLE key = nullptr;
    CHECK(BCRYPT_SUCCESS(BCryptGenerateKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, &key, 256, 0)));
    CHECK(BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0)));
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG size = 0;
    CHECK(BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &size, 0)));
    uint8_t pub[64];
    std::memcpy(pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);

    const std::string text = R"({"product": "SFVR", "version": "9.9.9"})";
    uint8_t digest[32];
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)text.data(), (ULONG)text.size(), digest, 32);
    uint8_t sig[64];
    CHECK(BCRYPT_SUCCESS(BCryptSignHash(key, nullptr, digest, 32, sig, 64, &size, 0)) && size == 64);
    BCryptDestroyKey(key);

    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    CHECK(VerifySignature(bytes, text.size(), sig, 64, pub));
    std::string tampered = text;
    tampered[tampered.size() - 3] = '8';
    CHECK(!VerifySignature(reinterpret_cast<const uint8_t*>(tampered.data()), tampered.size(), sig, 64, pub));
    uint8_t otherSig[64];
    std::memcpy(otherSig, sig, 64);
    otherSig[5] ^= 1;
    CHECK(!VerifySignature(bytes, text.size(), otherSig, 64, pub));
    const uint8_t noKey[64] = {};
    CHECK(!VerifySignature(bytes, text.size(), sig, 64, noKey));
}
