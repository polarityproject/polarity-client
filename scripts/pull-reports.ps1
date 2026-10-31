# Pulls the desync reports players' games sent to the server (sfvr-reports,
# sfvr-server repository) over SSH: each match's folder into
# StreetFighterV\Binaries\Win64\reports\server\<match id>, only files not
# here yet (a match's other half often comes later). A match that now has
# both players' desync reports is compared with `sfvr peers`, into peers.txt
# in its folder.
#   pull-reports.ps1 [-Login root@159.69.32.141] [-Identity <key file>] [-Sudo] [-Out <folder>]
# -Login: an SSH login to the server (a key login: ssh asks nothing here; the
# server's host key is taken on first use). -Identity: that key, when it is
# not one ssh finds by itself. -Sudo: the login is not root (the reports
# folder is the sfvr-server user's).
param([string]$Login = 'root@159.69.32.141', [string]$Identity = '', [switch]$Sudo, [string]$Out = '')
$ErrorActionPreference = 'Stop'
$remote = '/var/lib/sfvr-server/reports'
$w = Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64'
$outDir = if ($Out) { $Out } else { Join-Path $w 'reports\server' }
$sfvr = Join-Path $PSScriptRoot '..\build\Release\sfvr.exe'
$asRoot = if ($Sudo) { 'sudo -n ' } else { '' }
$ssh = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new')
if ($Identity) { $ssh += @('-i', $Identity) }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outDir = (Resolve-Path $outDir).Path

# What the server has: "<match>/<file>\t<size>".
$listing = & ssh @ssh $Login "${asRoot}find $remote -mindepth 2 -maxdepth 2 -type f ! -name '*.part' -printf '%P\t%s\n'"
if ($LASTEXITCODE -ne 0) { throw "ssh $Login could not list $remote (exit $LASTEXITCODE)" }
$wanted = @()
foreach ($line in $listing) {
  $path, $size = $line -split "`t"
  # Only the names the server stores (sfvr-reports.py): <8 hex>/<report name>.
  if ($path -notmatch '^[0-9a-f]{8}/[A-Za-z0-9_.-]+$') { continue }
  $local = Join-Path $outDir ($path -replace '/', '\')
  $have = Test-Path $local
  if ($path.EndsWith('/received.txt') -or -not $have -or (Get-Item $local).Length -ne [int64]$size) { $wanted += $path }
}
$new = @($wanted | Where-Object { -not $_.EndsWith('/received.txt') })
"$(@($listing).Count) files on the server, $($new.Count) new"
if ($new.Count -eq 0) { return }

# In batches (one ssh command line each), as a tar stream: cmd's pipe keeps
# the bytes as they are (PowerShell's would not).
for ($at = 0; $at -lt $wanted.Count; $at += 200) {
  $batch = ($wanted[$at..([Math]::Min($at + 199, $wanted.Count - 1))]) -join ' '
  cmd /c "ssh $(($ssh | ForEach-Object { "`"$_`"" }) -join ' ') $Login `"${asRoot}tar -C $remote -cf - $batch`" | tar -xf - -C `"$outDir`""
  if ($LASTEXITCODE -ne 0) { throw "copying from $Login failed (exit $LASTEXITCODE)" }
}

# Matches with both players' halves: the first desync report of each side.
$ids = $new | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique
foreach ($m in $ids) {
  $dir = Join-Path $outDir $m
  $p1 = Get-ChildItem $dir -Filter '*_p1_desync.sfvr' | Sort-Object Name | Select-Object -First 1
  $p2 = Get-ChildItem $dir -Filter '*_p2_desync.sfvr' | Sort-Object Name | Select-Object -First 1
  $spec = Get-ChildItem $dir -Filter '*_spec_desync.sfvr' | Select-Object -First 1
  $note = if ($p1 -and $p2) { 'both halves' } elseif ($p1 -or $p2) { 'one half' } elseif ($spec) { "a spectator's report" } else { 'no desync report' }
  if ($p1 -and $p2 -and (Test-Path $sfvr)) {
    & $sfvr peers $p1.FullName $p2.FullName > (Join-Path $dir 'peers.txt') 2>&1
    $note += ', compared in peers.txt'
  }
  "$m  $note"
}
"in $outDir"
