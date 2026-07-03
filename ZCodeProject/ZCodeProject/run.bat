@echo off
REM ============================================================================
REM  run.bat - inject the built DLL into the Stay Out process (sogame.exe)
REM  via the ModernInjector kernel-mode driver mapped by KDU (Kernel Driver Utility).
REM
REM  This is a KERNEL-MODE flow (different from the old user-mode injector):
REM    1. kdu.exe -prv <P> -map maps modern_injector.sys into the kernel via a
REM       vulnerable-driver "provider" (default P=20, Dell DBUtilDrv2, WHQL).
REM    2. loader.exe opens \\.\ModernInjector and sends IOCTL_INJECT_DLL.
REM    3. The driver maps the DLL into the target process from ring 0.
REM
REM  IMPORTANT: run AS ADMINISTRATOR (right-click -^> "Run as administrator").
REM  Mapping/loading a kernel driver requires administrator rights.
REM
REM  Prerequisites:
REM    - build.bat completed successfully.
REM    - KDU-1.4.9\bin\kdu.exe (+ drv64.dll) exists.
REM    - modern_injector.sys exists.
REM
REM  Usage:
REM      run.bat                      -> sogame.exe (Release, provider 20)
REM      run.bat Debug                -> if you built with build.bat Debug
REM      run.bat Release 12345        -> inject by PID instead of process name
REM      run.bat Release sogame.exe 19-> use a different KDU provider (PROCEXP)
REM                                    (3rd arg overrides KDU_PRV; see -list)
REM ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "TARGET=%~2"
if "%TARGET%"=="" set "TARGET=sogame.exe"
set "MSB_PLAT=x64"

REM --- Locate built artifacts. ---
REM Ninja generator puts the DLL flat: build\stayout_dll.dll
set "DLL="
if exist "build\stayout_dll.dll" (
    set "DLL=build\stayout_dll.dll"
) else if exist "build\%CONFIG%\stayout_dll.dll" (
    set "DLL=build\%CONFIG%\stayout_dll.dll"
)
set "INJOUT=%~dp0injector2\%MSB_PLAT%\%CONFIG%"
set "LOADER=%INJOUT%\loader.exe"
set "DRIVER_SYS=%INJOUT%\modern_injector.sys"
set "KDU_EXE=%~dp0KDU-1.4.9\bin\kdu.exe"
set "KDU_DB=%~dp0KDU-1.4.9\bin\drv64.dll"

REM KDU vulnerable-driver provider. 20 = Dell DBUtilDrv2 (WHQL/Microsoft
REM signed, loads under DSE without test-signing). It is HW-independent and is
REM the most reliable default on an Intel CPU. See the big comment block at the
REM mapping step below for alternatives. Override on the cmdline too:
REM   run.bat Release <PID> <providerId>
set "KDU_PRV=%~3"
if "%KDU_PRV%"=="" set "KDU_PRV=20"

if "!DLL!"=="" (
    echo [-] DLL not found. Run build.bat first.
    pause
    exit /b 1
)
if not exist "%LOADER%" (
    echo [-] Loader not found: %LOADER%
    echo     Run build.bat first.
    pause
    exit /b 1
)
if not exist "%KDU_EXE%" (
    echo [-] KDU not found: %KDU_EXE%
    echo     Run build.bat first.
    pause
    exit /b 1
)
if not exist "%KDU_DB%" (
    echo [-] drv64.dll not found next to kdu.exe: %KDU_DB%
    echo     KDU refuses to run without drv64.dll in the same directory.
    echo     Run build.bat first.
    pause
    exit /b 1
)
if not exist "%DRIVER_SYS%" (
    echo [-] Driver .sys not found: %DRIVER_SYS%
    echo     Run build.bat first with WDK installed.
    pause
    exit /b 1
)

echo [*] DLL:      !DLL!
echo [*] Loader:   %LOADER%
echo [*] Driver:   %DRIVER_SYS%
echo [*] KDU:      %KDU_EXE% (provider %KDU_PRV%)
echo [*] Target:   %TARGET%
echo.

