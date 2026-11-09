@echo off
rem Builds tools\xadpcm_bank and turns the converted banks
rem (xbox\build\audio_adpcm\*.adpcm, IMA ADPCM) into Xbox ADPCM banks in
rem xbox\build\audio_xadpcm, which tools\stage_disc.py stages.
setlocal
set XBOX=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set OUT=%XBOX%\build\xadpcm_bank
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%XBOX%\build\audio_xadpcm" mkdir "%XBOX%\build\audio_xadpcm"
"C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc -Wno-everything ^
  /I"%XBOX%\include" /I"%XBOX%\src" /I"%XBOX%\..\tools\rexglue\win-amd64\include" ^
  "%XBOX%\tools\xadpcm_bank\main.cpp" "%XBOX%\src\xadpcm.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\xadpcm_bank.exe" /link libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib || exit /b 1
for %%f in ("%XBOX%\build\audio_adpcm\*.adpcm") do "%OUT%\xadpcm_bank.exe" "%%f" "%XBOX%\build\audio_xadpcm\%%~nxf" || exit /b 1
