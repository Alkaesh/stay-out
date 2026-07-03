// =============================================================================
//  python_bridge.cpp — Реализация моста к CPython.
// =============================================================================

#include "python_bridge.hpp"

#include <cstdio>
#include <vector>
#include <Psapi.h>  // EnumProcessModules / GetModuleBaseNameW

#pragma comment(lib, "Psapi.lib")

namespace py {

namespace {

Api         g_api;
std::string g_dll_name;
bool        g_initialized = false;

// Список возможных имён DLL с встроенным Python. BigWorld обычно использует
// python27.dll (стандартный CPython 2.7), но возможны кастомные сборки
// (wot_python.dll в WoT). Stay Out, скорее всего, использует python27.dll.
const char* kCandidateDlls[] = {
    "python27.dll",
    "python26.dll",
    "wot_python.dll",
    "python34.dll",
    "python35.dll",
    "python36.dll",
    "python37.dll",
    "python38.dll",
    "python3.dll",
};

// Резолв одной функции по имени (типизированный шаблон-хелпер).
template <typename Fn>
Fn resolve(const char* name) noexcept {
    return reinterpret_cast<Fn>(GetProcAddress(g_api.dll, name));
}

// ---------------------------------------------------------------------------
//  resolve_all_for — заполнить Api-таблицу для уже выбранного модуля (g_api.dll).
//  Вынесено отдельно, потому что модуль может быть найден двумя путями:
//    1) по списку известных имён (kCandidateDlls);
//    2) fallback-перебором всех модулей процесса по экспорту PyRun_SimpleString.
//  Возвращает true, если минимум обязательных функций зарезолвлен.
// ---------------------------------------------------------------------------
bool resolve_all_for() noexcept {
    g_api.PyRun_SimpleString     = resolve<PyRun_SimpleStringFn>("PyRun_SimpleString");
    g_api.PyImport_ImportModule  = resolve<PyImport_ImportModuleFn>("PyImport_ImportModule");
    g_api.PyObject_GetAttrString = resolve<PyObject_GetAttrStringFn>("PyObject_GetAttrString");
    g_api.PyObject_CallObject    = resolve<PyObject_CallObjectFn>("PyObject_CallObject");
    g_api.PyObject_CallMethod    = resolve<PyObject_CallMethodFn>("PyObject_CallMethod");
    g_api.PyLong_AsLong          = resolve<PyLong_AsLongFn>("PyLong_AsLong");
    g_api.PyFloat_AsDouble       = resolve<PyFloat_AsDoubleFn>("PyFloat_AsDouble");
    g_api.Py_DecRef              = resolve<Py_DecRefFn>("Py_DecRef");
    g_api.Py_IncRef              = resolve<Py_IncRefFn>("Py_IncRef");
    g_api.PyGILState_Ensure      = resolve<PyGILState_EnsureFn>("PyGILState_Ensure");
    g_api.PyGILState_Release     = resolve<PyGILState_ReleaseFn>("PyGILState_Release");
    g_api.Py_IsInitialized       = resolve<Py_IsInitializedFn>("Py_IsInitialized");

    // Sequence / list / dict.
    g_api.PySequence_Length      = resolve<PySequence_LengthFn>("PySequence_Length");
    g_api.PySequence_GetItem     = resolve<PySequence_GetItemFn>("PySequence_GetItem");
    g_api.PyList_GetItem         = resolve<PyList_GetItemFn>("PyList_GetItem");
    g_api.PyList_Size            = resolve<PyList_SizeFn>("PyList_Size");
    g_api.PyDict_Values          = resolve<PyDict_ValuesFn>("PyDict_Values");

    // Строковый API определяет версию интерпретатора.
    g_api.PyString_AsString = resolve<PyString_AsStringFn>("PyString_AsString");
    g_api.PyUnicode_AsUTF8  = resolve<PyUnicode_AsUTF8Fn>("PyUnicode_AsUTF8");
    g_api.is_py3 = (g_api.PyUnicode_AsUTF8 != nullptr && g_api.PyString_AsString == nullptr);

    // Минимум для работы: интерпретатор должен уметь запускать строку кода
    // и захватывать GIL. Остальное — лучшее усилие.
    return g_api.PyRun_SimpleString != nullptr
        && g_api.PyGILState_Ensure   != nullptr
        && g_api.Py_IsInitialized    != nullptr;
}

// ---------------------------------------------------------------------------
//  find_python_by_export — FALLBACK-поиск Python-DLL.
//
//  BigWorld иногда влинковывает CPython под нестандартным именем
//  (wot_python.dll, sogame_python.dll, или вовсе статически в экзешник).
//  В таком случае список kCandidateDlls не сработает. Тогда перебираем ВСЕ
//  загруженные в процесс модули и проверяем, экспортирует ли кто-то
//  PyRun_SimpleString — это верный признак встроенного CPython.
//
//  Возвращает HMODULE найденного модуля или nullptr.
// ---------------------------------------------------------------------------
HMODULE find_python_by_export() noexcept {
    // Первый вызов с cb=0 — узнать требуемый размер массива.
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), nullptr, 0, &needed)) return nullptr;
    if (needed == 0) return nullptr;

    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    DWORD got = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods.data(), needed, &got)) return nullptr;
    const DWORD count = got / sizeof(HMODULE);

    for (DWORD i = 0; i < count; ++i) {
        // Дешёвая предварительная проверка: есть ли нужный экспорт?
        if (GetProcAddress(mods[i], "PyRun_SimpleString") == nullptr) continue;

        // Получаем имя файла для лога.
        wchar_t name[MAX_PATH] = {};
        GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH);

        g_api.dll    = mods[i];
        g_dll_name   = "";
        // wstring→string: копируем ASCII-часть (имена системных DLL ASCII).
        for (wchar_t c : name) {
            if (c == 0) break;
            g_dll_name.push_back(static_cast<char>(c < 128 ? c : '?'));
        }

        if (resolve_all_for()) return mods[i];

        // Экспорт нашёлся, но критичные функции — нет: продолжаем поиск.
        g_api = Api{};
        g_dll_name.clear();
    }
    return nullptr;
}

} // namespace

