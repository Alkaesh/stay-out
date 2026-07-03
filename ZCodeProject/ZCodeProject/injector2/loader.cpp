#include <windows.h>
#include <iostream>
#include <string>
#include <vector>

#define IOCTL_INJECT_DLL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

struct INJECTION_REQUEST {
    ULONG TargetPid;
    WCHAR DllPath[260];
};

// =============================================================================
//  User-mode loader for the ModernInjector kernel driver.
//
//  Usage:
//     loader.exe <PID> <full\path\to\dll>
//
//  If no arguments are given, falls back to the hardcoded defaults below
//  (useful for quick manual testing).
//
//  Prerequisites:
//     - The ModernInjector driver must already be mapped by run.bat.
//       Otherwise CreateFileW fails with error 2/5.
// =============================================================================
int main(int argc, char** argv) {
    // Defaults — overridden by argv when present.
    ULONG targetPid = 1234;
    const wchar_t* dllPath = L"C:\\path\\to\\your\\test.dll";

    // Wide copy of argv[2] for the request struct (driver takes WCHAR path).
    std::wstring dllPathWide;

    if (argc >= 2) {
        targetPid = static_cast<ULONG>(strtoul(argv[1], nullptr, 10));
        if (targetPid == 0) {
            std::cerr << "Invalid PID: " << argv[1] << std::endl;
            return 1;
        }
    }
    if (argc >= 3) {
        // Convert the ANSI dll path from argv[2] to a wide string.
        // MultiByteToWideChar wants a non-const LPWSTR, so use a raw buffer.
        const int wlen = MultiByteToWideChar(CP_ACP, 0, argv[2], -1, nullptr, 0);
        if (wlen <= 0) {
            std::cerr << "Invalid DLL path." << std::endl;
            return 1;
        }
        std::vector<wchar_t> buf(static_cast<size_t>(wlen));
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, buf.data(), wlen);
        dllPathWide.assign(buf.data());
        dllPath = dllPathWide.c_str();
    }

    if (argc < 3) {
        std::cerr << "Usage: loader.exe <PID> <full\\path\\to\\dll>" << std::endl;
        std::cerr << "No args given - using hardcoded defaults "
                  << "(PID=" << targetPid << ")." << std::endl;
    }

    HANDLE hDevice = CreateFileW(L"\\\\.\\ModernInjector", GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open driver: " << GetLastError() << std::endl;
        std::cerr << "Was ModernInjector mapped successfully by kdmapper?" << std::endl;
        return 1;
    }

    INJECTION_REQUEST Request = { 0 };
    Request.TargetPid = targetPid;
    if (wcslen(dllPath) >= 260) {
        std::cerr << "DLL path too long (max 259 chars)." << std::endl;
        CloseHandle(hDevice);
        return 1;
    }
    wcscpy_s(Request.DllPath, dllPath);

    std::cout << "[*] Requesting injection: PID=" << targetPid
              << " DLL=" << Request.DllPath << std::endl;

    DWORD BytesReturned;
    if (!DeviceIoControl(hDevice, IOCTL_INJECT_DLL, &Request, sizeof(Request),
        NULL, 0, &BytesReturned, NULL)) {
        std::cerr << "IOCTL failed: " << GetLastError() << std::endl;
        CloseHandle(hDevice);
        return 1;
    }

    std::cout << "DLL injection requested successfully!" << std::endl;
    CloseHandle(hDevice);
    return 0;
}


