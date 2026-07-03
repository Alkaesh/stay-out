# Manual Map Инжектор (x64)

Продвинутый инжектор методом Manual Map со скрытием DLL от базовых проверок
античитов и процессов-инспекторов.

## Структура

| Файл              | Назначение                                                       |
|-------------------|------------------------------------------------------------------|
| `pe.hpp`          | Лёгкий разбор PE над локальным буфером (reloc, imports, exports). |
| `manual_map.hpp`  | Публичный API: `inject(pid, path)` / `inject_by_name(...)`.      |
| `manual_map.cpp`  | Полный конвейер Manual Map (10 шагов, см. шапку файла).          |
| `main.cpp`        | CLI: `injector.exe <PID|process.exe> <dll>`.                     |

## Сборка

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# → build/Release/injector.exe
```

Или добавить папку `injector/` как отдельный проект в Visual Studio (пустой
Console App, добавить файлы, линковать `Psapi.lib`).

## Использование

```bash
# По PID
injector.exe 12345 internal_dll.dll

# По имени процесса
injector.exe game.exe internal_dll.dll
```

Запускать **от администратора** (нужна `SeDebugPrivilege` для `PROCESS_ALL_ACCESS`).

## Что даёт скрытие (Stealth-фичи)

1. **Не регистрируется в PEB** — DLL не проходит через загрузчик Windows, её
   нет в `PEB->Ldr->InLoadOrderModuleList`. Не видна через:
   - `GetModuleHandle` / `GetModuleHandleEx`
   - `EnumProcessModules` / `EnumProcessModulesEx`
   - `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)`
   - `Module32First` / `Module32Next`
   - Любые итераторы `InLoadOrderModuleList` напрямую через PEB.

2. **Стирание PE-заголовков** — DOS + NT заголовки замаппленного образа
   обнуляются, страница помечается `PAGE_NOACCESS`. Невозможно обнаружить:
   - Сканом памяти по сигнатурам `"MZ"` / `"PE\0\0"`
   - Ручным разбором PE через `IMAGE_DOS_HEADER->e_lfanew`.

3. **Очистка shellcode-стаба** — после отработки `DllMain` стаб освобождается
   через `VirtualFreeEx(MEM_RELEASE)`. В памяти не остаётся инжектированного
   «обрывка кода», который можно детектить.

4. **Резолв импортов без следов нашей DLL** — зависимые DLL грузятся через
   `LoadLibraryA` в целике (это их собственная загрузка через системный
   загрузчик), а не через нашу. Экспорт-таблицы читаются напрямую — никаких
   лишних вызовов `GetProcAddress` на нашу DLL.

## Пошаговое описание работы

1. Включение `SeDebugPrivilege`.
2. Чтение файла DLL в локальный буфер.
3. Открытие процесса (`OpenProcess(PROCESS_ALL_ACCESS)`).
4. Выделение `SizeOfImage` памяти (`VirtualAllocEx`).
5. Копирование секций по их `VirtualAddress`.
6. Применение базовых релокаций (дельта = `mapped - ImageBase`).
   - Поддерживаются `IMAGE_REL_BASED_DIR64` (основной на x64) и
     `IMAGE_REL_BASED_HIGHLOW`.
7. Резолв импортов: для каждой зависимой DLL — `LoadLibraryA` удалённо,
   затем обход экспорт-таблицы для поиска адресов функций.
8. Генерация x64 PIC-shellcode для вызова `DllMain(base, DLL_PROCESS_ATTACH, 0)`.
9. `CreateRemoteThread` на стаб; опц. ожидание кода возврата.
10. Стирание заголовков + освобождение стаба (stealth).

## Ограничения (намеренные)

- **Только x64.** Поддержка x86 добавляется отдельным путём (32-битные
  thunk'и, другая calling convention в shellcode).
- **Экспорт-форвардеры не резолвятся.** Возвращается 0, что вызывает
  читаемое сообщение об ошибке. Покрывает 99% кейсов для WinAPI; для
  нетипичных зависимостей с форвардерами добавь рекурсивный резолв.
- **TLS-колбэки не вызываются.** Стандартные DLL CRT запускаются корректно,
  но если твоя DLL использует TLS-колбэки для инициализации — их нужно
  вызвать вручную перед `DllMain` (см. `IMAGE_DIRECTORY_ENTRY_TLS`).
- **SEH/исключения в целике** не покрываются дополнительной таблицей —
  для сложных DLL может потребоваться регистрация через `RtlAddFunctionTable`.
