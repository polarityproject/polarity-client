# Publishes a release to the project site (the polarity-web repository next to
# this one) for the updater (src/mod/updater.h):
#   release.ps1
#     builds, runs the unit tests, copies the files into
#     polarity-web\public\releases\<version>\ (and the full package for first
#     installs), then writes public\update.json and signs it into
#     public\update.json.sig. The version is kPolarityVersion in
#     src\core\version.h: raise it first, and give it a section in
#     CHANGELOG.md, whose first paragraph becomes the notes players see
#     (-Notes overrides them).
#   release.ps1 -Bridge
#     the same, and also the old site's update info (polarity-web\legacy\,
#     served at sfvr.pages.dev) for SFVR installs: product "SFVR", the font
#     under SFVR/fonts/, absolute URLs on the new site. SFVR's updaters
#     (0.3.1 and older) read only that, so this release reaches them, and
#     from it on they read the new site. Done once, for the first Polarity
#     release; later releases leave it as it is (prune-releases.ps1 keeps
#     what it names).
#   release.ps1 -Channel rooms
#     the same for the rooms branch's builds (docs/custom-rooms-plan.md):
#     writes public\rooms\update.json and its .sig instead, with the files in
#     the shared releases\<version>\. The build's own channel (kUpdateChannel
#     in src\mod\updater.h) must match. Rooms versions carry -rooms.N. Since
#     0.3.0 rooms installs move to main: for a plain version, -Channel rooms
#     mirrors main's update info (re-signed, same files), and every main
#     publish refreshes that mirror, so it never points at a pruned release.
#   prune-releases.ps1
#     removes the release folders no update info points at (updaters only
#     ever read the current update.json).
#   release.ps1 -NewKey
#     makes the update signing key, once: the private half goes to -KeyFile
#     (outside the repositories: keep a backup, never share it), the public
#     half into src\mod\update_key.h. Builds made after that install only
#     updates signed with it.
# Then commit here and in polarity-web, tag this repository v<version>, and
# build the site's upload (polarity-web\build.ps1).
# Needs PowerShell 7 (for the key's PKCS#8 import and export).
# -Site: the polarity-web folder beside this repository. The signing key keeps
# its place under .sfvr\.
param([string]$Notes = '',
      [string]$Site = (Join-Path $PSScriptRoot '..\..\polarity-web'),
      [string]$KeyFile = (Join-Path $env:USERPROFILE '.sfvr\update-signing-key.txt'),
      [ValidateSet('main', 'rooms')][string]$Channel = 'main',
      [string]$SiteUrl = 'https://polaritymod.link',
      [switch]$NewKey, [switch]$Force, [switch]$SkipBuild, [switch]$Bridge)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build\Release'
$keyHeader = Join-Path $root 'src\mod\update_key.h'

function PublicKeyOf($ecdsa) {
  $q = $ecdsa.ExportParameters($false).Q
  return [byte[]]($q.X + $q.Y)
}

if ($NewKey) {
  if ((Test-Path $KeyFile) -and -not $Force) { throw "$KeyFile exists; -Force replaces it (and every build made since stops taking updates)" }
  $ecdsa = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
  New-Item -ItemType Directory -Force (Split-Path $KeyFile) | Out-Null
  [Convert]::ToBase64String($ecdsa.ExportPkcs8PrivateKey()) | Set-Content $KeyFile -NoNewline
  $hex = (PublicKeyOf $ecdsa | ForEach-Object { '0x{0:x2}' -f $_ })
  $rows = for ($i = 0; $i -lt 64; $i += 16) { '    ' + ($hex[$i..($i + 15)] -join ', ') + ',' }
  $text = Get-Content $keyHeader -Raw
  $array = "constexpr uint8_t kUpdatePublicKey[64] = {`n" + ($rows -join "`n") + "`n};"
  $text = [regex]::Replace($text, 'constexpr uint8_t kUpdatePublicKey\[64\] = \{[^;]*\};', $array.Replace('$', '$$'))
  $text = $text.Replace("// private half stays with whoever publishes releases. All zeros: this build`n// installs no updates.",
                        "// private half stays with whoever publishes releases.")
  [IO.File]::WriteAllText($keyHeader, $text)
  "key written to $KeyFile (back it up); public half in src\mod\update_key.h: rebuild"
  exit 0
}

