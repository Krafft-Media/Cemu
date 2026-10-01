@echo off
rem Configure and build the Dispatcher fork of Cemu (Release, Ninja, MSVC) into out\build\Release.
rem Usage: build-dispatcher.cmd [configure]   -- "configure" forces a fresh CMake configure.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "VSCMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"
rem The vcpkg pinned at v2.6 downloads an msys2 pkgconf build the mirrors no longer have; use a local one.
if exist "C:\msys64\ucrt64\bin\pkg-config.exe" (
  set "PKG_CONFIG=C:\msys64\ucrt64\bin\pkg-config.exe"
  set "VCPKG_KEEP_ENV_VARS=PKG_CONFIG"
)
cd /d "%~dp0"
if "%1"=="configure" goto configure
if exist out\build\Release\build.ninja goto build
:configure
rem Debug info (bin\Cemu_release.pdb) lets Cemu's crash log name the functions; the PDB is not shipped.
cmake -S . -B out\build\Release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7" "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7" ^
  "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF" || exit /b 1
:build
cmake --build out\build\Release || exit /b 1
