<#
.SYNOPSIS
  Prepares Geometry Wars: Retro Evolved 2 for the original Xbox from your own
  Xbox 360 LIVE package.

.DESCRIPTION
  1. Checks that the package is Geometry Wars: Retro Evolved 2 (title ID
     584108FF) and extracts it.
  2. Writes the game's decrypted image (gw2\default.image.bin) with
     dump_image.exe, and checks that it is the supported version.
  3. Copies the game's files next to default.xbe in the output folder, ready
     to copy to the Xbox (or to build a disc image from), and gives the XBE
     the dashboard's title name and the game's own icon.
  4. Converts the sounds from XMA to IMA ADPCM (audio\<bank>.adpcm), with
     ffmpeg from PATH or ffmpeg.exe next to this script.

  The output folder must be new or empty. Nothing is written anywhere else
  except a temporary folder, removed at the end.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install.ps1 -Package 'D:\360\584108FF\000D0000\<package>' -Output 'D:\GW2 Xbox'
#>
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$titleId = [uint32]'0x584108FF'
$imageSha256 = '4168A02A62542D32660E63EFDE3244BB423D593C288402F177BF5E6F8C33B901'

foreach ($file in 'default.xbe', 'StfsExtract.cs', 'SoundConvert.cs', 'DashboardIcon.cs', 'dump_image.exe', 'rexruntime.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $here $file))) { throw "$file is missing from $here; unpack the whole installer first." }
}
$pkg = (Resolve-Path -LiteralPath $Package).Path
$out = [IO.Path]::GetFullPath($Output)
if ((Test-Path -LiteralPath $out) -and (Get-ChildItem -LiteralPath $out -Force | Select-Object -First 1)) {
    throw "$out is not empty; choose a new folder."
}

Add-Type -Path (Join-Path $here 'StfsExtract.cs')
$found = [StfsExtract]::TitleId($pkg)
if ($found -ne $titleId) { throw ('This package is title {0:X8}, not Geometry Wars: Retro Evolved 2 ({1:X8}).' -f $found, $titleId) }

$work = Join-Path ([IO.Path]::GetTempPath()) ('gw2-xbox-' + [Guid]::NewGuid().ToString('N'))
try {
    Write-Host 'Extracting the package...'
    $extracted = Join-Path $work 'extracted'
    # The extractor lists every file on the console; keep the installer quiet.
    $console = [Console]::Out
    [Console]::SetOut([IO.TextWriter]::Null)
    try { [StfsExtract]::Run($pkg, $extracted) | Out-Null } finally { [Console]::SetOut($console) }
    Write-Host ('  {0} files' -f (Get-ChildItem -LiteralPath $extracted -Recurse -File).Count)

    Write-Host 'Writing the game image...'
    $image = Join-Path $work 'default.image.bin'
    & (Join-Path $here 'dump_image.exe') $extracted 'default.xex' $image | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $image)) { throw 'dump_image.exe could not read default.xex.' }
    if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash -ne $imageSha256) {
        throw 'This is not the supported version of the game (the title update may differ).'
    }

    Write-Host "Writing $out..."
    New-Item -ItemType Directory -Force $out | Out-Null
    Copy-Item -Path (Join-Path $extracted '*') -Destination $out -Recurse
    New-Item -ItemType Directory -Force (Join-Path $out 'gw2') | Out-Null
    Copy-Item -LiteralPath $image -Destination (Join-Path $out 'gw2\default.image.bin')
    Copy-Item -LiteralPath (Join-Path $here 'default.xbe') -Destination (Join-Path $out 'default.xbe')

    # The dashboard's title name and the game's own icon, from this copy of
    # the game: into the XBE and next to it (the download carries none of
    # the game's art).
    Write-Host 'Adding the dashboard icon...'
    Add-Type -Path (Join-Path $here 'DashboardIcon.cs')
    [DashboardIcon]::Run($image, (Join-Path $out 'default.xbe'), $out)

    # The Xbox has no XMA: the sounds become IMA ADPCM, through ffmpeg (on
    # PATH, or ffmpeg.exe next to this script).
    $ffmpeg = Join-Path $here 'ffmpeg.exe'
    if (-not (Test-Path -LiteralPath $ffmpeg)) { $ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source }
    if ($ffmpeg) {
        Write-Host 'Converting the sounds (several minutes)...'
        Add-Type -Path (Join-Path $here 'SoundConvert.cs')
        $sounds = [SoundConvert]::Run($ffmpeg, (Join-Path $extracted 'audio'), (Join-Path $out 'audio'))
        Write-Host "  $sounds sounds converted"
    } else {
        Write-Warning 'ffmpeg was not found, so the sounds were not converted (the game runs silent). Install ffmpeg (https://ffmpeg.org) or put ffmpeg.exe next to install.ps1, then run the installer again.'
    }
    Write-Host "Done: copy the contents of $out to a folder on the Xbox and run default.xbe."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
