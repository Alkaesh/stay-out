// =============================================================================
//  main.cpp — Stay Out research DLL: BigWorld + ESP overlay + inventory.
//
//  Архитектура процесса после инжекта:
//
//    ┌─ Главный поток DLL (worker_thread) ──────────────────────────┐
//    │  • init Python bridge + BigWorld                              │
//    │  • горячие клавиши (END/F6..F12)                              │
//    │  • периодический dump в консоль                               │
//    └───────────────────────────────────────────────────────────────┘
//    ┌─ Render-поток (render_thread) ───────────────────────────────┐
//    │  • ~60 FPS цикл отрисовки overlay                             │
//    │  • sync overlay с окном игры                                  │
//    │  • сбор сущностей (Python/GIL) → ESP-расчёт → D3D9-рисование  │
//    └───────────────────────────────────────────────────────────────┘
//
//  ПОЧЕМУ ОТДЕЛЬНЫЙ RENDER-ПОТОК:
//    Отрисовка должна идти с постоянным FPS (~60), а Python-вызовы (с GIL)
//    могут задерживаться. Если делать всё в одном потоке — ESP будет
//    «дёргаться». Разделение: render-поток стабильно крутит кадры, GIL
//    захватывается только на момент сбора данных.
//
//  СИНХРОНИЗАЦИЯ:
//    Render-поток берёт GIL только для bigworld::collect() (коротко),
//    затем отпускает и рисует без GIL. Главный поток тоже изредка берёт
//    GIL для дампов. Конфликтов нет: GIL — мьютекс, Python сериализует.
// =============================================================================

#include <Windows.h>

#include <cstdio>
#include <format>
#include <string>
#include <thread>
#include <atomic>

#include "python_bridge.hpp"
#include "bigworld.hpp"
#include "math.hpp"
#include "overlay.hpp"
#include "render.hpp"
#include "esp.hpp"
#include "inventory.hpp"
#include "config.hpp"
#include "diagnostics.hpp"

// ---------------------------------------------------------------------------
//  Глобальные флаги состояния.
// ---------------------------------------------------------------------------
namespace {
    HMODULE        g_hModule     = nullptr;
    FILE*          g_console_fp  = nullptr;
    std::atomic<bool> g_should_exit{ false };

    // Состояние фич.
    std::atomic<bool> g_esp_enabled{ true };
    std::atomic<bool> g_show_inventory{ false };
}

// =========================================================================
//  console + log
// =========================================================================
namespace console {
    bool init() {
        AllocConsole();
        freopen_s(&g_console_fp, "CONOUT$", "w", stdout);
        freopen_s(&g_console_fp, "CONOUT$", "w", stderr);
        SetConsoleTitleW(L"[Stay Out] BigWorld Research Console");
        SetConsoleOutputCP(CP_UTF8);
        return true;
    }
    void free() {
        if (g_console_fp) { fclose(g_console_fp); g_console_fp = nullptr; }
        FreeConsole();
    }
}

// Путь к лог-файлу определяется один раз при старте потока (см. worker_thread):
//   %TEMP%\stayout_log.txt
// %TEMP% всегда доступен для записи от имени любого процесса, в отличие от
// жёстко зашитого пути профиля конкретного пользователя.
// fopen'аем файл на КАЖДУЮ запись (append) — это медленно, но гарантирует,
// что при жёстком краше/выгрузке лог не потеряется в несброшенном буфере.
static std::string g_log_path;

template <typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = std::format(fmt, std::forward<Args>(args)...);
    msg += '\n';
    fputs(msg.c_str(), stdout);
    fflush(stdout);
    OutputDebugStringA(msg.c_str());

    // Дублирование в файл — чтобы получить точный лог без скриншотов.
    if (!g_log_path.empty()) {
        if (FILE* f = std::fopen(g_log_path.c_str(), "a"); f) {
            std::fputs(msg.c_str(), f);
            std::fclose(f);
        }
    }
}

