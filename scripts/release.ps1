# Publishes a release that the build workflow (.github/workflows/build.yml)
# put in a draft GitHub Release, with the signed update info the updater
# (src/mod/updater.h) and the installers read. The site redirects
# /update.json and /update.json.sig to the latest release's copies, so
# publishing is what offers the update:
#   release.ps1
#     for v<kPolarityVersion>: downloads the draft's files into
#     dist\release\<version>\, checks that GitHub Actions built each one (its
#     attestation), keeps polarity.pdb in dist\symbols\<version>\, writes
#     update.json with the files' GitHub addresses, signs it into
#     update.json.sig, attaches both to the release and publishes it as the
#     latest. The notes players see are the first paragraph of the release's
#     description (the version's CHANGELOG.md section; edit the draft to
#     change them). -Force signs a published release's update info again.
#   release.ps1 -NewKey
#     makes the update signing key, once: the private half goes to -KeyFile
#     (outside the repositories: keep a backup, never share it), the public
#     half into src\mod\update_key.h. Builds made after that install only
#     updates signed with it.
# Needs PowerShell 7 (for the key's PKCS#8 import and export) and gh signed in
# with an account that can publish releases in -Repo.
param([string]$KeyFile = (Join-Path $env:USERPROFILE '.sfvr\update-signing-key.txt'),
      [string]$Repo = 'polarityproject/polarity-client',
      [switch]$NewKey, [switch]$Force)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
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
$tag = "v$version"

# The release the workflow made for the tag.
function Gh { & gh @args; if ($LASTEXITCODE) { throw "gh $($args -join ' ') failed" } }
$release = Gh release view $tag --repo $Repo --json isDraft,body,assets,url | ConvertFrom-Json
if (-not $release.isDraft -and -not $Force) { throw "$($release.url) is published already; -Force signs its update info again" }
$notes = (($release.body.Trim() -split "\r?\n\s*\r?\n")[0] -replace '\s*\r?\n\s*', ' ').Trim()
if (-not $notes -or $notes.StartsWith('#')) { throw "the description of $($release.url) does not start with the summary paragraph" }

# Its files, each checked to come from this repository's build workflow. The
# update info is signed here instead, so a -Force run leaves it out.
$built = @($release.assets | Where-Object { $_.name -notlike 'update.json*' })
$dir = Join-Path $root "dist\release\$version"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force $dir | Out-Null
foreach ($asset in $built) {
  Gh release download $tag --repo $Repo --dir $dir --pattern $asset.name
  Gh attestation verify (Join-Path $dir $asset.name) --repo $Repo --signer-workflow "$Repo/.github/workflows/build.yml" | Out-Null
}
"$($built.Count) files checked: built by $Repo's build workflow"
$symbols = New-Item -ItemType Directory -Force (Join-Path $root "dist\symbols\$version")
Copy-Item (Join-Path $dir 'polarity.pdb') $symbols -Force

# The update info, from where each installed file goes. A path with a folder
# goes in "more_files": updaters before 0.3.0 refuse update info whose
# "files" has one, and read nothing else (0.3.0 fetches a missing one).
$address = "https://github.com/$Repo/releases/download/$tag"
$files = [Collections.Generic.List[object]]::new()
$moreFiles = [Collections.Generic.List[object]]::new()
foreach ($f in (Get-Content (Join-Path $dir 'update-files.json') -Raw | ConvertFrom-Json)) {
  $item = Get-Item (Join-Path $dir $f.asset)
  $entry = [ordered]@{ path = $f.path; size = $item.Length; sha256 = (Get-FileHash $item -Algorithm SHA256).Hash.ToLower(); url = "$address/$($f.asset)" }
  if ($f.path.Contains('/')) { $moreFiles.Add($entry) } else { $files.Add($entry) }
}
$info = [ordered]@{
  product    = 'Polarity'
  version    = $version
  published  = (Get-Date -Format 'yyyy-MM-dd')
  notes      = $notes
  download   = "$address/polarity-$version.zip"
  files      = @($files)
  more_files = @($moreFiles)
}

$bytes = [Text.UTF8Encoding]::new($false).GetBytes(($info | ConvertTo-Json -Depth 4) + "`n")
$infoFile = Join-Path $dir 'update.json'
[IO.File]::WriteAllBytes($infoFile, $bytes)
[IO.File]::WriteAllText("$infoFile.sig", [Convert]::ToBase64String($ecdsa.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256)))
"update info signed ($($files.Count) files + $($moreFiles.Count) more), notes: $notes"

# Both go on the release before it is published, so the latest release always
# carries its own update info.
Gh release upload $tag --repo $Repo --clobber $infoFile "$infoFile.sig"
if ($release.isDraft) { Gh release edit $tag --repo $Repo --draft=false --latest | Out-Null }
"Polarity $version is offered: $($release.url)"
