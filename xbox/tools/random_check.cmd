@echo off
rem Builds and runs the PC check of the native random number generator
rem (src\random_native.cpp) against the recompiled originals.
setlocal
set XBOX=%~dp0..
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set OUT=%XBOX%\build\random_check
if not exist "%OUT%" mkdir "%OUT%"
python "%XBOX%\tools\native_check\extract.py" "%XBOX%\build\gen" "%OUT%\original.inc" sub_82030058 sub_82030220 sub_82030430 sub_8203B8B0 || exit /b 1
"C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc -march=pentium3 -Wno-everything ^
  /I"%XBOX%\include" /I"%XBOX%\src" /I"%OUT%" /I"%XBOX%\..\tools\rexglue\win-amd64\include" ^
  "%XBOX%\tools\random_check\main.cpp" "%XBOX%\src\random_native.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\random_check.exe" /link /LARGEADDRESSAWARE:NO /DYNAMICBASE:NO libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib || exit /b 1
pushd "%XBOX%\.."
"%OUT%\random_check.exe" %*
popd
