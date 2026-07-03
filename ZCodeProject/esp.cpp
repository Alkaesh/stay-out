// =============================================================================
//  esp.cpp — Реализация ESP.
// =============================================================================

#include "esp.hpp"
#include "overlay.hpp"

#include <cmath>
#include <format>
#include <algorithm>

namespace esp {

namespace {

Config g_cfg;

// ---------------------------------------------------------------------------
//  build_view_matrix — строим right-handed lookAt матрицу 4x4 из позиции и
//  направления камеры. Это аналог gluLookAt.
//
//  right-handed lookAt:
//    forward = normalize(direction)
//    right   = normalize(cross(world_up, forward))   // world_up = (0,1,0)
//    up      = cross(forward, right)
//  Матрица view переводит world → camera space.
// ---------------------------------------------------------------------------
math::Matrix4x4 build_view_matrix(math::Vec3 cam_pos, math::Vec3 cam_dir) noexcept {
    math::Vec3 forward = cam_dir.normalized();
    // Если forward коллинеарен up — кросс-произведение обнуляется. Защита.
    math::Vec3 world_up{ 0.f, 1.f, 0.f };
    if (std::fabs(forward.dot(world_up)) > 0.999f) {
        world_up = { 0.f, 0.f, 1.f };
    }

    math::Vec3 right = world_up.cross(forward).normalized();
    math::Vec3 up    = forward.cross(right);

    // View matrix (row-major). Трансляция = -dot(cam_pos, axis).
    math::Matrix4x4 v{};
    v.m[0][0] = right.x;  v.m[0][1] = right.y;  v.m[0][2] = right.z;  v.m[0][3] = -right.dot(cam_pos);
    v.m[1][0] = up.x;     v.m[1][1] = up.y;     v.m[1][2] = up.z;     v.m[1][3] = -up.dot(cam_pos);
    v.m[2][0] = forward.x;v.m[2][1] = forward.y;v.m[2][2] = forward.z;v.m[2][3] = -forward.dot(cam_pos);
    v.m[3][0] = 0.f;      v.m[3][1] = 0.f;      v.m[3][2] = 0.f;      v.m[3][3] = 1.f;
    return v;
}

// ---------------------------------------------------------------------------
//  build_proj_matrix — перспективная проекция (right-handed, OpenGL-style).
//
//  f = 1 / tan(fov/2)
//  aspect = width / height
//  Матрица переводит camera space → clip space.
// ---------------------------------------------------------------------------
math::Matrix4x4 build_proj_matrix(float fov_deg, float aspect) noexcept {
    const float f = 1.f / std::tan(fov_deg * 0.5f * 3.14159265358979f / 180.f);
    math::Matrix4x4 p{};
    p.m[0][0] = f / aspect;
    p.m[1][1] = f;
    p.m[2][2] = -1.f;       // far = бесконечность, near = 1
    p.m[2][3] = -1.f;
    p.m[3][2] = -1.f;       // near plane distance
    p.m[3][3] = 0.f;
    return p;
}

// ---------------------------------------------------------------------------
//  project — финальная проекция world → screen.
//  Возвращает nullopt, если точка за камерой (z < 0 в camera space).
// ---------------------------------------------------------------------------
struct ScreenOut { float x, y; };

std::optional<ScreenOut> project(math::Vec3 world,
                                 const math::Matrix4x4& view,
                                 const math::Matrix4x4& proj,
                                 int screen_w, int screen_h) noexcept {
    // world → camera space (умножаем на view).
    math::Vec4 cam = {
        view.m[0][0]*world.x + view.m[0][1]*world.y + view.m[0][2]*world.z + view.m[0][3],
        view.m[1][0]*world.x + view.m[1][1]*world.y + view.m[1][2]*world.z + view.m[1][3],
        view.m[2][0]*world.x + view.m[2][1]*world.y + view.m[2][2]*world.z + view.m[2][3],
        1.f
    };

    // В right-handed: видна только если cam.z < 0 (точка впереди камеры).
    // Если cam.z >= 0 → точка сзади, не рисуем.
    if (cam.z >= 0.f) return std::nullopt;

    // camera → clip space (умножаем на proj).
    float clip_x = proj.m[0][0]*cam.x;
    float clip_y = proj.m[1][1]*cam.y;
    float clip_w = proj.m[3][2]*cam.z + proj.m[3][3]*cam.w; // = -cam.z

    if (std::fabs(clip_w) < 1e-6f) return std::nullopt;
    const float inv_w = 1.f / clip_w;

    // NDC [-1, 1].
    const float ndc_x = clip_x * inv_w;
    const float ndc_y = clip_y * inv_w;

    // NDC → пиксели. Y перевёрнут (0 сверху).
    ScreenOut out;
    out.x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(screen_w);
    out.y = (0.5f - ndc_y * 0.5f) * static_cast<float>(screen_h);
    return out;
}

} // namespace

// =========================================================================
//  Public API.
// =========================================================================
void configure(Config cfg) noexcept { g_cfg = std::move(cfg); }
const Config& current_config() noexcept { return g_cfg; }

std::vector<EspEntry> collect(const Config& cfg) noexcept {
    std::vector<EspEntry> out;

    // Локальный игрок — для расчёта дистанций и отсева «себя».
    bigworld::Entity me = bigworld::player();
    std::optional<math::Vec3> my_pos;
    if (me.valid()) my_pos = me.position();

    const auto entities = bigworld::all_entities();
    out.reserve(entities.size());

    for (const auto& e : entities) {
        if (!e.valid()) continue;
        if (my_pos && e.id() == me.id()) continue; // себя не рисуем.

        const auto pos = e.position();
        if (!pos) continue;

        // Дистанция от игрока.
        const float dist = my_pos ? my_pos->dist_to(*pos) : 0.f;
        if (dist > cfg.max_distance) continue;

        // Фильтр по типу.
        const std::string tname = e.type_name();
        const bool is_player = tname.find("Avatar") != std::string::npos ||
                               tname.find("Player") != std::string::npos;
        const bool is_npc = tname.find("Monster") != std::string::npos ||
                            tname.find("NPC") != std::string::npos ||
                            tname.find("Bot") != std::string::npos;

        if (is_player && !cfg.show_players) continue;
        if (is_npc && !cfg.show_npcs) continue;
        if (!is_player && !is_npc && !cfg.show_items) continue;

        EspEntry entry;
        entry.world_pos = *pos;
        entry.hp        = e.health().value_or(-1);
        entry.max_hp    = e.max_health().value_or(-1);
        entry.is_enemy  = is_npc || (is_player && cfg.show_players);

        // Подпись.
        std::string nm = e.name();
        if (nm.empty()) nm = is_npc ? "NPC" : (is_player ? "Player" : tname);
        entry.label = std::format("{} [{}m]", nm, static_cast<int>(dist));
        out.push_back(std::move(entry));
    }
    return out;
}

void draw(const std::vector<EspEntry>& entries,
          int screen_w, int screen_h,
          math::Vec3 cam_pos, math::Vec3 cam_dir,
          float fov_deg, const Config& cfg) noexcept {
    if (entries.empty()) return;

    auto& r = render::Renderer::instance();
    if (!r.ready()) return;

    // Строим матрицы один раз для всех сущностей.
    const math::Matrix4x4 view = build_view_matrix(cam_pos, cam_dir);
    const float aspect = static_cast<float>(screen_w) /
                         static_cast<float>(std::max(1, screen_h));
    const math::Matrix4x4 proj = build_proj_matrix(fov_deg, aspect);

    for (const auto& e : entries) {
        // Проецируем позицию ног и головы.
        const auto feet = project(e.world_pos, view, proj, screen_w, screen_h);
        if (!feet) continue;

        math::Vec3 head_world = e.world_pos;
        if (cfg.z_up) {
            head_world.z += cfg.box_height_units; // Z-up (типичный BigWorld)
        } else {
            head_world.y += cfg.box_height_units; // Y-up (Source-подобные)
        }
        const auto head = project(head_world, view, proj, screen_w, screen_h);
        if (!head) continue;

        // Размер box'а на экране.
        const float box_h = std::fabs(feet->y - head->y);
        if (box_h < 4.f) continue; // слишком далеко / мелко
        const float box_w = box_h * (cfg.box_width_units / cfg.box_height_units);

        const float bx = head->x - box_w * 0.5f;
        const float by = head->y;

        const render::Color color = e.is_enemy ? render::kRed : render::kGreen;

        if (cfg.draw_box) {
            r.rect(bx, by, box_w, box_h, color);
        }
        if (cfg.draw_health_bar && e.hp >= 0 && e.max_hp > 0) {
            const float frac = std::clamp(static_cast<float>(e.hp) / e.max_hp, 0.f, 1.f);
            // Фон.
            r.rect_filled(bx - 6.f, by, 4, box_h, render::kBlack);
            // Заполнение (зелёное→жёлтое→красное по убыванию HP).
            render::Color hc = frac > 0.5f ? render::kGreen :
                              (frac > 0.25f ? render::kYellow : render::kRed);
            r.rect_filled(bx - 6.f, by + box_h * (1.f - frac), 4, box_h * frac, hc);
        }
        if (cfg.draw_name || cfg.draw_distance) {
            r.text(bx, by - 16.f, e.label, color);
        }
        if (cfg.draw_snapline) {
            r.line(static_cast<float>(screen_w) * 0.5f,
                   static_cast<float>(screen_h),
                   feet->x, feet->y, color);
        }
    }
}

void render_once(const Config& cfg) noexcept {
    if (!cfg.enabled) return;

    auto& ov = overlay::Overlay::instance();
    if (!ov.ready()) return;

    // Собрать данные.
    auto entries = collect(cfg);
    if (entries.empty()) return;

    // Камера.
    const auto cam_pos = bigworld::camera_position().value_or(math::Vec3{});
    const auto cam_dir = bigworld::camera_direction().value_or(math::Vec3{0,0,1});

    // FOV: пробуем camera.fov через Python, иначе fallback из конфига.
    float fov = cfg.default_fov_deg;
    {
        py::GilScope gil;
        if (gil.ok()) {
            py::Object mod = bigworld::module();
            if (mod.valid()) {
                py::Object cam = mod.call("camera");
                if (cam.valid()) {
                    py::Object fov_obj = cam.get_attr("fov");
                    if (fov_obj.valid()) {
                        const double v = fov_obj.as_double();
                        if (v > 0.0 && v < 3.15) fov = static_cast<float>(v);
                    }
                }
            }
        }
    }

    const auto rect = ov.game_rect();
    draw(entries, rect.w, rect.h, cam_pos, cam_dir, fov, cfg);
}

} // namespace esp
