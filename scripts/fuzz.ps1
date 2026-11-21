# Builds the libFuzzer targets in tests\fuzz with MSVC's AddressSanitizer and
# runs each for -Seconds. The recording loader starts from the replay corpus
# (tests\replays), the decoder from the corpus's packed chunks. Inputs that
# crash land in build\fuzz\<target>\ as crash-* files.
#   scripts\fuzz.ps1 [-Seconds 300] [-Target replay|xpress|all]
param([int]$Seconds = 300, [ValidateSet('replay', 'xpress', 'all')][string]$Target = 'all')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools not found' }
# The sanitizer's runtime is a DLL beside the compiler, even with /MT.
$asan = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll" | Select-Object -Last 1
if (-not $asan) { throw 'AddressSanitizer not installed (the C++ AddressSanitizer component)' }
$env:PATH = "$($asan.DirectoryName);$env:PATH"
$sources = @{
    replay = @('tests\fuzz\fuzz_replay.cpp', 'src\core\replay_file.cpp', 'src\core\compress.cpp', 'src\core\file_open.cpp', 'src\core\state.cpp')
    xpress = @('tests\fuzz\fuzz_xpress.cpp', 'src\core\compress.cpp')
}

# Seeds: each recording, cut to the fuzzer's length limit, and each packed
# chunk as the decoder sees it (the claimed size, then the packed bytes).
function Add-Seeds([string]$target, [string]$corpus) {
    if (Get-ChildItem $corpus -File | Select-Object -First 1) { return }
    foreach ($f in Get-ChildItem (Join-Path $root 'tests\replays') -Recurse -Filter *.sfvr) {
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        if ($target -eq 'replay') {
            [IO.File]::WriteAllBytes((Join-Path $corpus $f.Name), $bytes[0..([Math]::Min($bytes.Length, 262144) - 1)])
            continue
        }
        $at = 8
        $n = 0
        while ($at + 8 -le $bytes.Length) {
            $tag = [Text.Encoding]::ASCII.GetString($bytes, $at, 4)
            $size = [BitConverter]::ToInt32($bytes, $at + 4)
            if ($tag -eq 'PACK' -and $size -gt 8 -and $size -le 262144) {
                $seed = $bytes[($at + 12)..($at + 8 + $size - 1)]
                [IO.File]::WriteAllBytes((Join-Path $corpus "$($f.BaseName)-$n"), [byte[]]$seed)
                $n++
            }
            if ($tag -eq 'END ') { break }
            $at += 8 + $size
        }
    }
}

$targets = if ($Target -eq 'all') { @('replay', 'xpress') } else { @($Target) }
foreach ($t in $targets) {
    $dir = Join-Path $root "build\fuzz\$t"
    $corpus = Join-Path $dir 'corpus'
    New-Item -ItemType Directory -Force $corpus | Out-Null
    $files = ($sources[$t] | ForEach-Object { "`"$(Join-Path $root $_)`"" }) -join ' '
    $exe = Join-Path $dir "fuzz_$t.exe"
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cl /nologo /std:c++20 /O2 /Zi /EHsc /MT /utf-8 /fsanitize=address /fsanitize=fuzzer /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /I`"$root\src`" $files /Fo`"$dir\\`" /Fd`"$dir\\`" /Fe`"$exe`" /link bcrypt.lib"
    if ($LASTEXITCODE -ne 0) { throw "building fuzz_$t failed" }
    Add-Seeds $t $corpus
    Write-Host "fuzz_${t}: $Seconds s from $((Get-ChildItem $corpus -File).Count) seeds"
    & $exe $corpus "-max_total_time=$Seconds" "-artifact_prefix=$dir\" -rss_limit_mb=4096 -max_len=262144 -print_final_stats=1
    if ($LASTEXITCODE -ne 0) { throw "fuzz_$t found a crash: see $dir" }
}
