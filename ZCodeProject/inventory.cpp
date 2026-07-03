// =============================================================================
//  inventory.cpp — Реализация доступа к инвентарю.
// =============================================================================

#include "inventory.hpp"

#include <utility>

#include "bigworld.hpp"

#include <cstdio>
#include <format>
#include <algorithm>

namespace inventory {

// ---------------------------------------------------------------------------
//  read_item — вытащить Item из Python-объекта предмета.
//  Пробуем несколько возможных имён атрибутов для каждого поля — точные
//  имена зависят от игры. Пробуем по приоритету, берём первое валидное.
// ---------------------------------------------------------------------------
namespace {

std::string first_str_attr(const py::Object& obj,
                           std::initializer_list<const char*> names) noexcept {
    for (const char* n : names) {
        py::Object v = obj.get_attr(n);
        if (v.valid()) {
            std::string s = v.as_string();
            if (!s.empty()) return s;
        }
    }
    return {};
}

long first_int_attr(const py::Object& obj,
                    std::initializer_list<const char*> names) noexcept {
    for (const char* n : names) {
        py::Object v = obj.get_attr(n);
        if (v.valid()) return v.as_long();
    }
    return 0;
}

double first_float_attr(const py::Object& obj,
                        std::initializer_list<const char*> names) noexcept {
    for (const char* n : names) {
        py::Object v = obj.get_attr(n);
        if (v.valid()) return v.as_double();
    }
    return 0.0;
}

Item read_item(const py::Object& obj) noexcept {
    Item it;
    if (!obj.valid()) return it;

    it.id       = first_int_attr(obj, { "id", "itemID", "itemID_" });
    it.name     = first_str_attr(obj, { "displayName", "name", "itemName", "shortName" });
    it.type     = first_str_attr(obj, { "itemType", "type", "category", "className" });
    it.quantity = static_cast<int>(first_int_attr(obj, { "quantity", "count", "stack", "amount" }));
    it.weight   = static_cast<float>(first_float_attr(obj, { "weight", "mass" }));
    it.level    = static_cast<int>(first_int_attr(obj, { "level", "tier", "grade" }));

    if (it.quantity <= 0) it.quantity = 1;
    return it;
}

} // namespace

// =========================================================================
//  Public API
// =========================================================================
py::Object get_inventory_object() noexcept {
    py::GilScope gil;
    if (!gil.ok()) return {};

    // BigWorld.player().inventory — стандартный путь в BigWorld.
    py::Object mod = bigworld::module();
    if (!mod.valid()) return {};
    py::Object player = mod.call("player");
    if (!player.valid()) return {};
    return player.get_attr("inventory");
}

std::vector<Item> get_all() noexcept {
    std::vector<Item> out;
    py::GilScope gil;
    if (!gil.ok()) return out;

    py::Object inv = get_inventory_object();
    if (!inv.valid()) return out;

    // Путь A: inv.items — dict-подобный объект.
    py::Object items = inv.get_attr("items");
    if (items.valid()) {
        // items может быть dict или callable. Если callable — вызываем.
        // У Python dict .items() — это method, возвращающий view.
        // Проверяем: если у items есть __call__ — вызываем.
        py::Object maybe_call = items.call("__call__");
        if (maybe_call.valid()) {
            items = std::move(maybe_call);
        }

        // Если items — это dict {id: item}, берём .values().
        py::Object values(reinterpret_cast<void*>(
            py::api().PyDict_Values(items.get())));
        if (values.valid()) {
            const long len = values.length();
            for (long i = 0; i < len && i < 4096; ++i) {
                py::Object v = values.at(i);
                if (v.valid()) out.push_back(read_item(v));
            }
            return out;
        }
    }

    // Путь B: inv.getItems() — метод, возвращающий list.
    py::Object got = inv.call("getItems");
    if (got.valid()) {
        const long len = got.length();
        for (long i = 0; i < len && i < 4096; ++i) {
            py::Object v = got.at(i);
            if (v.valid()) out.push_back(read_item(v));
        }
        return out;
    }

    // Путь C: сам inv — это list/tuple.
    const long len = inv.length();
    if (len > 0) {
        for (long i = 0; i < len && i < 4096; ++i) {
            py::Object v = inv.at(i);
            if (v.valid()) out.push_back(read_item(v));
        }
        return out;
    }

    return out;
}

std::optional<Item> get_by_id(std::int64_t id) noexcept {
    const auto all = get_all();
    for (const auto& it : all) {
        if (it.id == id) return it;
    }
    return std::nullopt;
}

std::optional<int> count() noexcept {
    py::GilScope gil;
    if (!gil.ok()) return std::nullopt;
    py::Object inv = get_inventory_object();
    if (!inv.valid()) return std::nullopt;

    // Пробуем .count / .size / len().
    py::Object c = inv.get_attr("count");
    if (c.valid()) return static_cast<int>(c.as_long());

    const long len = inv.length();
    if (len >= 0) return static_cast<int>(len);
    return std::nullopt;
}

float total_weight() noexcept {
    const auto all = get_all();
    float sum = 0.f;
    for (const auto& it : all) sum += it.weight * it.quantity;
    return sum;
}

std::string dump_string() noexcept {
    const auto all = get_all();
    std::string out = std::format("Inventory ({} items, {:.1f}kg):\n",
                                  all.size(), total_weight());
    for (const auto& it : all) {
        out += std::format("  [{:>5}] {:<24} x{:>3}  {:.1f}kg  [{}]\n",
                           it.id, it.name, it.quantity, it.weight, it.type);
    }
    return out;
}

int count_by_type(std::string_view type) noexcept {
    const auto all = get_all();
    int n = 0;
    for (const auto& it : all) {
        if (it.type.find(type) != std::string::npos) ++n;
    }
    return n;
}

} // namespace inventory
