<#
.SYNOPSIS
  Assembles the Windows installer for the Xbox build in
  xbox\build\installer (and xbox\build\gw2-xbox-installer.zip): install.ps1
  and install.cmd, the package extractor, dump_image.exe with ReXGlue's
  runtime DLL, and default.xbe. Users run install.cmd with their own LIVE
  package; the installer writes a folder ready for the Xbox.

  Needs xbox\build\release\default.xbe (xbox/tools/build.sh) and
  build\dump_image (xbox\tools\dump_image.cmd).
#>
$ErrorActionPreference = 'Stop'
$xbox = Split-Path $PSScriptRoot -Parent
$root = Split-Path $xbox -Parent
$out = Join-Path $xbox 'build\installer'
$sources = @{
    'install.ps1'    = Join-Path $xbox 'installer\install.ps1'
    'install.cmd'    = Join-Path $xbox 'installer\install.cmd'
    'StfsExtract.cs' = Join-Path $root 'tools\StfsExtract.cs'
    'dump_image.exe' = Join-Path $root 'build\dump_image\dump_image.exe'
    'rexruntime.dll' = Join-Path $root 'build\dump_image\rexruntime.dll'
    'default.xbe'    = Join-Path $xbox 'build\release\default.xbe'
}
foreach ($name in $sources.Keys) {
    if (-not (Test-Path -LiteralPath $sources[$name])) { throw "$($sources[$name]) is missing." }
}
Remove-Item -LiteralPath $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($name in $sources.Keys) { Copy-Item -LiteralPath $sources[$name] -Destination (Join-Path $out $name) }
$zip = Join-Path $xbox 'build\gw2-xbox-installer.zip'
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip
Write-Host "Installer in $out and $zip"
