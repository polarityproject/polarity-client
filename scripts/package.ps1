# Packs this build for another PC: dist\polarity-<version>.zip (Polarity) with
#   Win64\          what goes next to StreetFighterV.exe (StreetFighterV\Binaries\Win64):
#                   the loader, the mod, the noise mask, an ini
#   polarity-client\   what goes into the game's folder for tests (the scripts and
#                   the build's DLL and tools, where the scripts expect them)
# -Server sets [weyve] server in the ini (default: the public server); -Testing also turns on the virtual pads
# and background input the test scripts need (mashing, the agent). -Public (what release-files.ps1 publishes):
# only Win64\ and a player's INSTALL.txt, no test folder.
#   package.ps1 -Server 192.0.2.10 -Testing
param([string]$Server = '2.29.123.240:5555', [switch]$Testing, [switch]$Public)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build\Release'
$version = (Select-String -Path (Join-Path $root 'src\core\version.h') -Pattern 'kPolarityVersion = "([^"]+)"').Matches.Groups[1].Value
$name = "polarity-$version"
$out = Join-Path $root "dist\$name"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$win = New-Item -ItemType Directory -Force (Join-Path $out 'Win64')
$repo = New-Item -ItemType Directory -Force (Join-Path $out 'polarity-client')

Copy-Item (Join-Path $build 'xinput1_3.dll'), (Join-Path $build 'polarity.dll') $win
# The bundled CJK font, and the fonts' license (third_party/fonts/README.md).
$fonts = New-Item -ItemType Directory -Force (Join-Path $win 'Polarity\fonts')
Copy-Item (Join-Path $build 'fonts\NotoSansCJK-Medium.ttc') $fonts
& (Join-Path $PSScriptRoot 'fonts-license.ps1') (Join-Path $win 'polarity_fonts_license.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $win 'polarity_third_party_notices.txt')
$ini = Get-Content (Join-Path $root 'polarity.ini.example')
$serverAddress = if ($Server -match ':') { $Server } else { "${Server}:5555" }
$ini = $ini -replace '^server=.*$', "server=$serverAddress"
if ($Testing) {
  $ini = $ini -replace '^virtual_pads=.*$', 'virtual_pads=1' -replace '^always_active=.*$', 'always_active=1'
}
Set-Content (Join-Path $win 'polarity.ini') $ini

if ($Public) {
  Remove-Item $repo -Recurse -Force
  @"
Polarity ${version}: rollback netplay for Street Fighter V (test build)

Install
1. Copy the files in Win64\ into StreetFighterV\StreetFighterV\Binaries\Win64\
   (the folder with StreetFighterV.exe; in Steam: right-click the game,
   Manage, Browse local files).
2. Start the game from Steam as usual. F1, or Back + Start on a pad, opens
   the Polarity panel: host a match and send the code, or join with one.

Updates: the game offers new versions in the panel (Settings, Updates).
Both players need the same version to play each other.

Problems: in the panel, Settings, Logs, "Pack logs to share", then send the
zip it makes (in Binaries\Win64\Polarity\).

Reports: after a match with a desync, its recording and match log (player
names, picks, inputs) are sent to the Polarity server so it can be fixed; after a
crash, the end of the log and a small crash dump, at the next start. To turn
that off: in the panel, Settings, Logs, untick "Send desync and crash
reports".

Uninstall: delete xinput1_3.dll, polarity.dll, sfv_noise.txt (0.1.0),
polarity_fonts_license.txt, polarity_third_party_notices.txt and polarity.ini
from Binaries\Win64\ (and the Polarity
folder, with the logs and the fonts).
"@ | Set-Content (Join-Path $out 'INSTALL.txt')
  $zip = Join-Path $root "dist\$name.zip"
  if (Test-Path $zip) { Remove-Item $zip -Force }
  Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal
  "$zip ($([int]((Get-Item $zip).Length / 1KB)) KB)"
  exit 0
}

New-Item -ItemType Directory -Force (Join-Path $repo 'build\Release'), (Join-Path $repo 'data') | Out-Null
Copy-Item (Join-Path $root 'scripts') $repo -Recurse
foreach ($f in 'polarity.dll', 'xinput1_3.dll', 'polarity.exe', 'weyve_server.exe') { Copy-Item (Join-Path $build $f) (Join-Path $repo 'build\Release') }
Copy-Item (Join-Path $root 'data\sfv_noise.txt') (Join-Path $repo 'data')

@"
$name

1. Copy the files in Win64\ into StreetFighterV\StreetFighterV\Binaries\Win64\
   (next to StreetFighterV.exe).
2. For tests: copy the polarity-client\ folder into the game's folder
   (StreetFighterV\polarity-client\), so its scripts find the game.
3. Start the game from Steam as usual. F1 opens the panel.

Server: $serverAddress (polarity.ini, [weyve] server).
"@ | Set-Content (Join-Path $out 'INSTALL.txt')

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip
"$zip ($([int]((Get-Item $zip).Length / 1KB)) KB)"
