#pragma once

// =============================================================================
//  python_bridge.hpp — Мост к встроенному CPython интерпретору игры.
//
//  ПРИНЦИПИАЛЬНОЕ РЕШЕНИЕ ДЛЯ BIGWORLD:
//    Stay Out (как и World of Tanks) построена на BigWorld Technology, где
//    ВСЯ игровая логика клиента написана на Python 2.7 (Stackless Python).
//    Движок экспонирует C++ ядро в Python через модуль `BigWorld`:
//        BigWorld.player()      → локальный игрок (PlayerAvatar)
//        BigWorld.entities      → dict всех сущностей {id: entity}
//        BigWorld.camera()      → камера
//        BigWorld.time()        → серверное время
//
//    Это значит, что вместо memory scanning по оффсетам (как в Source) мы
//    ИСПОЛЬЗУЕМ СОБСТВЕННЫЙ PYTHON ИНТЕРПРЕТАТОР ИГРЫ для доступа к данным.
//    Это в разы стабильнее:
//      * не зависит от оффсетов, которые меняются между патчами;
//      * не нужно реверсить структуру C++ объектов;
//      * всё, что видит игровой Python-код, видим и мы.
//
//  BigWorld встраивает CPython (обычно 2.7, но возможны сборки 3.x).
//  Интерпретатор линкуется либо статически в экзешник, либо живёт в
//  python27.dll / wot_python.dll / аналоге рядом с игрой.
//
//  Мост состоит из двух частей:
//    1. Динамический резолв Python C-API (Py_Initialize, PyObject_GetAttrString,
//       PyRun_SimpleString, ...) из загруженной DLL. Мы НЕ линкуем python.lib
//       статически — иначе конфликт версий. Берём указатели на функции прямо
//       из адресного пространства игры.
//    2. RAII-обёртка над GIL (Global Interpreter Lock). Python C-API НЕ
//       потокобезопасен: любой вызов должен держать GIL. Наш рабочий поток
//       обязан захватывать GIL перед любым Python-вызовом, иначе — краш игры.
// =============================================================================

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace py {

// ---------------------------------------------------------------------------
//  Типы указателей на ключевые функции Python C-API (CPython 2.7 / 3.x).
//  Объявления взяты из CPython API, упрощены до минимума, нужного нам.
// ---------------------------------------------------------------------------

using PyRun_SimpleStringFn   = int  (*)(const char*);
using PyImport_ImportModuleFn= void* (*)(const char*);
using PyObject_GetAttrStringFn = void* (*)(void*, const char*);
using PyObject_CallObjectFn  = void* (*)(void*, void*);
using PyObject_CallMethodFn  = void* (*)(void*, const char*, const char*, ...);
using PyObject_GetAttrStringObjFn = void* (*)(void*, const char*);
using PyObject_SetAttrStringFn = int  (*)(void*, const char*, void*);
using PyLong_AsLongFn        = long (*)(void*);
using PyFloat_AsDoubleFn     = double (*)(void*);
using PyUnicode_AsUTF8Fn     = const char* (*)(void*);   // Py3
using PyString_AsStringFn    = const char* (*)(void*);   // Py2
using Py_DecRefFn            = void (*)(void*);
using Py_IncRefFn            = void (*)(void*);
using PyGILState_EnsureFn    = int  (*)();               // возвращает enum
using PyGILState_ReleaseFn   = void (*)(int);
using Py_IsInitializedFn     = int  (*)();

// Sequence / list / dict API — нужны для перебора BigWorld.entities.
using PySequence_LengthFn    = long (*)(void*);
using PySequence_GetItemFn   = void* (*)(void*, long);   // НОВАЯ ссылка
using PyList_GetItemFn       = void* (*)(void*, long);   // ЗАЁМНАЯ ссылка
using PyList_SizeFn          = long (*)(void*);
using PyDict_ValuesFn        = void* (*)(void*);

// ---------------------------------------------------------------------------
//  API-таблица: указатели на функции, резолвленные из python27.dll.
// ---------------------------------------------------------------------------
struct Api {
    HMODULE dll = nullptr;
    bool    is_py3 = false;  // true для Python 3.x, false для 2.7

    PyRun_SimpleStringFn       PyRun_SimpleString       = nullptr;
    PyImport_ImportModuleFn    PyImport_ImportModule    = nullptr;
    PyObject_GetAttrStringFn   PyObject_GetAttrString   = nullptr;
    PyObject_CallObjectFn      PyObject_CallObject      = nullptr;
    PyObject_CallMethodFn      PyObject_CallMethod      = nullptr;
    PyLong_AsLongFn            PyLong_AsLong            = nullptr;
    PyFloat_AsDoubleFn         PyFloat_AsDouble         = nullptr;
    PyUnicode_AsUTF8Fn         PyUnicode_AsUTF8         = nullptr;
    PyString_AsStringFn        PyString_AsString        = nullptr;
    Py_DecRefFn                Py_DecRef                = nullptr;
    Py_IncRefFn                Py_IncRef                = nullptr;
    PyGILState_EnsureFn        PyGILState_Ensure        = nullptr;
    PyGILState_ReleaseFn       PyGILState_Release       = nullptr;
    Py_IsInitializedFn         Py_IsInitialized         = nullptr;

    // Sequence / list / dict.
    PySequence_LengthFn        PySequence_Length        = nullptr;
    PySequence_GetItemFn       PySequence_GetItem       = nullptr;
    PyList_GetItemFn           PyList_GetItem           = nullptr;
    PyList_SizeFn              PyList_Size              = nullptr;
    PyDict_ValuesFn            PyDict_Values            = nullptr;
};

/// Резолвит Python C-API из загруженной в процесс DLL.
/// Ищет по списку имён: "python27.dll", "wot_python.dll", "python3.dll", ...
/// Возвращает true при успехе.
[[nodiscard]] bool initialize() noexcept;

/// Готов ли мост.
[[nodiscard]] bool ready() noexcept;

/// Доступ к API-таблице (после initialize()).
[[nodiscard]] const Api& api() noexcept;

/// Имя найденной DLL ("python27.dll").
[[nodiscard]] std::string_view dll_name() noexcept;

/// Выполнить Python-код (ОБЯЗАТЕЛЬНО внутри GilScope).
/// Пример: exec("import BigWorld; print BigWorld.player().position").
/// Возвращает 0 при успехе, -1 при ошибке.
[[nodiscard]] int exec(std::string_view code) noexcept;

/// Безопасный вызов: выполнить Python-код под SEH-защитой.
/// Гасит access violation, чтобы краш интерпретатора не утащил игру.
/// Возвращает 0 при успехе, -1 при ошибке, -2 при SEH-исключении.
[[nodiscard]] int exec_safe(std::string_view code) noexcept;

// (import_module_safe / getattr_safe / call_method_safe объявлены после
//  определения класса Object — они возвращают Object по значению.)

// ---------------------------------------------------------------------------
//  GilScope — RAII захват/освобождение GIL.
//
//  ИСПОЛЬЗОВАНИЕ ВСЕГДА:
//    {
//        py::GilScope gil;
//        py::exec("...");
//        auto* player = bigworld::get_player();
//    }  // ← GIL освобождается здесь.
//
//  Захватывать GIL нужно в ЛЮБОМ потоке, отличном от главного потока Python.
//  Без GIL вызов любой Python C-API функции → почти гарантированный краш.
// ---------------------------------------------------------------------------
class GilScope {
public:
    GilScope() noexcept;
    ~GilScope();
    GilScope(const GilScope&) = delete;
    GilScope& operator=(const GilScope&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }
private:
    int  state_ = 0;
    bool ok_    = false;
};

// ---------------------------------------------------------------------------
//  Object — RAII-обёртка над PyObject* (автоматический Py_DecRef).
//
//  Большинство Python C-API функций возвращают «новую ссылку» (new reference),
//  которую ВЫЗЫВАЮЩИЙ обязан освободить через Py_DecRef. Забыл — утечка
//  памяти в куче Python (утечки в Python не возвращаются ОС до финализации
//  интерпретатора → накапливаются). Object берёт владение на себя.
// ---------------------------------------------------------------------------
class Object {
public:
    Object() = default;
    explicit Object(void* ptr) noexcept : ptr_(ptr) {}

    Object(Object&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    Object& operator=(Object&& o) noexcept {
        if (this != &o) { decref(); ptr_ = o.ptr_; o.ptr_ = nullptr; }
        return *this;
    }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    ~Object() { decref(); }

    [[nodiscard]] void* get() const noexcept { return ptr_; }
    [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// Освобождает владение, возвращая сырой указатель (без DecRef!).
    [[nodiscard]] void* release() noexcept { void* p = ptr_; ptr_ = nullptr; return p; }

    // Преобразования в C++-типы (только внутри GilScope!).
    [[nodiscard]] long         as_long()   const noexcept;
    [[nodiscard]] double       as_double() const noexcept;
    [[nodiscard]] std::string  as_string() const noexcept;

    /// Получить атрибут как Object (новая ссылка).
    [[nodiscard]] Object get_attr(const char* name) const noexcept;

    /// Вызвать метод без аргументов (возвращает новую ссылку).
    [[nodiscard]] Object call(const char* method) const noexcept;

    // --- Sequence / list / dict helpers ---
    /// Длина sequence-like объекта (list, tuple, dict.values()). -1 при ошибке.
    [[nodiscard]] long length() const noexcept;

    /// Элемент sequence по индексу (НОВАЯ ссылка — безопасно для GC).
    /// Работает с list/tuple/dict.values() через PySequence_GetItem.
    [[nodiscard]] Object at(long index) const noexcept;

private:
    void decref() noexcept;
    void* ptr_ = nullptr;
};

// SEH-защищённые варианты Python-вызовов (объявлены здесь — после Object).
[[nodiscard]] Object import_module_safe(const char* name) noexcept;
[[nodiscard]] Object getattr_safe(const Object& obj, const char* name) noexcept;
[[nodiscard]] Object call_method_safe(const Object& obj,
                                      const char* method) noexcept;

} // namespace py
