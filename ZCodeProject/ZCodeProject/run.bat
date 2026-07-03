@echo off
REM ============================================================================
REM  run.bat - inject the built DLL into the Stay Out process (sogame.exe).
REM
REM  IMPORTANT: run AS ADMINISTRATOR (right-click -^> "Run as administrator").
REM  SeDebugPrivilege is required for PROCESS_ALL_ACCESS.
REM
REM  Usage:
REM      run.bat              -> sogame.exe (Release default)
REM      run.bat Debug        -> if you built with build.bat Debug
REM      run.bat Release PID  -> inject by PID instead of process name
REM
REM  After injection:
REM      - "[Stay Out] BigWorld Research Console" window appears
REM      - log is written to %TEMP%\stayout_log.txt
REM      - END  -> unload DLL
REM      - F9   -> ESP on/off
REM      - F10  -> inventory on screen
REM      - F11  -> inventory dump to log
REM      - F12  -> dir(player())/dir(inventory) to log (for attribute RE)
REM ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "TARGET=%~2"
if "%TARGET%"=="" set "TARGET=sogame.exe"

REM Ninja generator puts artifacts flat: build\stayout_dll.dll
set "DLL="
if exist "build\stayout_dll.dll" (
    set "DLL=build\stayout_dll.dll"
) else if exist "build\%CONFIG%\stayout_dll.dll" (
    set "DLL=build\%CONFIG%\stayout_dll.dll"
)
set "INJ="
if exist "build\injector\injector.exe" (
    set "INJ=build\injector\injector.exe"
) else if exist "build\injector\%CONFIG%\injector.exe" (
    set "INJ=build\injector\%CONFIG%\injector.exe"
)

if "!DLL!"=="" (
    echo [-] DLL not found. Run build.bat first.
    pause
    exit /b 1
)
if "!INJ!"=="" (
    echo [-] Injector not found. Run build.bat first.
    pause
    exit /b 1
)

echo [*] DLL:      !DLL!
echo [*] Injector: !INJ!
echo [*] Target:   %TARGET%
echo.

REM ---------------------------------------------------------------------------
REM Admin-rights check. Without elevation OpenProcess(PROCESS_ALL_ACCESS) may
REM succeed but VirtualAllocEx / WriteProcessMemory / CreateRemoteThread get
REM ERROR_ACCESS_DENIED (5). So we MUST refuse to run non-elevated rather than
REM waste an attempt that fails confusingly.
REM
REM Detection: an elevated process has the "S-1-16-12288" (High Mandatory
REM Level) SID in its token. whoami /groups contains it only when elevated.
REM ---------------------------------------------------------------------------
net session >nul 2>&1
if "%errorlevel%"=="0" goto :is_admin

echo [-] NOT running as administrator.
echo     Injection REQUIRES elevation: SeDebugPrivilege + write access to the
echo     game process memory. Without admin, VirtualAllocEx fails with
echo     ERROR_ACCESS_DENIED (5).
echo.
echo     HOW TO RUN:
echo       1) Right-click run.bat
echo       2) Choose "Run as administrator"
echo.
pause
exit /b 1

:is_admin
echo [+] Running elevated - OK.

REM Файл-протокол ВСЕГО вывода инжектора — чтобы получить точный текст без
REM скриншотов (скриншоты теряются/искажаются). Те же строки идут и в консоль.
set "INJECT_LOG=%TEMP%\inject_log.txt"
if exist "%INJECT_LOG%" del "%INJECT_LOG%"
echo [*] Full injector output will also be saved to: %INJECT_LOG%
echo.

REM Запуск инжектора: stdout+stderr пишется В ФАЙЛ, затем файл выводится в
REM консоль. Так мы имеем и точный текстовый лог (для пересылки), и живой вывод.
"!INJ!" "%TARGET%" "!DLL!" > "%INJECT_LOG%" 2>&1
set "RC=%errorlevel%"
type "%INJECT_LOG%"
echo.
echo [*] Same output saved to: %INJECT_LOG%
echo.

if "%RC%"=="0" (
    echo [+] Injection returned success. Watch the game console and
    echo     %%TEMP%%\stayout_log.txt for diagnostics output.
) else (
    echo [-] Injection returned error code %RC%.
    echo     Common causes:
    echo       - Game process not running ^(target: sogame.exe^)
    echo       - Not running as administrator
    echo       - Anti-cheat blocking OpenProcess / CreateRemoteThread
    echo       - 32-bit game vs 64-bit injector ^(injector is x64 only^)
)
echo.
echo SEND ME THE CONTENTS OF: %INJECT_LOG%
echo.
pause
exit /b %RC%
