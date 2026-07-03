// =============================================================================
//  render.cpp — Реализация D3D9-отрисовки поверх overlay.
// =============================================================================

#include "render.hpp"

#include <d3d9.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <vector>

#pragma comment(lib, "d3d9.lib")

namespace render {

namespace {

// Вспомогательный Vertex для линий/прямоугольников (FVF).
// D3DFVF_XYZRHW — уже трансформированная вершина (в экранных координатах),
// не нужна view/projection матрица. z=0, rhw=1 → 2D-режим.
struct Vtx {
    float x, y, z, rhw;
    D3DCOLOR color;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

inline D3DCOLOR to_d3d(Color c) noexcept {
    return D3DCOLOR_RGBA(c.r, c.g, c.b, c.a);
}

} // namespace

Renderer& Renderer::instance() {
    static Renderer inst;
    return inst;
}

// =========================================================================
//  initialize / shutdown
// =========================================================================
bool Renderer::initialize(HWND hwnd) noexcept {
    hwnd_ = hwnd;
    d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    width_  = rc.right - rc.left;
    height_ = rc.bottom - rc.top;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed               = TRUE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8; // важно: с альфой
    pp.BackBufferWidth        = width_;
    pp.BackBufferHeight       = height_;
    pp.BackBufferCount        = 1;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE; // без vsync

    HRESULT hr = d3d_->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &pp, &device_);

    if (FAILED(hr)) {
        // Фолбэк на software vertex processing.
        hr = d3d_->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &pp, &device_);
        if (FAILED(hr)) {
            d3d_->Release();
            d3d_ = nullptr;
            return false;
        }
    }

    // GDI-шрифт для текста (фолбэк без D3DX).
    font_ = CreateFontW(
        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FF_DONTCARE, L"Consolas");

    return true;
}

void Renderer::shutdown() noexcept {
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (d3d_) { d3d_->Release(); d3d_ = nullptr; }
    hwnd_ = nullptr;
}

// ---------------------------------------------------------------------------
//  check_device — восстановление после потери устройства (alt+tab).
//
//  D3D9 cooperative level: при потере устройства Present возвращает
//  D3DERR_DEVICELOST. Нужно вызывать TestCooperativeLevel и при
//  D3DERR_DEVICENOTRESET → Reset().
// ---------------------------------------------------------------------------
bool Renderer::check_device() noexcept {
    if (!device_) return false;
    HRESULT hr = device_->TestCooperativeLevel();
    if (SUCCEEDED(hr)) return true;
    if (hr == D3DERR_DEVICELOST) return false; // ещё нельзя сбросить

    // D3DERR_DEVICENOTRESET — можно сбросить.
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed             = TRUE;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat     = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth      = width_;
    pp.BackBufferHeight     = height_;
    pp.BackBufferCount      = 1;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = device_->Reset(&pp);
    return SUCCEEDED(hr);
}