// ---------------------------------------------------------------------------
//  resolve_log_path — получить %TEMP%\stayout_log.txt.
//  GetEnvironmentVariableA("TEMP") возвращает что-то вроде
//  "C:\Users\<user>\AppData\Local\Temp" — гарантированно перезаписываемо.
// ---------------------------------------------------------------------------
static std::string resolve_log_path() noexcept {
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("TEMP", buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) {
        // Фолбэк: текущий каталог процесса.
        GetCurrentDirectoryA(static_cast<DWORD>(std::size(buf)), buf);
    }
    std::string p = buf;
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p += '\\';
    p += "stayout_log.txt";
    return p;
}

// =========================================================================
//  RENDER-поток — цикл отрисовки overlay.
// =========================================================================
static DWORD WINAPI render_thread(LPVOID) {
    // Инициализация overlay + renderer.
    auto& ov = overlay::Overlay::instance();
    if (!ov.initialize(config::g_config.window_title)) {
        log("[-] Overlay: окно игры '{}' не найдено. ESP отключён.",
            "Stay Out");
        return 0;
    }
    log("[+] Overlay создан поверх окна игры.");

    auto& r = render::Renderer::instance();
    if (!r.initialize(ov.hwnd())) {
        log("[-] D3D9 Renderer не инициализирован.");
        ov.shutdown();
        return 0;
    }
    log("[+] D3D9 Renderer готов.");

    // Цикл отрисовки ~60 FPS.
    while (!g_should_exit) {
        ov.sync_with_game();

        if (!r.check_device()) {
            Sleep(50);
            continue;
        }

        r.begin();

        if (g_esp_enabled) {
            esp::Config cfg = esp::current_config();
            cfg.enabled = true;
            esp::render_once(cfg);
        }

        // Опционально: показать инвентарь в углу экрана.
        if (g_show_inventory) {
            const auto rect = ov.game_rect();
            std::string inv = inventory::dump_string();
            // Рисуем построчно — простой способ без переноса строк в D3D.
            int y = 50;
            std::size_t pos = 0;
            while (pos < inv.size()) {
                auto nl = inv.find('\n', pos);
                if (nl == std::string::npos) nl = inv.size();
                r.text(10.f, static_cast<float>(y),
                       std::string_view(inv.data() + pos, nl - pos),
                       render::kYellow);
                y += 16;
                pos = nl + 1;
                if (y > rect.h - 50) break;
            }
        }

        r.end();
        r.flush_text();   // GDI-текст — строго после EndScene.
        Sleep(16); // ~60 FPS
    }

    r.shutdown();
    ov.shutdown();
    return 0;
}

// =========================================================================
//  Главный поток — инициализация + горячие клавиши.
// =========================================================================
//  init_python_phase — рискованная инициализация Python/BigWorld.
//  Вынесена отдельно, чтобы можно было обернуть в SEH без C++-объектов.
//  Возвращает true при успехе.
// =========================================================================
static int run_init_under_seh() noexcept {
    __try {
        if (!py::initialize()) return 1;  // python не найден — не краш
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;  // SEH-исключение при инициализации
    }
}

