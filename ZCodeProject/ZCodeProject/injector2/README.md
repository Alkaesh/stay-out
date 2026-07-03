<div align="center"> <h1>Kernel-Mode DLL Injector</h1> <p><em>Advanced Anti-Cheat Testing for Game Development</em></p> <img src="https://img.shields.io/badge/Windows-11-blue?style=flat-square&logo=windows" alt="Windows 11"> <img src="https://img.shields.io/badge/Language-C++-green?style=flat-square" alt="C++"> <img src="https://img.shields.io/badge/License-MIT-orange?style=flat-square" alt="MIT License"> </div>
🚀 Overview

The Kernel-Mode DLL Injector is a cutting-edge tool built in C++ to test anti-cheat systems in game development. Targeting Windows 11, it uses kernel-level techniques to inject DLLs into processes, challenging modern anti-cheats like BattlEye and EAC.

    Components:
        modern_injector.sys: Kernel driver for injection.
        loader.exe: User-mode utility to trigger it.
    Purpose: Research and anti-cheat validation.

🛠️ How It Works

The driver performs a **kernel-side manual map** and runs the DLL's DllMain via a
freshly created remote thread in the target. No XOR, no thread hijacking (the
previous design hijacked the IOCTL-handling kernel thread, which is invalid).

Flow of `modern_injector.sys` on `IOCTL_INJECT_DLL`:

    1. Read the target .dll from disk (raw PE bytes).
    2. KeStackAttachProcess(target) → ZwAllocateVirtualMemory(SizeOfImage).
    3. Map headers + each section into the target; zero-fill slack.
    4. Apply x64 base relocations (IMAGE_REL_BASED_DIR64 / HIGHLOW / ABSOLUTE).
    5. Set per-section page protection via ZwProtectVirtualMemory.
    6. Resolve kernel32!LoadLibraryA + GetProcAddress + ExitThread by walking the
       target's PEB loaded-module list (ZwQueryInformationProcess → PEB→Ldr).
    7. Copy a position-independent "loader stub" (loader_stub.asm, MASM) into a
       RWX page in the target. The stub walks the DLL's import directory, fills
       the IAT using those two kernel32 functions, then calls
       DllMain(base, DLL_PROCESS_ATTACH, NULL), then ExitThread(0).
    8. RtlCreateUserThread in the target to run the stub. The IOCTL waits (≤5s)
       for DllMain to return so it can report success/failure.

The driver uses only EXPORTED ntoskrnl routines (no undocumented
KeSuspendThread/KeGetContextThread pattern scanning), so it links cleanly under
the WDK. `loader.exe` opens `\\.\ModernInjector` and sends the IOCTL with
`{ TargetPid, DllPath }` (both from command-line args: `loader.exe <PID> <dll>`).

Perfect for simulating sophisticated injection methods in a controlled environment.
📋 Prerequisites

    OS: Windows 11 (23H2+, Build 22631.xxx)
    IDE: Visual Studio 2022
    Tools: WDK 10.0.26100.2454, Windows SDK 10.0.26100.x
    Privileges: Admin access

📂 Project Structure
text
injector2/
├── modern_injector.cpp   # Kernel driver (built with WDK; /p:BuildDriver=true)
├── loader_stub.asm       # PIC user-mode stub assembled by MASM (IAT + DllMain)
├── loader.cpp            # User-mode loader (console app, always built -> loader.exe)
├── test_client.cpp       # Optional IOCTL test client (/p:BuildTestClient=true)
├── kernelmode.vcxproj    # Single MSBuild project (Application); see notes below
├── kernelmode.sln
└── x64/Release/          # Output: loader.exe (+ modern_injector.sys with WDK)

NOTE on the single-project layout: `kernelmode.vcxproj` is an **Application**
project that lists three sources. Because `loader.cpp` and `test_client.cpp`
both define `main()`, they can't link into one .exe, so:
  - loader.cpp          -> always built (-> loader.exe, via TargetName=loader)
  - test_client.cpp     -> only with /p:BuildTestClient=true
  - modern_injector.cpp -> only with /p:BuildDriver=true (needs WDK + MASM)
The default `msbuild` invocation produces loader.exe alone.

🔧 Compilation

The whole project (DLL + injector) builds with `build.bat` at the repo root:

    build.bat            # Release: stayout_dll.dll + injector2\loader.exe (+ .sys if WDK)

`build.bat` auto-detects the WDK and passes `/p:BuildDriver=true` when present.
To build only the injector by hand:

    msbuild injector2\kernelmode.sln /p:Configuration=Release /p:Platform=x64
    # + /p:BuildDriver=true        to also build modern_injector.sys (WDK required)
    # + /p:BuildTestClient=true    to also build test_client.exe

Environment: Visual Studio 2022 + WDK 10.0.26100.x + matching Windows SDK.

▶️ Running

Use `run.bat` at the repo root (run as Administrator). It:
  - resolves the target PID (sogame.exe, or a PID argument),
  - installs/starts the `ModernInjector` driver service via `sc`,
  - invokes `loader.exe <PID> <abs_dll_path>`.

Manual equivalent (Admin CMD):

    REM one-time: enable test signing and reboot
    bcdedit /set testsigning on

    REM register + start the driver (point at the built .sys)
    sc create ModernInjector binPath= "C:\full\path\injector2\x64\Release\modern_injector.sys" type= kernel
    sc start ModernInjector

    REM run the loader
    loader.exe <PID> <C:\full\path\stayout_dll.dll>

⚠️ Troubleshooting

    sc start fails: test signing not enabled (reboot after bcdedit), or the .sys
      was built with a mismatched WDK/SDK, or the driver crashed on load
      (check Event Viewer → System).
    "Failed to open driver: 2": the ModernInjector service is not running.
    "IOCTL failed: 5": driver loaded but DeviceControl rejected the request —
      usually the target PID was not found by PsLookupProcessByProcessId.
    MASM/ml64 errors: the masm.props/.targets imports in kernelmode.vcxproj
      require "C++ build tools" with MASM enabled in the VS installer.

🤝 Contributing

    Fork → Modify → Pull Request.
    Open issues for bugs or enhancements.

📜 License

MIT License – For testing only. Use responsibly in development environments.
<div align="center"> <p><strong>Built for innovation, tested with precision.</strong></p> </div>
