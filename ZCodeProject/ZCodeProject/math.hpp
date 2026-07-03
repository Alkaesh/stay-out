#pragma once

// =============================================================================
//  math.hpp — Векторная математика и матрицы для игровых структур.
//
//  Используется для:
//    * Координат игрока / сущностей (Vec3).
//    * Вычисления дистанции, углов между точками, нормализации векторов.
//    * World-to-screen трансформации (Matrix4x4).
//    * Имитации camera-forward для raycast'а в будущее (x/y/z printers).
//
//  Намеренно без внешних зависимостей (ни DirectXMath, ни GLM): чистый C++20,
//  всё инлайн-оптимизируется. Под x64 выравнивание по умолчанию 16 байт для
//  SIMD-совместимой укладки (Vec4 = 16 байт).
// =============================================================================

#include <cmath>
#include <cstdint>
#include <limits>

namespace math {

// ---------------------------------------------------------------------------
//  Vec2 — 2D (X, Y). Удобно для экранных координат, углов yaw.
// ---------------------------------------------------------------------------
struct Vec2 {
    float x{0.f}, y{0.f};

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(const Vec2& o) const noexcept { return { x + o.x, y + o.y }; }
    constexpr Vec2 operator-(const Vec2& o) const noexcept { return { x - o.x, y - o.y }; }
    constexpr Vec2 operator*(float s)       const noexcept { return { x * s, y * s }; }
    Vec2& operator+=(const Vec2& o) noexcept { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) noexcept { x -= o.x; y -= o.y; return *this; }

    [[nodiscard]] constexpr float length_sqr() const noexcept { return x * x + y * y; }
    [[nodiscard]] float length() const noexcept { return std::sqrt(length_sqr()); }

    // Нормализация с защитой от деления на ноль.
    [[nodiscard]] Vec2 normalized() const noexcept {
        const float len = length();
        if (len < std::numeric_limits<float>::epsilon()) return {};
        return { x / len, y / len };
    }

    [[nodiscard]] constexpr float dot(const Vec2& o) const noexcept { return x * o.x + y * o.y; }
};

// ---------------------------------------------------------------------------
//  Vec3 — 3D (X, Y, Z). Главный тип для игровых координат.
//
//  В разных движках может быть разная система координат:
//    * Source/GoldSrc:       X — east,  Y — north, Z — up.
//    * Unity (left-handed):  X — east,  Y — up,    Z — north.
//    * Unreal:               X — north, Y — up,    Z — east.
//  Привязки осей задаются в entities.hpp под конкретный движок.
// ---------------------------------------------------------------------------
struct Vec3 {
    float x{0.f}, y{0.f}, z{0.f};

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(const Vec3& o) const noexcept {
        return { x + o.x, y + o.y, z + o.z };
    }
    constexpr Vec3 operator-(const Vec3& o) const noexcept {
        return { x - o.x, y - o.y, z - o.z };
    }
    constexpr Vec3 operator*(float s) const noexcept {
        return { x * s, y * s, z * s };
    }
    constexpr Vec3 operator/(float s) const noexcept {
        return { x / s, y / s, z / s };
    }
    Vec3& operator+=(const Vec3& o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }

    [[nodiscard]] constexpr bool operator==(const Vec3& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }

    [[nodiscard]] constexpr float length_sqr() const noexcept {
        return x * x + y * y + z * z;
    }
    [[nodiscard]] float length() const noexcept { return std::sqrt(length_sqr()); }

    // Евклидово расстояние между двумя точками.
    [[nodiscard]] float dist_to(const Vec3& o) const noexcept {
        return (*this - o).length();
    }
    [[nodiscard]] constexpr float dist_sqr_to(const Vec3& o) const noexcept {
        return (*this - o).length_sqr();
    }

    [[nodiscard]] Vec3 normalized() const noexcept {
        const float len = length();
        if (len < std::numeric_limits<float>::epsilon()) return {};
        return { x / len, y / len, z / len };
    }

    [[nodiscard]] constexpr float dot(const Vec3& o) const noexcept {
        return x * o.x + y * o.y + z * o.z;
    }

    // Векторное произведение — для построения перпендикуляров (например,
    // правого/левого вектора относительно forward).
    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const noexcept {
        return { y * o.z - z * o.y,
                 z * o.x - x * o.z,
                 x * o.y - y * o.x };
    }

