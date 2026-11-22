# The replay regression check: the reference recordings for this build's sim
# version (tests/replays/sim-<n>, src/mod/sim_version.h) replayed straight on
# the second PC's game, each to PASS or FAIL against its own checksums; one
# or more also under stress 8 (a load and 8 resimulated frames every frame).
#
# A FAIL is a regression, or a change to what battles compute that needs the
# sim version bumped (and a new corpus: tests/replays/README.md).
#
# First stage: the game reaches the menu; each replay's battle is ready within
# 120 s. Mid-run: no fatal error, freeze or stress session failure; a replay
# with no result after -MaxSeconds is stopped and counted as failed.
#
#   .\scripts\replay-regression.ps1 -Agent <second PC> -Token <agent token> [-Quick] [-Stress 1] [-NoDeploy]
param([Parameter(Mandatory)][string]$Token, [Parameter(Mandatory)][string]$Agent, [switch]$Quick, [int]$Stress = 1, [switch]$StressAll, [string]$AllMode = '',
      [switch]$NoDeploy, [int]$MaxSeconds = 600, [string]$Out = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'remote.ps1') -Agent $Agent -Token $Token

$header = Get-Content (Join-Path $repo 'src\mod\sim_version.h') -Raw
if ($header -notmatch 'kSimVersion = (\d+);') { throw 'no kSimVersion in src/mod/sim_version.h' }
$sim = [int]$Matches[1]
$corpus = Join-Path $repo "tests\replays\sim-$sim"
if (-not (Test-Path $corpus)) { throw "no reference recordings for sim $sim ($corpus): tests/replays/README.md says how to make them" }
$files = @(Get-ChildItem $corpus -Filter *.sfvr | Sort-Object Name)
if ($Quick) {
  $list = Join-Path $corpus 'quick.txt'
  $names = @(Get-Content $list | Where-Object { $_ -and -not $_.StartsWith('#') })
  $files = @($files | Where-Object { $names -contains $_.Name })
}
if (-not $files.Count) { throw "no recordings in $corpus" }
$stressFiles = @(Get-Content (Join-Path $corpus 'stress.txt') -ErrorAction SilentlyContinue | Where-Object { $_ -and -not $_.StartsWith('#') }) | Select-Object -First $Stress
$jobs = @(foreach ($f in $files) { , @($f.Name, '') }) + @(foreach ($n in $stressFiles) { , @($n, 'stress 8') })
# -StressAll: every recording under stress 8 only (research: rollbacks through every round stage).
if ($StressAll) { $stressFiles = @($files | ForEach-Object { $_.Name }); $jobs = @(foreach ($n in $stressFiles) { , @($n, 'stress 8') }) }
# -AllMode '<mode>': every recording in that mode only ('mispredict 7 remote 2': rollbacks on mispredicted inputs).
if ($AllMode) { $jobs = @(foreach ($f in $files) { , @($f.Name, $AllMode) }) }

if (-not $Out) { $Out = Join-Path $repo ("build\regression\" + (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss')) }
New-Item -ItemType Directory -Force $Out | Out-Null
function Say($t) { $t; Add-Content -Path (Join-Path $Out 'summary.txt') -Value $t -Encoding utf8 }
$log = [Collections.Generic.List[string]]::new()
function Lines { foreach ($l in @(RemoteLog 0)) { if ($null -ne $l) { $log.Add([string]$l) } }; return $log }
function Since([int]$from) { $all = @(Lines); if ($from -ge $all.Count) { return @() }; return $all[$from..($all.Count - 1)] }

$saved = @{}
$failed = 0
try {
  Say "== replay regression, sim $sim ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')): $($files.Count) recordings straight, $($stressFiles.Count) under stress 8"
  try { Remote 'stop' | Out-Null } catch {}
  foreach ($f in 'polarity.ini', 'polarity.panel.ini') { try { $saved[$f] = RemoteGet $f (Join-Path $Out "before-$f") } catch {} }
  foreach ($kv in @(@('control', 'virtual_pads', '1'), @('control', 'always_active', '1'))) {
    Remote 'ini' @{ file = 'polarity.ini'; section = $kv[0]; key = $kv[1]; value = $kv[2] } | Out-Null
  }
  Remote 'ini' @{ file = 'polarity.panel.ini'; section = 'report'; key = 'upload_reports'; value = '0' } | Out-Null
  if (-not $NoDeploy) { RemoteDeploy; Say '  this build deployed' }
  # In a folder of their own: the matches folder keeps only its newest recordings.
  foreach ($f in $files) { RemotePut $f.FullName "Polarity\matches\regression\$($f.Name)" | Out-Null }
  Say "  $(RemoteStart 1 | Select-Object -Last 1 | Out-String)".Trim()
  foreach ($job in $jobs) {
    $name = $job[0]; $mode = $job[1]
    $label = $name + $(if ($mode) { " [$mode]" } else { '' })
    $at = @(Lines).Count
    $t0 = Get-Date
    RemoteCmd 0 "replay regression\$name $mode" -Wait | Out-Null
    $started = $false; $result = $null
    while (((Get-Date) - $t0).TotalSeconds -lt $MaxSeconds) {
      Start-Sleep 3
      $new = @(Since $at)
      if (-not $started -and ($new -match 'replay: battle objects ready')) { $started = $true }
      $fatal = @($new | Where-Object { $_ -match 'Fatal error|has not ticked for|cannot start the stress|the recording has no state' })
      if ($fatal) { $fatal | Select-Object -First 3 | ForEach-Object { Say "   $_" }; throw "$label : fatal error, freeze or no session" }
      if (-not $started -and ((Get-Date) - $t0).TotalSeconds -gt 120) { throw "$label : the battle was not ready within 120 s (first stage)" }
      $result = @($new | Where-Object { $_ -match 'replay: .*\.sfvr (PASS|FAIL)' }) | Select-Object -Last 1
      if ($result) { break }
    }
    $new = @(Since $at)
    $new | Set-Content (Join-Path $Out (($label -replace '[^A-Za-z0-9_.-]', '_') + '.log')) -Encoding utf8
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    if (-not $result) {
      $failed++
      Say "FAIL  $label : no result within $MaxSeconds s"
      RemoteCmd 0 'replay stop' | Out-Null
      Start-Sleep 5
      continue
    }
    $ok = $result -match 'PASS'
    if (-not $ok) { $failed++ }
    $detail = $result.Substring($result.IndexOf('.sfvr') + 6)
    Say ("{0}  {1} ({2} s): {3}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $label, $secs, $detail)
    if (-not $ok) {
      @($new | Where-Object { $_ -match 'differs|resimulated differently|regions:' }) | Select-Object -First 4 | ForEach-Object { Say "      $($_.Substring([Math]::Min(21, $_.Length)))" }
    }
  }
  Say ''
  if ($failed) {
    Say "$failed of $($jobs.Count) failed: a regression, or a change to what battles compute that needs the sim version bumped (src/mod/sim_version.h)"
  }
  else {
    Say "all $($jobs.Count) passed"
  }
}
catch {
  $failed++
  Say "STOPPED: $_"
}
finally {
  try { RemoteCmd 0 'replay stop' | Out-Null } catch {}
  Start-Sleep 2
  try { Remote 'stop' | Out-Null } catch {}
  foreach ($f in $saved.Keys) { try { RemotePut $saved[$f] $f | Out-Null } catch { Say "  (could not put back $f)" } }
  Say "  the second PC's ini files put back; results in $Out"
}
exit $(if ($failed) { 1 } else { 0 })
