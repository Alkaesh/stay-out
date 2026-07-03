// =============================================================================
//  manual_map.cpp — Реализация Manual Map инжектора (x64).
//
//  Конвейер инъекции:
//    1. Читаем файл DLL в локальный буфер.
//    2. Открываем целевой процесс.
//    3. Выделяем память в целике под SizeOfImage.
//    4. Копируем секции на правильные RVA-смещения.
//    5. Применяем базовые релокации (дельта = фактический_база - ImageBase).
//    6. Резолвим импорты: для каждой DLL — LoadLibraryA (удалённо), затем
//       читаем её экспорт-таблицу в целике и находим адреса функций.
//    7. Генерируем x64 shellcode-стаб, вызывающий DllMain(base, 1, 0).
//    8. CreateRemoteThread на стаб; (опц.) ждём завершения.
//    9. Стираем PE-заголовки и освобождаем стаб (опц.).
//
//  ПОЧЕМУ ЭТО СКРЫВАЕТ DLL ОТ БАЗОВЫХ ПРОВЕРОК:
//    * DLL НИКОГДА не проходит через загрузчик Windows → её нет в
//      PEB->Ldr->InLoadOrderModuleList. Следовательно её не видят
//      GetModuleHandle, EnumProcessModules, CreateToolhelp32Snapshot(MODULE).
//    * PE-заголовки стираются → скан памяти по "MZ"/"PE\0\0" ничего не находит.
//    * Shellcode-стаб освобождается → не остаётся инжектированного кода-следа.
//    * Зависимые DLL резолвятся через чтение экспорт-таблиц, а не через
//      LoadLibrary на нашу DLL — следов лишних записей в PEB не появляется.
// =============================================================================

#include "manual_map.hpp"
#include "pe.hpp"

#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <string>
#include <cstring>
#include <fstream>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace injector {

namespace {

// -------------------------------------------------------------------------
//  Вспомогательные функции для PE над ЛОКАЛЬНЫМ буфером файла.
//  (pe::Image уже умеет rva_to_file_offset — обёртки ниже для краткости.)
// -------------------------------------------------------------------------

/// DOS-заголовок локального буфера.
inline const IMAGE_DOS_HEADER* dos_of(const std::uint8_t* buf) noexcept {
    return reinterpret_cast<const IMAGE_DOS_HEADER*>(buf);
}

/// NT-заголовки (x64) локального буфера.
inline const IMAGE_NT_HEADERS64* nt_of(const std::uint8_t* buf) noexcept {
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(buf + dos_of(buf)->e_lfanew);
}

// =========================================================================
//  Утилиты удалённого процесса.
// =========================================================================

class RemoteProcess {
public:
    explicit RemoteProcess(DWORD pid) {
        // PROCESS_ALL_ACCESS нужен для VirtualAllocEx / WriteProcessMemory /
        // CreateRemoteThread. На Vista+ требуется SeDebugPrivilege для
        // защищённых процессов.
        handle_ = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    }
    ~RemoteProcess() { if (handle_) CloseHandle(handle_); }
    RemoteProcess(const RemoteProcess&) = delete;
    RemoteProcess& operator=(const RemoteProcess&) = delete;

    HANDLE handle() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    /// Выделить память в целике.
    std::uintptr_t alloc(std::size_t size, std::uintptr_t base = 0,
                         DWORD prot = PAGE_EXECUTE_READWRITE) noexcept {
        return reinterpret_cast<std::uintptr_t>(
            VirtualAllocEx(handle_, reinterpret_cast<LPVOID>(base), size,
                           MEM_COMMIT | MEM_RESERVE, prot));
    }

    bool free(std::uintptr_t addr, std::size_t size = 0) noexcept {
        return VirtualFreeEx(handle_, reinterpret_cast<LPVOID>(addr), size,
                             MEM_RELEASE);
    }

    bool write(std::uintptr_t addr, const void* src, std::size_t size) noexcept {
        SIZE_T written = 0;
        return WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(addr),
                                  src, size, &written) && written == size;
    }