// =========================================================================
//  begin / end
// =========================================================================
void Renderer::begin() noexcept {
    if (!device_) return;

    // Очистка с alpha=0 — полностью прозрачный фон (видно игру под overlay).
    device_->Clear(0, nullptr,
                   D3DCLEAR_TARGET,
                   D3DCOLOR_RGBA(0, 0, 0, 0),  // alpha=0
                   1.0f, 0);

    device_->BeginScene();

    // Настраиваем 2D-рендер: FVF без освещения/тумана, alpha-blend включён.
    device_->SetFVF(kFvf);
    device_->SetRenderState(D3DRS_LIGHTING, FALSE);
    device_->SetRenderState(D3DRS_ZENABLE, FALSE);
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

void Renderer::end() noexcept {
    if (!device_) return;
    device_->EndScene();
    device_->Present(nullptr, nullptr, nullptr, nullptr);
}

// =========================================================================
//  Примитивы.
//
//  Все используют DrawPrimitiveUP (User Pointer) — рисуем напрямую из массива
//  Vtx без вершинных буферов. Для оверлея это нормально (мало геометрии).
// =========================================================================
void Renderer::rect(float x, float y, float w, float h, Color c) noexcept {
    if (!device_) return;
    const D3DCOLOR col = to_d3d(c);
    // 4 угла → 5 вершин (замыкаем контур).
    Vtx v[5] = {
        { x,     y,     0, 1, col },
        { x + w, y,     0, 1, col },
        { x + w, y + h, 0, 1, col },
        { x,     y + h, 0, 1, col },
        { x,     y,     0, 1, col },
    };
    device_->DrawPrimitiveUP(D3DPT_LINESTRIP, 4, v, sizeof(Vtx));
}

void Renderer::rect_filled(float x, float y, float w, float h, Color c) noexcept {
    if (!device_) return;
    const D3DCOLOR col = to_d3d(c);
    Vtx v[4] = {
        { x,     y,     0, 1, col },
        { x + w, y,     0, 1, col },
        { x,     y + h, 0, 1, col },
        { x + w, y + h, 0, 1, col },
    };
    device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

void Renderer::line(float x1, float y1, float x2, float y2, Color c) noexcept {
    if (!device_) return;
    const D3DCOLOR col = to_d3d(c);
    Vtx v[2] = { { x1, y1, 0, 1, col }, { x2, y2, 0, 1, col } };
    device_->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(Vtx));
}

void Renderer::circle(float cx, float cy, float radius, Color c,
                      int segments) noexcept {
    if (!device_ || segments < 3) return;
    const D3DCOLOR col = to_d3d(c);

    std::vector<Vtx> v;
    v.reserve(segments + 1);
    for (int i = 0; i <= segments; ++i) {
        const float ang = (2.0f * 3.14159265358979f * i) / segments;
        v.push_back({ cx + std::cos(ang) * radius,
                      cy + std::sin(ang) * radius,
                      0, 1, col });
    }
    device_->DrawPrimitiveUP(D3DPT_LINESTRIP, segments, v.data(), sizeof(Vtx));
}

// ---------------------------------------------------------------------------
//  text / flush_text — отложенное рисование текста через GDI.
//
//  Контракт D3D9: IDirect3DSurface9::GetDC нельзя вызывать между BeginScene
//  и EndScene. Поэтому text() только копит команды в text_queue_, а
//  flush_text() (вызывается ИЗВНЕ сцены, после end()) получает HDC backbuffer'а
//  один раз и выводит весь пакет через TextOutA.
// ---------------------------------------------------------------------------
void Renderer::text(float x, float y, std::string_view str, Color c) noexcept {
    if (!device_ || !font_ || str.empty()) return;
    text_queue_.push_back({
        static_cast<int>(x), static_cast<int>(y),
        std::string(str),
        RGB(c.r, c.g, c.b)
    });
}

void Renderer::flush_text() noexcept {
    if (!device_ || !font_ || text_queue_.empty()) return;

    IDirect3DSurface9* back = nullptr;
    if (FAILED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return;

    HDC hdc = nullptr;
    if (SUCCEEDED(back->GetDC(&hdc))) {
        HGDIOBJ old_font = SelectObject(hdc, font_);
        SetBkMode(hdc, TRANSPARENT);

        for (const auto& cmd : text_queue_) {
            SetTextColor(hdc, cmd.color);
            TextOutA(hdc, cmd.x, cmd.y,
                     cmd.str.c_str(), static_cast<int>(cmd.str.size()));
        }

        SelectObject(hdc, old_font);
        back->ReleaseDC(hdc);
    }
    back->Release();
    text_queue_.clear();
}

Renderer::Size Renderer::text_size(std::string_view str) noexcept {
    if (!device_ || str.empty()) return { 0, 0 };
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return { static_cast<int>(str.size()) * 8, 16 };

    Size sz{ 0, 16 };
    HDC hdc = nullptr;
    if (SUCCEEDED(back->GetDC(&hdc))) {
        HGDIOBJ old_font = SelectObject(hdc, font_);
        const std::string s(str);
        SIZE gdi_sz{};
        if (GetTextExtentPoint32A(hdc, s.c_str(), static_cast<int>(s.size()), &gdi_sz)) {
            sz.w = gdi_sz.cx;
            sz.h = gdi_sz.cy;
        }
        SelectObject(hdc, old_font);
        back->ReleaseDC(hdc);
    }
    back->Release();
    return sz;
}

} // namespace render