REM ---------------------------------------------------------------------------
REM Admin-rights check. Mapping the driver + opening the device require
REM elevation. Without it KDU and CreateFileW(\\.\ModernInjector) fail.
REM ---------------------------------------------------------------------------
net session >nul 2>&1
if "%errorlevel%"=="0" goto :is_admin

echo [-] NOT running as administrator.
echo     This kernel-mode flow REQUIRES elevation to:
echo       - map the ModernInjector driver with KDU
echo       - open the \\.\ModernInjector device
echo.
echo     HOW TO RUN:
echo       1) Right-click run.bat
echo       2) Choose "Run as administrator"
echo.
pause
exit /b 1

:is_admin
echo [+] Running elevated - OK.

REM ---------------------------------------------------------------------------
REM Clean up any STALE ModernInjector service from a previous run. KDU does a
REM MANUAL map (it does not register an SCM service), so no ModernInjector
REM service should exist. If an old run (or the legacy sc-based flow) left one
REM behind and Running, it occupies the \Device\ModernInjector name and the
REM KDU-mapped DriverEntry then fails IoCreateDevice with NAME_COLLISION - the
REM device never appears and loader.exe gets error 2. So stop+delete it first.
REM This is harmless if no service exists.
REM ---------------------------------------------------------------------------
echo [*] Cleaning up any stale ModernInjector service...
sc stop ModernInjector >nul 2>&1
sc delete ModernInjector >nul 2>&1
REM Also drop leftover KDU provider services from previous KDU runs.
sc stop DBUtilDrv2 >nul 2>&1
sc delete DBUtilDrv2 >nul 2>&1
sc stop PROCEXP152 >nul 2>&1
sc delete PROCEXP152 >nul 2>&1

REM ---------------------------------------------------------------------------
REM Resolve the target PID. loader.exe needs a numeric PID.
REM   - If %TARGET% is numeric, use it directly.
REM   - Otherwise look it up via tasklist (first match).
REM ---------------------------------------------------------------------------
set "PID="
echo %TARGET%| findstr /r "^[0-9][0-9]*$" >nul
if "%errorlevel%"=="0" (
    set "PID=%TARGET%"
) else (
    echo [*] Looking up PID for %TARGET% ...
    for /f "tokens=2" %%p in ('tasklist /fi "imagename eq %TARGET%" /fo list 2^>nul ^| findstr /i "PID:"') do (
        if not defined PID set "PID=%%p"
    )
    if not defined PID (
        echo [-] Process not found: %TARGET%
        echo     Make sure the game is running, or pass a PID: run.bat %CONFIG% 12345
        pause
        exit /b 1
    )
)
echo [+] Target PID: %PID%
echo.

REM ---------------------------------------------------------------------------
REM Map the kernel driver through KDU. loader.exe can only work after this
REM creates the \\.\ModernInjector device.
REM
REM KDU runs from KDU-1.4.9\bin\ where kdu.exe sits next to drv64.dll (its
REM drivers database). -map maps the .sys using a "vulnerable driver" provider
REM that KDU abuses to gain kernel read/write.
REM
REM PROVIDER CHOICE: the default provider (Intel NAL, id 0) is unsigned and is
REM rejected by Driver Signature Enforcement on stock Windows 10/11 with
REM NTSTATUS 0xC0000022 (STATUS_ACCESS_DENIED) at NtLoadDriver. We therefore
REM use a WHQL (Microsoft-signed) provider, which loads under DSE without
REM test-signing. NOTE: only the SIGNATURE matters for loading, BUT some
REM providers additionally gate on CPU/hardware:
REM   - AMD providers (30 RyzenMaster, 44/45 AMD PDFW/AOD, 23 HW64-AMD ...) have
REM     a ValidatePrerequisites callback that aborts with "AMD CPU is required"
REM     on an Intel box. Do NOT use them on Intel.
REM   - Default 20 = Dell DBUtilDrv2 (DBUtil_2_5) - WHQL, HW-independent, the
REM     most reliable on Intel. WHQL fallbacks (all HW-independent): 19
REM     (Process Explorer / PROCEXP152), 21 (DBK/DebugView CEDRIVER73),
REM     27 (PassMark OSForensics). Run "kdu.exe -list" for the full table.
REM
REM NOTE: kdu.exe always returns exit code 0 even when mapping fails, so we
REM cannot trust %errorlevel%. Instead we scan the captured log: the success
REM path prints "Executing shellcode", while a load failure prints a known
REM error string (see LOAD_ERR / PROV_ERR / MAP_OK below). loader.exe is
REM only launched when the map looks successful - otherwise there is no
REM \\.\ModernInjector device.
REM ---------------------------------------------------------------------------
set "KDU_LOG=%TEMP%\kdu_log.txt"
if exist "%KDU_LOG%" del "%KDU_LOG%"
echo [*] Mapping ModernInjector with KDU (provider %KDU_PRV%) ...
"%KDU_EXE%" -prv %KDU_PRV% -map "%DRIVER_SYS%" > "%KDU_LOG%" 2>&1
type "%KDU_LOG%"
echo.