bool initialize() noexcept {
    if (g_initialized) return g_api.dll != nullptr;

    // --- ПУТЬ 1: список известных имён DLL (быстро, точно). ---
    for (const char* name : kCandidateDlls) {
        // GetModuleHandleA — DLL уже загружена процессом игры; мы НЕ
        // LoadLibrary'им её (иначе поднимем второй интерпретатор).
        HMODULE h = GetModuleHandleA(name);
        if (!h) continue;

        g_api.dll  = h;
        g_dll_name = name;
        if (resolve_all_for()) {
            g_initialized = true;
            std::printf("[py] Found %s (Python %s)\n",
                        name, g_api.is_py3 ? "3.x" : "2.7");
            return true;
        }
        // Имя совпало, но критичных экспортов нет — не Python.
        g_api = Api{};
        g_dll_name.clear();
    }

    // --- ПУТЬ 2: fallback — перебор всех модулей по экспорту. ---
    // Срабатывает, если Stay Out использует кастомное имя Python-DLL
    // (BigWorld иногда влинковывает интерпретатор под нестандартным именем).
    std::printf("[py] Standard names not found, scanning all modules...\n");
    if (find_python_by_export()) {
        g_initialized = true;
        std::printf("[py] Found embedded Python via export scan: %s (Python %s)\n",
                    g_dll_name.c_str(), g_api.is_py3 ? "3.x" : "2.7");
        return true;
    }

    std::printf("[py] ERROR: no python DLL found among candidates or exports.\n");
    g_initialized = true;  // не пытаться снова
    return false;
}

bool ready() noexcept { return g_api.dll != nullptr; }
const Api& api() noexcept { return g_api; }
std::string_view dll_name() noexcept { return g_dll_name; }

int exec(std::string_view code) noexcept {
    if (!ready() || !g_api.PyRun_SimpleString) return -1;
    const std::string s(code);
    return g_api.PyRun_SimpleString(s.c_str());
}

