// =============================================================================
//  diagnostics.cpp — реализация исследовательских дампов.
//
//  СТРАТЕГИЯ ИЗОЛЯЦИИ:
//    Мы намеренно НЕ расширяем python_bridge.hpp новым функционалом (риск
//    сломать уже отлаженный мост). Вместо этого резолвим пару дополнительных
//    функций C-API (PyObject_Dir, PySequence_List) ЛОКАЛЬНО из того же
//    дескриптора DLL, который уже нашёл py::initialize(). Поле py::api().dll
//    публичное — этого достаточно.
//
//    dir(obj)  в Python 2.7 = встроенная C-функция PyObject_Dir(PyObject*).
//    Она возвращает НОВЫЙ list имён атрибутов. Обходим его через уже имеющийся
//    в мосте PySequence_Length / PySequence_GetItem и конвертируем элементы в
//    строки (PyString_AsString для 2.7).
//
//  SEH-БЕЗОПАСНОСТЬ:
//    Каждый низкоуровнеый вызов обёрнут в __try/__except, потому что мы лезем
//    к «чужим» Python-объектам, чьё состояние может быть любым. C++-объекты
//    (std::string) при исключении не разрушатся: они создаются ПОСЛЕ возврата
//    из SEH-функций.
// =============================================================================

#include "diagnostics.hpp"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "python_bridge.hpp"

