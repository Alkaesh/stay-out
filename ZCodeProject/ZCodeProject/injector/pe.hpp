#pragma once

// =============================================================================
//  pe.hpp — Лёгкий разбор PE-образа над локальным буфером (байты DLL в памяти
//  инжектора). Никаких внешних зависимостей, только Windows.h.
//
//  Предоставляет:
//    * Доступ к DOS/NT заголовкам, секциям, точке входа, ImageBase.
//    * Итератор по базовым релокациям (для применения дельты при маппинге).
//    * Итератор по таблице импортов (для резолва IAT).
//    * Поиск RVA экспорта по имени (для удалённого GetProcAddress).
//
//  Класс НЕ владеет памятью — работает с переданным указателем.
// =============================================================================

#include <Windows.h>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace pe {

class Image {
public:
    explicit Image(const void* base) noexcept
        : base_(static_cast<const std::uint8_t*>(base)) {
        if (!base_) return;
        dos_ = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        if (dos_->e_magic != IMAGE_DOS_SIGNATURE) return; // 'MZ'
        nt_ = reinterpret_cast<const IMAGE_NT_HEADERS*>(base_ + dos_->e_lfanew);
        if (!IsPe64()) return;
        if (nt_->Signature != IMAGE_NT_SIGNATURE) return; // 'PE\0\0'
        valid_ = true;
    }

    bool valid()        const noexcept { return valid_; }
    bool IsPe64()       const noexcept {
        return nt_ && nt_->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
    }

    std::uint32_t size_of_image() const noexcept { return nt_->OptionalHeader.SizeOfImage; }
    std::uintptr_t image_base()   const noexcept { return nt_->OptionalHeader.ImageBase; }
    std::uint32_t entry_rva()     const noexcept { return nt_->OptionalHeader.AddressOfEntryPoint; }

    std::uint16_t section_count() const noexcept { return nt_->FileHeader.NumberOfSections; }
    const IMAGE_SECTION_HEADER* sections() const noexcept { return IMAGE_FIRST_SECTION(nt_); }

