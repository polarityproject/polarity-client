# Removes the site's release folders (polarity-web\public\releases\<version>)
# that no update info points at. Updaters only ever read the current
# update.json of their channel (public\update.json, public\rooms\update.json),
# so a release nothing names is never fetched again. Releases from GitHub
# (release.ps1) keep no folder on the site; only addresses on -SiteUrl count.
# -WhatIf lists the folders without removing anything.
# -Site: the polarity-web folder beside this repository.
param([string]$Site = (Join-Path $PSScriptRoot '..\..\polarity-web'),
      [string]$SiteUrl = 'https://polaritymod.link', [switch]$WhatIf)
$ErrorActionPreference = 'Stop'
$public = Join-Path (Resolve-Path $Site) 'public'
# legacy\: the old site's update info (written at 0.4.0), frozen at the
# first Polarity release: what it names is never removed.
$legacy = Join-Path (Resolve-Path $Site) 'legacy'
$infos = @(Get-ChildItem $public -Recurse -Filter 'update.json') + @(Get-ChildItem $legacy -Recurse -Filter 'update.json' -ErrorAction SilentlyContinue)
if (-not $infos) { throw "no update.json under $public" }
# The versions whose folders the update info names (files, more_files, download).
$kept = [Collections.Generic.HashSet[string]]::new()
foreach ($f in $infos) {
  $info = Get-Content $f.FullName -Raw | ConvertFrom-Json
  $urls = @($info.download) + @($info.files | ForEach-Object url) + @($info.more_files | Where-Object { $_ } | ForEach-Object url)
  foreach ($u in $urls) {
    if ($u.Contains('://') -and -not $u.StartsWith("$SiteUrl/")) { continue }
    $m = [regex]::Match($u, '(?:^|/)releases/([^/]+)/')
    if (-not $m.Success) { throw "$($f.FullName): '$u' is not under releases/" }
    [void]$kept.Add($m.Groups[1].Value)
  }
  "$($f.FullName.Substring($public.Length + 1)): $($info.version)"
}
foreach ($v in $kept) {
  if (-not (Test-Path (Join-Path $public "releases\$v"))) { throw "update info names releases/$v, which is missing" }
}
$freed = 0
foreach ($d in Get-ChildItem (Join-Path $public 'releases') -Directory) {
  if ($kept.Contains($d.Name)) { "keep   $($d.Name)"; continue }
  $size = (Get-ChildItem $d.FullName -Recurse -File | Measure-Object Length -Sum).Sum
  $freed += $size
  if ($WhatIf) { "would remove $($d.Name) ($([math]::Round($size / 1MB, 1)) MB)" }
  else { Remove-Item $d.FullName -Recurse -Force; "remove $($d.Name) ($([math]::Round($size / 1MB, 1)) MB)" }
}
"$([math]::Round($freed / 1MB, 1)) MB $(if ($WhatIf) { 'would be' } else { 'was' }) freed; then polarity-web\build.ps1 for the upload"
