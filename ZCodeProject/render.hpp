#pragma once

// =============================================================================
//  render.hpp — D3D9 отрисовка поверх overlay-окна.
//
//  Создаём IDirect3DDevice9 для overlay-окна (см. overlay.hpp). Каждый кадр:
//    1. device->BeginScene();
//    2. рисуем box/line/text (Sprite + D3DXFont / линиями);
//    3. device->EndScene(); device->Present();
//
//  Ключевой момент для прозрачного overlay: device создаётся с
//  D3DPRESENT_PARAMETERS, у которых BackBufferFormat = отображаемый формат
//  (A8R8G8B8), и НЕ очищаем кадр цветом — фон должен оставаться прозрачным,
//  чтобы было видно игру под overlay. Вместо Clear используем отключение
//  alpha-blending'а для «заливки» и включаем его только для рисуемых
//  примитивов.
//
//  ТЕХНИЧЕСКИЙ ТРЮК: Прозрачность overlay-окна реализована на уровне DWM
//  (DwmExtendFrameIntoClientArea с margins = -1). D3D9 рисует поверх этого
//  стеклянного окна. Пиксели с alpha=0 остаются прозрачными, остальные —
//  alpha-blend'ятся с игрой. Поэтому:
//    * Clear() делаем с alpha=0 (полностью прозрачно).
//    * Все примитивы рисуем с alpha=255 (непрозрачно).
//
//  Шрифт: используем DirectWrite/D3DXFont для читаемого текста. D3DX
//  подключается через d3dx9.h; если в твоей сборке нет d3dx9 — замени на
//  GDI-рисование через TextOut (см. fallback в .cpp).
// =============================================================================

#include <Windows.h>
#include <d3d9.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace render {

struct Color { std::uint8_t r, g, b, a; };

// Предустановленные цвета.
inline constexpr Color kRed    = { 255,   0,   0, 255 };
inline constexpr Color kGreen  = {   0, 255,   0, 255 };
inline constexpr Color kBlue   = {   0,   0, 255, 255 };
inline constexpr Color kWhite  = { 255, 255, 255, 255 };
inline constexpr Color kBlack  = {   0,   0,   0, 255 };
inline constexpr Color kYellow = { 255, 255,   0, 255 };

class Renderer {
public:
    static Renderer& instance();

    /// Создать D3D9-устройство для overlay-окна.
    /// \param hwnd  HWND overlay-окна (см. overlay::Overlay::hwnd()).
    [[nodiscard]] bool initialize(HWND hwnd) noexcept;

    /// Освободить ресурсы D3D9.
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept { return device_ != nullptr; }

    /// Обработать потерю устройства (после alt+tab / смены разрешения).
    /// Возвращает true, если устройство восстановлено и можно рисовать.
    [[nodiscard]] bool check_device() noexcept;

    /// Начать кадр (BeginScene + Clear с прозрачностью).
    void begin() noexcept;

    /// Завершить кадр (EndScene + Present).
    void end() noexcept;

    // -----------------------------------------------------------------
    //  Примитивы.
    // -----------------------------------------------------------------

    /// Прямоугольник (только рамка).
    void rect(float x, float y, float w, float h, Color c) noexcept;

    /// Заполненный прямоугольник.
    void rect_filled(float x, float y, float w, float h, Color c) noexcept;

    /// Линия.
    void line(float x1, float y1, float x2, float y2, Color c) noexcept;

    /// Круг (приближённо — линиями по сегментам).
    void circle(float cx, float cy, float radius, Color c,
                int segments = 24) noexcept;

    /// Текст. Накапливает команды в очередь; отрисовка происходит в
    /// flush_text() ПОСЛЕ EndScene (GDI нельзя вызывать внутри сцены).
    void text(float x, float y, std::string_view str, Color c) noexcept;

    /// Сбросить накопленный текст через GDI. Вызывать после end().
    void flush_text() noexcept;

    /// Узнать размер строки (для центрирования). Возвращает {w,h}.
    struct Size { int w, h; };
    [[nodiscard]] Size text_size(std::string_view str) noexcept;

private:
    Renderer() = default;

    HWND hwnd_ = nullptr;
    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;

    // GDI-шрифт для текста.
    HFONT font_ = nullptr;

    // Очередь отложенных текстовых команд.
    struct TextCmd {
        int x, y;
        std::string str;
        std::uint32_t color; // RGB
    };
    std::vector<TextCmd> text_queue_;

    // Размер backbuffer (для Clear и present).
    int width_ = 0, height_ = 0;
};

} // namespace render
