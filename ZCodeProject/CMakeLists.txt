cmake_minimum_required(VERSION 3.20)
project(stayout_research LANGUAGES CXX)

# ===========================================================================
#  Stay Out Research DLL — BigWorld engine (Python C-API bridge + D3D9 overlay).
# ===========================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# _CRT_SECURE_NO_WARNINGS — отключить шум MSVC про fopen/strcpy и т.п.
# Мы осознанно используем fopen в логировании (см. main.cpp): открытие файла
# на КАЖДУЮ запись гарантирует, что лог не потеряется при жёстком краше.
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

add_library(stayout_dll SHARED
    main.cpp
    python_bridge.cpp
    bigworld.cpp
    overlay.cpp
    render.cpp
    esp.cpp
    inventory.cpp
    diagnostics.cpp
)

target_include_directories(stayout_dll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Линкуем системные библиотеки:
#   d3d9   — IDirect3D9 для overlay-рендера.
#   dwmapi — DwmExtendFrameIntoClientArea для прозрачного окна.
# Python.lib НЕ линкуем — функции резолвятся динамически через GetProcAddress.
target_link_libraries(stayout_dll PRIVATE d3d9 dwmapi)

if(MSVC)
    target_compile_options(stayout_dll PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
    target_compile_options(stayout_dll PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(stayout_dll PRIVATE $<$<CONFIG:Debug>:/Od /Zi>)
endif()

set_target_properties(stayout_dll PROPERTIES
    PREFIX ""
    OUTPUT_NAME "stayout_dll"
)

# ===========================================================================
#  Инжектор собирается в том же дереве сборки — одной командой CMake.
#  Результат:
#    <build>/<config>/stayout_dll.dll
#    <build>/injector/<config>/injector.exe
# ===========================================================================
add_subdirectory(injector)
