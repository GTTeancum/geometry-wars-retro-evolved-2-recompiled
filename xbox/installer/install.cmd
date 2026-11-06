@echo off
rem Geometry Wars: Retro Evolved 2 for the original Xbox: prepares the game
rem from your own Xbox 360 LIVE package (see install.ps1).
rem   install.cmd <LIVE package> <new output folder>
if "%~2"=="" (
  echo usage: install.cmd ^<LIVE package^> ^<new output folder^>
  exit /b 2
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Package "%~1" -Output "%~2"
