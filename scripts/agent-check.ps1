# Checks a test agent end to end (scripts/agent_steamos.py on a Steam Deck,
# or agent.ps1 on a Windows PC) through remote.ps1, as the two-machine tests
# use it: every op, its errors, and with -Match a Direct Match against this
# PC on a Weyvelength server.
#   $env:SFV_AGENT_TOKEN = '<the agent's token>'
#   scripts\agent-check.ps1 -Agent 192.0.2.20 [-Deploy] [-Match -Server 192.0.2.10:5555]
# -Fake: the agent runs against a stand-in game (the WSL check), so the game's
# own effects (pads, a match) are not checked.
param([Parameter(Mandatory)][string]$Agent, [string]$Token = $env:SFV_AGENT_TOKEN, [int]$Port = 47100,
      [switch]$Deploy, [switch]$Match, [string]$Server = '', [switch]$Fake,
      [string]$Out = (Join-Path $env:TEMP 'polarity-agent-check'))
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
. (Join-Path $PSScriptRoot 'remote.ps1') -Agent $Agent -Token $Token -Port $Port
$checks = [System.Collections.Generic.List[string]]::new()
function Check([string]$name, [bool]$ok, [string]$detail = '') {
  $line = "$(if ($ok) { 'PASS' } else { 'FAIL' })  $name$(if ($detail) { ": $detail" })"
  $checks.Add($line); $line
}
function Throws([scriptblock]$block, [string]$pattern) {
  try { & $block; return "no error" } catch { if ("$_" -match $pattern) { return '' } else { return "$_" } }
}
function Sha([byte[]]$b) { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($b)) }

'== ping'
$p = Remote 'ping'
Check 'ping answers' ([bool]$p.ok) "host $($p.host), $($p.platform) $($p.python), game $($p.game), $($p.games) running"
if ($p.platform -eq 'steamos' -and -not $Fake) {
  Check 'the game loads the mod (launch options)' ([bool]$p.modLoads) "launch options: $($p.launchOptions)"
  Check 'steam is on the PATH' ([bool]$p.steam) "$($p.steam)"
}

'== errors'
$saved = $script:agentToken
$script:agentToken = 'not-the-token'
$e = Throws { Remote 'ping' } 'bad token'
$script:agentToken = $saved
Check 'a wrong token is refused' (-not $e) $e
$e = Throws { Remote 'nosuchop' } 'unknown op'
Check 'an unknown op is refused' (-not $e) $e
$e = Throws { Remote 'get' @{ path = '../../../../../../etc/hostname' } } 'outside'
Check 'a path outside the game folder is refused' (-not $e) $e

'== files'
$blob = [byte[]]::new(700000)
[Random]::new(7).NextBytes($blob)
$local = Join-Path $Out 'blob.bin'
[IO.File]::WriteAllBytes($local, $blob)
foreach ($remotePath in 'Polarity\agent-check\blob.bin', 'sfv-rollback\agent-check\blob.bin') {
  RemotePut $local $remotePath | Out-Null
  $back = Join-Path $Out 'blob-back.bin'
  RemoteGet $remotePath $back | Out-Null
  Check "put and get round trip ($remotePath, 700 KB)" ((Sha $blob) -eq (Sha ([IO.File]::ReadAllBytes($back))))
}

'== ini'
$iniLocal = Join-Path $Out 'agent-check.ini'
[IO.File]::WriteAllText($iniLocal, "[netplay]`r`nname=old`r`n; a comment`r`nother=1`r`n`r`n[boot]`r`noffline=1`r`n")
RemotePut $iniLocal 'agent-check.ini' | Out-Null
Remote 'ini' @{ file = 'agent-check.ini'; section = 'netplay'; key = 'name'; value = 'Deck' } | Out-Null
Remote 'ini' @{ file = 'agent-check.ini'; section = 'netplay'; key = 'added'; value = '2' } | Out-Null
Remote 'ini' @{ file = 'agent-check.ini'; section = 'rooms'; key = 'keeper_key'; value = 'abc' } | Out-Null
RemoteGet 'agent-check.ini' $iniLocal | Out-Null
$ini = Get-Content $iniLocal
$names = @($ini | Where-Object { $_ -match '^name=' })
$okIni = $names.Count -eq 1 -and $names[0] -eq 'name=Deck' -and ($ini -contains 'added=2') -and ($ini -contains 'other=1') -and
         ($ini -contains '[rooms]') -and ($ini -contains 'keeper_key=abc') -and ($ini -contains 'offline=1') -and
         ([array]::IndexOf($ini, 'added=2') -lt [array]::IndexOf($ini, '[boot]'))
Check 'ini: a key replaced, one added in its section, a new section' $okIni (($ini -join ' | '))

'== no game'
Remote 'stop' | Out-Null
Check 'stop with nothing running' ((Remote 'ping').games -eq 0)
# agent.ps1 answers it as an error, agent_steamos.py in its output: RemoteCmd
# throws on either.
$said = try { (Remote 'cmd' @{ instance = 0; commands = 'echo x' }).output } catch { "$_" }
$e = Throws { RemoteCmd 0 'echo x' } 'no game is running'
Check 'cmd with no game says so' ($said -match 'no game is running' -and -not $e) "$said"

