/* probe.c — расширенный диагностический пробник перед инжекцией.
 * Компилируется diag.bat, запускается ОТ АДМИНИСТРАТОРА.
 *
 * Цель: при err=5 на VirtualAllocEx (даже с SeDebugPrivilege) точно выяснить,
 * КАКАЯ защита блокирует запись в sogame.exe. Проверяет матрицу:
 *   1. Каждое право по отдельности (PROCESS_VM_OPERATION/WRITE/READ/CREATE_THREAD)
 *      → узнаём, не стрипает ли античит конкретные rights через ObRegisterCallbacks.
 *   2. ReadProcessMemory → работает ли чтение.
 *   3. VirtualAllocEx с разными protection (RWX vs RW) → блокируется ли именно exec.
 *   4. WriteProcessMemory в выделенный регион → работает ли запись.
 *
 * Используем W-варианты структур SDK.
 */
#define _WIN32_WINNT 0x0602
#define WINVER       0x0602

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

static DWORD find_pid(const wchar_t* wname) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W p; p.dwSize = sizeof(p);
    DWORD pid = 0;
    if (Process32FirstW(s, &p)) {
        do {
            if (_wcsicmp(p.szExeFile, wname) == 0) pid = p.th32ProcessID;
        } while (Process32NextW(s, &p));
    }
    CloseHandle(s);
    return pid;
}

static void try_open(DWORD pid, DWORD access, const char* label) {
    HANDLE h = OpenProcess(access, FALSE, pid);
    DWORD e = h ? 0 : GetLastError();
    printf("    OpenProcess(%-32s): %s err=%lu\n",
           label, h ? "OK" : "FAIL", e);
    if (h) CloseHandle(h);
}

int main(void) {
    const wchar_t* proc = L"sogame.exe";

    /* 1. SeDebugPrivilege. */
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        printf("[-] OpenProcessToken err=%lu\n", GetLastError());
        return 1;
    }
    LUID luid;
    LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &luid);
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    DWORD e = GetLastError();
    printf("[1] SeDebugPrivilege: %s (err=%lu)\n",
           e == ERROR_SUCCESS ? "ENABLED" :
           e == ERROR_NOT_ALL_ASSIGNED ? "NOT_ALL_ASSIGNED (run elevated!)" :
           "error", e);
    CloseHandle(tok);

    /* 2. PID. */
    DWORD pid = find_pid(proc);
    if (!pid) {
        printf("[-] sogame.exe not found. Is the game running?\n");
        return 1;
    }
    printf("[2] sogame.exe PID = %lu\n\n", pid);

    /* 3. Матрица прав: узнаём, не стрипает ли античит конкретные биты. */
    printf("[3] Access-rights matrix (detects ObRegisterCallbacks stripping):\n");
    try_open(pid, PROCESS_ALL_ACCESS,          "PROCESS_ALL_ACCESS");
    try_open(pid, PROCESS_VM_OPERATION,        "PROCESS_VM_OPERATION");
    try_open(pid, PROCESS_VM_WRITE,            "PROCESS_VM_WRITE");
    try_open(pid, PROCESS_VM_READ,             "PROCESS_VM_READ");
    try_open(pid, PROCESS_CREATE_THREAD,       "PROCESS_CREATE_THREAD");
    try_open(pid, PROCESS_QUERY_INFORMATION,   "PROCESS_QUERY_INFORMATION");
    printf("\n");

    /* 4. Реальные операции на handle с PROCESS_ALL_ACCESS. */
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) {
        printf("[-] Cannot open with PROCESS_ALL_ACCESS (err=%lu).\n",
               GetLastError());
        printf("    Если VM_OPERATION/VM_WRITE FAIL — античит ObRegisterCallbacks\n");
        printf("    стрипает права. Manual Map НЕ ВОЗМОЖЕН без driver-level инжекта.\n");
        return 1;
    }
    printf("[4] Got PROCESS_ALL_ACCESS handle. Probing real operations:\n\n");

    /* 4a. Чтение первых 8 байта из базового адреса процесса. */
    HMODULE hMod = NULL;
    DWORD cbNeeded = 0;
    /* EnumProcessModules нужен psapi — обойдёмся без него: берём base через
     * GetModuleHandle(NULL) НЕ подходит (это наш процесс). Используем
     * QueryFullProcessImageName + NtQueryInformationProcess слишком сложно.
     * Проще: читать по типичному base 0x140000000 (x64 exe ImageBase). Если
     * не там — ReadProcessMemory просто упадёт, нас интересует сам факт err. */
    uintptr_t probe_addr = 0x140000000ULL;
    unsigned char buf[8] = {0};
    SIZE_T rd = 0;
    BOOL ok = ReadProcessMemory(h, (LPCVOID)probe_addr, buf, 8, &rd);
    printf("    ReadProcessMemory @0x%llx: %s err=%lu (got %zu bytes)\n",
           (unsigned long long)probe_addr, ok ? "OK" : "FAIL",
           ok ? 0 : GetLastError(), (size_t)rd);

    /* 4b. VirtualAllocEx с RWX (как делает инжектор). */
    void* a1 = VirtualAllocEx(h, NULL, 0x1000,
                              MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    DWORD e1 = a1 ? 0 : GetLastError();
    printf("    VirtualAllocEx(PAGE_EXECUTE_READWRITE): %s err=%lu\n",
           a1 ? "OK" : "FAIL", e1);
    if (a1) VirtualFreeEx(h, a1, 0, MEM_RELEASE);

    /* 4c. VirtualAllocEx с RW (без exec). Если это работает — блокируется
     *     именно executable-память (CFG / dynamic code policy). */
    void* a2 = VirtualAllocEx(h, NULL, 0x1000,
                              MEM_COMMIT | MEM_RESERVE,
                              PAGE_READWRITE);
    DWORD e2 = a2 ? 0 : GetLastError();
    printf("    VirtualAllocEx(PAGE_READWRITE):        %s err=%lu\n",
           a2 ? "OK" : "FAIL", e2);

    /* 4d. WriteProcessMemory в RW-регион. */
    if (a2) {
        SIZE_T wr = 0;
        unsigned char payload[4] = {0x90, 0x90, 0x90, 0x90};
        BOOL wok = WriteProcessMemory(h, a2, payload, 4, &wr);
        printf("    WriteProcessMemory(into RW region):   %s err=%lu (wrote %zu)\n",
               wok ? "OK" : "FAIL", wok ? 0 : GetLastError(), (size_t)wr);
        VirtualFreeEx(h, a2, 0, MEM_RELEASE);
    }
    printf("\n");

    /* 5. Итоговый диагноз. */
    printf("[5] DIAGNOSIS:\n");
    if (a1) {
        printf("    -> VirtualAllocEx(RWX) работает. Инжектор должен был пройти.\n");
        printf("       Если всё равно err=5 в инжекторе — проблема в размере/адресе.\n");
    } else if (a2) {
        printf("    -> RWX блокируется, но RW работает.\n");
        printf("       Защита: Control Flow Guard / ProcessDynamicCodePolicy.\n");
        printf("       Manual Map невозможен без изменения protection-region.\n");
    } else {
        printf("    -> ВСЕ alloc-операции err=5 даже с SeDebugPrivilege.\n");
        printf("       Защита: kernel-mode античит (ObRegisterCallbacks / PsSet* ).\n");
        printf("       Manual Map из user-mode НЕВОЗМОЖЕН.\n");
        printf("       Варианты: kernel-driver инжектор, или играть в офлайн/песочнице\n");
        printf("       с выключенным античитом.\n");
    }

    CloseHandle(h);
    return 0;
}
