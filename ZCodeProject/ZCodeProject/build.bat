@echo off
REM ============================================================================
REM  build.bat - build stayout_dll.dll (CMake/Ninja), the injector2 kernel-mode
REM  toolchain (MSBuild), and the KDU mapper (MSBuild) in one step.
REM
REM  Three build systems, intentionally:
REM    * stayout_dll.dll  -> CMake + Ninja (research DLL, x64)
REM    * injector2        -> MSBuild (.vcxproj/.sln):
REM        - loader.exe        user-mode loader (always built)
REM        - test_client.exe   user-mode test client (always built)
REM        - modern_injector.sys kernel driver (BUILT ONLY IF WDK IS INSTALLED)
REM    * KDU-1.4.9        -> MSBuild (KDU.sln): kdu.exe + drv64.dll, staged to
REM                        KDU-1.4.9\bin\. kdu.exe replaces the old kdmapper and
REM                        maps modern_injector.sys into the kernel at runtime.
REM
REM  The driver source (modern_injector.cpp) uses ntifs.h and Zw* kernel APIs
REM  and CANNOT compile without the WDK. When the WDK is absent, BuildDriver is
REM  left unset so the .sys target is skipped (the console apps still build).
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
REM      injector2\x64\Release\loader.exe
REM      injector2\x64\Release\test_client.exe
REM      injector2\x64\Release\modern_injector.sys   (only if WDK installed)
REM      KDU-1.4.9\bin\kdu.exe  (+ drv64.dll)         (kernel mapper)
REM ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

REM MSBuild Configuration names are Release/Debug (no change), but the vcxproj
REM uses "Win32" for x86 — we only ever target x64.
set "MSB_PLAT=x64"

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

REM --- 2. Load compiler environment (cl.exe, cmake.exe, msbuild.exe) via vcvarsall. ---
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

REM --- 3. Configure CMake. Ninja ships with Build Tools and is faster
REM        and more reliable for x64-only builds than the VS generator.
REM        If the project was moved, CMakeCache.txt still points to the old
REM        source path, so rebuild the generated build/ directory.
set "SRC_FWD=%CD:\=/%"
if not exist build\CMakeCache.txt goto :cmake_configure
findstr /x /c:"CMAKE_HOME_DIRECTORY:INTERNAL=!SRC_FWD!" "build\CMakeCache.txt" >nul 2>&1
if not errorlevel 1 goto :cmake_configure
echo [!] CMake cache belongs to another source path - recreating build\.
rmdir /s /q build

:cmake_configure
echo [*] Configuring CMake (Ninja, x64)...
"!CMAKE!" -B build -G "Ninja" -DCMAKE_BUILD_TYPE=%CONFIG% -S .
if errorlevel 1 goto :cmake_config_fail
goto :do_build

:cmake_config_fail
echo [-] CMake configuration failed.
echo     If Ninja is missing, install "C++ CMake tools for Windows".
pause
exit /b 1

:do_build
echo [*] Building stayout_dll (%CONFIG%)...
"!CMAKE!" --build build --config %CONFIG% --parallel
if errorlevel 1 goto :build_fail

