# Gathers a release from the Release build into dist\release\, the files the
# build workflow (.github/workflows/build.yml) attaches to a GitHub Release:
#   - the files the updater installs, and update-files.json, which says where
#     each one goes (release.ps1 writes the update info from it);
#   - polarity-<version>.zip, the package for first installs;
#   - polarity.pdb, for reading crash addresses.
# It also writes the version's CHANGELOG.md section to dist\release-notes.md,
# the release's description. With -Tag (the pushed tag) it fails unless the tag
# is v<version> and CHANGELOG.md has a section for that version that starts
# with the summary paragraph players see.
#   release-files.ps1 [-Tag v<version>]
param([string]$Tag = '')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build\Release'
$dist = Join-Path $root 'dist'
$out = Join-Path $dist 'release'
$version = (Select-String -Path (Join-Path $root 'src\core\version.h') -Pattern 'kPolarityVersion = "([^"]+)"').Matches.Groups[1].Value
if ($Tag -and $Tag -ne "v$version") { throw "the tag $Tag does not match kPolarityVersion $version" }

$changelog = Get-Content (Join-Path $root 'CHANGELOG.md') -Raw
$entry = [regex]::Match($changelog, "(?ms)^## \[$([regex]::Escape($version))\][^\n]*\n(.*?)(?=^## \[|\z)")
$notes = $entry.Groups[1].Value.Trim()
if ($Tag -and (-not $notes -or $notes.StartsWith('#'))) { throw "CHANGELOG.md needs a section for $version that starts with its summary paragraph" }

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null
[IO.File]::WriteAllText((Join-Path $dist 'release-notes.md'), $notes + "`n")

$license = Join-Path $build 'polarity_fonts_license.txt'
& (Join-Path $PSScriptRoot 'fonts-license.ps1') $license
# Where the updater puts each file, from where. A path is a plain name in
# Binaries\Win64, or a font under Polarity/fonts/ (SafeUpdatePath in
# src/core/update_info.cpp); each is uploaded under its file name.
$installed = [ordered]@{
  'xinput1_3.dll'                         = Join-Path $build 'xinput1_3.dll'
  'polarity.dll'                          = Join-Path $build 'polarity.dll'
  'Polarity/fonts/NotoSansCJK-Medium.ttc' = Join-Path $build 'fonts\NotoSansCJK-Medium.ttc'
  'polarity_fonts_license.txt'            = $license
  'polarity_third_party_notices.txt'      = Join-Path $root 'THIRD_PARTY_NOTICES.md'
}
$manifest = foreach ($path in $installed.Keys) {
  $name = Split-Path $path -Leaf
  Copy-Item $installed[$path] (Join-Path $out $name)
  [ordered]@{ path = $path; asset = $name }
}
[IO.File]::WriteAllText((Join-Path $out 'update-files.json'), (ConvertTo-Json -InputObject @($manifest)) + "`n")

& (Join-Path $PSScriptRoot 'package.ps1') -Public | Out-Null
Copy-Item (Join-Path $dist "polarity-$version.zip") $out
Copy-Item (Join-Path $build 'polarity.pdb') $out
"Polarity $version release files in $out"
Get-ChildItem $out | ForEach-Object { "  $($_.Name) ($([math]::Round($_.Length / 1MB, 2)) MB)" }
