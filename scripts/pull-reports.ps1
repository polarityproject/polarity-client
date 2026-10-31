# Pulls the desync and crash reports players' games sent to the server
# (sfvr-reports, sfvr-server repository) over SSH, only files not here yet
# (a match's other half often comes later):
#   <out>\desyncs\<match id>\  a match's desync reports, recordings, logs
#   <out>\crashes\<date>\      crash reports (.log, and .dmp after a fault)
# <out> is StreetFighterV\Binaries\Win64\reports unless -Out says otherwise.
# A match that now has both players' desync reports is compared with
# `sfvr peers`, into peers.txt in its folder; each new crash report's first
# lines are shown.
#   pull-reports.ps1 [-Login root@159.69.32.141] [-Identity <key file>] [-Sudo] [-Out <folder>]
# -Login: an SSH login to the server (a key login: ssh asks nothing here; the
# server's host key is taken on first use). -Identity: that key, when it is
# not one ssh finds by itself. -Sudo: the login is not root (the reports
# folders are the sfvr-server user's).
param([string]$Login = 'root@159.69.32.141', [string]$Identity = '', [switch]$Sudo, [string]$Out = '')
$ErrorActionPreference = 'Stop'
$w = Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64'
$base = if ($Out) { $Out } else { Join-Path $w 'reports' }
$sfvr = Join-Path $PSScriptRoot '..\build\Release\sfvr.exe'
$asRoot = if ($Sudo) { 'sudo -n ' } else { '' }
$ssh = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new')
if ($Identity) { $ssh += @('-i', $Identity) }

# The files of the server's `remote` folder not in `local` yet, copied; their
# paths ("<folder>/<file>"). `pattern`: the folder names the server makes.
function Pull([string]$remote, [string]$local, [string]$pattern) {
  New-Item -ItemType Directory -Force -Path $local | Out-Null
  $local = (Resolve-Path $local).Path
  $listing = & ssh @ssh $Login "${asRoot}find $remote -mindepth 2 -maxdepth 2 -type f ! -name '*.part' -printf '%P\t%s\n'"
  if ($LASTEXITCODE -ne 0) { throw "ssh $Login could not list $remote (exit $LASTEXITCODE)" }
  $wanted = @()
  foreach ($line in $listing) {
    $path, $size = $line -split "`t"
    # Only the names the server stores (sfvr-reports.py).
    if ($path -notmatch "^$pattern/[A-Za-z0-9_.-]+$") { continue }
    $file = Join-Path $local ($path -replace '/', '\')
    if ($path.EndsWith('/received.txt') -or -not (Test-Path $file) -or (Get-Item $file).Length -ne [int64]$size) { $wanted += $path }
  }
  $new = @($wanted | Where-Object { -not $_.EndsWith('/received.txt') })
  "${remote}: $(@($listing).Count) files, $($new.Count) new" | Write-Host
  if ($new.Count -eq 0) { return @() }
  # In batches (one ssh command line each), as a tar stream: cmd's pipe
  # keeps the bytes as they are (PowerShell's would not).
  for ($at = 0; $at -lt $wanted.Count; $at += 200) {
    $batch = ($wanted[$at..([Math]::Min($at + 199, $wanted.Count - 1))]) -join ' '
    cmd /c "ssh $(($ssh | ForEach-Object { "`"$_`"" }) -join ' ') $Login `"${asRoot}tar -C $remote -cf - $batch`" | tar -xf - -C `"$local`""
    if ($LASTEXITCODE -ne 0) { throw "copying from $Login failed (exit $LASTEXITCODE)" }
  }
  return $new
}

# Desyncs: matches with both players' halves, the first desync report of each side.
$desyncs = Join-Path $base 'desyncs'
$new = @(Pull '/var/lib/sfvr-server/reports' $desyncs '[0-9a-f]{8}')
foreach ($m in ($new | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique)) {
  $dir = Join-Path $desyncs $m
  $p1 = Get-ChildItem $dir -Filter '*_p1_desync.sfvr' | Sort-Object Name | Select-Object -First 1
  $p2 = Get-ChildItem $dir -Filter '*_p2_desync.sfvr' | Sort-Object Name | Select-Object -First 1
  $spec = Get-ChildItem $dir -Filter '*_spec_desync.sfvr' | Select-Object -First 1
  $note = if ($p1 -and $p2) { 'both halves' } elseif ($p1 -or $p2) { 'one half' } elseif ($spec) { "a spectator's report" } else { 'no desync report' }
  if ($p1 -and $p2 -and (Test-Path $sfvr)) {
    & $sfvr peers $p1.FullName $p2.FullName > (Join-Path $dir 'peers.txt') 2>&1
    $note += ', compared in peers.txt'
  }
  "  $m  $note"
}

# Crashes: what each new report says ended the run.
$crashes = Join-Path $base 'crashes'
$new = @(Pull '/var/lib/sfvr-server/crashes' $crashes '\d{4}-\d{2}-\d{2}')
foreach ($path in ($new | Where-Object { $_.EndsWith('.log') })) {
  $file = Join-Path $crashes ($path -replace '/', '\')
  $head = Get-Content $file -TotalCount 6
  $what = ($head | Where-Object { $_ -match '^(fault|how it ended: no)' } | Select-Object -First 1)
  "  $(Split-Path $path -Leaf)  $($head[0] -replace ' crash report', ''): $what"
}
"in $base"