    // Линейная интерполяция между этим и other по t ∈ [0, 1].
    [[nodiscard]] constexpr Vec3 lerp(const Vec3& other, float t) const noexcept {
        return { x + (other.x - x) * t,
                 y + (other.y - y) * t,
                 z + (other.z - z) * t };
    }

    // Обнулить компоненты, меньшие epsilon — полезно после накопления
    // погрешностей (камера «дрожит»). Не constexpr: std::fabs не constexpr до C++23.
    Vec3& clamp_to_zero(float eps) noexcept {
        if (std::fabs(x) < eps) x = 0.f;
        if (std::fabs(y) < eps) y = 0.f;
        if (std::fabs(z) < eps) z = 0.f;
        return *this;
    }
};

// ---------------------------------------------------------------------------
//  Vec4 — 4D (X, Y, Z, W). Удобен для однородных координат, RGBA цвета,
//  квантернионов ориентации.
// ---------------------------------------------------------------------------
struct Vec4 {
    float x{0.f}, y{0.f}, z{0.f}, w{0.f};

    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_)
        : x(x_), y(y_), z(z_), w(w_) {}

    [[nodiscard]] constexpr Vec3 xyz() const noexcept { return { x, y, z }; }
};

// ---------------------------------------------------------------------------
//  Matrix4x4 — матрица 4×4 (row-major). Используется для view/projection
//  матриц, world-to-screen.
//  Укладка совместима с DirectX (row-major) и большинством движков.
// ---------------------------------------------------------------------------
struct Matrix4x4 {
    // m[row][col]
    float m[4][4]{};

    constexpr Matrix4x4() = default;

    // Единичная матрица.
    [[nodiscard]] static constexpr Matrix4x4 identity() noexcept {
        Matrix4x4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
        return r;
    }

    // Умножение матриц (this * other) — для композиции трансформаций.
    [[nodiscard]] constexpr Matrix4x4 operator*(const Matrix4x4& o) const noexcept {
        Matrix4x4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0.f;
                for (int k = 0; k < 4; ++k) s += m[i][k] * o.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }

    // Трансформация однородной точки.
    [[nodiscard]] constexpr Vec3 transform_point(const Vec3& p) const noexcept {
        const float x = m[0][0]*p.x + m[0][1]*p.y + m[0][2]*p.z + m[0][3];
        const float y = m[1][0]*p.x + m[1][1]*p.y + m[1][2]*p.z + m[1][3];
        const float z = m[2][0]*p.x + m[2][1]*p.y + m[2][2]*p.z + m[2][3];
        return { x, y, z };
    }

    // Удобный доступ к элементам.
    constexpr float& at(int row, int col) noexcept { return m[row][col]; }
    constexpr float  at(int row, int col) const noexcept { return m[row][col]; }
};

// ---------------------------------------------------------------------------
//  QAngle — углы Эйлера (pitch, yaw, roll) в градусах. Source-style.
// ---------------------------------------------------------------------------
struct QAngle {
    float pitch{0.f};  // Вокруг Y (вверх/вниз)
    float yaw{0.f};    // Вокруг Z (влево/вправо)
    float roll{0.f};   // Вокруг X (наклон)

    constexpr QAngle() = default;
    constexpr QAngle(float p, float y, float r) : pitch(p), yaw(y), roll(r) {}

    // Прямой угол между двумя точками: куда нужно смотреть из from, чтобы
    // попасть в to. Возвращает (pitch, yaw, 0). roll всегда 0.
    [[nodiscard]] static QAngle calc_angle(const Vec3& from, const Vec3& to) noexcept {
        const Vec3 delta = to - from;
        const float hyp = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        QAngle a;
        a.pitch = -std::atan2(delta.z, hyp) * (180.f / 3.14159265358979f);
        a.yaw   =  std::atan2(delta.y, delta.x) * (180.f / 3.14159265358979f);
        a.roll  = 0.f;
        return a;
    }

    // Разница между двумя углами, нормализованная в [-180, 180].
    [[nodiscard]] static float angle_delta(float a1, float a2) noexcept {
        float d = a1 - a2;
        while (d >  180.f) d -= 360.f;
        while (d < -180.f) d += 360.f;
        return d;
    }
};

} // namespace math
