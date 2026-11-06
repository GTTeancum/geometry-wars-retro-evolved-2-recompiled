<#
.SYNOPSIS
  Stages the disc, builds xbox\test-artifacts\gw2-current.iso and boots it in
  an isolated XEMU (its own config, a test EEPROM and a snapshot of the hard
  disk). Adapted from the Jedi Power Battles project's boot_xemu.ps1.

  Prints the XEMU process id. The monitor listens on 127.0.0.1:$MonitorPort
  (xbox\tools\read_log.py reads the game log through it).
#>
param(
    [string]$XemuRoot = 'C:\Games\Emulators\Xemu',
    [string]$NxdkRoot = 'C:\nxdk',
    [int]$MonitorPort = 9257,
    # Test-only GDB stub, used by tools\xemu_pad.py to write the remote pad.
    [int]$GdbPort = 9258,
    [ValidateRange(1,10)][int]$RenderScale = 1,
    [ValidateSet(64,128)][int]$MemoryMb = 64
)
$ErrorActionPreference = 'Stop'
$xboxRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $xboxRoot 'build'
$stage = Join-Path $buildRoot 'staged-disc'
$artifacts = Join-Path $xboxRoot 'test-artifacts'
$captures = Join-Path $artifacts 'captures'
$runRoot = Join-Path $buildRoot 'xemu'
New-Item -ItemType Directory -Force $artifacts, $captures, $runRoot | Out-Null

$stageArgs = @((Join-Path $PSScriptRoot 'stage_disc.py'))
if ($MemoryMb -eq 128) { $stageArgs += '--devkit-128mb' }
& python @stageArgs
if ($LASTEXITCODE -ne 0) { throw 'Staging the disc failed.' }

$configPath = Join-Path $runRoot 'xemu.toml'
$testEeprom = Join-Path $runRoot 'eeprom-test.bin'
& python (Join-Path $PSScriptRoot 'prepare_test_eeprom.py') --source (Join-Path $XemuRoot 'eeprom.bin') --output $testEeprom --720p
if ($LASTEXITCODE -ne 0) { throw 'Failed to prepare the test EEPROM.' }

# Stop only an XEMU started with this exact config.
Get-CimInstance Win32_Process -Filter "Name='xemu.exe'" | Where-Object {
    $_.CommandLine -and $_.CommandLine.Replace('/','\').ToLowerInvariant().Contains($configPath.Replace('/','\').ToLowerInvariant())
} | ForEach-Object { Stop-Process -Id $_.ProcessId; Start-Sleep -Milliseconds 500 }

$isoName = 'gw2-current.iso'
Push-Location $artifacts
try {
    & (Join-Path $NxdkRoot 'tools/extract-xiso/build/extract-xiso.exe') -c $stage $isoName *> (Join-Path $artifacts 'iso.log')
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $isoName)) { throw 'Disc image creation failed.' }
} finally { Pop-Location }

$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'xemu.template.toml') -Raw
$config = [regex]::Replace($config, '(?m)^surface_scale\s*=.*$', "surface_scale=$RenderScale")
$config = [regex]::Replace($config, "(?m)^mem_limit\s*=.*$", "mem_limit='$MemoryMb'")
Set-Content -LiteralPath $configPath -Value $config

$launchArgs = @('-config_path', ('"' + $configPath + '"'), '-monitor', "tcp:127.0.0.1:$MonitorPort,server,nowait", '-snapshot')
if ($GdbPort -gt 0) { $launchArgs += @('-gdb', "tcp:127.0.0.1:$GdbPort") }
$previousTemp = $env:TEMP; $previousTmp = $env:TMP
try {
    $env:TEMP = $artifacts; $env:TMP = $artifacts
    $process = Start-Process -FilePath (Join-Path $XemuRoot 'xemu.exe') -ArgumentList $launchArgs -WorkingDirectory $artifacts -PassThru -RedirectStandardOutput (Join-Path $artifacts 'stdout.log') -RedirectStandardError (Join-Path $artifacts 'stderr.log')
} finally { $env:TEMP = $previousTemp; $env:TMP = $previousTmp }
$process.Id | Set-Content -LiteralPath (Join-Path $runRoot 'pid.txt')
[pscustomobject]@{ ProcessId = $process.Id; Disc = (Join-Path $artifacts $isoName); Config = $configPath }
