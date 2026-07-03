cmake_minimum_required(VERSION 3.20)
project(injector LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  Инжектор Manual Map (x64 EXE).
#  Зависимости: только WinAPI (kernel32, psapi). CRT используем стандартную
#  (нужны std::ifstream/std::vector/std::string).
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(injector
    main.cpp
    manual_map.cpp
)

target_include_directories(injector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(injector PRIVATE
    Psapi
)

# Подсистема: консольное приложение (wmain).
if(MSVC)
    target_compile_options(injector PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
    target_compile_options(injector PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(injector PRIVATE $<$<CONFIG:Debug>:/Od /Zi>)
endif()

set_target_properties(injector PROPERTIES
    OUTPUT_NAME "injector"
)
