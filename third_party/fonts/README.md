# Bundled fonts

The overlay's type (src/mod/overlay.cpp, BuildFonts), the same on Windows and
Proton. All are under the SIL Open Font License 1.1 (the OFL-*.txt files;
releases carry them as sfvr_fonts_license.txt).

- Barlow Regular, SemiBold, Bold, Bold Italic: google/fonts, ofl/barlow
- Archivo Black: google/fonts, ofl/archivoblack
  (both embedded in sfv_rollback.dll as resources: src/mod/fonts.rc)
- Noto Sans CJK Medium (NotoSansCJK-Medium.ttc, 18 MB, faces JP, KR, SC, TC,
  HK): notofonts/noto-cjk, tag Sans2.004, Sans/OTC. Not kept here: the
  build downloads it (CMakeLists.txt, checked by SHA-256) and releases ship
  it as SFVR\fonts\NotoSansCJK-Medium.ttc next to the mod.
