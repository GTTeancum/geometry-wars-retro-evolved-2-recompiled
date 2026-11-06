<#
.SYNOPSIS
  Saves the XEMU window started by boot_xemu.ps1 (build\xemu\pid.txt) as a PNG.
#>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class XemuWindow {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public struct RECT { public int L, T, R, B; }
}
'@
$processId = [int](Get-Content -LiteralPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\xemu\pid.txt'))
$window = (Get-Process -Id $processId).MainWindowHandle
$rect = New-Object XemuWindow+RECT
[XemuWindow]::GetWindowRect($window, [ref]$rect) | Out-Null
$bitmap = New-Object System.Drawing.Bitmap ($rect.R - $rect.L), ($rect.B - $rect.T)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$dc = $graphics.GetHdc()
[XemuWindow]::PrintWindow($window, $dc, 2) | Out-Null  # PW_RENDERFULLCONTENT
$graphics.ReleaseHdc($dc)
$bitmap.Save($Out)
