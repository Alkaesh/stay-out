// =============================================================================
//  hooks.hpp — заготовка под будущие хуки функций.
//
//  Сейчас здесь только пустые Init/Shutdown, чтобы main.cpp компилировался
//  «из коробки» без MinHook. Когда подключишь библиотеку — замени тела.
// =============================================================================
#pragma once

#include <Windows.h>

namespace Hooks
{
    // Инициализация подсистемы хуков.
    //   MinHook:  MH_Initialize();
    //   VMT:      здесь обычно сохраняются оригинальные указатели из VTable.
    // Возвращает true при успехе.
    inline bool Init()
    {
        // TODO: MH_Initialize();
        return true;
    }

    // Корректное снятие всех хуков перед выгрузкой DLL.
    //   MinHook:  MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
    //   VMT:      восстановление оригинальных указателей в VTable.
    inline void Shutdown()
    {
        // TODO: MH_DisableHook(MH_ALL_HOOKS);
        // TODO: MH_Uninitialize();
    }
} // namespace Hooks
