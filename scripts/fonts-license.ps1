# Writes the bundled fonts' license texts as one file (releases ship it as
# sfvr_fonts_license.txt next to the mod; third_party/fonts/README.md).
#   fonts-license.ps1 <out file>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot '..\third_party\fonts'
$parts = foreach ($f in @(
    @{ Name = 'Barlow (Regular, SemiBold, Bold, Bold Italic), embedded in sfv_rollback.dll'; File = 'OFL-Barlow.txt' },
    @{ Name = 'Archivo Black, embedded in sfv_rollback.dll'; File = 'OFL-ArchivoBlack.txt' },
    @{ Name = 'Noto Sans CJK Medium (SFVR\fonts\NotoSansCJK-Medium.ttc)'; File = 'OFL-NotoSansCJK.txt' })) {
  "== $($f.Name)`r`n`r`n" + (Get-Content (Join-Path $dir $f.File) -Raw)
}
[IO.File]::WriteAllText($Out, "SFVR's fonts, each under the SIL Open Font License 1.1.`r`n`r`n" + ($parts -join "`r`n`r`n"))
