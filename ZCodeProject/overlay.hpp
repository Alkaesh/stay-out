#pragma once

// =============================================================================
//  overlay.hpp — Прозрачное topmost окно поверх окна игры.
//
//  Идея: создаём отдельное WS_EX_LAYERED + WS_EX_TRANSPARENT + WS_EX_TOPMOST
//  окно размером с окно игры и рисуем на нём через D3D9 (см. render.hpp).
//  Окно:
//    * всегда поверх игры (topmost);
//    * прозрачное для мыши (WS_EX_TRANSPARENT — клики проходят сквозь);
//    * синхронизировано по позиции/размеру с окном игры (трекаем в каждом кадре).
//
//  Почему overlay, а не hook IDirect3DDevice9::EndScene:
//    1. Не нужно реверсить адрес D3D-девайса игры → устойчивость к патчам.
//    2. Не вмешиваемся в рендер-пайплайн → не крашим игру при ошибках.
//    3. Работает в windowed/borderless режиме (большинство современных игр).
//
//  Минус: не работает в exclusive fullscreen (там ОС блокирует overlay).
//  Решение: запускать игру в borderless/windowed (стандарт для Stay Out).
// =============================================================================

#include <Windows.h>

#include <cstdint>
#include <string>

namespace overlay {

struct Rect { int x, y, w, h; };

class Overlay {
public:
    static Overlay& instance();

    /// \param game_window_title  Заголовок окна игры (для FindWindow).
    /// \param class_name         (опц.) имя класса окна.
    /// Инициализация: находит окно игры, создаёт overlay-окно поверх.
    [[nodiscard]] bool initialize(std::wstring_view game_window_title,
                                  std::wstring_view class_name = {}) noexcept;

    /// Завершение: уничтожает окно и освобождает ресурсы.
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept { return hwnd_ != nullptr; }
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

    /// Прямоугольник окна игры в экранных координатах.
    [[nodiscard]] Rect game_rect() const noexcept;

    /// Синхронизировать позицию/размер overlay с игрой + проверить видимость.
    /// Вызывать каждый кадр перед отрисовкой.
    void sync_with_game() noexcept;

    /// Скрыть/показать overlay (для меню настроек или паузы).
    void show(bool visible) noexcept;

    /// HWND окна игры (для трекинга).
    [[nodiscard]] HWND game_hwnd() const noexcept { return game_hwnd_; }

private:
    Overlay() = default;

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    HWND game_hwnd_ = nullptr;  // окно игры
    HWND hwnd_      = nullptr;  // наше overlay-окно
    HINSTANCE inst_ = nullptr;
    bool visible_   = true;
};

} // namespace overlay