    bool read(std::uintptr_t addr, void* dest, std::size_t size) noexcept {
        SIZE_T got = 0;
        return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(addr),
                                 dest, size, &got) && got == size;
    }

    /// Удалённо вызвать функцию одного аргумента (LoadLibraryA) через
    /// CreateRemoteThread. Возвращаемое значение потока = код выхода,
    /// для LoadLibraryA это HMODULE загруженной DLL в целике.
    std::uintptr_t remote_call_1arg(std::uintptr_t fn_addr,
                                    std::uintptr_t arg) noexcept {
        DWORD tid = 0;
        HANDLE h = CreateRemoteThread(
            handle_, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(fn_addr),
            reinterpret_cast<LPVOID>(arg), 0, &tid);
        if (!h) return 0;

        WaitForSingleObject(h, 30000);
        DWORD exit_code = 0;
        GetExitCodeThread(h, &exit_code);
        CloseHandle(h);
        return static_cast<std::uintptr_t>(exit_code);
    }

private:
    HANDLE handle_ = nullptr;
};

// -------------------------------------------------------------------------
//  Чтение файла в буфер.
// -------------------------------------------------------------------------
std::vector<std::uint8_t> read_file_all(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

// -------------------------------------------------------------------------
//  Создание shellcode-стаба для вызова DllMain.
//
//  x64, Windows calling convention. rcx уже = base (приходит из
//  CreateRemoteThread->lpParameter). Нам нужно дополнительно:
//    rdx = 1 (DLL_PROCESS_ATTACH), r8 = 0 (lpReserved), вызвать Entry.
//
//  Дизассемблер:
//    sub  rsp, 28h            ; 48 83 EC 28   (shadow space + выравнивание)
//    mov  edx, 1              ; BA 01 00 00 00
//    xor  r8d, r8d            ; 45 31 C0
//    mov  rax, <entry>        ; 48 B8 [8 байт]  (адрес точки входа в целике)
//    call rax                 ; FF D0
//    add  rsp, 28h            ; 48 83 C4 28
//    ret                      ; C3
// -------------------------------------------------------------------------
std::vector<std::uint8_t> make_loader_stub(std::uintptr_t entry_addr) {
    // Размеры: 4 + 5 + 3 + 10 + 2 + 4 + 1 = 29 байт.
    std::vector<std::uint8_t> stub = {
        0x48, 0x83, 0xEC, 0x28,                   // sub rsp, 28h
        0xBA, 0x01, 0x00, 0x00, 0x00,             // mov edx, 1
        0x45, 0x31, 0xC0,                         // xor r8d, r8d
        0x48, 0xB8, 0,0,0,0,0,0,0,0,             // mov rax, <entry> (imm64 placeholder)
        0xFF, 0xD0,                               // call rax
        0x48, 0x83, 0xC4, 0x28,                   // add rsp, 28h
        0xC3,                                     // ret
    };
    // Патчим 8-байтный адрес точки входа (смещение 13 — после 0x48 0xB8).
    std::uint64_t ea = entry_addr;
    std::memcpy(&stub[13], &ea, sizeof(ea));
    return stub;
}

// -------------------------------------------------------------------------
//  Удалённый GetProcAddress: читаем экспорт-таблицу зависимой DLL
//  ПРЯМО из адресного пространства целика и находим функцию по имени.
//
//  Это позволяет НЕ вызывать GetProcAddress удалённо (он принимает 2 аргумента
//  и не подходит для CreateRemoteThread напрямую), а вместо этого обойти
//  экспорт-таблицу вручную.
//
//  Возвращает абсолютный адрес функции в целике или 0.
//
//  Обрабатываем форвардеры: если RVA функции попадает в диапазон
//  экспорт-директории — это строка-форвардер вида "OTHER.dll.FuncName".
//  Для простоты разрешаем только НЕ-форвардеры (это покрывает 99% кейсов
//  для WinAPI). На форвардер возвращаем 0 (вызывающий отрапортует ошибку).
// -------------------------------------------------------------------------
std::uintptr_t remote_get_proc_address(RemoteProcess& proc,
                                       std::uintptr_t dll_base,
                                       std::string_view func_name,
                                       bool by_ordinal,
                                       std::uint16_t ordinal) {
    // Читаем заголовки (DOS + NT) — достаточно первых 0x400 байт.
    std::uint8_t hdr[0x400] = {};
    if (!proc.read(dll_base, hdr, sizeof(hdr))) return 0;

    pe::Image img(hdr);
    if (!img.valid()) return 0;

    // Читаем экспорт-директорию.
    auto [exp_rva, exp_size] = img.export_range();
    if (!exp_rva) return 0;

    IMAGE_EXPORT_DIRECTORY exp_dir{};
    if (!proc.read(dll_base + exp_rva, &exp_dir, sizeof(exp_dir))) return 0;

    // Читаем массивы AddressOfNames / AddressOfNameOrdinals / AddressOfFunctions.
    std::vector<std::uint32_t> names(exp_dir.NumberOfNames);
    std::vector<std::uint16_t> ords(exp_dir.NumberOfNames);
    std::vector<std::uint32_t> funcs(exp_dir.NumberOfFunctions);
    if (exp_dir.NumberOfNames) {
        if (!proc.read(dll_base + exp_dir.AddressOfNames, names.data(),
                       names.size() * sizeof(std::uint32_t))) return 0;
        if (!proc.read(dll_base + exp_dir.AddressOfNameOrdinals, ords.data(),
                       ords.size() * sizeof(std::uint16_t))) return 0;
    }
    if (exp_dir.NumberOfFunctions) {
        if (!proc.read(dll_base + exp_dir.AddressOfFunctions, funcs.data(),
                       funcs.size() * sizeof(std::uint32_t))) return 0;
    }

    // Если импорт по ординалу — ординал абсолютный (base + index).
    if (by_ordinal) {
        const std::uint32_t idx = static_cast<std::uint32_t>(ordinal) - exp_dir.Base;
        if (idx >= exp_dir.NumberOfFunctions) return 0;
        const std::uint32_t fn_rva = funcs[idx];
        // Проверка на форвардер.
        if (fn_rva >= exp_rva && fn_rva < exp_rva + exp_size) return 0;
        return dll_base + fn_rva;
    }

    // Импорт по имени — ищем в массиве имён.
    for (std::uint32_t i = 0; i < exp_dir.NumberOfNames; ++i) {
        char buf[256] = {};
        if (!proc.read(dll_base + names[i], buf, sizeof(buf) - 1)) continue;
        if (func_name == std::string_view(buf)) {
            const std::uint16_t ord = ords[i];
            const std::uint32_t fn_rva = funcs[ord];
            if (fn_rva >= exp_rva && fn_rva < exp_rva + exp_size) {
                // Форвардер — не поддерживаем.
                return 0;
            }
            return dll_base + fn_rva;
        }
    }
    return 0;
}

// -------------------------------------------------------------------------
//  Загрузить зависимую DLL в целике через удалённый LoadLibraryA.
//  Адрес LoadLibraryA берём из текущего процесса — это безопасно, потому что
//  kernel32.dll на x64 Windows грузится по ОДНОМУ И ТОМУ ЖЕ ВА для всех
//  процессов в рамках одной загрузки ОС (ASLR per-boot, не per-process).
//  Это широко известная и корректная оптимизация для инжекторов.
// -------------------------------------------------------------------------
std::uintptr_t remote_load_library(RemoteProcess& proc,
                                   std::string_view dll_name) {
    static const auto load_library_a = reinterpret_cast<std::uintptr_t>(&LoadLibraryA);

    // Выделяем строку с именем в целике.
    const std::string name_str(dll_name);
    std::uintptr_t str_addr = proc.alloc(name_str.size() + 1, 0, PAGE_READWRITE);
    if (!str_addr) return 0;
    if (!proc.write(str_addr, name_str.c_str(), name_str.size() + 1)) {
        proc.free(str_addr);
        return 0;
    }

    const std::uintptr_t hmod = proc.remote_call_1arg(load_library_a, str_addr);
    proc.free(str_addr);
    return hmod; // HMODULE в целике (= база DLL)
}

// -------------------------------------------------------------------------
//  Поиск PID по имени процесса (через Toolhelp32).
//  Поддерживает частичное совпадение (contains): "sogame" найдёт "sogame.exe".
// -------------------------------------------------------------------------
DWORD find_pid_by_name(std::wstring_view name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    // Приводим искомое имя к нижнему регистру.
    std::wstring target(name);
    std::transform(target.begin(), target.end(), target.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    // Убираем возможное расширение .exe из запроса — сравниваем по базе.
    if (target.size() > 4 && target.substr(target.size() - 4) == L".exe") {
        target = target.substr(0, target.size() - 4);
    }

    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring cur(pe.szExeFile);
            std::transform(cur.begin(), cur.end(), cur.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            // Частичное совпадение: cur содержит target (или наоборот).
            if (cur.find(target) != std::wstring::npos ||
                target.find(cur) != std::wstring::npos) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

} // namespace

// =========================================================================
//  Публичные функции.
// =========================================================================

bool enable_debug_privilege() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const BOOL ok = (GetLastError() == ERROR_SUCCESS);
    CloseHandle(token);
    return ok != FALSE;
}

InjectResult inject(DWORD pid, const std::filesystem::path& dll_path,
                    const InjectOptions& opts) {
    InjectResult res;

    // --- 1. Читаем файл DLL. ---
    std::vector<std::uint8_t> raw = read_file_all(dll_path);
    if (raw.empty()) { res.error = "Не удалось прочитать файл DLL"; return res; }

    pe::Image local(raw.data());
    if (!local.valid()) { res.error = "Невалидный PE-образ (требуется x64 PE)"; return res; }

    // --- 2. Открываем целевой процесс. ---
    RemoteProcess proc(pid);
    if (!proc) {
        res.error = "OpenProcess failed (нужны права админа / SeDebugPrivilege)";
        return res;
    }

    // --- 3. Выделяем память под SizeOfImage. ---
    const std::size_t image_size = local.size_of_image();
    std::uintptr_t preferred = opts.prefer_image_base ? local.image_base() : 0;
    std::uintptr_t remote_base = proc.alloc(image_size, preferred);
    if (!remote_base) {
        // Если не вышло по предпочтительному адресу — пробуем по любому.
        if (preferred) remote_base = proc.alloc(image_size, 0);
        if (!remote_base) {
            res.error = "VirtualAllocEx failed (GetLastError=" +
                        std::to_string(GetLastError()) +
                        "). Access denied=5. Process protected/higher integrity?";
            return res;
        }
    }
    res.mapped_base = remote_base;

    // --- 4. Копируем секции на правильные смещения. ---
    //  Сначала заголовки (DOS + NT + таблица секций): SizeOfHeaders байт из файла.
    const std::uint32_t headers_size = nt_of(raw.data())->OptionalHeader.SizeOfHeaders;
    if (!proc.write(remote_base, raw.data(), headers_size)) {
        res.error = "write headers failed";
        return res;
    }
    //  Каждую секцию — по её VirtualAddress.
    const auto* secs = local.sections();
    for (std::uint16_t i = 0; i < local.section_count(); ++i) {
        if (!secs[i].SizeOfRawData) continue; // bss/неинициализированная
        const std::uint8_t* src = raw.data() + secs[i].PointerToRawData;
        if (!proc.write(remote_base + secs[i].VirtualAddress, src, secs[i].SizeOfRawData)) {
            res.error = "write section failed";
            return res;
        }
    }

    // --- 5. Базовые релокации. ---
    //  Дельта = куда_положили - куда_хотели. Если дельта != 0 — патчим все
    //  места, указанные в reloc-таблице.
    const std::int64_t delta =
        static_cast<std::int64_t>(remote_base) -
        static_cast<std::int64_t>(local.image_base());

    if (delta != 0) {
        local.for_each_relocation([&](std::uint32_t patch_rva, std::uint16_t type) {
            const std::uintptr_t patch_addr = remote_base + patch_rva;
            switch (type) {
                case IMAGE_REL_BASED_DIR64: {
                    // 64-битный адрес: добавляем дельту целиком.
                    std::uint64_t v = 0;
                    if (proc.read(patch_addr, &v, sizeof(v))) {
                        v += static_cast<std::uint64_t>(delta);
                        proc.write(patch_addr, &v, sizeof(v));
                    }
                    break;
                }
                case IMAGE_REL_BASED_HIGHLOW: {
                    // 32-битный адрес (редкость для x64, но поддерживаем).
                    std::uint32_t v = 0;
                    if (proc.read(patch_addr, &v, sizeof(v))) {
                        v += static_cast<std::uint32_t>(delta);
                        proc.write(patch_addr, &v, sizeof(v));
                    }
                    break;
                }
                default:
                    break; // Остальные типы на x64 не встречаются.
            }
        });
    }

    // --- 6. Резолв импортов. ---
    //  Для каждой зависимой DLL: грузим её в целике (если ещё не загружена),
    //  затем обходим экспорт-таблицу и пишем адреса функций в IAT замаппленного
    //  образа. Используем ПРЯМОЙ двойной цикл по IMAGE_IMPORT_DESCRIPTOR —
    //  это чище, чем накручивать колбэки, потому что имя DLL нужно именно
    //  в момент резолва её функций.
    {
        auto& dir = nt_of(raw.data())->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.Size && dir.VirtualAddress) {
            const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                raw.data() + local.rva_to_file_offset(dir.VirtualAddress));

            for (; desc->Name != 0; ++desc) {
                const char* dep_name = reinterpret_cast<const char*>(
                    raw.data() + local.rva_to_file_offset(desc->Name));

                // Загружаем зависимую DLL в целике → её база.
                const std::uintptr_t dep_base = remote_load_library(proc, dep_name);
                if (!dep_base) {
                    res.error = std::string("Не удалось загрузить зависимую DLL: ") + dep_name;
                    return res;
                }

                // INT (Import Name Table) — предпочтительный источник имён.
                // Если OriginalFirstThunk == 0 (старые компоновщики), берём FirstThunk.
                const std::uint32_t thunk_rva = desc->OriginalFirstThunk
                                                ? desc->OriginalFirstThunk
                                                : desc->FirstThunk;
                const std::uint32_t iat_rva   = desc->FirstThunk;
                if (!thunk_rva || !iat_rva) continue;

                const auto* thunk = reinterpret_cast<const std::uint64_t*>(
                    raw.data() + local.rva_to_file_offset(thunk_rva));

                for (std::size_t i = 0; thunk[i] != 0; ++i) {
                    const std::uintptr_t iat_slot =
                        remote_base + iat_rva + i * sizeof(std::uint64_t);
                    std::uintptr_t fn_addr = 0;

                    if (thunk[i] & IMAGE_ORDINAL_FLAG64) {
                        // Импорт по ординалу: младшие 16 бит.
                        const std::uint16_t ord = static_cast<std::uint16_t>(thunk[i] & 0xFFFF);
                        fn_addr = remote_get_proc_address(proc, dep_base, {}, true, ord);
                    } else {
                        // Импорт по имени: thunk -> IMAGE_IMPORT_BY_NAME (WORD hint + имя).
                        const char* fn_name = reinterpret_cast<const char*>(
                            raw.data() + local.rva_to_file_offset(
                                static_cast<std::uint32_t>(thunk[i]))) + sizeof(std::uint16_t);
                        fn_addr = remote_get_proc_address(proc, dep_base, fn_name, false, 0);
                    }

                    if (!fn_addr) {
                        res.error = std::string("Не удалось резолвить импорт из ") + dep_name;
                        return res;
                    }
                    if (!proc.write(iat_slot, &fn_addr, sizeof(fn_addr))) {
                        res.error = "write IAT failed";
                        return res;
                    }
                }
            }
        }
    }

    // --- 7. Shellcode-стаб для вызова точки входа (DllMain). ---
    const std::uintptr_t entry_addr = remote_base + local.entry_rva();
    if (!entry_addr) {
        res.error = "У DLL нет точки входа (AddressOfEntryPoint == 0)";
        return res;
    }

    // Диагностика SEH/TLS-директорий (информативно — см. TODO в .hpp).
    // Если DLL использует CRT с исключениями или thread_local, эти директории
    // будут ненулевыми, и без их обработки возможны краши.
    {
        auto [pdata_rva, pdata_size] = local.exception_range();
        auto [tls_rva,   tls_size]   = local.tls_range();
        if (pdata_size) {
            // .pdata присутствует: DLL использует SEH. При register_seh_table=true
            // нужна регистрация через RtlAddFunctionTable (не реализовано — см. hpp).
        }
        if (tls_size) {
            // TLS-директория присутствует: DLL имеет TLS-колбэки.
        }
        (void)pdata_rva; (void)tls_rva;
    }
    auto stub = make_loader_stub(entry_addr);
    const std::uintptr_t stub_addr = proc.alloc(stub.size(), 0, PAGE_EXECUTE_READWRITE);
    if (!stub_addr) { res.error = "alloc stub failed"; return res; }
    if (!proc.write(stub_addr, stub.data(), stub.size())) {
        res.error = "write stub failed";
        proc.free(stub_addr);
        return res;
    }

    // --- 8. Запускаем точку входа через CreateRemoteThread. ---
    //  lpParameter попадает в rcx (Windows x64 ABI) — это и есть base DLL,
    //  который DllMain/HMODULE ожидает в первом аргументе.
    DWORD tid = 0;
    HANDLE h_thread = CreateRemoteThread(
        proc.handle(), nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(stub_addr),
        reinterpret_cast<LPVOID>(remote_base), 0, &tid);
    if (!h_thread) {
        res.error = "CreateRemoteThread failed";
        proc.free(stub_addr);
        return res;
    }

    if (opts.wait_for_entry) {
        WaitForSingleObject(h_thread, 30000);
        DWORD ec = 0;
        GetExitCodeThread(h_thread, &ec);
        res.entry_exit_code = ec;
        // DllMain возвращает BOOL: TRUE (1) = успех, FALSE (0) = отказ.
        if (ec == 0) {
            res.error = "DllMain вернул FALSE — инициализация отклонена";
        }
    }
    CloseHandle(h_thread);

    // --- 9. Стирание PE-заголовков (stealth). ---
    //  Обнуляем DOS + NT заголовки замаппленного образа — это делает
    //  невозможным обнаружение DLL по сигнатурам "MZ"/"PE\0\0" и мешает
    //  ручному разбору PE. На работоспособность уже-инициализированной DLL
    //  не влияет: заголовки нужны только при загрузке/выгрузке.
    if (opts.erase_headers) {
        std::vector<std::uint8_t> zeroes(headers_size, 0);
        proc.write(remote_base, zeroes.data(), headers_size);
        // Дополнительно: делаем страницу заголовков NOACCESS, чтобы
        // отличить замаппленную область от «настоящих» PE-модулей.
        DWORD oldp = 0;
        VirtualProtectEx(proc.handle(), reinterpret_cast<LPVOID>(remote_base),
                         headers_size, PAGE_NOACCESS, &oldp);
    }

    // --- 10. Освобождение стаба (stealth). ---
    if (opts.wipe_loader_stub) {
        proc.free(stub_addr);
    }

    res.ok = res.error.empty();
    return res;
}

InjectResult inject_by_name(std::wstring_view process_name,
                            const std::filesystem::path& dll_path,
                            const InjectOptions& opts) {
    const DWORD pid = find_pid_by_name(process_name);
    if (!pid) {
        InjectResult r;
        // Простое преобразование wstring → string для сообщения об ошибке.
        r.error.assign(process_name.begin(), process_name.end());
        r.error = "Процесс не найден: " + r.error;
        return r;
    }
    return inject(pid, dll_path, opts);
}

} // namespace injector
