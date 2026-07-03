@echo off
REM ============================================================================
REM  diag.bat - pre-injection diagnostic probe. Run AS ADMINISTRATOR.
REM
REM  Compiles probe.c (already in this folder) and runs it against sogame.exe.
REM  Reports EXACTLY which step of injection would fail:
REM    SeDebugPrivilege? OpenProcess? VirtualAllocEx?
REM  This isolates the cause: missing admin / anti-cheat / process protection.
REM ============================================================================
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if "%errorlevel%"=="0" goto :is_admin
echo [-] NOT running as administrator.
echo     Right-click diag.bat then "Run as administrator".
pause
exit /b 1

:is_admin
echo [+] Running elevated - OK.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do set "VSINSTALL=%%i"
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto :no_vs

echo [*] Compiling probe.c ...
call "%VCVARS%" x64 >nul 2>nul
cl /nologo /Fe:probe.exe probe.c advapi32.lib >nul 2>&1
if not exist probe.exe goto :compile_fail

echo [*] Running probe against sogame.exe:
echo ====================================
probe.exe
echo ====================================
echo.
echo Interpretation:
echo   SeDebug err=0 + VirtualAllocEx OK  -> injection WILL work, retry run.bat
echo   SeDebug err=1300                   -> still not elevated
echo   OpenProcess err=5                  -> anti-cheat / PPL blocking access
echo   VirtualAllocEx err=5               -> kernel-level write protection
echo.
pause
exit /b 0

:no_vs
echo [-] Build Tools / vcvarsall not found. Run build.bat first.
pause
exit /b 1

:compile_fail
echo [-] probe.exe compile failed.
pause
exit /b 1
