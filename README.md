# Internal DLL — каркас для исследования памяти

Универсальный шаблон internal DLL (C++20, только WinAPI). Без привязки к игре.

## Структура

| Файл          | Назначение                                              |
|---------------|--------------------------------------------------------|
| `main.cpp`    | `DllMain`, рабочий поток, отладочная консоль, выгрузка по `END` |
| `memory.hpp`  | Декларации: `Read`/`Write`/`ReadString` + `PatternScanner` |
| `memory.cpp`  | Реализация pattern-сканера (поиск по регионам памяти)  |
| `hooks.hpp`   | Пустая заготовка под MinHook / VMT-хуки                |

## Сборка в Visual Studio

1. **File → New → Project → Empty Project (C++)**.
2. В свойствах проекта: **Configuration Type = Dynamic Library (.dll)**.
3. **C++ Language Standard = ISO C++20**.
4. Платформа: **x64** (или x86 — под разрядность игры).
5. **Character Set = Use Unicode Character Set** (используется `…W` API).
6. Зависимости линкера: `Psapi.lib` уже подключена через `#pragma comment(lib, ...)`
   в `memory.cpp` — дополнительно настраивать не нужно.
7. Добавить в проект все 4 файла и собрать → `ProjectName.dll`.

## Проверка работоспособности

1. Инжектируйте DLL в процесс игры любым базовым инжектором.
2. Должно появиться окно консоли `Internal DLL — Debug Console`
   со строкой `[*] Worker thread started.`
3. По нажатию **END** DLL чисто выгружается (`FreeLibraryAndExitThread`).

## Использование pattern-сканера

```cpp
const uintptr_t addr = Memory::PatternScanner::Find(
    L"StayOut.exe", "45 8B ? ? 00", /*addition=*/0);

if (addr)
    std::printf("[+] Hit @ 0x%p\n", reinterpret_cast<void*>(addr));
```

- `?` / `??` — wildcard (любой байт).
- `addition` — смещение, прибавляемое к найденному адресу
  (полезно для ручного учёта offset внутри сигнатуры).

## Дальше

Перепишу сигнатуры/оффсеты и модульную структуру под конкретный проект
(*Stay Out*) после получения твоих файлов.
