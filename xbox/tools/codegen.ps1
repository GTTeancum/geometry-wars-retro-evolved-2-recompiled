<#
.SYNOPSIS
  Generates the Xbox build's recompiled C++ (xbox\codegen.toml) into
  xbox\build\generated, with scratch and saved registers as C++ locals.
  Needs the SDK that tools\codegen.ps1 downloads.
#>
$ErrorActionPreference = 'Stop'
$xbox = Split-Path $PSScriptRoot -Parent
$root = Split-Path $xbox -Parent
$rexglue = Join-Path $root 'tools\rexglue\win-amd64\bin\rexglue.exe'
if (-not (Test-Path $rexglue)) { throw "Missing $rexglue (run tools\codegen.ps1 once first)" }
New-Item -ItemType Directory -Force (Join-Path $xbox 'build') | Out-Null
$ErrorActionPreference = 'Continue'
& $rexglue codegen (Join-Path $xbox 'codegen.toml') 2>&1 | ForEach-Object { "$_" } | Set-Content (Join-Path $xbox 'build\codegen.log')
$exitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
# ReXGlue also drops a CMake helper next to the manifest; the Xbox build has no use for it.
Remove-Item -Recurse -Force (Join-Path $xbox 'generated') -ErrorAction SilentlyContinue
if ($exitCode -ne 0) { throw "codegen failed ($exitCode), see xbox\build\codegen.log" }
Write-Host 'Generated xbox\build\generated'
