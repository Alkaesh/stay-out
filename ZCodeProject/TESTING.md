# Процедура тестирования — Stay Out research DLL

Пошаговый гайд: от сборки до получения первых данных из памяти/Python игры.

---

## 0. Предварительные требования

| Что | Зачем |
|---|---|
| Visual Studio 2022 (Desktop C++ + CMake) | Сборка |
| Игра **Stay Out** (клиент `sogame.exe`, x64) | Цель инжекта |
| Запуск **от администратора** | `SeDebugPrivilege` для `PROCESS_ALL_ACCESS` |
| Закрытая песочница / приватный сервер | Авторизованный контекст исследования |

> ⚠️ **Этика.** Этот инструмент предназначен для исследования памяти в
> *авторизованной* закрытой среде (песочница/приватный сервер). Использование
> против официальных серверов с живыми игроками — нарушение правил и может
> привести к бану. Не делайте этого.

---

## 1. Сборка

Дважды кликнуть **`build.bat`** в корне проекта (или из cmd):

```
build.bat            # Release (по умолчанию)
build.bat Debug      # отладочная сборка с /Zi
```

`build.bat` сам:
1. Находит Visual Studio через `vswhere`.
2. Конфигурирует CMake (`Visual Studio 17 2022`, `x64`).
3. Собирает оба target'а.

**Результат:**
```
build\stayout_dll.dll
build\injector\injector.exe
```

Если `build.bat` падает на «Visual Studio / Build Tools not found» — у тебя
не установлен компилятор. Поставь **Visual Studio Build Tools 2022**
(скачать: https://visualstudio.microsoft.com/downloads/ → «Build Tools for
Visual Studio 2022») с компонентами:
- MSVC v143 — VS 2022 C++ x64/x86 build tools
- C++ CMake tools for Windows (включает Ninja)
- Windows 11 SDK (или Windows 10 SDK)

Полная IDE не обязательна — `build.bat` работает и с одними Build Tools.

---

## 2. Запуск игры

1. Запусти **Stay Out** обычным способом.
2. Дождись главного меню (или зайди в мир — для дампа `BigWorld.player()`).
3. Убедись, что игра в **windowed / borderless** режиме — overlay не работает
   в exclusive fullscreen (DWM блокирует прозрачные окна поверх fullscreen).

---

## 3. Инжект

Правый клик по **`run.bat`** → **«Запуск от имени администратора»**.

```
run.bat                    # Release + sogame.exe
run.bat Debug              # Debug-сборка
run.bat Release 12345      # инжект по PID вместо имени
```

`run.bat` сам проверит права админа и вызовет:
```
injector.exe sogame.exe build\stayout_dll.dll
```

**Успешный вывод инжектора:**
```
[+] SeDebugPrivilege включена.
[*] Загружаемая DLL: ...\stayout_dll.dll
[*] Stealth: erase_headers=1, wipe_stub=1
[+] Инъекция успешна. Замапплено по адресу 0x...
[+] DllMain вернул код: 1
[+] DLL скрыта: не в PEB, заголовки стёрты, стаб очищен.
```

---

## 4. Первичная диагностика

Сразу после инжекта откроется консоль **«[Stay Out] BigWorld Research Console»**.
Полный лог дублируется в **`%TEMP%\stayout_log.txt`**
(открыть: `Win+R` → `%TEMP%\stayout_log.txt`).

Ожидаемые строки:
```
[+] Stay Out research DLL loaded. TID = ...
[*] Log file: C:\Users\<user>\AppData\Local\Temp\stayout_log.txt
[*] END=unload | F9=ESP | F10=inv-on-screen | F11=inv-log | F12=attrs
[py] Found python27.dll (Python 2.7)        ← либо "via export scan"
[bw] BigWorld module accessible.
[*] === Diagnostics: run dir() on key objects ===
[+] Python bridge: python27.dll (Py2.7)
[+] BigWorld module: OK
[+] BigWorld.player(): present (in game)
=== dir(BigWorld.player()) ===
  __class__
  __doc__
  ...
  position
  health
  ...
```

