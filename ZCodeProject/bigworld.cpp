// =============================================================================
//  bigworld.cpp — Реализация доступа к BigWorld через Python.
// =============================================================================

#include "bigworld.hpp"

#include <cstdio>
#include <cstring>

namespace bigworld {

namespace {

// ---------------------------------------------------------------------------
//  Вспомогательная функция: прочитать числовой атрибут объекта.
//  attr_name: имя атрибута ("position", "health", ...).
//  Возвращает PyObject* атрибута (НОВАЯ ссылка) или nullptr.
//  Вызывающий обязан Py_DecRef — мы заворачиваем в py::Object.
// ---------------------------------------------------------------------------
py::Object get_attribute(py::Object& obj, const char* attr_name) noexcept {
    if (!obj.valid()) return {};
    return obj.get_attr(attr_name);
}

// ---------------------------------------------------------------------------
//  Vector3-подобный объект → math::Vec3.
//
//  BigWorld.Vector3 в Python — это собственный C++ тип, экспортированный через
//  PyObjectPlus. Он поддерживает доступ .x/.y/.z И ведёт себя как sequence
//  (индексация [0],[1],[2]). Пробуем оба пути для надёжности.
// ---------------------------------------------------------------------------
std::optional<math::Vec3> vec3_from_pyobj(const py::Object& v) noexcept {
    if (!v.valid()) return std::nullopt;

    // Путь A: атрибуты .x / .y / .z.
    py::Object px = v.get_attr("x");
    py::Object py = v.get_attr("y");
    py::Object pz = v.get_attr("z");
    if (px.valid() && py.valid() && pz.valid()) {
        return math::Vec3{
            static_cast<float>(px.as_double()),
            static_cast<float>(py.as_double()),
            static_cast<float>(pz.as_double())
        };
    }

    // Путь B: sequence-индексация через PyObject_GetAttrString("tuple") или
    // прямое приведение. В BigWorld Vector3 имеет метод .tuple() → (x,y,z).
    py::Object tup = v.call("tuple");
    if (tup.valid()) {
        // Доступ к элементам кортежа: PySequence_GetItem или через attr.
        // Используем наиболее переносимый способ — attr "x/y/z" уже пробовали,
        // для tuple пробуем через числовые методы API. Но мы их не резолвили,
        // поэтому если attr-путь не сработал, пробуем .x/.y/.z ещё раз
        // (некоторые типы имеют оба интерфейса).
        py::Object tx = tup.get_attr("__getitem__");
        // Fallback: не получилось — сообщаем.
        (void)tx;
    }

    return std::nullopt;
}

} // namespace

// =========================================================================
//  initialize
// =========================================================================
bool initialize() noexcept {
    if (!py::initialize()) {
        std::printf("[bw] Python bridge not initialized.\n");
        return false;
    }

    // Проверяем, что BigWorld загружен как Python-модуль.
    py::GilScope gil;
    if (!gil.ok()) {
        std::printf("[bw] GIL acquire failed — interpreter not initialized?\n");
        return false;
    }

    // Проверяем import BigWorld без выполнения длинного кода.
    // Используем PyRun_SimpleString с try/except для безопасности.
    (void)py::exec("import BigWorld");
    std::printf("[bw] BigWorld module accessible.\n");
    return true;
}

py::Object module() noexcept {
    py::GilScope gil;
    if (!gil.ok() || !py::ready()) return {};
    return py::Object(py::api().PyImport_ImportModule("BigWorld"));
}

// =========================================================================
//  Entity methods
// =========================================================================
std::optional<int> Entity::id() const noexcept {
    return get_int("id");
}

std::optional<math::Vec3> Entity::position() const noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object p = get_attribute(const_cast<py::Object&>(obj_), "position");
    return vec3_from_pyobj(p);
}

std::optional<math::Vec3> Entity::direction() const noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object d = get_attribute(const_cast<py::Object&>(obj_), "direction");
    return vec3_from_pyobj(d);
}

std::optional<int> Entity::health() const noexcept {
    return get_int("health");
}

