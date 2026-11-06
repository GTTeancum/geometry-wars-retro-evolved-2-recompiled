@echo off
rem Builds and runs the PC check of the native rewrites (src\*_native.cpp)
rem against the recompiled originals (see tools\native_check\main.cpp). Needs
rem xbox\build\gen (bash xbox/tools/build.sh) and game\analysis\default.image.bin.
setlocal
set XBOX=%~dp0..
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set OUT=%XBOX%\build\native_check
if not exist "%OUT%" mkdir "%OUT%"
rem Exact reciprocal square roots by default, to compare like with like.
if not defined CHECK_FLAGS set CHECK_FLAGS=-DGW_GRID_EXACT_RSQ -DGW_PARTICLE_GLOW -DGW_GRID_VERTICES
python "%XBOX%\tools\native_check\extract.py" "%XBOX%\build\gen" "%OUT%\original.inc" sub_8205B880 sub_8212E9B0 sub_82063448 sub_82075548 || exit /b 1
"C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc -march=pentium3 -Wno-everything %CHECK_FLAGS% ^
  /I"%XBOX%\include" /I"%XBOX%\src" /I"%OUT%" /I"%XBOX%\..\tools\rexglue\win-amd64\include" ^
  "%XBOX%\tools\native_check\main.cpp" "%XBOX%\src\grid_native.cpp" "%XBOX%\src\math_native.cpp" "%XBOX%\src\particle_native.cpp" "%XBOX%\src\stars_native.cpp" "%XBOX%\src\dxt.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\native_check.exe" /link /MAP:"%OUT%\native_check.map" /LARGEADDRESSAWARE:NO /DYNAMICBASE:NO libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib || exit /b 1
pushd "%XBOX%\.."
"%OUT%\native_check.exe" %*
popd