REM ===========================================================================
REM  4. Build injector2 via MSBuild.
REM
REM  injector2\kernelmode.sln contains two projects:
REM    kernelmode.vcxproj      -> loader.exe   (user-mode, plain v143, always)
REM    modern_injector.vcxproj -> modern_injector.sys (kernel driver, WDK)
REM  The driver project uses v143 + explicit WDK km/ include+lib paths (NOT the
REM  WindowsKernelModeDriver10.0 toolset, so it needs no WDK VSIX). It builds
REM  only if the WDK headers are present; otherwise we build loader.exe alone.
REM  test_client.cpp is opt-in via /p:BuildTestClient=true (it shares main()
REM  with loader.cpp, so it's excluded from the default loader project).
REM ===========================================================================
echo.
echo [*] Building injector2 (MSBuild %CONFIG% %MSB_PLAT%)...

REM Detect the WDK by looking for the kernel-mode ntifs.h header. If present,
REM build the whole solution (both loader + driver). Otherwise build only the
REM user-mode loader project so the .sys step doesn't fail.
set "WDK_FOUND="
for /f "delims=" %%V in ('dir /b /ad "%ProgramFiles(x86)%\Windows Kits\10\Include" 2^>nul') do (
    if exist "%ProgramFiles(x86)%\Windows Kits\10\Include\%%V\km\ntifs.h" set "WDK_FOUND=1"
)

REM vcvarsall put msbuild.exe on PATH. Use it directly.
set "MSBUILD=msbuild"
where /q msbuild
if errorlevel 1 set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe"

set "INJ_SLN=%~dp0injector2\kernelmode.sln"
set "INJ_DIR=%~dp0injector2"
if not exist "%INJ_SLN%" goto :inj_missing

if defined WDK_FOUND (
    echo [+] WDK detected ^(ntifs.h found^) - building loader.exe + modern_injector.sys.
    "%MSBUILD%" "%INJ_SLN%" /t:Rebuild /p:Configuration=%CONFIG% /p:Platform=%MSB_PLAT% -nologo -verbosity:minimal
) else (
    echo [!] WDK NOT detected - building loader.exe only ^(modern_injector.sys skipped^).
    echo     To build the .sys: install the WDK ^(matching the Windows SDK version^).
    "%MSBUILD%" "%INJ_DIR%\kernelmode.vcxproj" /t:Rebuild /p:Configuration=%CONFIG% /p:Platform=%MSB_PLAT% -nologo -verbosity:minimal
)
if errorlevel 1 goto :inj_fail
goto :inj_done

:inj_missing
echo [-] injector2\kernelmode.sln not found: %INJ_SLN%
goto :build_fail

:inj_fail
echo [-] MSBuild build of injector2 failed.
echo     For the driver, the project uses v143 + explicit WDK paths (no VSIX
echo     needed). If modern_injector.cpp fails, check that the WDK version folder
echo     under Include\ matches WDKVer in modern_injector.vcxproj.
goto :build_fail

:inj_done
REM ===========================================================================
REM  5. Build the bundled KDU-1.4.9 solution (Kernel Driver Utility) which
REM     replaces the old kdmapper. KDU maps modern_injector.sys into the kernel.
REM
REM  KDU.sln builds (x64):
REM    Hamakaze\KDU.vcxproj      -> kdu.exe    (main executable, Application)
REM    Tanikaze\Tanikaze.vcxproj -> drv64.dll  (drivers database, must sit next
REM                                             to kdu.exe at runtime)
REM    Taigei\Taigei.vcxproj     -> Taigei32/64.dll (helpers; their precompiled
REM       .bin resources already ship in Source\Hamakaze\res\, so Taigei is not
REM       needed for the runtime set here).
REM  OutDir of each project is .\output\$(Platform)\$(Configuration)\.
REM ===========================================================================
set "KDU_ROOT=%~dp0KDU-1.4.9"
set "KDU_SLN=%KDU_ROOT%\Source\KDU.sln"
set "KDU_BIN=%KDU_ROOT%\bin"
set "KDU_HAMAKAZE=%KDU_ROOT%\Source\Hamakaze\output\%MSB_PLAT%\%CONFIG%\kdu.exe"
set "KDU_TANIKAZE=%KDU_ROOT%\Source\Tanikaze\output\%MSB_PLAT%\%CONFIG%\drv64.dll"
if not exist "%KDU_SLN%" goto :kdu_missing

echo.
echo [*] Building KDU (MSBuild %CONFIG% %MSB_PLAT%)...
"%MSBUILD%" "%KDU_SLN%" /t:Rebuild /p:Configuration=%CONFIG% /p:Platform=%MSB_PLAT% /p:PlatformToolset=v143 -nologo -verbosity:minimal
if errorlevel 1 goto :kdu_fail

REM Stage kdu.exe + drv64.dll together in bin\. KDU refuses to run unless
REM drv64.dll lives in the same directory as kdu.exe.
if not exist "%KDU_BIN%" mkdir "%KDU_BIN%"
copy /Y "%KDU_HAMAKAZE%" "%KDU_BIN%\" >nul
copy /Y "%KDU_TANIKAZE%" "%KDU_BIN%\" >nul
goto :kdu_done

:kdu_missing
echo [-] KDU\Source\KDU.sln not found: %KDU_SLN%
goto :build_fail

:kdu_fail
echo [-] MSBuild build of KDU failed.
echo     KDU needs Visual Studio 2022 + v143 toolset ^(not just Build Tools^).
echo     If Hamakaze fails on resource.rc or MASM, install the VS workload
echo     "Desktop development with C++" with MASM enabled.
goto :build_fail

:kdu_done
echo.
echo [+] SUCCESS.
if exist "build\stayout_dll.dll" (
    echo     DLL:        build\stayout_dll.dll
) else (
    echo     DLL:        build\%CONFIG%\stayout_dll.dll
)
set "INJOUT=%~dp0injector2\%MSB_PLAT%\%CONFIG%"
if exist "%INJOUT%\loader.exe" (
    echo     Loader:     injector2\%MSB_PLAT%\%CONFIG%\loader.exe
) else (
    echo     Loader:     ^(not found^)
)
if exist "%INJOUT%\modern_injector.sys" (
    echo     Driver:     injector2\%MSB_PLAT%\%CONFIG%\modern_injector.sys
) else (
    echo     Driver:     ^(skipped - WDK required, see injector2/README.md^)
)
if exist "%KDU_BIN%\kdu.exe" (
    if exist "%KDU_BIN%\drv64.dll" (
        echo     KDU:        KDU-1.4.9\bin\kdu.exe ^(+ drv64.dll^)
    ) else (
        echo     KDU:        KDU-1.4.9\bin\kdu.exe ^(drv64.dll missing^)
    )
) else (
    echo     KDU:        ^(not found^)
)
echo     TestClient: build separately with  /p:BuildTestClient=true
echo                ^(shares a main^(^) with loader.cpp, so not in default build^)
echo.
echo Next: right-click run.bat then "Run as administrator".
echo.
pause
exit /b 0

:build_fail
echo [-] Build failed.
pause
exit /b 1
