// =============================================================================
//  memory.cpp — реализация утилит памяти и pattern-сканера
// =============================================================================
#include "memory.hpp"

#include <algorithm>   // (std::max)/(std::min) — в скобках, чтобы не словить
                       // макросы min/max из Windows.h
#include <Psapi.h>     // GetModuleInformation (линковать с Psapi.lib)

#pragma comment(lib, "Psapi.lib")

namespace Memory
{
    std::string ReadString(uintptr_t address, std::size_t maxLen)
    {
        std::string result;
        result.reserve(maxLen);

        for (std::size_t i = 0; i < maxLen; ++i)
        {
            const char c = *reinterpret_cast<volatile char*>(address + i);
            if (c == '\0')
                break;
            result.push_back(c);
        }
        return result;
    }

    namespace PatternScanner
    {
        // -------------------------------------------------------------------------
        //  Парсер сигнатуры.
        //  Формат: hex-байты через пробел, '?' — любой байт.
        //  Пример: "45 8B ? ? 00" → bytes={0x45,0x8B,0,0,0x00} mask={T,T,F,F,T}
        // -------------------------------------------------------------------------
        Pattern Parse(std::string_view signature)
        {
            Pattern pattern{};
            pattern.bytes.reserve(signature.size() / 2);

            // Локальная лямбда: hex-цифра → значение 0..15 (или -1 при ошибке).
            const auto hexVal = [](char ch) -> int
            {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                return -1;
            };

            for (std::size_t i = 0; i < signature.size(); ++i)
            {
                const char c = signature[i];

                if (c == ' ')
                    continue; // разделитель

                if (c == '?')
                {
                    // Wildcard: кладём плейсхолдер и снимаем маску.
                    pattern.bytes.push_back(0x00);
                    pattern.mask .push_back(false);

                    // Пропускаем возможный второй '?' в форме "??".
                    if (i + 1 < signature.size() && signature[i + 1] == '?')
                        ++i;
                    continue;
                }

                // Ожидаем две hex-цифры подряд.
                if (i + 1 < signature.size())
                {
                    const int h = hexVal(signature[i]);
                    const int l = hexVal(signature[i + 1]);
                    if (h >= 0 && l >= 0)
                    {
                        pattern.bytes.push_back(
                            static_cast<std::uint8_t>((h << 4) | l));
                        pattern.mask.push_back(true);
                        ++i; // младший полубайт уже обработан
                        continue;
                    }
                }
                // Любой другой символ просто игнорируем (некритично).
            }

            return pattern;
        }

        // -------------------------------------------------------------------------
        //  Внутренний движок поиска внутри одного диапазона адресов.
        //
        //  КРИТИЧЕСКИЙ МОМЕНТ: мы НЕ можем просто线性но идти по всему диапазону
        //  [start .. start+size), потому что внутри него могут встретиться
        //  НЕкоммитнутые страницы (MEM_RESERVE) или страницы с защитой PAGE_NOACCESS.
        //  Чтение такой страницы → Access Violation → краш процесса.
        //
        //  Поэтому движемся по регионам памяти через VirtualQuery:
        //    1. Получаем информацию о текущем регионе (mbi).
        //    2. Проверяем, что он commit'нут и читаем.
        //    3. Сканируем только его.
        //    4. Прыгаем на начало следующего региона (BaseAddress + RegionSize).
        // -------------------------------------------------------------------------
        static uintptr_t FindInRange(uintptr_t start, std::size_t size,
                                     const Pattern& pattern)
        {
            if (pattern.bytes.empty())
                return 0;

            const std::size_t   patLen     = pattern.bytes.size();
            const std::uint8_t  firstByte  = pattern.bytes[0];
            const bool          firstIsWild = !pattern.mask[0];

            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t current = start;
            const uintptr_t end = start + size;

            while (current < end)
            {
                if (VirtualQuery(reinterpret_cast<LPCVOID>(current),
                                 &mbi, sizeof(mbi)) == 0)
                    break; // VirtualQuery сломалась — дальше не лезем

                const uintptr_t regionStart =
                    reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                const uintptr_t regionEnd  = regionStart + mbi.RegionSize;

                // Сканируем только коммитнутые и читаемые регионы.
                // Тип Protection объединяет битовыми флагами доступ R/W/X.
                const bool readable =
                    (mbi.State == MEM_COMMIT) &&
                    (mbi.Protect & (PAGE_READONLY       | PAGE_READWRITE     |
                                    PAGE_EXECUTE_READ   | PAGE_EXECUTE_READWRITE |
                                    PAGE_WRITECOPY      | PAGE_EXECUTE_WRITECOPY));

                if (readable)
                {
                    // Пересечение текущего региона с границами модуля.
                    const uintptr_t scanStart = (std::max)(regionStart, current);
                    const uintptr_t scanEnd   = (std::min)(regionEnd,   end);

                    if (scanEnd > scanStart && (scanEnd - scanStart) >= patLen)
                    {
                        const std::uint8_t* data    = reinterpret_cast<const std::uint8_t*>(scanStart);
                        const std::size_t   dataLen = scanEnd - scanStart;

                        // Линейный поиск по данным региона.
                        // Микрооптимизация: сначала сравниваем первый байт
                        // (кроме случая, когда он wildcard), и только при
                        // совпадении проверяем всю сигнатуру целиком.
                        for (std::size_t i = 0; i + patLen <= dataLen; ++i)
                        {
                            if (!firstIsWild && data[i] != firstByte)
                                continue;

                            bool match = true;
                            for (std::size_t j = 1; j < patLen; ++j)
                            {
                                if (pattern.mask[j] &&
                                    data[i + j] != pattern.bytes[j])
                                {
                                    match = false;
                                    break;
                                }
                            }
                            if (match)
                                return scanStart + i;
                        }
                    }
                }

                // К следующему региону. RegionSize всегда > 0, поэтому
                // гарантированно продвигаемся и не зависаем в бесконечном цикле.
                current = regionEnd;
            }

            return 0; // совпадений нет
        }

        // -------------------------------------------------------------------------
        //  Публичные обёртки.
        // -------------------------------------------------------------------------
        uintptr_t Find(uintptr_t moduleBase, std::size_t moduleSize,
                       std::string_view signature, std::ptrdiff_t addition)
        {
            const Pattern  pattern = Parse(signature);
            const uintptr_t addr   = FindInRange(moduleBase, moduleSize, pattern);
            return addr ? (addr + addition) : 0;
        }

        uintptr_t Find(const wchar_t* moduleName, std::string_view signature,
                       std::ptrdiff_t addition)
        {
            const HMODULE hModule = GetModuleHandleW(moduleName);
            if (!hModule)
                return 0;

            // lpBaseOfDll/SizeOfImage — корректный способ получить границы PE.
            MODULEINFO modInfo{};
            if (!GetModuleInformation(GetCurrentProcess(),
                                      hModule, &modInfo, sizeof(modInfo)))
                return 0;

            const uintptr_t base = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
            return Find(base, modInfo.SizeOfImage, signature, addition);
        }
    } // namespace PatternScanner
} // namespace Memory