REM Detect the well-known "input driver file" load failure. KDU loads the .sys
REM via LdrLoadDll(IMAGE_FILE_EXECUTABLE_IMAGE) to read its PE bytes, which runs
REM it through Code Integrity. The .sys is unsigned and carries no
REM FORCE_INTEGRITY flag (see modern_injector.vcxproj - /INTEGRITYCHECK is
REM intentionally omitted), so on a normal machine it loads fine. If CI still
REM rejects it with 0xC0000428 (STATUS_INVALID_IMAGE_HASH), either the .sys was
REM rebuilt WITH /INTEGRITYCHECK, or test signing / DSE got toggled.
set "LOAD_ERR="
findstr /c:"Error while loading input driver file" "%KDU_LOG%" >nul
if not errorlevel 1 set "LOAD_ERR=1"
findstr /c:"0xC0000428" "%KDU_LOG%" >nul
if not errorlevel 1 set "LOAD_ERR=1"

if defined LOAD_ERR (
    echo.
    echo [-] KDU could not load the driver file: Code Integrity rejected it.
    echo     0xC0000428 = STATUS_INVALID_IMAGE_HASH. This means the .sys still
    echo     carries the FORCE_INTEGRITY PE flag, OR the driver was tampered with.
    echo     Rebuild the driver WITHOUT /INTEGRITYCHECK:
    echo       - check modern_injector.vcxproj: the AdditionalOptions line must
    echo         NOT contain /INTEGRITYCHECK ^(it is omitted by default^).
    echo       - run build.bat so the .sys is relinked, then run.bat again.
    echo     Full log: %KDU_LOG%
    echo.
    pause
    exit /b 1
)

REM Detect the "vulnerable driver" load / prerequisite failure. KDU registers
REM and loads its chosen provider (a third-party signed-but-abusable driver)
REM via NtLoadDriver, and may also run a ValidatePrerequisites callback. Two
REM distinct failure modes land here:
REM   (a) 0xC0000022 STATUS_ACCESS_DENIED - DSE/VBS rejected the provider driver.
REM   (b) "AMD CPU is required" / "prerequisites are not meet" - the provider is
REM       hardware-gated (e.g. all AMD providers abort on an Intel CPU).
REM Fix for both: switch to another WHQL-signed, HW-INDEPENDENT provider by
REM editing KDU_PRV above. Do NOT use AMD providers (30,44,45,23...) on Intel.
REM WHQL + HW-independent fallbacks: 20=Dell DBUtil, 19=Process Explorer,
REM 21=DBK/DebugView, 27=PassMark OSForensics. Run "kdu.exe -list" for the rest.
set "PROV_ERR="
findstr /c:"Unable to load vulnerable driver" "%KDU_LOG%" >nul
if not errorlevel 1 set "PROV_ERR=1"
findstr /c:"0xC0000022" "%KDU_LOG%" >nul
if not errorlevel 1 set "PROV_ERR=1"
findstr /c:"CPU is required" "%KDU_LOG%" >nul
if not errorlevel 1 set "PROV_ERR=1"
findstr /c:"prerequisites are not meet" "%KDU_LOG%" >nul
if not errorlevel 1 set "PROV_ERR=1"