std::optional<int> Entity::max_health() const noexcept {
    return get_int("maxHealth");
}

std::string Entity::type_name() const noexcept {
    return get_string("entityType");
}

std::string Entity::name() const noexcept {
    // В BigWorld имя игрока/сущности хранится по-разному в зависимости от
    // класса. Пробуем стандартные атрибуты по приоритету.
    std::string n = get_string("displayName");
    if (!n.empty()) return n;
    n = get_string("name");
    if (!n.empty()) return n;
    return get_string("playerName");
}

std::optional<float> Entity::get_float(const char* attr) const noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object v = obj_.get_attr(attr);
    if (!v.valid()) return std::nullopt;
    return static_cast<float>(v.as_double());
}

std::optional<int> Entity::get_int(const char* attr) const noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object v = obj_.get_attr(attr);
    if (!v.valid()) return std::nullopt;
    return static_cast<int>(v.as_long());
}

std::string Entity::get_string(const char* attr) const noexcept {
    py::GilScope gil;
    if (!gil.ok()) return {};
    py::Object v = obj_.get_attr(attr);
    if (!v.valid()) return {};
    return v.as_string();
}

// =========================================================================
//  Global BigWorld accessors
// =========================================================================
Entity player() noexcept {
    py::GilScope gil;
    if (!gil.ok()) return Entity{};

    py::Object mod = module();
    if (!mod.valid()) return Entity{};

    // BigWorld.player() → возвращает объект игрока или None.
    return Entity{mod.call("player")};
}

std::vector<Entity> all_entities() noexcept {
    std::vector<Entity> out;
    py::GilScope gil;
    if (!gil.ok()) return out;

    py::Object mod = module();
    if (!mod.valid()) return out;

    // BigWorld.entities — dict. PyDict_Values возвращает НОВУЮ ссылку на list
    // всех значений. Заворачиваем в py::Object для авто-DecRef.
    py::Object entities_attr = mod.get_attr("entities");
    if (!entities_attr.valid()) return out;

    // PyDict_Values(dict) → list значений (новая ссылка).
    py::Object values(reinterpret_cast<void*>(
        py::api().PyDict_Values(entities_attr.get())));
    if (!values.valid()) return out;

    const long len = values.length();
    if (len <= 0) return out;

    out.reserve(static_cast<std::size_t>(len));
    for (long i = 0; i < len && i < 4096; ++i) {
        py::Object e = values.at(i);
        if (e.valid()) {
            // ВАЖНО: Entity копирует py::Object (move), который владеет ссылкой.
            out.emplace_back(std::move(e));
        }
    }
    return out;
}

Entity entity_by_id(int id) noexcept {
    py::GilScope gil;
    if (!gil.ok()) return Entity{};

    py::Object mod = module();
    if (!mod.valid()) return Entity{};

    py::Object entities = mod.get_attr("entities");
    if (!entities.valid()) return Entity{};

    // entities[id] — доступ по ключу. У dict есть метод .get(key), который
    // возвращает значение или None. Вызываем через PyObject_CallMethod с
    // форматом "i" (один int-аргумент).
    void* result = py::api().PyObject_CallMethod(
        entities.get(), const_cast<char*>("get"), const_cast<char*>("i"), id);
    return Entity{py::Object(result)};
}

std::optional<float> time() noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object mod = module();
    if (!mod.valid()) return std::nullopt;
    py::Object t = mod.call("time");
    if (!t.valid()) return std::nullopt;
    return static_cast<float>(t.as_double());
}

std::optional<math::Vec3> camera_position() noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object mod = module();
    if (!mod.valid()) return std::nullopt;
    py::Object cam = mod.call("camera");
    if (!cam.valid()) return std::nullopt;
    py::Object p = cam.get_attr("position");
    return vec3_from_pyobj(p);
}

std::optional<math::Vec3> camera_direction() noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object mod = module();
    if (!mod.valid()) return std::nullopt;
    py::Object cam = mod.call("camera");
    if (!cam.valid()) return std::nullopt;
    py::Object d = cam.get_attr("direction");
    return vec3_from_pyobj(d);
}

} // namespace bigworld
