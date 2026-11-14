@echo off
rem Builds and runs the PC check of the native rewrites (src\*_native.cpp)
rem against the recompiled originals (see tools\native_check\main.cpp). Needs
rem xbox\build\gen (bash xbox/tools/build.sh) and game\analysis\default.image.bin.
setlocal
set XBOX=%~dp0..
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
rem (OUT may be given, so two checks can run at once in different folders)
if not defined OUT set OUT=%XBOX%\build\native_check
if not exist "%OUT%" mkdir "%OUT%"
rem Exact reciprocal square roots by default, to compare like with like.
if not defined CHECK_FLAGS set CHECK_FLAGS=-DGW_GRID_EXACT_RSQ -DGW_PARTICLE_GLOW -DGW_GRID_VERTICES -DGW_GRID_GUEST_SYNC -DGW_ENEMY_WIP -DGW_BIG_NATIVE
python "%XBOX%\tools\native_check\extract.py" "%XBOX%\build\gen" "%OUT%\original.inc" sub_8205B880 sub_8212E9B0 sub_82063448 sub_82063A38 sub_82064110 sub_82064060 sub_82063100 sub_82063018 sub_8209D288 sub_820A68B0 sub_820A65F8 sub_82061DA0 sub_8209D8A0 sub_82075548 sub_820A91A0 sub_821308B8 sub_82096F30 sub_820A6438 sub_820A6518 sub_8205FBA8 sub_8205EB58 sub_82030338 sub_82041CA0 sub_82046248 sub_8212EA80 sub_82041600 sub_82052860 sub_82046BD8 sub_820631E8 sub_8205A2B0 sub_82059FC0 sub_8205A100 sub_8205A0F0 sub_8205C5D8 sub_8205C6D8 sub_820453E0 sub_82047180 sub_8204F458 sub_82040F28 sub_82056CD8 sub_82059838 sub_82049E60 sub_82056B48 sub_82045AB8 sub_82059CD0 sub_82060CC0 sub_8205FA98 sub_82045B18 sub_82044330 sub_82054E30 sub_82044168 sub_82074B28 sub_82074538 sub_82045740 sub_82045628 sub_82061908 sub_82061A30 sub_8212FE70 sub_82030678 sub_82065E38 sub_8206A570 sub_8205F6B8 sub_8204D8E0 sub_82056FA0 sub_82046D88 sub_82047DF8 sub_82045CF0 sub_82040FF0 sub_82030C58 sub_82067A20 sub_8205EF18 sub_820366A0 sub_8207CA48 || exit /b 1
"C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc -march=pentium3 -Wno-everything %CHECK_FLAGS% ^
  /I"%XBOX%\include" /I"%XBOX%\src" /I"%OUT%" /I"%XBOX%\..\tools\rexglue\win-amd64\include" ^
  "%XBOX%\tools\native_check\main.cpp" "%XBOX%\src\grid_native.cpp" "%XBOX%\src\math_native.cpp" "%XBOX%\src\particle_native.cpp" "%XBOX%\src\stars_native.cpp" "%XBOX%\src\dxt.cpp" "%XBOX%\src\d3d_native.cpp" "%XBOX%\src\memory_native.cpp" "%XBOX%\src\shapes_native.cpp" "%XBOX%\src\entity_native.cpp" "%XBOX%\src\enemy_native.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\native_check.exe" /link /STACK:16777216 /MAP:"%OUT%\native_check.map" /LARGEADDRESSAWARE:NO /DYNAMICBASE:NO libcmt.lib libucrt.lib libvcruntime.lib kernel32.lib || exit /b 1
pushd "%XBOX%\.."
"%OUT%\native_check.exe" %*
popd
