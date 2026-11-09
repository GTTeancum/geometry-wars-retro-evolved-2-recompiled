@echo off
rem Builds and runs the PC check of the Xbox ADPCM conversion (src\xadpcm.cpp)
rem on the converted banks (xbox\build\audio_adpcm\*.adpcm).
setlocal
set XBOX=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set OUT=%XBOX%\build\xadpcm_check
if not exist "%OUT%" mkdir "%OUT%"
"C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc -Wno-everything ^
  /I"%XBOX%\include" /I"%XBOX%\src" /I"%XBOX%\..\tools\rexglue\win-amd64\include" ^
  "%XBOX%\tools\xadpcm_check\main.cpp" "%XBOX%\src\xadpcm.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\xadpcm_check.exe" /link libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib || exit /b 1
"%OUT%\xadpcm_check.exe" "%XBOX%\build\audio_adpcm\GW3.adpcm" "%XBOX%\build\audio_adpcm\MenuBank.adpcm" "%XBOX%\build\audio_adpcm\Song1Bank.adpcm" "%XBOX%\build\audio_adpcm\Song3Bank.adpcm"