// ---------------------------------------------------------------------------
//  SEH-защищённые варианты. __try/__except НЕ может сосуществовать с
//  C++-объектами в одной функции (деструкторы не вызовутся при раскрутке),
//  поэтому каждая разбита на 2 функции:
//    * impl-функция — чистый SEH, возвращает сырой void* / int.
//    * публичная функция — заворачивает результат в py::Object.
// ---------------------------------------------------------------------------
namespace {

int run_simple_string_seh(const char* code) noexcept {
    __try {
        return g_api.PyRun_SimpleString(code);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

void* import_module_seh(const char* name) noexcept {
    if (!g_api.PyImport_ImportModule) return nullptr;
    __try {
        return g_api.PyImport_ImportModule(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* getattr_seh(void* obj, const char* name) noexcept {
    if (!g_api.PyObject_GetAttrString) return nullptr;
    __try {
        return g_api.PyObject_GetAttrString(obj, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* call_method_seh(void* obj, const char* method) noexcept {
    if (!g_api.PyObject_CallMethod) return nullptr;
    __try {
        return g_api.PyObject_CallMethod(obj, const_cast<char*>(method), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace

int exec_safe(std::string_view code) noexcept {
    if (!ready()) return -1;
    const std::string s(code);
    return run_simple_string_seh(s.c_str());
}

Object import_module_safe(const char* name) noexcept {
    if (!ready()) return {};
    return Object(import_module_seh(name));
}

Object getattr_safe(const Object& obj, const char* name) noexcept {
    if (!ready() || !obj.valid()) return {};
    return Object(getattr_seh(obj.get(), name));
}

Object call_method_safe(const Object& obj, const char* method) noexcept {
    if (!ready() || !obj.valid()) return {};
    return Object(call_method_seh(obj.get(), method));
}

// =========================================================================
//  GilScope
// =========================================================================
GilScope::GilScope() noexcept {
    if (!ready()) return;
    // PyGILState_Ensure потокобезопасна: если GIL уже принадлежит этому
    // потоку, она корректно это понимает (возвращает то же состояние).
    if (g_api.Py_IsInitialized && g_api.Py_IsInitialized()) {
        state_ = g_api.PyGILState_Ensure();
        ok_ = true;
    }
}

GilScope::~GilScope() {
    if (ok_ && ready()) {
        g_api.PyGILState_Release(state_);
    }
}

// =========================================================================
//  Object
// =========================================================================
void Object::decref() noexcept {
    if (ptr_ && py::ready()) {
        const Api& a = py::api();
        if (a.Py_IsInitialized && a.Py_IsInitialized()) {
            a.Py_DecRef(ptr_);
        }
    }
    ptr_ = nullptr;
}

long Object::as_long() const noexcept {
    if (!ptr_ || !py::ready()) return 0;
    const Api& a = py::api();
    if (!a.PyLong_AsLong) return 0;
    return a.PyLong_AsLong(ptr_);
}

double Object::as_double() const noexcept {
    if (!ptr_ || !py::ready()) return 0.0;
    const Api& a = py::api();
    if (!a.PyFloat_AsDouble) return 0.0;
    return a.PyFloat_AsDouble(ptr_);
}

std::string Object::as_string() const noexcept {
    if (!ptr_ || !py::ready()) return {};
    const Api& a = py::api();
    // В Py2 str → PyString_AsString; в Py3 str → PyUnicode_AsUTF8.
    const char* s = nullptr;
    if (a.is_py3) {
        if (a.PyUnicode_AsUTF8) s = a.PyUnicode_AsUTF8(ptr_);
    } else {
        if (a.PyString_AsString) s = a.PyString_AsString(ptr_);
    }
    return s ? std::string(s) : std::string();
}

Object Object::get_attr(const char* name) const noexcept {
    if (!ptr_ || !py::ready()) return {};
    return getattr_safe(*this, name);
}

Object Object::call(const char* method) const noexcept {
    if (!ptr_ || !py::ready()) return {};
    return call_method_safe(*this, method);
}

long Object::length() const noexcept {
    if (!ptr_ || !py::ready()) return -1;
    const Api& a = py::api();
    if (!a.PySequence_Length) return -1;
    return a.PySequence_Length(ptr_);
}

Object Object::at(long index) const noexcept {
    if (!ptr_ || !py::ready()) return {};
    const Api& a = py::api();
    if (!a.PySequence_GetItem) return {};
    return Object(a.PySequence_GetItem(ptr_, index));
}

} // namespace py
