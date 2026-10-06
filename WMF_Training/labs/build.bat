@echo off
rem One-click build for the WMF labs.
rem Needs Visual Studio 2019/2022 ("Desktop development with C++") and CMake.
rem Run from "x64 Native Tools Command Prompt for VS 2022" (has cmake on PATH) or any cmd with cmake.
rem (ASCII only on purpose: cmd's default code page would garble UTF-8 text.)
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cmake not found.
  echo         Open "x64 Native Tools Command Prompt for VS 2022" from the Start menu and run this again,
  echo         or install CMake from https://cmake.org/download/
  exit /b 1
)

rem No -G: CMake picks the newest installed Visual Studio.
cmake -S . -B build -A x64 || goto :fail
cmake --build build --config Release || goto :fail

echo.
echo ============================================================
echo  Build OK. Executables are in: %~dp0build\Release
echo  Next:  cd build\Release
echo         portable_tests.exe
echo         codec_loopback.exe
echo ============================================================
exit /b 0

:fail
echo [ERROR] Build failed. Copy the FIRST error message above and send it over.
exit /b 1
