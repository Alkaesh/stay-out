#pragma once

// =============================================================================
//  bigworld.hpp — Высокоуровневый доступ к BigWorld через Python.
//
//  Это «родной» интерфейс Stay Out. Все вызовы идут через встроенный
//  интерпретатор Python игры (см. python_bridge.hpp).
//
//  Ключевые объекты BigWorld:
//
//    BigWorld.player()       → локальный игрок (Avatar). None, если не в игре.
//    BigWorld.entities       → dict всех известных клиенту сущностей.
//                              Ключ — entity ID (int), значение — объект.
//    BigWorld.camera()       → камера.
//    BigWorld.time()         → серверное время (float).
//    BigWorld.playerVehicleID → ID своего транспортного средства (контекстно).
//
//  Свойства сущностей (Avatar/NPC/Item) задаются в entitydef (*.def) файлах
//  и доступны как атрибуты Python-объекта:
//        avatar.position          → Vector3 (кортеж (x, y, z))
//        avatar.health            → int
//        avatar.maxHealth         → int
//        avatar.direction         → Vector3 (направление взгляда)
//        avatar.yaw / pitch       → углы
//        avatar.entityType        → строка ("Avatar", "Monster", ...)
//        avatar.id                → int
//
//  Vector3 в BigWorld — это собственный тип Vector3 (не кортеж Python!), он
//  экспортирован из C++ в Python. Доступ к компонентам: v.x, v.y, v.z.
//  Но на всякий случай поддерживаем и кортеж — в некоторых сборках движка
//  Vector3 ведёт себя как sequence.
//
//  ВАЖНО: все функции этого модуля ДОЛЖНЫ вызываться внутри py::GilScope.
//  Нарушение → краш игры.
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "python_bridge.hpp"
#include "math.hpp"

namespace bigworld {

/// Инициализация модуля: резолвит Python C-API и проверяет, что BigWorld
/// доступен. Возвращает true, если можно работать.
[[nodiscard]] bool initialize() noexcept;

// ---------------------------------------------------------------------------
//  Entity — обёртка над Python-объектом сущности.
//  Копируется дёшево (разделяет ссылку на PyObject*). Внутри удерживает
//  ссылку через py::Object.
// ---------------------------------------------------------------------------
class Entity {
public:
    Entity() = default;
    explicit Entity(py::Object obj) noexcept : obj_(std::move(obj)) {}

    [[nodiscard]] bool valid() const noexcept { return obj_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /// ID сущности (int).
    [[nodiscard]] std::optional<int> id() const noexcept;

    /// Мировая позиция (Vector3: x, y, z).
    /// В BigWorld z — вертикальная ось (как в Source Y), но это зависит
    /// от settings мира. Stay Out: типично BigWorld-система.
    [[nodiscard]] std::optional<math::Vec3> position() const noexcept;

    /// Направление взгляда (Vector3, нормализованный).
    [[nodiscard]] std::optional<math::Vec3> direction() const noexcept;

    /// Здоровье (int). Может отсутствовать у неживых сущностей.
    [[nodiscard]] std::optional<int> health() const noexcept;
    [[nodiscard]] std::optional<int> max_health() const noexcept;

    /// Имя типа сущности ("Avatar", "Monster", "Vehicle"...).
    [[nodiscard]] std::string type_name() const noexcept;

    /// Имя/никнейм (если есть). Часто хранится в .displayName или .name.
    [[nodiscard]] std::string name() const noexcept;

    /// Универсальный геттер атрибута (как число).
    [[nodiscard]] std::optional<float> get_float(const char* attr) const noexcept;
    [[nodiscard]] std::optional<int>   get_int(const char* attr) const noexcept;
    [[nodiscard]] std::string          get_string(const char* attr) const noexcept;

private:
    py::Object obj_;
};

// ---------------------------------------------------------------------------
//  Доступ к глобальному состоянию BigWorld.
// ---------------------------------------------------------------------------

/// Локальный игрок. Возвращает невалидную Entity, если игрок не в игре.
[[nodiscard]] Entity player() noexcept;

/// Все сущности в виде vector<Entity>. Перебирает BigWorld.entities.
[[nodiscard]] std::vector<Entity> all_entities() noexcept;

/// Сущность по ID.
[[nodiscard]] Entity entity_by_id(int id) noexcept;

/// Серверное время.
[[nodiscard]] std::optional<float> time() noexcept;

/// Позиция камеры.
[[nodiscard]] std::optional<math::Vec3> camera_position() noexcept;

/// Направление камеры (forward).
[[nodiscard]] std::optional<math::Vec3> camera_direction() noexcept;

// ---------------------------------------------------------------------------
//  Утилиты.
// ---------------------------------------------------------------------------

/// Выполнить Python-код в контексте игры (внутри GilScope!).
/// Полезно для отладки и исследования доступных атрибутов:
///     bigworld::exec("print dir(BigWorld.player())")
[[nodiscard]] inline int exec(std::string_view code) noexcept {
    return py::exec(code);
}

/// Получить сырой PyObject* модуля BigWorld (для продвинутых манипуляций).
[[nodiscard]] py::Object module() noexcept;

} // namespace bigworld
