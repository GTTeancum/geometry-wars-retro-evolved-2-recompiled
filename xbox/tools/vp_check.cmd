@echo off
rem Builds and runs the PC check of the vertex program translator against the
rem captured shaders in xbox\test-artifacts\gpu (see tools\vp_check\main.cpp).
setlocal
set XBOX=%~dp0..
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set OUT=%XBOX%\build\vp_check
if not exist "%OUT%" mkdir "%OUT%"
"C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc -Wno-everything ^
  /I"%XBOX%\include" /I"%XBOX%\src" /I"%XBOX%\..\tools\rexglue\win-amd64\include" ^
  "%XBOX%\tools\vp_check\main.cpp" "%XBOX%\src\xenos_vs.cpp" "%XBOX%\src\xenos_vp.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\vp_check.exe" /link /LARGEADDRESSAWARE:NO libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib || exit /b 1
"%OUT%\vp_check.exe" %*
