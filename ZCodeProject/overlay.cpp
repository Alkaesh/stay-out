// =============================================================================
//  overlay.cpp — Реализация прозрачного overlay-окна.
// =============================================================================

#include "overlay.hpp"

#include <dwmapi.h>
#include <algorithm>   // std::search

#pragma comment(lib, "dwmapi.lib")

namespace overlay {

// ---------------------------------------------------------------------------
//  find_window_by_title_contains — fallback-поиск окна игры.
//
//  Перебираем все top-level окна системы через EnumWindows и ищем то, чей
//  заголовок СОДЕРЖИТ искомую подстроку (регистронезависимо). Сравниваем
//  также класс окна, если задан.
//
//  Зачем: FindWindowW требует ТОЧНОГО совпадения. Но Stay Out часто добавляет
//  к заголовку суффикс версии/бренда, и точный поиск мажет. Частичный поиск
//  по "Stay Out" найдёт и "Stay Out", и "Stay Out v0.4.2 (Sandbox)".
//
//  Реализация через lParam-параметр EnumWindows: передаём структуру с
//  условием поиска и адресом результата.
// ---------------------------------------------------------------------------
namespace {

struct FindCtx {
    std::wstring_view needle;
    std::wstring_view class_name;
    HWND result = nullptr;
};

BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp) noexcept {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    if (!ctx) return TRUE; // продолжаем

    // Пропускаем невидимые окна — экономим вызовы GetWindowTextW.
    if (!IsWindowVisible(hwnd)) return TRUE;

    // Проверка класса окна, если он задан.
    if (!ctx->class_name.empty()) {
        wchar_t cls_buf[256] = {};
        GetClassNameW(hwnd, cls_buf, 256);
        const std::wstring_view cls(cls_buf);
        // Регистронезависивое сравнение классов.
        if (cls.size() != ctx->class_name.size() ||
            _wcsnicmp(cls.data(), ctx->class_name.data(), cls.size()) != 0) {
            return TRUE;
        }
    }

    // Читаем заголовок и ищем needle как подстроку (без учёта регистра).
    wchar_t title_buf[512] = {};
    const int len = GetWindowTextW(hwnd, title_buf, 512);
    if (len <= 0) return TRUE;

    const std::wstring_view title(title_buf, static_cast<std::size_t>(len));
    // Регистронезависимый CONTAINS через _wcsnicmp по каждой стартовой позиции.
    if (title.size() < ctx->needle.size()) return TRUE;
    for (std::size_t i = 0; i + ctx->needle.size() <= title.size(); ++i) {
        if (_wcsnicmp(title.data() + i, ctx->needle.data(),
                      ctx->needle.size()) == 0) {
            ctx->result = hwnd;
            return FALSE; // нашли — останавливаем перебор
        }
    }
    return TRUE;
}

HWND find_window_by_title_contains(std::wstring_view needle,
                                   std::wstring_view class_name) noexcept {
    FindCtx ctx{ needle, class_name, nullptr };
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

} // namespace

Overlay& Overlay::instance() {
    static Overlay inst;
    return inst;
}

// ---------------------------------------------------------------------------
//  wnd_proc — оконная процедура overlay-окна.
//  Почти всё пропускаем через DefWindowProc; WM_PAINT не обрабатываем
//  (рисуем через D3D9 в render-модуле, а не через GDI).
// ---------------------------------------------------------------------------
LRESULT CALLBACK Overlay::wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(h, msg, wp, lp);
}

bool Overlay::initialize(std::wstring_view game_window_title,
                         std::wstring_view class_name) noexcept {
    inst_ = GetModuleHandleW(nullptr);

    const std::wstring title(game_window_title);
    const std::wstring cls(class_name);

    // 1. Находим окно игры.

    // ПУТЬ A — точное совпадение через FindWindowW (быстро).
    game_hwnd_ = FindWindowW(
        cls.empty() ? nullptr : cls.c_str(),
        title.empty() ? nullptr : title.c_str());

    // ПУТЬ B — fallback: перебор всех top-level окон через EnumWindows с
    // частичным совпадением заголовка. Нужен, потому что игра часто добавляет
    // к заголовку суффикс версии (например, "Stay Out v0.4.2 build 12345").
    // Сравнение регистронезависимое, проверяем CONTAINS.
    if (!game_hwnd_ && !title.empty()) {
        game_hwnd_ = find_window_by_title_contains(title, cls);
    }

    if (!game_hwnd_) {
        // Окно не найдено ни одним способом — ESP будет недоступен.
        return false;
    }

    // 2. Регистрируем класс окна overlay.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst_;
    wc.lpszClassName = L"StayOutOverlayClass";
    RegisterClassExW(&wc);

    // 3. Получаем начальный прямоугольник игры.
    RECT rc{};
    GetWindowRect(game_hwnd_, &rc);

    // 4. Создаём окно с расширенными стилями для прозрачности + клик-сквозности.
    //    WS_EX_LAYERED     — позволяет установить прозрачность через
    //                        DwmExtendFrameIntoClientArea / SetLayeredWindowAttributes.
    //    WS_EX_TRANSPARENT — мышь проходит сквозь окно к игре.
    //    WS_EX_TOPMOST     — всегда поверх остальных окон.
    //    WS_EX_NOACTIVATE  — окно не крадёт фокус при клике.
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"StayOutOverlay",
        WS_POPUP,
        rc.left, rc.top, w, h,
        nullptr, nullptr, inst_, nullptr);

    if (!hwnd_) return false;

    // Расширяяем рамку на всю клиентскую область → делаем фон полностью
    // прозрачным через DWM. Это надёжнее SetLayeredWindowAttributes для
    // alpha-blending'а и совместимо с D3D9.
    MARGINS margins = { -1 }; // -1 = расширенная рамка на всё окно (glass)
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    return true;
}

void Overlay::shutdown() noexcept {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    // Класс можно не анрегистрировать — он живёт до конца процесса.
    game_hwnd_ = nullptr;
}

Rect Overlay::game_rect() const noexcept {
    if (!game_hwnd_) return {0, 0, 0, 0};
    RECT rc{};
    GetWindowRect(game_hwnd_, &rc);
    return { rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top };
}

void Overlay::sync_with_game() noexcept {
    if (!hwnd_ || !game_hwnd_) return;

    RECT rc{};
    GetWindowRect(game_hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // Двигаем overlay вслед за окном игры. SetWindowPos с SWP_NOACTIVATE
    // чтобы не красть фокус.
    SetWindowPos(hwnd_, HWND_TOPMOST,
                 rc.left, rc.top, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Если игра свернута — скрываем overlay.
    if (IsIconic(game_hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    } else if (visible_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

void Overlay::show(bool visible) noexcept {
    visible_ = visible;
    if (!hwnd_) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

} // namespace overlay
