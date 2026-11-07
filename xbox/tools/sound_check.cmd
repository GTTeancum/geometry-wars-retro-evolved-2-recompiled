@echo off
rem Builds and runs the PC check of the native sound engine (src\sound_native.cpp)
rem on the staged disc's sound files (python xbox\tools\stage_disc.py first).
rem Writes menu.wav, game.wav and song3.wav into xbox\build\sound_check.
setlocal
set XBOX=%~dp0..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set OUT=%XBOX%\build\sound_check
set FWD=%XBOX:\=/%
if not exist "%OUT%" mkdir "%OUT%"
echo #define GW_AUDIO_DIR "%FWD%/build/staged-disc/Audio/" > "%OUT%\audio_dir.h"
"C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc -march=pentium3 -Wno-everything /clang:-mfpmath=sse -DGW_SOUND_PROFILE ^
  /FI"%OUT%\audio_dir.h" ^
  /I"%XBOX%\include" /I"%XBOX%\src" /I"%XBOX%\..\tools\rexglue\win-amd64\include" ^
  "%XBOX%\tools\sound_check\main.cpp" "%XBOX%\src\sound_native.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\sound_check.exe" /link libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib || exit /b 1
"%OUT%\sound_check.exe" "%OUT%"
