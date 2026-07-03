#pragma once

// =============================================================================
//  manual_map.hpp — Публичный API инжектора Manual Map.
// =============================================================================

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace injector {

struct InjectOptions {
    bool erase_headers = true;
    bool wipe_loader_stub = true;
    bool wait_for_entry = true;
    bool prefer_image_base = false;

    /// Зарегистрировать SEH-таблицу DLL (RtlAddFunctionTable) в целевом
    /// процессе. Нужно, если CRT DLL использует SEH-исключения — иначе
    /// необработанное исключение крашнет процесс.
    ///
    /// ВНИМАНИЕ: в текущей реализации ФЛАГ СОхранён, но вызов не реализован —
    /// требует C-bootstrap-функции с собственными релоками (см. TODO в .cpp).
    /// Для DLL без CRT (как stayout_dll) SEH не требуется.
    bool register_seh_table = false;

    /// Вызвать TLS-колбэки DLL перед DllMain.
    /// ВНИМАНИЕ: флаг сохранён, но вызов не реализован по той же причине.
    /// Для DLL без thread_local CRT-объектов TLS не требуется.
    bool call_tls_callbacks = false;
};

/// Результат инъекции.
struct InjectResult {
    bool        ok        = false;
    std::string error;             ///< Описание ошибки, если ok == false
    std::uintptr_t mapped_base = 0; ///< Адрес, куда замаппили DLL в целевом процессе
    DWORD       entry_exit_code = 0; ///< Код возврата DllMain (если wait_for_entry)
};

/// Главная точка: маппит DLL в процесс с заданным PID.
InjectResult inject(DWORD pid, const std::filesystem::path& dll_path,
                    const InjectOptions& opts = {});

/// Удобная перегрузка: ищет процесс по имени (например, "game.exe").
/// Берёт первый совпавший. Если процессов несколько — используй inject() по PID.
InjectResult inject_by_name(std::wstring_view process_name,
                            const std::filesystem::path& dll_path,
                            const InjectOptions& opts = {});

/// Включает привилегию SeDebugPrivilege в токене текущего процесса —
/// нужна для OpenProcess(PROCESS_ALL_ACCESS) над защищёнными процессами.
bool enable_debug_privilege() noexcept;

} // namespace injector