# The key, and that the source has its public half.
if (-not (Test-Path $KeyFile)) { throw "no signing key at $KeyFile (release.ps1 -NewKey makes one)" }
$ecdsa = [System.Security.Cryptography.ECDsa]::Create()
$read = 0
$ecdsa.ImportPkcs8PrivateKey([Convert]::FromBase64String((Get-Content $KeyFile -Raw)), [ref]$read)
$built = [regex]::Matches((Get-Content $keyHeader -Raw), '0x([0-9a-fA-F]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) }
if ((@($built) -join ',') -ne ((PublicKeyOf $ecdsa) -join ',')) { throw "src\mod\update_key.h does not hold $KeyFile's public key" }

$version = (Select-String -Path (Join-Path $root 'src\core\version.h') -Pattern 'kPolarityVersion = "([^"]+)"').Matches.Groups[1].Value
$public = Join-Path (Resolve-Path $Site) 'public'

# The channel: the build's own, and the versions that belong on it.
$isRooms = $version -match '-rooms\.\d+$'
if ($Channel -eq 'main' -and $isRooms) { throw "$version is a rooms build; publish it with -Channel rooms" }
$infoDir = if ($Channel -eq 'rooms') { Join-Path $public 'rooms' } else { $public }
# Rooms' update info sits a folder down; its URLs start at the site's root.
$prefix = if ($Channel -eq 'rooms') { '/' } else { '' }

# Rooms installs move to main: main's update info, re-signed for rooms with
# the same files (which stay byte for byte; nothing built or copied). Its
# URLs start at the site's root, as rooms' update info sits a folder down.
function Write-RoomsMirror {
  $main = Get-Content (Join-Path $public 'update.json') -Raw | ConvertFrom-Json
  $roomsDir = Join-Path $public 'rooms'
  $info = [ordered]@{
    product = $main.product; version = $main.version; published = $main.published; notes = $main.notes
    download = "/$($main.download)"
    files = @($main.files | ForEach-Object { [ordered]@{ path = $_.path; size = $_.size; sha256 = $_.sha256; url = "/$($_.url)" } })
    more_files = @($main.more_files | Where-Object { $_ } | ForEach-Object { [ordered]@{ path = $_.path; size = $_.size; sha256 = $_.sha256; url = "/$($_.url)" } })
  }
  $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($info | ConvertTo-Json -Depth 4) + "`n")
  New-Item -ItemType Directory -Force $roomsDir | Out-Null
  [IO.File]::WriteAllBytes((Join-Path $roomsDir 'update.json'), $bytes)
  [IO.File]::WriteAllText((Join-Path $roomsDir 'update.json.sig'), [Convert]::ToBase64String($ecdsa.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256)))
  "Polarity $($main.version) offered on the rooms channel too ($roomsDir): rooms installs move to main with it"
}
if ($Channel -eq 'rooms' -and -not $isRooms) {
  $main = Get-Content (Join-Path $public 'update.json') -Raw | ConvertFrom-Json
  if ($main.version -ne $version) { throw "publish $version on main first (public\update.json has $($main.version))" }
  Write-RoomsMirror
  exit 0
}
$followed = (Select-String -Path (Join-Path $root 'src\mod\updater.h') -Pattern 'kUpdateChannel = "([^"]+)"').Matches.Groups[1].Value
if ($followed -ne $Channel) { throw "this build follows the $followed channel (kUpdateChannel in src\mod\updater.h), not $Channel" }
$release = Join-Path $public "releases\$version"
if ((Test-Path $release) -and -not $Force) { throw "release $version is already on the site ($release); raise kPolarityVersion" }

# The version's changelog entry; its first paragraph is the players' notes.
$changelog = Get-Content (Join-Path $root 'CHANGELOG.md') -Raw
$entry = [regex]::Match($changelog, "(?ms)^## \[$([regex]::Escape($version))\][^\n]*\n(.*?)(?=^## \[|\z)")
if (-not $entry.Success) { throw "CHANGELOG.md has no section for $version" }
if (-not $Notes) {
  $Notes = (($entry.Groups[1].Value.Trim() -split "\r?\n\s*\r?\n")[0] -replace '\s*\r?\n\s*', ' ').Trim()
  if ($Notes.StartsWith('#')) { throw "the $version section in CHANGELOG.md starts without a summary paragraph" }
}

if (-not $SkipBuild) {
  $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
  cmd /c "`"$vcvars`" >nul 2>nul && cmake --build `"$build`""
  if ($LASTEXITCODE) { throw 'the build failed' }
  & (Join-Path $build 'sfvtests.exe') | Select-Object -Last 1
  if ($LASTEXITCODE) { throw 'the unit tests failed' }
}