    /// RVA → смещение в файле (по таблице секций). 0, если RVA не попадает ни
    /// в одну секцию (например, лежит в заголовках).
    std::uint32_t rva_to_file_offset(std::uint32_t rva) const noexcept {
        const auto* sec = sections();
        for (std::uint16_t i = 0; i < section_count(); ++i) {
            // VirtualAddress — RVA начала секции в памяти; VirtualSize — её размер.
            if (rva >= sec[i].VirtualAddress &&
                rva <  sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
                return rva - sec[i].VirtualAddress + sec[i].PointerToRawData;
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    //  Базовые релокации.
    //
    //  Таблица состоит из блоков IMAGE_BASE_RELOCATION. Каждый блок покрывает
    //  одну страницу (4 КБ) и содержит массив записей по 16 бит:
    //    старшие 4 бита  — тип (IMAGE_REL_BASED_*),
    //    младшие 12 бит  — смещение внутри страницы.
    //  Тип ABSOLUTE (0) — это padding, ничего не патчит.
    // -----------------------------------------------------------------------
    template <typename Fn>
    void for_each_relocation(Fn&& fn) const {
        // Директория BaseRelocationTable.
        auto& dir = nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!dir.Size || !dir.VirtualAddress) return;

        auto* reloc = ptr<IMAGE_BASE_RELOCATION>(dir.VirtualAddress);
        auto* end   = reinterpret_cast<const std::uint8_t*>(reloc) + dir.Size;

        while (reinterpret_cast<const std::uint8_t*>(reloc) < end) {
            if (reloc->SizeOfBlock == 0) break;
            // Количество записей = (размер блока - заголовок) / размер записи (2 байта).
            const std::size_t entries =
                (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(std::uint16_t);
            const auto* entry = reinterpret_cast<const std::uint16_t*>(reloc + 1);

            for (std::size_t i = 0; i < entries; ++i) {
                const std::uint16_t type   = (entry[i] >> 12) & 0xF;
                const std::uint16_t offset = entry[i] & 0x0FFF;
                // Вызываем колбэк: (RVA патч-места, тип). Тип 0 = ABSOLUTE — пропускаем.
                if (type != IMAGE_REL_BASED_ABSOLUTE)
                    fn(reloc->VirtualAddress + offset, type);
            }
            // Сдвигаемся к следующему блоку.
            reloc = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                reinterpret_cast<const std::uint8_t*>(reloc) + reloc->SizeOfBlock);
        }
    }

    // -----------------------------------------------------------------------
    //  Импорты.
    //
    //  Для каждого IMAGE_IMPORT_DESCRIPTOR даём колбэк:
    //    (имя DLL, итератор по thunk'ам).
    //  Внутренний итератор отдаёт пары (RVA места в IAT, имя функции или ординал).
    // -----------------------------------------------------------------------
    struct ImportFunc {
        std::uint32_t iat_rva;          // Куда писать резолв (FirstThunk[i])
        bool          by_ordinal;
        std::uint16_t ordinal;          // Импорт по ординалу
        std::string_view name;          // Импорт по имени (by_ordinal == false)
    };

    template <typename DllFn, typename FuncFn>
    void for_each_import(DllFn&& on_dll, FuncFn&& on_func) const {
        auto& dir = nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.Size || !dir.VirtualAddress) return;

        auto* desc = ptr<IMAGE_IMPORT_DESCRIPTOR>(dir.VirtualAddress);
        for (; desc->Name != 0; ++desc) {
            const char* dll_name = cstr(desc->Name);
            on_dll(std::string_view(dll_name));

            // INT (Import Name Table) — предпочтительный источник имён.
            // Если OriginalFirstThunk == 0 (старые компоновщики), берём FirstThunk.
            std::uint32_t thunk_rva = desc->OriginalFirstThunk
                                      ? desc->OriginalFirstThunk
                                      : desc->FirstThunk;
            std::uint32_t iat_rva   = desc->FirstThunk;

            if (!thunk_rva || !iat_rva) continue;

            // На x64 thunk'и — 8 байт (ULONGLONG).
            auto* thunk = ptr<std::uint64_t>(thunk_rva);
            auto* iat   = ptr<std::uint64_t>(iat_rva);

            for (std::size_t i = 0; thunk[i] != 0; ++i) {
                ImportFunc info{};
                info.iat_rva = iat_rva + static_cast<std::uint32_t>(i * sizeof(std::uint64_t));

                if (thunk[i] & IMAGE_ORDINAL_FLAG64) {
                    // Импорт по ординалу: младшие 16 бит — ординал.
                    info.by_ordinal = true;
                    info.ordinal = static_cast<std::uint16_t>(thunk[i] & 0xFFFF);
                } else {
                    // Импорт по имени: thunk указывает на IMAGE_IMPORT_BY_NAME
                    // (WORD hint + имя). Пропускаем 2 байта hint'а.
                    info.by_ordinal = false;
                    const char* fn_name = cstr(
                        static_cast<std::uint32_t>(thunk[i])) + sizeof(std::uint16_t);
                    info.name = std::string_view(fn_name);
                }
                on_func(info);
            }
        }
    }

    // -----------------------------------------------------------------------
    //  Экспорты — поиск RVA функции по имени.
    //  Используется для «удалённого GetProcAddress»: читаем экспорт-таблицу
    //  зависимой DLL уже внутри целевого процесса и находим нужные функции.
    // -----------------------------------------------------------------------
    std::uint32_t find_export_rva(std::string_view name) const {
        auto& dir = nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.Size || !dir.VirtualAddress) return 0;

        auto* exp = ptr<IMAGE_EXPORT_DIRECTORY>(dir.VirtualAddress);
        auto* names      = ptr<std::uint32_t>(exp->AddressOfNames);
        auto* ordinals   = ptr<std::uint16_t>(exp->AddressOfNameOrdinals);
        auto* functions  = ptr<std::uint32_t>(exp->AddressOfFunctions);

        for (std::uint32_t i = 0; i < exp->NumberOfNames; ++i) {
            const char* fn = cstr(names[i]);
            if (name == std::string_view(fn)) {
                const std::uint16_t ord = ordinals[i];
                return functions[ord];
            }
        }
        return 0;
    }

    /// Диапазон экспорт-директории (для детекта форвардов).
    std::pair<std::uint32_t, std::uint32_t> export_range() const noexcept {
        auto& dir = nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        return { dir.VirtualAddress, dir.Size };
    }

    /// Диапазон директории по индексу (RVA, size). 0/0 если отсутствует.
    std::pair<std::uint32_t, std::uint32_t>
    data_directory(int index) const noexcept {
        const auto& dir = nt_->OptionalHeader.DataDirectory[index];
        return { dir.VirtualAddress, dir.Size };
    }

    /// Директория исключений (.pdata: RUNTIME_FUNCTION[]). Для x64 SEH.
    std::pair<std::uint32_t, std::uint32_t> exception_range() const noexcept {
        return data_directory(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    }

    /// Директория TLS (IMAGE_TLS_DIRECTORY).
    std::pair<std::uint32_t, std::uint32_t> tls_range() const noexcept {
        return data_directory(IMAGE_DIRECTORY_ENTRY_TLS);
    }

private:
    template <typename T>
    const T* ptr(std::uint32_t rva) const noexcept {
        return reinterpret_cast<const T*>(base_ + rva_to_file_offset(rva));
    }

    // Доступ к C-строке по RVA (для имён DLL/функций в импорт/экспорт-таблицах).
    const char* cstr(std::uint32_t rva) const noexcept {
        return reinterpret_cast<const char*>(base_ + rva_to_file_offset(rva));
    }

    const std::uint8_t*       base_ = nullptr;
    const IMAGE_DOS_HEADER*   dos_  = nullptr;
    const IMAGE_NT_HEADERS*   nt_   = nullptr;
    bool valid_ = false;
};

} // namespace pe