if (-not $Fake) {
  '== test settings'
  # What scripted tests need of the mod there (as lib.ps1's Use-TestSettings
  # sets them here): commands and virtual pads, and running unfocused. The
  # ini as it was is kept in $Out; the settings stay for the tests to come.
  $before = Join-Path $Out 'agent-ini-before.ini'
  try { RemoteGet 'polarity.ini' $before | Out-Null; "  the ini as it was: $before" } catch { "  (no polarity.ini there yet)" }
  Remote 'ini' @{ file = 'polarity.ini'; section = 'control'; key = 'virtual_pads'; value = '1' } | Out-Null
  Remote 'ini' @{ file = 'polarity.ini'; section = 'control'; key = 'always_active'; value = '1' } | Out-Null
  RemoteGet 'polarity.ini' (Join-Path $Out 'agent-ini-after.ini') | Out-Null
  $after = Get-Content (Join-Path $Out 'agent-ini-after.ini')
  Check 'test settings set (virtual pads, always active)' (($after -contains 'virtual_pads=1') -and ($after -contains 'always_active=1'))
}

if ($Deploy) {
  '== deploy'
  RemoteDeploy
  $dll = Join-Path $PSScriptRoot '..\build\Release\polarity.dll'
  $back = Join-Path $Out 'dll-back.bin'
  RemoteGet 'sfv-rollback\build\Release\polarity.dll' $back | Out-Null
  Check 'the mod deploys (the build folder''s DLL matches)' ((Sha ([IO.File]::ReadAllBytes($dll))) -eq (Sha ([IO.File]::ReadAllBytes($back))))
}

'== start'
$t0 = Get-Date
$s = $null
try { $s = RemoteStart 1 } catch { $s = "$_" }
Check 'the game starts and reaches its main menu' ($s -match 'at the main menu') "$(($s -split "`n") -join ' / ') ($([int]((Get-Date) - $t0).TotalSeconds) s)"
Check 'ping counts the game' ((Remote 'ping').games -ge 1)

'== commands and the log'
RemoteLog 0 -FromNow
$marker = 'check-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$e = Throws { RemoteCmd 0 "echo $marker" -Wait -Seconds 30 } 'x^'
Check 'a command runs (cmd, and the log follows it)' (-not $e -or $e -eq 'no error') $e
$lines = @(RemoteLog 0)
Check 'the log returns only the new lines' ($lines.Count -lt 400) "$($lines.Count) lines"
$all = Remote 'log' @{ instance = 0; from = 0 }
Check 'the log reads from its start' ([int]$all.total -gt 3 -and @($all.lines).Count -gt 3) "$($all.total) lines"
$e = Throws { RemoteCmd 0 'echo a; echo b; echo c' -Wait } 'x^'
Check 'several commands in one call' (-not $e -or $e -eq 'no error') $e

'== screenshot'
$e = ''
try {
  $a = Remote 'shot' @{ instance = 0; scale = 2 }
  $png = [Convert]::FromBase64String($a.data)
  [IO.File]::WriteAllBytes((Join-Path $Out 'shot.png'), $png)
  $sig = ($png[0..7] | ForEach-Object { $_.ToString('X2') }) -join ''
  Check 'a screenshot (PNG)' ($sig -eq '89504E470D0A1A0A') "$($png.Length) bytes, $(Join-Path $Out 'shot.png')"
}
catch { Check 'a screenshot (PNG)' $false "$_" }

if (-not $Fake) {
  '== pads'
  $e = Throws { RemoteCmd 0 'pad 0 down 120 250; pad 0 up 120 250' -Wait } 'x^'
  Check 'virtual pad presses run' (-not $e -or $e -eq 'no error') $e
  $pads = Remote 'log' @{ instance = 0; from = 0 }
  Check 'the mod''s virtual pads are on' ([bool](@($pads.lines) -match 'virtual pads ready')) ''
}

if ($Match) {
  '== a match against this PC'
  if (-not $Server) { throw '-Match needs -Server host:port (a Weyvelength server both reach)' }
  . (Join-Path $PSScriptRoot 'lib.ps1')
  Use-TestSettings -Instances 1 {
    & (Join-Path $PSScriptRoot 'up.ps1') -Count 1 -Restart | Select-Object -Last 1
    $tail = New-GameTail 0
    Send-Cmd 0 "server $Server; delay 2; mash 11; stage CHN; host" -Wait
    $h = Wait-Log $tail 'netplay: hosting, code (\w+)' 60
    $code = [regex]::Match($h.Value, 'code (\w+)').Groups[1].Value
    RemoteLog 0 -FromNow
    RemoteCmd 0 "server $Server; delay 2; mash 22; join $code" -Wait
    $decided = Wait-Log $tail 'netplay: the match is decided' 360
    Start-Sleep 5
    $deck = @(RemoteLog 0)
    Check 'the Deck joins and plays a match' ([bool]$decided -and [bool]($deck -match 'netplay: the match is decided')) ''
    Send-Cmd 0 'leave' -Wait; RemoteCmd 0 'leave' -Wait
    Start-Sleep 5
    $deck += @(RemoteLog 0)
    $sum = $deck | Where-Object { $_ -match 'netplay: match [0-9a-f]{8}: .*desync events' } | Select-Object -Last 1
    Check 'no desync on the Deck' ($sum -match ' 0 desync events') "$sum"
  }
  Get-Process StreetFighterV -ErrorAction SilentlyContinue | Stop-Process -Force
}

'== stop'
$st = Remote 'stop'
Check 'stop closes the game' ($st.output -eq 'stopped' -and (Remote 'ping').games -eq 0) "$($st.output), killed $($st.killed), crash marks cleared $($st.marksCleared)"

'== checks'
$checks
$failed = @($checks | Where-Object { $_ -like 'FAIL*' }).Count
"$($checks.Count - $failed) of $($checks.Count) pass"
