#pragma once

// =============================================================================
//  inventory.hpp — Доступ к инвентарю игрока через Python (BigWorld).
//
//  В BigWorld инвентарь игрока доступен через BigWorld.player().inventory.
//  Это Python-объект с методами/атрибутами, зависящими от конкретной игры.
//  Типичный интерфейс (на примере WoT-подобных и Stay Out):
//    inventory.items      → dict {item_id: item_obj} или list[item_obj]
//    inventory.getItem(id) → item_obj
//    inventory.count      → число предметов
//
//  Каждый item_obj обычно имеет атрибуты:
//    .id        → int
//    .name / .displayName → str
//    .quantity  → int (стопка)
//    .itemType  → str ("Weapon", "Ammo", "Resource", ...)
//    .weight    → float
//    .level     → int
//    .position  → позиция в сетке инвентаря (tuple/int)
//
//  Stay Out — survival, значит в инвентаре: оружие, расходники, ресурсы,
//  броня. Структура может отличаться от перечисленной — это КАРКАС.
//
//  Для точной структуры запусти dir(inventory) / dir(item) в Python-REPL
//  (F8 в нашем main.cpp) и подставь актуальные имена атрибутов.
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "python_bridge.hpp"

namespace inventory {

/// Один предмет инвентаря.
struct Item {
    std::int64_t id{};
    std::string  name;
    std::string  type;
    int          quantity  = 1;
    float        weight    = 0.f;
    int          level     = 0;
    std::string  raw_attrs;  // dump dir(item) для отладки
};

/// Получить сам объект inventory (Python). Возвращает невалидный, если
/// игрок не в игре или инвентарь недоступен.
[[nodiscard]] py::Object get_inventory_object() noexcept;

/// Считать все предметы инвентаря в вектор Item.
/// Пробует несколько стандартных путей доступа (items/getItems/values),
/// потому что точный API зависит от игры.
[[nodiscard]] std::vector<Item> get_all() noexcept;

/// Найти предмет по ID.
[[nodiscard]] std::optional<Item> get_by_id(std::int64_t id) noexcept;

/// Общее число предметов (быстро, без перебора).
[[nodiscard]] std::optional<int> count() noexcept;

/// Общий вес (сумма weight всех предметов).
[[nodiscard]] float total_weight() noexcept;

/// Дамп инвентаря в строку (для лога / overlay).
[[nodiscard]] std::string dump_string() noexcept;

/// Подсчёт предметов определённого типа.
[[nodiscard]] int count_by_type(std::string_view type) noexcept;

} // namespace inventory
