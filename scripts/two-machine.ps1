# A netplay session between two machines. By default this PC runs game
# instance 0 (the host) and a second PC running scripts\agent.ps1 (or
# agent_steamos.py) joins with its instance 0. With -HostAgent another agent
# hosts instead, so two remote machines play while this PC only runs the
# Weyvelength server and this script. -Spectate N: the host machine's
# instance N watches. All players mash; after each decided match both make
# the next choice from -Choices; then everyone leaves. The agents' recordings
# are fetched and each match is compared with `polarity peers`.
#   two-machine.ps1 -Agent 192.168.1.20 -Token <secret> -Server 192.168.1.10 [-Deploy] [-Choices rematch,select]
#   two-machine.ps1 -HostAgent 192.168.1.20 -Agent 192.168.1.30 -Token <secret> -Server 192.168.1.10 -Deploy -Spectate 1
# -Server: this PC's address as the agents reach it (the server's port 5555
# must be allowed in this PC's firewall). -Deploy first copies this build's
# mod DLL and scripts to the agents and restarts their games. On the agents
# the run sets the test settings (virtual pads, always active, a second
# instance when needed, report uploads off) and puts their ini files back
# afterwards. -Delay: both players' input delay (default 0: the most
# rollbacks). -Choices with no entries ('-Choices @()') plays one match.
# -Commands: mod commands every game runs before the session ('skipcallbacks ...').
param([Parameter(Mandatory)][string]$Agent, [Parameter(Mandatory)][string]$Token, [Parameter(Mandatory)][string]$Server,
      [string]$HostAgent = '', [int]$Spectate = -1,
      [string[]]$Choices = @('rematch', 'select'), [string[]]$Picks = @('RYU', 'KEN'), [string]$Stage = 'CHN',
      [switch]$Deploy, [switch]$NoMash, [int]$EndHoldSeconds = 0, [int]$TimeoutSeconds = 1200, [string]$Out = '', [int]$Delay = 0,
      [string]$Commands = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'remote.ps1') -Agent $Agent -Token $Token