---

## 5. Горячие клавиши

| Клавиша | Действие |
|---|---|
| **END** | Выгрузить DLL (`FreeLibraryAndExitThread`). Игра должна остаться живой. |
| **F9** | ESP вкл/выкл (overlay-окно поверх игры). |
| **F10** | Инвентарь поверх экрана (toggle). |
| **F11** | Дамп инвентаря в лог. |
| **F12** | `dir(player())` + `dir(player().inventory)` в лог — для RE имён атрибутов. |

---

## 6. Интерпретация результатов и типичные проблемы

### ✅ Успех: Python + BigWorld найдены
`diagnostics::dump_player_dir()` выдаёт список атрибутов. Это и есть
**реальные имена под текущий билд Stay Out**. Подставь их в:
- `bigworld.cpp` → поля `Entity::position/health/...`
- `inventory.cpp` → списки имён в `first_*_attr(...)`

### ❌ «python27.dll не найден»
Python под нестандартным именем или влинкован в `sogame.exe`.
**Уже реализовано:** fallback-перебор всех модулей по экспорту
`PyRun_SimpleString`. Если и он не сработал — пришли лог целиком.

Проверь вручную через Process Hacker / Process Explorer:
какая DLL в процессе экспортирует `Py_*` функции?

### ❌ «BigWorld module: NOT accessible»
Python есть, но модуль `BigWorld` не импортируется. Возможные причины:
- Игра ещё на экране загрузки (BigWorld не инициализирован) — зайди в мир.
- Модуль называется иначе (`BigWorldClient`, `BWClient`, ...). Поправь строку
  в `bigworld.cpp::module()`: `PyImport_ImportModule("BigWorld")`.

### ❌ Overlay не появляется (ESP не рисуется)
`overlay::initialize` не нашёл окно. Сначала шёл точный поиск по заголовку
`Stay Out`, теперь добавлен fallback по `EnumWindows` (содержит подстроку).
Если всё равно не находит:
- Открой Spy++ / `GetWindowText` и узнай точный заголовок и класс окна.
- Пропиши их в `config.hpp` (`window_title`) и при необходимости класс в
  вызове `ov.initialize(config::g_config.window_title, L"<class>")`.

### ❌ Игра крашится при инжекте
- Проверь разрядность: инжектор **строго x64**. Если `sogame.exe` 32-битный —
  нужен x86-вариант (другая calling convention, 32-битные thunk'и).
- Античит: `OpenProcess`/`CreateRemoteThread` могут блокироваться.
  Вывод инжектора покажет `GetLastError=5` (Access Denied).
- SEH/CRT: manual-map без регистрации `.pdata` может крашить DLL с CRT.
  Текущая DLL почти не использует CRT, но если добавишь — см. TODO в
  `injector/manual_map.hpp` (`register_seh_table`).

### ❌ END не выгружает DLL / игра зависает
Render-поток ждёт 3 секунды (`WaitForSingleObject(h_render, 3000)`).
Если D3D-устройство зависло — поток может не успеть. В логе ищи
`[*] Cleaning up...` — если строки нет, зависание в cleanup.

---

## 7. Цикл доработки (после первого успешного дампа)

1. Из `dump_player_dir()` выписываешь реальные имена атрибутов.
2. Правишь `bigworld.cpp` (методы `Entity::*`) и `inventory.cpp`
   (списки `first_str_attr({...})`).
3. Пересобираешь: `build.bat`.
4. Если DLL уже в процессе — нажимаешь **END** (чистая выгрузка),
   затем повторный инжект `run.bat`. Перезапускать игру не нужно.

---

## 8. Сводка файлов

| Файл | Назначение |
|---|---|
| `build.bat` | Сборка DLL + инжектора (одной командой). |
| `run.bat` | Инжект в `sogame.exe` от админа. |
| `config.hpp` | Все настройки игры в одном месте. |
| `diagnostics.cpp` | Дампы `dir()` для RE атрибутов. |
| `%TEMP%\stayout_log.txt` | Полный лог (перезаписывается каждый инжект). |
