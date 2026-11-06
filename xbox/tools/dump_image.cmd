@echo off
rem Builds tools\dump_image against the pinned ReXGlue SDK and writes the
rem decrypted, decompressed guest image to game\analysis\default.image.bin.
setlocal
set ROOT=%~dp0..\..
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S "%ROOT%\tools\dump_image" -B "%ROOT%\build\dump_image" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="%ROOT%\tools\rexglue\win-amd64" || exit /b 1
cmake --build "%ROOT%\build\dump_image" || exit /b 1
if not exist "%ROOT%\game\analysis" mkdir "%ROOT%\game\analysis"
"%ROOT%\build\dump_image\dump_image.exe" "%ROOT%\game\extracted" default.xex "%ROOT%\game\analysis\default.image.bin"