. (Join-Path $PSScriptRoot 'lib.ps1')
. (Join-Path $PSScriptRoot 'logtail.ps1')
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$polarity = Join-Path $PSScriptRoot '..\build\Release\polarity.exe'
$outDir = if ($Out) { Join-Path $Out '.' } else { Join-Path $w ("reports\two-machine-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
function Say($text) { $text; Add-Content -Path (Join-Path $outDir 'summary.txt') -Value $text }

# A side of the session: a game instance on this PC (no agent) or on an agent.
function New-Side([string]$Name, [string]$AgentAddress, [int]$Instance) {
  @{ Name = $Name; Agent = $AgentAddress; Instance = $Instance; Lines = [Collections.ArrayList]::new(); Tail = $null }
}
function Send-SideCmd($side, [string]$commands, [switch]$Wait) {
  if ($side.Agent) { Use-Agent $side.Agent; RemoteCmd $side.Instance $commands -Wait:$Wait }
  else { Send-Cmd $side.Instance $commands -Wait:$Wait }
}
# The side's log lines since the last call; every line read stays in $side.Lines.
function Read-Side($side) {
  if ($side.Agent) { Use-Agent $side.Agent; $new = @(RemoteLog $side.Instance) }
  else {
    Update-LogTail $side.Tail
    $new = @($side.Tail.Lines | Select-Object -Skip $side.Lines.Count)
  }
  foreach ($l in $new) { [void]$side.Lines.Add($l) }
  $new
}
function Skip-SideLog($side) {
  $side.Lines.Clear()
  if ($side.Agent) { Use-Agent $side.Agent; RemoteLog $side.Instance -FromNow }
  else { $side.Tail = New-LogTail (LogOf $side.Instance) }
}
function Wait-Side($side, [string]$pattern, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    foreach ($l in Read-Side $side) { if ($l -match $pattern) { return $Matches } }
    Start-Sleep -Milliseconds 500
  }
  throw "$($side.Name): nothing matched '$pattern' within $seconds s"
}

$serverAddress = if ($Server -match ':') { $Server } else { "${Server}:5555" }
$hostSide = New-Side 'host' $HostAgent 0
$joinSide = New-Side 'joiner' $Agent 0
$sides = @($hostSide, $joinSide)
if ($Spectate -ge 0) { $sides += New-Side 'spectator' $HostAgent $Spectate }
$hostName = if ($HostAgent) { $HostAgent } else { 'this PC' }
Say "two-machine session: $hostName hosts, $Agent joins$(if ($Spectate -ge 0) { ", $hostName instance $Spectate watches" }), server $serverAddress"

& (Join-Path $PSScriptRoot 'weyve-server.ps1') | ForEach-Object { Say "server: $_" }

# Each agent: its ini files kept, the test settings set, this build if
# -Deploy, and its games (re)started with the settings in effect.
$agents = @($sides | Where-Object { $_.Agent } | ForEach-Object { $_.Agent } | Select-Object -Unique)
$iniBackups = @{}
$iniFiles = @('polarity.ini', 'polarity.panel.ini', 'polarity.panel.1.ini', 'polarity.panel.2.ini')
foreach ($a in $agents) {
  Use-Agent $a
  $ping = Remote 'ping'
  Say "agent ${a}: $($ping.host), $($ping.games) game instance(s) running"
  $dir = Join-Path $outDir ("ini-" + $a.Replace(':', '_'))
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $iniBackups[$a] = @()
  foreach ($f in $iniFiles) {
    try { RemoteGet $f (Join-Path $dir $f) | Out-Null; $iniBackups[$a] += $f } catch { } # absent: nothing to keep
  }
  $count = 1 + @($sides | Where-Object { $_.Agent -eq $a -and $_.Instance -gt 0 }).Count
  $settings = @(@('control', 'virtual_pads', '1'), @('control', 'always_active', '1'), @('netplay', 'upload_reports', '0'),
                @('weyve', 'server', $serverAddress))
  if ($count -gt 1) { $settings += , @('mod', 'multi_instance', '1') }
  foreach ($s in $settings) { (Remote 'ini' @{ file = 'polarity.ini'; section = $s[0]; key = $s[1]; value = $s[2] }).output | Out-Null }
  # The panel files override the ini's upload setting.
  foreach ($f in $iniBackups[$a] | Where-Object { $_ -like 'polarity.panel*' }) {
    (Remote 'ini' @{ file = $f; section = 'netplay'; key = 'upload_reports'; value = '0' }).output | Out-Null
  }
  if ($Deploy) { RemoteDeploy; Say "deployed this build and the test scripts to $a" }
  if ($ping.games) { Remote 'stop' | Out-Null; Start-Sleep 3 }
  Say "starting $count game instance(s) on $a"
  Say (RemoteStart $count)
}
if ($agents.Count) { Start-Sleep 15 } # the menu ignores input for a while after its map loads
$localInstances = @($sides | Where-Object { -not $_.Agent } | ForEach-Object { $_.Instance })
if ($localInstances.Count) { Assert-GamesRunning (1 + ($localInstances | Measure-Object -Maximum).Maximum) }

try {
  foreach ($s in $sides) {
    Skip-SideLog $s
    # Nothing left over from an earlier run.
    Send-SideCmd $s 'leave; mash 0; netsim 0 0 0' -Wait
  }
  $pre = if ($Commands) { "$Commands; " } else { '' }
  if ($Commands) { Say "every game first: $Commands" }
  $hostServer = if ($HostAgent) { $serverAddress } else { "127.0.0.1:$($serverAddress.Split(':')[1])" }
  $mash = if ($NoMash) { '' } else { 'mash 11; ' }
  Send-SideCmd $hostSide "${pre}server $hostServer; delay $Delay; ${mash}pick $($Picks[0]) 0 1 0 1 1; stage $Stage; host"
  $m = Wait-Side $hostSide 'hosting, code (\S+)|session over: (.*)' 30
  if (-not $m[1]) { throw "host: hosting failed: $($m[2])" }
  $code = $m[1]
  Say "code $code"
  $mash = if ($NoMash) { '' } else { 'mash 22; ' }
  Send-SideCmd $joinSide "${pre}server $serverAddress; name Player 2; delay $Delay; ${mash}pick $($Picks[1]) 0 1 0 1 1; join $code"
  foreach ($s in $sides | Where-Object { $_.Name -eq 'spectator' }) {
    Send-SideCmd $s "${pre}server $(if ($s.Agent) { $serverAddress } else { $hostServer }); name Watcher; spectate $code"
  }

  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $decided = 0
  while ((Get-Date) -lt $deadline) {
    Start-Sleep 1
    foreach ($s in $sides) { Read-Side $s | Out-Null }
    if ($hostSide.Lines -match 'session over' -or $joinSide.Lines -match 'session over') { Say 'the session ended early'; break }
    $now = @($hostSide.Lines | Where-Object { $_ -match 'the match is decided' }).Count
    if ($now -le $decided) { continue }
    $decided = $now
    Say "match $decided decided"
    Start-Sleep 2
    if ($decided -gt $Choices.Count) {
      # -EndHoldSeconds: stay on the last match's post-match screen a while
      # (to look at it) before everyone leaves.
      if ($EndHoldSeconds -gt 0) { Say "holding the post-match screen for $EndHoldSeconds s"; Start-Sleep $EndHoldSeconds }
      break
    }
    $c = "choose $($Choices[$decided - 1])"
    Send-SideCmd $hostSide $c
    Send-SideCmd $joinSide $c
  }
  Start-Sleep 2
}
finally {
  # Mashing and simulated network conditions stay set in the game until a
  # reload: a session played by hand afterwards would get them. Off again.
  foreach ($s in $sides) { try { Send-SideCmd $s 'leave; mash 0; netsim 0 0 0' -Wait } catch { Say "$($s.Name): $_" } }
  Start-Sleep 4
  foreach ($s in $sides) { try { Read-Side $s | Out-Null } catch { } }
  # The agents' ini files as they were (games already running keep the test
  # settings until they restart).
  foreach ($a in $agents) {
    Use-Agent $a
    $dir = Join-Path $outDir ("ini-" + $a.Replace(':', '_'))
    foreach ($f in $iniBackups[$a]) { RemotePut (Join-Path $dir $f) $f | Out-Null }
    Say "agent ${a}: ini files put back ($($iniBackups[$a] -join ', '))"
  }
}

$pattern = 'netplay: (match [0-9a-f]{8}:|session started as|the match is decided|both chose|back to the lobby|session over|desync detected|frozen state checksums|frozen states differ|took the host|link|peer connected|watched)|frame 0 (word|region)|desync detected|flow: results screen held'
foreach ($s in $sides) {
  $s.Lines | Set-Content (Join-Path $outDir "$($s.Name).log")
  Say "== $($s.Name) ($(if ($s.Agent) { "$($s.Agent) instance $($s.Instance)" } else { "this PC instance $($s.Instance)" }))"
  $s.Lines | Where-Object { $_ -match $pattern } | ForEach-Object { Say $_.Substring(0, [Math]::Min(170, $_.Length)) }
}

# Each match's two players' recordings, side by side. Keyed by the match id
# in the file name: the side that leaves logs its last match as "left at
# frame N; recording ...". Paths may hold spaces (Program Files).
# (The names: <date>_<time>_<P1>_vs_<P2>_<match id>_<p1|p2>[_desync].sfvr.)
$named = 'recording (.+_([0-9a-f]{8})_p[12](?:_desync)?\.sfvr)'
$recordings = @{}
foreach ($s in @($hostSide, $joinSide)) {
  foreach ($l in $s.Lines) {
    if ($l -notmatch $named) { continue }
    $id = $Matches[2]
    $path = $Matches[1]
    if ($s.Agent) {
      Use-Agent $s.Agent
      $rel = $path -replace '^.*[\\/]Binaries[\\/]Win64[\\/]', ''
      $path = RemoteGet $rel (Join-Path $outDir (Split-Path $rel -Leaf))
    }
    if (-not $recordings.ContainsKey($id)) { $recordings[$id] = @() }
    $recordings[$id] += $path
  }
}
foreach ($id in $recordings.Keys) {
  if ($recordings[$id].Count -lt 2) { continue }
  Say "== match ${id}: polarity peers"
  & $polarity peers $recordings[$id][0] $recordings[$id][1] 2>&1 | Select-Object -First 12 | ForEach-Object { Say "  $_" }
}
Say "files in $outDir"
