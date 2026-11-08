# Removes the site's release folders (sfvr-site\public\releases\<version>)
# that no update info points at. Updaters only ever read the current
# update.json of their channel (public\update.json, public\rooms\update.json),
# so a release nothing names is never fetched again; the package and symbols
# stay in dist\ here. -WhatIf lists them without removing anything.
param([string]$Site = (Join-Path $PSScriptRoot '..\..\sfvr-site'), [switch]$WhatIf)
$ErrorActionPreference = 'Stop'
$public = Join-Path (Resolve-Path $Site) 'public'
$infos = @(Get-ChildItem $public -Recurse -Filter 'update.json')
if (-not $infos) { throw "no update.json under $public" }
# The versions whose folders the update info names (files, more_files, download).
$kept = [Collections.Generic.HashSet[string]]::new()
foreach ($f in $infos) {
  $info = Get-Content $f.FullName -Raw | ConvertFrom-Json
  $urls = @($info.download) + @($info.files | ForEach-Object url) + @($info.more_files | Where-Object { $_ } | ForEach-Object url)
  foreach ($u in $urls) {
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
"$([math]::Round($freed / 1MB, 1)) MB $(if ($WhatIf) { 'would be' } else { 'was' }) freed; then sfvr-site\build.ps1 for the upload"