static DWORD WINAPI worker_thread(LPVOID) {
    // Резолвим путь лога и очищаем файл перед новым запуском.
    g_log_path = resolve_log_path();
    if (FILE* f = std::fopen(g_log_path.c_str(), "w"); f) std::fclose(f);

    console::init();
    log("[+] Stay Out research DLL loaded. TID = {}", GetCurrentThreadId());
    log("[*] Log file: {}", g_log_path);
    log("[*] END=unload | F9=ESP | F10=inv-on-screen | F11=inv-log | F12=attrs");

    // --- Python bridge под SEH-защитой. ---
    const int init_rc = run_init_under_seh();
    if (init_rc == 1) {
        log("[-] python27.dll не найден. Игра без встроенного Python?");
        log("[*] Проверь через Process Hacker, какая DLL с Py_* экспортами загружена.");
    } else if (init_rc == 2) {
        log("[-] SEH-исключение при инициализации Python. Интерпретатор не готов?");
    } else {
        log("[+] Python bridge: {}", py::dll_name());
        // bigworld::initialize и диагностику оборачиваем SEH на уровне exec_safe.
        (void)bigworld::initialize();

        // --- Диагностика (каждый вызов идёт через exec_safe = SEH-защита). ---
        log("[*] === Diagnostics: run dir() on key objects ===");
        log("{}", diagnostics::health_check());
        log("{}", diagnostics::dump_player_dir());
        log("{}", diagnostics::dump_camera_dir());
        log("{}", diagnostics::dump_first_entity_dir());
        log("[*] Подставь найденные имена атрибутов в config::g_config.");
    }

    // --- ESP-конфиг (параметры из config::g_config). ---
    esp::Config esp_cfg;
    const auto& gc = config::g_config;
    esp_cfg.enabled         = true;
    esp_cfg.max_distance    = gc.esp_max_distance;
    esp_cfg.show_players    = true;
    esp_cfg.show_npcs       = true;
    esp_cfg.draw_box        = true;
    esp_cfg.draw_health_bar = true;
    esp_cfg.draw_name       = true;
    esp_cfg.draw_distance   = true;
    esp_cfg.box_height_units = gc.player_height;
    esp_cfg.box_width_units  = gc.player_width;
    esp_cfg.default_fov_deg  = gc.default_fov_deg;
    esp_cfg.z_up             = (gc.coord_system == config::CoordSystem::ZUp);
    esp::configure(esp_cfg);

    // --- Старт render-потока. ---
    HANDLE h_render = CreateThread(nullptr, 0, render_thread, nullptr, 0, nullptr);

    // --- Первичный прогон данных. ---
    {
        py::GilScope gil;
        if (gil.ok()) {
            bigworld::Entity me = bigworld::player();
            if (me.valid()) {
                log("[+] Player found.");
                if (auto p = me.position(); p)
                    log("    pos = ({:.0f}, {:.0f}, {:.0f})", p->x, p->y, p->z);
                if (auto hp = me.health(); hp)
                    log("    hp = {}", *hp);
            }
            const auto ents = bigworld::all_entities();
            log("[+] Entities in world: {}", ents.size());

            log("[*] Inventory preview:");
            log("{}", inventory::dump_string());
        }
    }

    // --- Основной цикл горячих клавиш. ---
    while (!g_should_exit) {
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            log("[*] [END] pressed — unload...");
            g_should_exit = true;
            break;
        }
        if (GetAsyncKeyState(VK_F9) & 0x8000) {
            g_esp_enabled = !g_esp_enabled;
            log("[*] ESP: {}", g_esp_enabled ? "ON" : "OFF");
            Sleep(300);
        }
        if (GetAsyncKeyState(VK_F10) & 0x8000) {
            g_show_inventory = !g_show_inventory;
            log("[*] Inventory on screen: {}", g_show_inventory ? "ON" : "OFF");
            Sleep(300);
        }
        if (GetAsyncKeyState(VK_F11) & 0x8000) {
            log("[*] === Inventory dump ===");
            log("{}", inventory::dump_string());
            Sleep(300);
        }
        if (GetAsyncKeyState(VK_F12) & 0x8000) {
            // dir(player()) + dir(inventory) — для RE реальных имён атрибутов.
            py::GilScope gil;
            if (gil.ok()) {
                log("[*] dir(BigWorld.player()):");
                (void)py::exec("import BigWorld; p = BigWorld.player()\n"
                         "if p: print(chr(10).join(dir(p)))");
                log("[*] dir(player().inventory):");
                (void)py::exec("import BigWorld; p = BigWorld.player()\n"
                         "if p: print(chr(10).join(dir(p.inventory)))");
            }
            Sleep(300);
        }
        Sleep(50);
    }

    // --- Cleanup: ждём render-поток. ---
    if (h_render) {
        WaitForSingleObject(h_render, 3000);
        CloseHandle(h_render);
    }

    log("[*] Cleaning up...");
    console::free();
    FreeLibraryAndExitThread(g_hModule, 0);
}

// =========================================================================
//  DllMain.
// =========================================================================
extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_hModule = hInstance;
            DisableThreadLibraryCalls(hInstance);
            HANDLE hThread = CreateThread(nullptr, 0, worker_thread, nullptr, 0, nullptr);
            if (hThread) CloseHandle(hThread);
            break;
        }
        case DLL_PROCESS_DETACH:
            g_should_exit = true;
            break;
        default:
            break;
    }
    return TRUE;
}
