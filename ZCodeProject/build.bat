@echo off
REM ============================================================================
REM  build.bat - build stayout_dll.dll and injector.exe in one step.
REM
REM  Works with full Visual Studio 2022 OR VS Build Tools 2022 (no IDE needed).
REM  vswhere with default filter does NOT see Build Tools, so we use -products *.
REM
REM  NOTE on cmd quirk: VS lives under "Program Files (x86)" which contains
REM  parentheses. Inside "if (...) ( ... )" blocks those parens prematurely
REM  close the block. To avoid that, this script uses goto/labels instead of
REM  parenthesized multi-line blocks. All echo text is pure ASCII so the file
REM  does not depend on console codepage (works in cp866 and cp1251 alike).
REM
REM  Usage:
REM      build.bat            -> Release (default)
REM      build.bat Debug      -> debug build
REM
REM  Output:
REM      build\stayout_dll.dll
REM      build\injector\injector.exe
REM ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

echo [*] Configuration: %CONFIG%
echo [*] Source dir:     %~dp0

REM --- 1. Locate Visual Studio (IDE or Build Tools) via vswhere. ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do set "VSINSTALL=%%i"

if not "%VSINSTALL%"=="" goto :vs_found
echo [-] Visual Studio / Build Tools not found via vswhere.
echo     Install "Visual Studio Build Tools 2022" with components:
echo       - MSVC v143 - VS 2022 C++ x64/x86 build tools
echo       - C++ CMake tools for Windows
echo       - Windows 11 SDK or Windows 10 SDK
echo     Download: https://visualstudio.microsoft.com/downloads/
pause
exit /b 1

:vs_found
echo [+] VS install: %VSINSTALL%

REM --- 2. Load compiler environment (cl.exe, cmake.exe) via vcvarsall. ---
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARS%" goto :vcvars_ok
echo [-] vcvarsall.bat not found: %VCVARS%
echo     In Build Tools installer tick "MSVC v143 build tools".
pause
exit /b 1

:vcvars_ok
echo [*] Loading compiler environment (vcvarsall x64)...
call "%VCVARS%" x64 >nul
if errorlevel 1 goto :vcvars_fail
goto :vcvars_done

:vcvars_fail
echo [-] vcvarsall failed.
pause
exit /b 1

:vcvars_done
REM Prefer CMake shipped with VS, otherwise the one on PATH.
set "VSCMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CMAKE=cmake"
if exist "%VSCMAKE%" set "CMAKE=%VSCMAKE%"
echo [+] CMake: !CMAKE!

REM --- 3. Configure CMake once. Ninja ships with Build Tools and is faster
REM        and more reliable for x64-only builds than the VS generator. ---
if exist build goto :cmake_configured
echo [*] Configuring CMake (Ninja, x64)...
"!CMAKE!" -B build -G "Ninja" -DCMAKE_BUILD_TYPE=%CONFIG% -S .
if errorlevel 1 goto :cmake_config_fail
goto :cmake_configured

:cmake_config_fail
echo [-] CMake configuration failed.
echo     If Ninja is missing, install "C++ CMake tools for Windows".
pause
exit /b 1

:cmake_configured
if exist build\build.ninja goto :do_build
if exist build\CMakeCache.txt goto :do_build
echo [*] build/ exists but not configured, re-configuring...
"!CMAKE!" -B build -G "Ninja" -DCMAKE_BUILD_TYPE=%CONFIG% -S .
if errorlevel 1 goto :cmake_config_fail

:do_build
echo [*] Building %CONFIG%...
"!CMAKE!" --build build --config %CONFIG% --parallel
if errorlevel 1 goto :build_fail

echo.
echo [+] SUCCESS.
if exist "build\stayout_dll.dll" (
    echo     DLL:      build\stayout_dll.dll
) else (
    echo     DLL:      build\%CONFIG%\stayout_dll.dll
)
if exist "build\injector\injector.exe" (
    echo     Injector: build\injector\injector.exe
) else (
    echo     Injector: build\injector\%CONFIG%\injector.exe
)
echo.
echo Next: right-click run.bat then "Run as administrator".
echo.
pause
exit /b 0

:build_fail
echo [-] Build failed.
pause
exit /b 1
