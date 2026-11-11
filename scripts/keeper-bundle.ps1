# Copies polarity-keeper's sources (src/keeper and the core files it shares with
# the mod), the Weyvelength patches and the deploy files into polarity-server's
# deploy/keeper/, so the VPS builds it with install-keeper.sh. Run after a
# keeper change, then commit the polarity-server side.
#   scripts/keeper-bundle.ps1 [-Server <polarity-server checkout>]
# (default: the polarity-server folder beside this repository, or sfvr-server,
# its name before the rename).
param([string]$Server = $(foreach ($n in 'polarity-server', 'sfvr-server') { $p = Join-Path $PSScriptRoot "..\..\$n"; if (Test-Path $p) { $p; break } }))
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$dest = Join-Path (Resolve-Path $Server) 'deploy\keeper'
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
$files = @(
  'src/keeper/CMakeLists.txt', 'src/keeper/main.cpp', 'src/keeper/keeper.cpp', 'src/keeper/keeper.h', 'src/keeper/sign.cpp', 'src/keeper/sign.h', 'src/keeper/connector.h',
  'src/core/keeper_protocol.cpp', 'src/core/keeper_protocol.h', 'src/core/rules.cpp', 'src/core/rules.h', 'src/core/lobby.h', 'src/core/lobby.cpp', 'src/core/game.h',
  'src/core/spectate_relay.h', 'src/core/spectate_relay.cpp',
  'third_party/weyvelength.patch', 'third_party/weyvelength-departed-links.patch', 'third_party/weyvelength-port-range.patch', 'third_party/weyvelength-link-mux.patch', 'third_party/weyvelength-decode-limit.patch',
  'cmake/apply-patch.cmake'
)
foreach ($f in $files) {
  $to = Join-Path $dest $f
  New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
  Copy-Item (Join-Path $repo $f) $to
}
Copy-Item (Join-Path $PSScriptRoot 'keeper-deploy\install-keeper.sh') $dest
# The unit keeps its deployed name (the VPS's sfvr-* units stay).
Copy-Item (Join-Path $PSScriptRoot 'keeper-deploy\sfvr-keeper.service') $dest
$commit = (git -C $repo rev-parse --short HEAD).Trim()
$dirty = if (git -C $repo status --porcelain -- src/keeper src/core third_party cmake) { ' (with uncommitted changes)' } else { '' }
"polarity-keeper sources from polarity-client $commit$dirty, $(Get-Date -Format 'yyyy-MM-dd HH:mm')" | Set-Content (Join-Path $dest 'SOURCE.txt')
"bundled $($files.Count) files into $dest"
