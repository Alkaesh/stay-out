// =============================================================================
//  main.cpp — точка входа DLL и логика рабочего потока.
//  Только WinAPI, C++20.
// =============================================================================

#include <Windows.h>

#include <atomic>
#include <cstdio>

#include "memory.hpp"
#include "hooks.hpp"

// Глобальный флаг мягкого завершения потока (используется при DETACH).
static std::atomic<bool> g_shouldUnload{ false };

// Дескриптор потока консоли (для чистого fclose при выгрузке).
static FILE* g_consoleStream = nullptr;

// -----------------------------------------------------------------------------
//  Отладочная консоль. AllocConsole создаёт новое окно консоли для DLL-процесса.
//  Удобно для логов в песочнице без отладчика.
// -----------------------------------------------------------------------------
static void InitDebugConsole()
{
    if (AllocConsole())
    {
        // Перенаправляем stdout/stderr в новую консоль.
        // freopen_s — безопасная (secure) замена устаревшему freopen.
        freopen_s(&g_consoleStream, "CONOUT$", "w", stdout);
        freopen_s(&g_consoleStream, "CONOUT$", "w", stderr);
        SetConsoleTitleW(L"Internal DLL — Debug Console");
    }
}

static void ShutdownDebugConsole()
{
    if (g_consoleStream)
    {
        fclose(g_consoleStream);
        g_consoleStream = nullptr;
    }
    FreeConsole();
}

// -----------------------------------------------------------------------------
//  Рабочий поток. Вся логика крутится здесь, т.к. DllMain блокировать нельзя:
//  загрузчик ОС держит loader-lock, и долгие операции в DllMain приведут к
//  дедлоку или крашу.
//
//  lpParam — дескриптор модуля DLL (HMODULE), переданный из DllMain.
//            Нужен для безопасной само-выгрузки в конце.
// -----------------------------------------------------------------------------
static DWORD WINAPI WorkerThread(LPVOID lpParam)
{
    InitDebugConsole();
    std::printf("[*] Worker thread started.\n");

    // --- Инициализация модулей ---
    if (!Hooks::Init())
    {
        std::printf("[-] Hooks::Init() failed.\n");
    }

    // Пример использования pattern-сканера (закомментировано — впиши свой модуль):
    //
    //   const uintptr_t addr = Memory::PatternScanner::Find(
    //       L"StayOut.exe", "45 8B ? ? 00", /*addition=*/0);
    //   if (addr)
    //       std::printf("[+] Signature found at 0x%p\n", reinterpret_cast<void*>(addr));
    //   else
    //       std::printf("[-] Signature not found.\n");

    // --- Главный цикл: слушаем VK_END ---
    // GetAsyncKeyState(...) & 0x8000 → старший бит = клавиша сейчас нажата.
    while (!g_shouldUnload.load(std::memory_order_relaxed))
    {
        if (GetAsyncKeyState(VK_END) & 0x8000)
        {
            std::printf("[*] Unload key (END) pressed. Shutting down...\n");
            break;
        }

        Sleep(50); // берегём CPU: 20 проверок в секунду достаточно
    }

    // --- Деинициализация: снимаем хуки и консоль ДО выгрузки DLL ---
    Hooks::Shutdown();
    ShutdownDebugConsole();

    // --- Безопасная само-выгрузка ---
    // FreeLibraryAndExitThread делает две атомарные вещи:
    //   1) FreeLibrary(thisModule) — уменьшает счётчик ссылок DLL;
    //   2) ExitThread(0)           — завершает текущий поток.
    // Атомарность критична: иначе между FreeLibrary и ExitThread поток мог бы
    // выполнить код в уже выгруженной DLL → краш.
    const HMODULE hModule = static_cast<HMODULE>(lpParam);
    if (hModule)
    {
        FreeLibraryAndExitThread(hModule, 0);
        // После этого вызова возврата не происходит.
    }

    return 0;
}

// -----------------------------------------------------------------------------
//  Точка входа DLL.
// -----------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID /*lpReserved*/)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
    {
        // Не интересуемся уведомлениями о создании/завершении потоков —
        // это лишний overhead и потенциальный источник дедлоков.
        DisableThreadLibraryCalls(hModule);

        // Запускаем рабочий поток. Дескриптор hModule передаём как параметр,
        // чтобы поток мог корректно выгрузить саму DLL по VK_END.
        if (HANDLE hThread = CreateThread(
                nullptr,        // default security
                0,              // default stack size
                WorkerThread,
                hModule,        // параметр потока
                0,              // запустить сразу
                nullptr))       // thread id не нужен
        {
            CloseHandle(hThread); // дескриптор не нужен — сразу освобождаем
        }
        break;
    }

    case DLL_PROCESS_DETACH:
    {
        // Сигнализируем потоку о мягком завершении.
        // ВНИМАНИЕ: здесь нельзя ждать завершения потока или вызывать
        // FreeLibrary — процесс выгружается, loader-lock занят.
        g_shouldUnload.store(true, std::memory_order_relaxed);
        break;
    }

    default:
        break;
    }

    return TRUE;
}