if defined PROV_ERR (
    echo.
    echo [-] KDU could not start provider ^(KDU_PRV=%KDU_PRV%^).
    echo     This is either STATUS_ACCESS_DENIED (0xC0000022: DSE/VBS rejected the
    echo     provider driver) OR a hardware gate (e.g. "AMD CPU is required" on an
    echo     Intel box). AMD providers do NOT work on Intel.
    echo     Switch to a WHQL + HW-independent provider: edit KDU_PRV above, or run
    echo       "KDU-1.4.9\bin\kdu.exe" -list
    echo     Recommended Intel-safe ids: 20 ^(Dell DBUtil^), 19 ^(Process Explorer^),
    echo     21 ^(DBK/DebugView^), 27 ^(PassMark OSForensics^).
    echo     Full log: %KDU_LOG%
    echo.
    pause
    exit /b 1
)

REM Success/failure detection. kdu.exe ALWAYS exits 0 and even prints
REM "Executing shellcode" + "Shellcode result: NTSTATUS (0x0)" - but that only
REM means KDU's OWN bootstrap ran. It does NOT prove our DriverEntry created the
REM \\.\ModernInjector device (the device can still fail to appear, e.g. from a
REM stale service name collision). The ONLY reliable success check is to probe
REM the device itself. We wait a moment, then try to open \\.\ModernInjector.
echo [*] Verifying \\.\ModernInjector device was created by the driver...
timeout /t 1 /nobreak >nul 2>&1
set "MAP_OK="
powershell -NoProfile -Command "try { $h=[System.IO.File]::Open('\\.\ModernInjector','Open','ReadWrite','None'); $h.Close(); exit 0 } catch { exit 1 }" >nul 2>&1
if "%errorlevel%"=="0" set "MAP_OK=1"

if not defined MAP_OK (
    echo.
    echo [!] KDU ran but the \\.\ModernInjector device was NOT created.
    echo     This means KDU's shellcode launched but our DriverEntry did not
    echo     succeed ^(IoCreateDevice/IoCreateDriver failed^), most often because
    echo     a stale ModernInjector service still owns the device name.
    echo     This run.bat already attempted sc stop/delete - if it still fails,
    echo     REBOOT and run run.bat once more, or check %KDU_LOG% for clues.
    echo.
    pause
    exit /b 1
)
echo [+] \\.\ModernInjector device is live - KDU mapped ModernInjector successfully.
echo [*] KDU output saved to: %KDU_LOG%
echo.

REM ---------------------------------------------------------------------------
REM Run the loader. stdout+stderr -> file, then echo the file, so we have both
REM a precise text log (for sending back) and live console output.
REM ---------------------------------------------------------------------------
:do_inject
set "INJECT_LOG=%TEMP%\inject_log.txt"
if exist "%INJECT_LOG%" del "%INJECT_LOG%"
echo [*] Full loader output will also be saved to: %INJECT_LOG%
echo.

REM Resolve DLL to an absolute path for the driver (kernel path resolution).
set "DLLABS=!DLL!"
for %%I in ("!DLL!") do set "DLLABS=%%~fI"

"%LOADER%" %PID% "!DLLABS!" > "%INJECT_LOG%" 2>&1
set "RC=%errorlevel%"
type "%INJECT_LOG%"
echo.
echo [*] Same output saved to: %INJECT_LOG%
echo.

if "%RC%"=="0" (
    echo [+] Loader returned success. Watch the game console and
    echo     %%TEMP%%\stayout_log.txt for diagnostics output.
) else (
    echo [-] Loader returned error code %RC%.
    echo     Common causes:
    echo       - Driver mapped but IOCTL_INJECT_DLL failed inside the driver
    echo       - Game process not running ^(target: sogame.exe^)
    echo       - Not running as administrator
    echo       - Check DebugView / WinDbg for kernel-side errors
)
echo.
echo SEND ME THE CONTENTS OF: %INJECT_LOG%
echo.
pause
exit /b %RC%