namespace diagnostics {

namespace {

// Указатель на PyObject_Dir(PyObject*) → возвращает новый list (или nullptr).
using PyObject_DirFn = void* (*)(void*);
// PySequence_List(list) → возвращает НОВЫЙ list (копию). Нужен, если dir()
// вернёт что-то необычное, но в 2.7 dir() уже возвращает list — используем
// как страховку для длины/индексации.
// (не обязателен — оставлен заголовком для возможного расширения).

// Резолв PyObject_Dir из уже найденной py::api().dll.
PyObject_DirFn resolve_dir() noexcept {
    HMODULE dll = py::api().dll;
    if (!dll) return nullptr;
    return reinterpret_cast<PyObject_DirFn>(GetProcAddress(dll, "PyObject_Dir"));
}

// SEH-обёртка: безопасно вызвать dir(obj). Возвращает сырой PyObject* (новую
// ссылку) или nullptr при ошибке/исключении. ВЫЗЫВАЮЩИЙ обязан Py_DecRef.
void* dir_object_seh(void* obj) noexcept {
    if (!obj) return nullptr;
    PyObject_DirFn dir_fn = resolve_dir();
    if (!dir_fn) return nullptr;
    __try {
        return dir_fn(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// SEH-обёртка для PyDict_Values. Вынесена отдельно по правилу C2712:
// __try/__except нельзя использовать в функции, где есть C++-объекты с
// деструкторами. Здесь их нет — чистый SEH, возвращает сырой указатель.
void* dict_values_seh(void* dict_obj) noexcept {
    if (!dict_obj) return nullptr;
    if (!py::api().PyDict_Values) return nullptr;
    __try {
        return py::api().PyDict_Values(dict_obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Преобразовать list имён атрибутов в многострочный текст. Работает ТОЛЬКО
// внутри py::GilScope (читает элементы через C-API моста).
std::string list_dir_to_string(void* list_obj) noexcept {
    if (!list_obj) return {};

    const auto& a = py::api();
    if (!a.PySequence_Length || !a.PySequence_GetItem) return {};

    const long n = a.PySequence_Length(list_obj);
    if (n <= 0) return "(empty dir)\n";

    std::string out;
    out.reserve(static_cast<std::size_t>(n) * 16);
    for (long i = 0; i < n && i < 4096; ++i) {
        void* item = a.PySequence_GetItem(list_obj, i); // новая ссылка
        if (!item) continue;

        // Конверсия в строку. Для Python 2.7 — PyString_AsString.
        const char* s = nullptr;
        if (!a.is_py3 && a.PyString_AsString) {
            s = a.PyString_AsString(item);
        } else if (a.is_py3 && a.PyUnicode_AsUTF8) {
            s = a.PyUnicode_AsUTF8(item);
        }
        if (s) {
            out += "  ";
            out += s;
            out += '\n';
        }
        a.Py_DecRef(item);
    }
    return out;
}

// Освободить list, полученный от dir_object_seh, через мост.
void release_list(void* list_obj) noexcept {
    if (list_obj && py::ready() && py::api().Py_DecRef) {
        py::api().Py_DecRef(list_obj);
    }
}

} // namespace

// =========================================================================
//  Публичные функции. Каждая сама захватывает GIL на время работы.
// =========================================================================
std::string health_check() noexcept {
    py::GilScope gil;
    if (!gil.ok())   return "[-] GIL: not acquired (interpreter not ready?)\n";
    if (!py::ready()) return "[-] Python bridge: not initialized\n";

    std::string out;
    out += "[+] Python bridge: ";
    out += std::string(py::dll_name());
    out += (py::api().is_py3 ? " (Py3.x)\n" : " (Py2.7)\n");

    // Доступность BigWorld.
    py::Object mod = py::import_module_safe("BigWorld");
    out += mod.valid() ? "[+] BigWorld module: OK\n"
                       : "[-] BigWorld module: NOT accessible\n";

    if (mod.valid()) {
        py::Object player = mod.call("player");
        out += player.valid()
            ? "[+] BigWorld.player(): present (in game)\n"
            : "[*] BigWorld.player(): None (not in game yet)\n";
    }
    return out;
}

std::string dump_player_dir() noexcept {
    py::GilScope gil;
    if (!gil.ok() || !py::ready()) return {};

    py::Object mod = py::import_module_safe("BigWorld");
    if (!mod.valid()) return "[-] BigWorld not accessible\n";

    py::Object player = mod.call("player");
    if (!player.valid()) return "[*] player is None (not in game)\n";

    void* dir_list = dir_object_seh(player.get());
    if (!dir_list) return "[-] dir(player) returned NULL\n";

    std::string out = "=== dir(BigWorld.player()) ===\n";
    out += list_dir_to_string(dir_list);
    release_list(dir_list);
    return out;
}

std::string dump_camera_dir() noexcept {
    py::GilScope gil;
    if (!gil.ok() || !py::ready()) return {};

    py::Object mod = py::import_module_safe("BigWorld");
    if (!mod.valid()) return "[-] BigWorld not accessible\n";

    py::Object cam = mod.call("camera");
    if (!cam.valid()) return "[*] camera is None\n";

    void* dir_list = dir_object_seh(cam.get());
    if (!dir_list) return "[-] dir(camera) returned NULL\n";

    std::string out = "=== dir(BigWorld.camera()) ===\n";
    out += list_dir_to_string(dir_list);
    release_list(dir_list);
    return out;
}

std::string dump_first_entity_dir() noexcept {
    py::GilScope gil;
    if (!gil.ok() || !py::ready()) return {};

    py::Object mod = py::import_module_safe("BigWorld");
    if (!mod.valid()) return "[-] BigWorld not accessible\n";

    // BigWorld.entities — dict; берём первое значение через .values().
    py::Object ents = mod.get_attr("entities");
    if (!ents.valid()) return "[-] BigWorld.entities not found\n";

    void* values_ptr = dict_values_seh(ents.get());
    py::Object values(values_ptr); // авто-DecRef
    if (!values.valid()) return "[-] entities.values() returned NULL\n";

    const long n = values.length();
    if (n <= 0) return "[*] entities is empty\n";

    // Берём ПЕРВУЮ сущность (индекс 0) и дампим её dir().
    py::Object first = values.at(0);
    if (!first.valid()) return "[-] first entity is NULL\n";

    void* dir_list = dir_object_seh(first.get());
    if (!dir_list) return "[-] dir(entity) returned NULL\n";

    std::string out = "=== dir(first entity in BigWorld.entities) ===\n";
    out += list_dir_to_string(dir_list);
    release_list(dir_list);
    return out;
}

} // namespace diagnostics