# The files the updater installs, and the package for first installs.
New-Item -ItemType Directory -Force $release | Out-Null
$license = Join-Path $build 'polarity_fonts_license.txt'
& (Join-Path $PSScriptRoot 'fonts-license.ps1') $license
# Where each file goes (SafeUpdatePath: a plain name, or Polarity/fonts/<name>.ttc),
# from where; on the site each is under its own name. The font goes in
# "more_files": updaters before 0.3.0 refuse update info whose "files" has a
# folder in a path, and read nothing else (0.3.0 fetches a missing one).
$sources = [ordered]@{
  'xinput1_3.dll'                          = Join-Path $build 'xinput1_3.dll'
  'polarity.dll'                           = Join-Path $build 'polarity.dll'
  'Polarity/fonts/NotoSansCJK-Medium.ttc'  = Join-Path $build 'fonts\NotoSansCJK-Medium.ttc'
  'polarity_fonts_license.txt'             = $license
}
$files = [Collections.Generic.List[object]]::new()
$moreFiles = [Collections.Generic.List[object]]::new()
foreach ($path in $sources.Keys) {
  $name = Split-Path $path -Leaf
  Copy-Item $sources[$path] (Join-Path $release $name) -Force
  $item = Get-Item (Join-Path $release $name)
  $entry = [ordered]@{ path = $path; size = $item.Length; sha256 = (Get-FileHash $item -Algorithm SHA256).Hash.ToLower(); url = "${prefix}releases/$version/$name" }
  if ($path.Contains('/')) { $moreFiles.Add($entry) } else { $files.Add($entry) }
}
& (Join-Path $PSScriptRoot 'package.ps1') -Public | Out-Null
Copy-Item (Join-Path $root "dist\polarity-$version.zip") $release -Force
# The symbols stay here, for reading crash addresses (scripts/symbolize.py).
$symbols = New-Item -ItemType Directory -Force (Join-Path $root "dist\symbols\$version")
Copy-Item (Join-Path $build 'polarity.pdb') $symbols -Force

function Write-Info($info, [string]$dir) {
  $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($info | ConvertTo-Json -Depth 4) + "`n")
  $signature = $ecdsa.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256)
  New-Item -ItemType Directory -Force $dir | Out-Null
  [IO.File]::WriteAllBytes((Join-Path $dir 'update.json'), $bytes)
  [IO.File]::WriteAllText((Join-Path $dir 'update.json.sig'), [Convert]::ToBase64String($signature))
}
$info = [ordered]@{
  product   = 'Polarity'
  version   = $version
  published = (Get-Date -Format 'yyyy-MM-dd')
  notes     = $Notes
  download  = "${prefix}releases/$version/polarity-$version.zip"
  files     = @($files)
  more_files = @($moreFiles)
}
Write-Info $info $infoDir
"Polarity $version published to $infoDir on the $Channel channel ($($files.Count) files + $($moreFiles.Count) more, signed)"
if ($Channel -eq 'main' -and (Test-Path (Join-Path $public 'rooms\update.json'))) { Write-RoomsMirror }

# The old site's update info (the bridge): what SFVR's updaters accept.
if ($Bridge) {
  if ($Channel -ne 'main') { throw '-Bridge goes with the main channel' }
  $legacy = Join-Path (Resolve-Path $Site) 'legacy'
  if ((Test-Path (Join-Path $legacy 'update.json')) -and -not $Force) { throw "the old site's update info is there already ($legacy); -Force replaces it" }
  $absolute = { param($e) [ordered]@{ path = ($e.path -replace '^Polarity/fonts/', 'SFVR/fonts/'); size = $e.size; sha256 = $e.sha256; url = "$SiteUrl/$($e.url.TrimStart('/'))" } }
  $old = [ordered]@{
    product    = 'SFVR'
    version    = $version
    published  = $info.published
    notes      = $Notes
    download   = "$SiteUrl/releases/$version/polarity-$version.zip"
    files      = @($files | ForEach-Object { & $absolute $_ })
    more_files = @($moreFiles | ForEach-Object { & $absolute $_ })
  }
  Write-Info $old $legacy
  # SFVR rooms installs (before 0.3.0 moved them to main) read rooms\update.json.
  Write-Info $old (Join-Path $legacy 'rooms')
  "the old site's update info: $legacy (product SFVR, files on $SiteUrl); it stays at $version from now on"
}
"notes: $Notes"
"next: commit here and in polarity-web, git tag v$version, then polarity-web\build.ps1 for the upload"
