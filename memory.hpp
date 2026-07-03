#pragma once

// =============================================================================
//  memory.hpp — утилиты работы с памятью (чтение/запись/сканирование паттернов)
//  Только WinAPI, без внешних зависимостей. C++20.
// =============================================================================

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Memory
{
    // -------------------------------------------------------------------------
    //  Чтение/запись скаляров в адресном пространстве текущего процесса.
    //  Так как DLL инжектируется внутрь процесса, обращения идут напрямую
    //  через указатель — ReadProcessMemory здесь не нужен (он медленнее).
    //  Квалификатор `volatile` отключает агрессивную оптимизацию компилятора,
    //  чтобы значение действительно читалось из памяти каждый раз.
    // -------------------------------------------------------------------------
    template <typename T>
    inline T Read(uintptr_t address)
    {
        return *reinterpret_cast<volatile T*>(address);
    }

    template <typename T>
    inline void Write(uintptr_t address, const T& value)
    {
        *reinterpret_cast<volatile T*>(address) = value;
    }

    // Чтение null-terminated строки из памяти (до maxLen байт).
    std::string ReadString(uintptr_t address, std::size_t maxLen = 256);

    // -------------------------------------------------------------------------
    //  Pattern Scanner
    // -------------------------------------------------------------------------
    namespace PatternScanner
    {
        // Внутреннее представление разобранной сигнатуры.
        struct Pattern
        {
            std::vector<std::uint8_t> bytes;  // байты (на месте '?' лежит 0)
            std::vector<bool>         mask;   // true → байт значимый, false → wildcard
        };

        // Разбор строки в стиле IDA ("45 8B ? ? 00") во внутреннюю структуру.
        // '?' и '??" — wildcard (любой байт).
        Pattern Parse(std::string_view signature);

        // Поиск по имени модуля ("StayOut.exe", "engine.dll", ...).
        // addition — смещение, прибавляемое к найденному адресу (удобно для
        //            ручного учёта offset внутри сигнатуры).
        // Возвращает 0, если совпадений нет.
        uintptr_t Find(const wchar_t* moduleName,
                       std::string_view signature,
                       std::ptrdiff_t addition = 0);

        // Перегрузка: явный базовый адрес и размер модуля (без GetModuleHandle).
        uintptr_t Find(uintptr_t moduleBase,
                       std::size_t moduleSize,
                       std::string_view signature,
                       std::ptrdiff_t addition = 0);
    } // namespace PatternScanner
} // namespace Memory
