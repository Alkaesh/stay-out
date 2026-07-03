#pragma once

// =============================================================================
//  esp.hpp — Extra Sensory Perception (ESP) поверх overlay.
//
//  Берёт данные сущностей через bigworld::* (Python-мост), проецирует их
//  мировые координаты в экранные и рисует box'ы через render::*.
//
//  КЛЮЧЕВОЙ МОМЕНТ — проекция в BigWorld.
//  В Source у нас была готовая view matrix. В BigWorld движок даёт:
//    * camera.position  → точка, где камера (Vec3)
//    * camera.direction → куда смотрит (нормализованный Vec3)
//  Полной 4x4 матрицы через Python обычно нет. Поэтому СТРОИМ её сами через
//  классическую lookAt + перспективную проекцию.
//
//  Допущения BigWorld (типичные):
//    * FOV часто фиксирован (75° для типичных MMO) или доступен через
//      BigWorld.camera().fov. Резолвим опционально; иначе fallback.
//    * Система координат: Y-up в некоторых сборках, Z-up в других.
//      Stay Out: типично Y-up (как Source) — но проверь по визуалу ESP.
//    * Aspect ratio = ширина/высота overlay-окна.
//
//  Что рисуется:
//    * Bounding box вокруг сущности (голова→ноги).
//    * Линия от низа экрана (radar-style) до сущности.
//    * Подпись: имя + HP.
//    * Health-bar слева от box'а.
// =============================================================================

#include <cstdint>
#include <optional>
#include <vector>

#include "bigworld.hpp"
#include "math.hpp"
#include "render.hpp"

namespace esp {

/// Настройки ESP.
struct Config {
    bool     enabled           = true;
    float    max_distance      = 300.f;     // в игровых единицах (метрах)
    bool     show_players      = true;
    bool     show_npcs         = true;
    bool     show_items        = false;
    bool     draw_box          = true;
    bool     draw_health_bar   = true;
    bool     draw_name         = true;
    bool     draw_distance     = true;
    bool     draw_snapline     = false;     // линия от низа экрана
    float    box_height_units  = 1.8f;      // рост человека в единицах
    float    box_width_units   = 0.6f;
    float    default_fov_deg   = 75.f;      // fallback FOV

    /// Ось роста игрока в мировой системе координат.
    /// YUp — рост по Y (Source-подобные), ZUp — рост по Z (типичный BigWorld).
    /// Берётся из config::g_config, но может быть переопределён здесь.
    bool     z_up              = false;
};

/// Один рассчитанный элемент для отрисовки.
struct EspEntry {
    math::Vec3 world_pos;       // позиция (ноги)
    std::string label;          // "Имя [50m]"
    int        hp       = -1;
    int        max_hp   = -1;
    bool       is_enemy = true;
};

/// Собрать список сущностей для отрисовки (фильтрация по distance/type).
/// ВАЖНО: вызывает Python → должно быть в главном потоке или с GIL.
[[nodiscard]] std::vector<EspEntry> collect(const Config& cfg) noexcept;

/// Отрисовать собранные элементы поверх overlay.
/// \param screen_w/h  размеры overlay-окна.
/// \param cam_pos     позиция камеры (из bigworld::camera_position).
/// \param cam_dir     направление камеры (из bigworld::camera_direction).
/// \param fov_deg     угол обзора камеры.
void draw(const std::vector<EspEntry>& entries,
          int screen_w, int screen_h,
          math::Vec3 cam_pos, math::Vec3 cam_dir,
          float fov_deg, const Config& cfg) noexcept;

/// Один проход: собрать + отрисовать. Удобный шорткат.
void render_once(const Config& cfg) noexcept;

/// Настроить конфигурацию (вызывается из main).
void configure(Config cfg) noexcept;
[[nodiscard]] const Config& current_config() noexcept;

} // namespace esp
