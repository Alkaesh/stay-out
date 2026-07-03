// =============================================================================
//  main.cpp — CLI для Manual Map инжектора.
//
//  Использование:
//     injector.exe <PID|имя_процесса.exe> <путь_к_dll>
//
//  Примеры:
//     injector.exe 12345 internal_dll.dll
//     injector.exe game.exe internal_dll.dll
// =============================================================================

#include <Windows.h>

#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <string>

#include "manual_map.hpp"

namespace {

void print_usage() {
    std::wcout << L"Использование: injector.exe <PID|имя_процесса.exe> <путь_к_dll>\n";
}

bool is_number(std::wstring_view s) {
    if (s.empty()) return false;
    for (wchar_t c : s)
        if (!std::iswdigit(c)) return false;
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Корректный вывод кириллицы.
    SetConsoleOutputCP(CP_UTF8);
    std::wcout.imbue(std::locale(""));

    if (argc < 3) {
        print_usage();
        return EXIT_FAILURE;
    }

    // 1. Включаем SeDebugPrivilege — нужно для защищённых/системных процессов.
    if (injector::enable_debug_privilege()) {
        std::wcout << L"[+] SeDebugPrivilege включена.\n";
    } else {
        std::wcout << L"[!] Не удалось включить SeDebugPrivilege (запусти от админа).\n";
    }

    const std::wstring target = argv[1];
    const std::filesystem::path dll_path = argv[2];

    // Резолвим цель: PID или имя процесса.
    DWORD pid = 0;
    if (is_number(target)) {
        pid = static_cast<DWORD>(std::wcstoul(target.c_str(), nullptr, 10));
    } else {
        // Если процесса нет, inject_by_name вернёт ошибку сам.
        std::wcout << L"[*] Поиск процесса по имени: " << target << L"\n";
    }

    // Конфиг инъекции.
    injector::InjectOptions opts;
    opts.erase_headers     = true;   // Стереть PE-заголовки (stealth)
    opts.wipe_loader_stub  = true;   // Освободить shellcode-стаб (stealth)
    opts.wait_for_entry    = true;   // Ждать завершения DllMain
    opts.prefer_image_base = false;  // Маппить по любому адресу (надёжнее)

    std::wcout << L"[*] Загружаемая DLL: " << dll_path.wstring() << L"\n";
    std::wcout << L"[*] Stealth: erase_headers=" << opts.erase_headers
              << L", wipe_stub=" << opts.wipe_loader_stub << L"\n";

    const auto result = pid
        ? injector::inject(pid, dll_path, opts)
        : injector::inject_by_name(target, dll_path, opts);

    if (result.ok) {
        std::wcout << L"[+] Инъекция успешна. Замапплено по адресу 0x"
                  << std::hex << result.mapped_base << std::dec << L"\n";
        if (opts.wait_for_entry) {
            std::wcout << L"[+] DllMain вернул код: " << result.entry_exit_code << L"\n";
        }
        std::wcout << L"[+] DLL скрыта: не в PEB, заголовки стёрты, стаб очищен.\n";
        return EXIT_SUCCESS;
    }

    std::wcerr << L"[-] Error.\n";
    std::cerr << "    " << result.error << "\n";
    return EXIT_FAILURE;
}
